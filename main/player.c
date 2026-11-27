/*
 * player.c - Cohiradia-WAV von SD -> DSP (Halbband x4, Rampe, LUT-NCO) -> PARLIO-Loop-Ring
 *
 * Datenfluss
 *   reader_task (Core 0)  : SD lesen, WAV-Kette (auxi "next file") verfolgen
 *        |  Queue mit Rohbloecken (4096 I/Q-Paare)
 *   dsp_task    (Core 1)  : sdr_dsp_process() -> Stage-Puffer (intern) -> Ring (PSRAM)
 *        |
 *   PARLIO-TX im Hardware-Loop ueber den Ring (DMA liest endlos)
 *
 * Synchronisation Schreibzeiger <-> DMA
 *   Der DMA-Lesezeiger ist im Loop-Modus nicht abfragbar.  CPU-Takt und
 *   PARLIO-Takt stammen vom selben 40-MHz-Quarz, daher gilt exakt
 *       gespielt(t) = (t - t0) * fs_out
 *   Der Schreibzeiger wird auf einen Vorsprung ("lead") von ~60 ms geregelt.
 *   Faellt er unter 1 ms, wird der Bereich mit Stille aufgefuellt (Underrun,
 *   gezaehlt) statt alte Ringdaten abzuspielen.
 */
#include "player.h"
#include "sdr_dsp.h"
#include "sdr_plan.h"
#include "wav_info.h"
#include "playlist.h"
#include "cohi_cfg.h"
#include "driver/gpio.h"
#include "board.h"

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_clk_tree.h"
#include "driver/parlio_tx.h"

static const char *TAG = "player";

/* ---- Board-Pins ---------------------------------------------------------- */
static const int k_pins[16] = { SD0, SD1, SD2, SD3, SD4, SD5, SD6, SD7,
                                SD8, SD9, SD10, SD11, SD12, SD13, SD14, SD15 };

bool player_width_ok(int bits, char *why, size_t n)
{
    int pins[17];
    for (int i = 0; i < bits; i++) pins[i] = k_pins[i];
    pins[bits] = DAC_CLK_PIN;
    for (int i = 0; i <= bits; i++) {
        int p = pins[i];
        const char *name = (i == bits) ? "Pixeltakt" : NULL;
        char nb[16];
        if (!name) { snprintf(nb, sizeof nb, "Datenbit %d", i); name = nb; }
        const char *bad = NULL;
        if (p < 0) bad = "nicht belegt";
        else if (p == BOARD_SD_PWR_GPIO) bad = "ist GPIO45 = SD-Versorgung (Karte wuerde abgeschaltet)";
        else if (p >= 39 && p <= 44) bad = "liegt auf den SD-Karten-Pins (GPIO39-44)";
        else if (p == 37 || p == 38) bad = "ist die Konsole (UART0 GPIO37/38)";
        if (bad) { if (why) snprintf(why, n, "%s (GPIO%d) %s", name, p, bad); return false; }
    }
    return true;
}
const char *player_board_name(void) { return BOARD_NAME; }


/* ---- Konfiguration ------------------------------------------------------ */
#define RAW_SAMPLES        4096                  /* I/Q-Paare pro Rohblock          */
#define RAW_BYTES          (RAW_SAMPLES * 4)
#define RAW_BLOCKS         12                    /* 12 * 16 KiB intern              */
#define RING_MAX_BYTES     (8u * 1024 * 1024)
#define RING_MIN_BYTES     (1u * 1024 * 1024)
#define RING_TIME_MS       500
#define LEAD_TARGET_MS     60
#define LEAD_HIGH_MS       100
#define PREFILL_MS         80
#define LEAD_MIN_US        1000
#define DEFAULT_MAXRATE_HZ 10000000u

/* ---- Typen -------------------------------------------------------------- */
typedef enum { MSG_FILE, MSG_DATA, MSG_END } msg_type_t;
typedef struct {
    msg_type_t type;
    int        blk;
    uint32_t   n_iq;
    wav_info_t info;
    char       path[160];
} msg_t;

typedef struct {
    parlio_tx_unit_handle_t unit;
    uint8_t  *ring;
    size_t    ring_bytes;
    uint64_t  ring_samples;
    int       bits, bps;                 /* bytes pro Sample */
    uint32_t  fs_out;
    uint16_t  silence;
    bool      dma_started;
    int64_t   t0_us;
    uint64_t  s_wr;                      /* absolut geschriebene Samples */
    int64_t   lead_target, lead_high, prefill;
    uint32_t  underruns;
} out_t;

/* ---- Zustand ------------------------------------------------------------ */
static struct {
    int      width;
    bool     offset;
    bool     agc;
    float    manual_gain;
    uint32_t maxrate;
    int      cores;          /* 0 = auto, 1, 2 */
} s_set = {
    8,
#ifdef CONFIG_COHI_DEFAULT_OFFSET_BINARY
    true,
#else
    false,
#endif
    true, 0.5f, DEFAULT_MAXRATE_HZ, 0 };

static out_t            o;
static uint8_t         *s_ring_mem;            /* einmalig 8 MiB PSRAM */
static uint8_t         *s_blk[RAW_BLOCKS];
static QueueHandle_t    s_free_q, s_msg_q;
static EventGroupHandle_t s_evt;
#define EVT_READER_DONE  BIT0
#define EVT_DSP_DONE     BIT1
static volatile bool    s_stop;
static bool             s_active;              /* Tasks laufen */
static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
static player_status_t  s_st;
static SemaphoreHandle_t s_lock;               /* play/stop serialisieren */
static sdr_dsp_t       *s_dsp;
static uint8_t         *s_stage;
static size_t           s_stage_bytes;

/* ---- Mischer-Worker (Core 0) fuer die Zwei-Core-Variante ---------------- */
typedef struct { const sdr_dsp_t *d; size_t first, count; void *out; } mixjob_t;
static volatile mixjob_t s_job;
static TaskHandle_t s_dsp_hdl, s_mix_hdl;

static void mix_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        mixjob_t j = { s_job.d, s_job.first, s_job.count, s_job.out };
        sdr_dsp_mix(j.d, j.first, j.count, j.out);
        xTaskNotifyGive(s_dsp_hdl);
    }
}

static playlist_t s_pl;                 /* Playlist bzw. Liste mit einem Eintrag */
static bool     s_loop;
static double   s_shift_override = NAN;

/* ---- Hilfen ------------------------------------------------------------- */
static inline int64_t played_now(void)
{
    int64_t dt = esp_timer_get_time() - o.t0_us;
    if (dt < 0) dt = 0;
    return (int64_t)(((uint64_t)dt * o.fs_out) / 1000000ULL);
}

static void msync_c2m(void *p, size_t n)
{
    esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static void fill_code(uint8_t *dst, size_t n_samples, int bits, uint16_t code)
{
    if (bits == 8) memset(dst, (int)code, n_samples);
    else { uint16_t *d = (uint16_t *)dst; for (size_t i = 0; i < n_samples; i++) d[i] = code; }
}

/* Samples [first, first+n) (absolute Zaehlung) mit Stille fuellen. */
static void ring_silence(uint64_t first, uint64_t n)
{
    if (n > o.ring_samples) n = o.ring_samples;
    while (n) {
        uint64_t idx = first % o.ring_samples;
        uint64_t seg = o.ring_samples - idx;
        if (seg > n) seg = n;
        fill_code(o.ring + idx * o.bps, seg, o.bits, o.silence);
        msync_c2m(o.ring + idx * o.bps, seg * o.bps);
        first += seg; n -= seg;
    }
}

static void out_close(void)
{
    if (o.unit) {
        parlio_tx_unit_disable(o.unit);
        parlio_del_tx_unit(o.unit);
        o.unit = NULL;
    }
    o.dma_started = false;
}

static esp_err_t out_open(const sdr_plan_t *pl, int bits, bool offset_binary)
{
    out_close();
    o.bits = bits;
    o.bps = bits / 8;
    o.fs_out = pl->fs_out;
    o.silence = sdr_silence_code(bits, offset_binary);

    uint64_t want = (uint64_t)pl->fs_out * o.bps * RING_TIME_MS / 1000;
    if (want > RING_MAX_BYTES) want = RING_MAX_BYTES;
    if (want < RING_MIN_BYTES) want = RING_MIN_BYTES;
    o.ring_bytes = (size_t)(want & ~(uint64_t)4095);
    o.ring_samples = o.ring_bytes / o.bps;
    o.ring = s_ring_mem;

    int64_t fs = pl->fs_out;
    o.lead_target = fs * LEAD_TARGET_MS / 1000;
    o.lead_high   = fs * LEAD_HIGH_MS / 1000;
    o.prefill     = fs * PREFILL_MS / 1000;
    if (o.lead_high   > (int64_t)o.ring_samples / 2) o.lead_high   = o.ring_samples / 2;
    if (o.lead_target > (int64_t)o.ring_samples / 4) o.lead_target = o.ring_samples / 4;
    if (o.prefill     > (int64_t)o.ring_samples / 3) o.prefill     = o.ring_samples / 3;
    o.s_wr = 0;
    o.underruns = 0;

    ring_silence(0, o.ring_samples);

    parlio_tx_unit_config_t cfg = {
        .clk_src = PARLIO_CLK_SRC_PLL_F160M,
        .data_width = (uint32_t)bits,
        .clk_in_gpio_num = -1,
        .clk_out_gpio_num = DAC_CLK_PIN,
        .valid_gpio_num = -1,
        .output_clk_freq_hz = pl->fs_out,
        .trans_queue_depth = 2,
        .max_transfer_size = o.ring_bytes,
    };
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = (i < bits) ? k_pins[i] : -1;

    esp_err_t e = parlio_new_tx_unit(&cfg, &o.unit);
    if (e != ESP_OK) { ESP_LOGE(TAG, "parlio_new_tx_unit: %s", esp_err_to_name(e)); o.unit = NULL; return e; }
    e = parlio_tx_unit_enable(o.unit);
    if (e != ESP_OK) { ESP_LOGE(TAG, "parlio enable: %s", esp_err_to_name(e)); out_close(); return e; }
    return ESP_OK;
}

static esp_err_t out_start_dma(void)
{
    if (o.dma_started || !o.unit) return ESP_OK;
    parlio_transmit_config_t tc = { .idle_value = o.silence };
    tc.flags.loop_transmission = 1;
    o.t0_us = esp_timer_get_time();
    esp_err_t e = parlio_tx_unit_transmit(o.unit, o.ring, o.ring_bytes * 8, &tc);
    if (e != ESP_OK) { ESP_LOGE(TAG, "parlio transmit: %s", esp_err_to_name(e)); return e; }
    o.dma_started = true;
    return ESP_OK;
}

static bool sleep_ms_stoppable(int ms)
{
    while (ms > 0 && !s_stop) {
        int d = ms > 10 ? 10 : ms;
        vTaskDelay(pdMS_TO_TICKS(d) ? pdMS_TO_TICKS(d) : 1);
        ms -= d;
    }
    return !s_stop;
}

/* n Samples (aus Stage) in den Ring schreiben; bremst auf lead_target/high. */
static void ring_put(const uint8_t *src, size_t n)
{
    if (o.dma_started) {
        for (;;) {
            int64_t lead = (int64_t)o.s_wr - played_now();
            if (lead <= o.lead_high || s_stop) break;
            int ms = (int)((lead - o.lead_target) * 1000 / o.fs_out);
            if (ms < 1) ms = 1;
            if (ms > 20) ms = 20;
            vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
        }
        int64_t pn = played_now();
        int64_t lead = (int64_t)o.s_wr - pn;
        if (lead * 1000000 / (int64_t)o.fs_out < LEAD_MIN_US) {          /* Underrun */
            uint64_t neu = (uint64_t)(pn + o.lead_target / 2);
            ring_silence(o.s_wr, neu - o.s_wr);                           /* Veraltetes (letzte Umdrehung) loeschen */
            o.underruns++;
            o.s_wr = neu;
        }
    }
    while (n && !s_stop) {
        uint64_t idx = o.s_wr % o.ring_samples;
        uint64_t seg = o.ring_samples - idx;
        if (seg > n) seg = n;
        memcpy(o.ring + idx * o.bps, src, seg * o.bps);
        msync_c2m(o.ring + idx * o.bps, seg * o.bps);
        o.s_wr += seg; src += seg * o.bps; n -= seg;
    }
    if (!o.dma_started && (int64_t)o.s_wr >= o.prefill) out_start_dma();
}

static void status_update(void (*fn)(player_status_t *, void *), void *arg)
{
    portENTER_CRITICAL(&s_mux);
    fn(&s_st, arg);
    portEXIT_CRITICAL(&s_mux);
}

/* ---- Reader-Task -------------------------------------------------------- */
static bool send_msg(const msg_t *m)
{
    while (!s_stop) {
        if (xQueueSend(s_msg_q, m, pdMS_TO_TICKS(50)) == pdTRUE) return true;
    }
    return false;
}


#define PREFETCH_BYTES (4u * 1024 * 1024)      /* naechste Datei auswerten, sobald noch <= 4 MB uebrig sind */

static bool exists_cb(const char *path, void *ctx)
{
    (void)ctx;
    struct stat sb;
    return stat(path, &sb) == 0;
}

/* Ablauf: Playlist-Eintrag -> alle auxi-Folgedateien -> naechster Eintrag (-> bei loop wieder von vorn). */
static void reader_task(void *arg)
{
    (void)arg;
    seq_t seq;
    seq_init(&seq, &s_pl, s_loop);
    msg_t *pre = NULL;                          /* bereits ausgewertete naechste Datei */
    char pre_path[PL_PATH_MAX] = "";
    bool more = true;

    while (more && !s_stop) {
        const char *cur = seq_current(&seq);
        msg_t *m = NULL;
        if (pre && !strcmp(pre_path, cur)) { m = pre; pre = NULL; }      /* Header lag schon vor */
        else {
            free(pre); pre = NULL;
            m = calloc(1, sizeof *m);
            if (!m) break;
            char err[128];
            if (wav_parse(cur, &m->info, err, sizeof err) != 0) {
                ESP_LOGW(TAG, "%s: %s - uebersprungen", cur, err);
                free(m); m = NULL;
            }
        }

        bool ok = false;
        char next_name[sizeof m->info.next_name] = "";
        if (m) {
            if (!isnan(s_shift_override)) {      /* manuelle Vorgabe gilt fuer alle Dateien */
                m->info.shift_hz = s_shift_override;
                m->info.shift_from_name = m->info.shift_from_auxi = false;
            }
            sdr_plan_t chk;
            int fd = -1;
            if (sdr_plan_rate(m->info.sample_rate, m->info.shift_hz, s_set.maxrate, &chk) != 0) {
                /* gar nicht erst lesen (eine 2-GiB-Datei zu lesen und zu verwerfen dauert Minuten) */
                ESP_LOGW(TAG, "%s: fs=%u Hz zu hoch fuer maxrate=%u Hz (4*fs > maxrate) - uebersprungen",
                         cur, (unsigned)m->info.sample_rate, (unsigned)s_set.maxrate);
            } else if ((fd = open(cur, O_RDONLY)) < 0) {
                ESP_LOGW(TAG, "open %s fehlgeschlagen - uebersprungen", cur);
            } else {
                m->type = MSG_FILE;
                strlcpy(m->path, cur, sizeof m->path);
                portENTER_CRITICAL(&s_mux);
                s_st.pl_index = seq.idx + 1;
                s_st.pl_passes = seq.passes;
                portEXIT_CRITICAL(&s_mux);
                if (!send_msg(m)) { close(fd); free(m); break; }
                ok = true;
                strlcpy(next_name, m->info.next_name, sizeof next_name);

                lseek(fd, (off_t)m->info.data_offset, SEEK_SET);
                uint64_t remaining = m->info.data_bytes;
                bool pre_tried = false;
                while (remaining >= 4 && !s_stop) {
                    int blk;
                    if (xQueueReceive(s_free_q, &blk, pdMS_TO_TICKS(50)) != pdTRUE) continue;
                    size_t want = remaining < RAW_BYTES ? (size_t)remaining : RAW_BYTES;
                    ssize_t r = read(fd, s_blk[blk], want);
                    if (r < 4) { xQueueSend(s_free_q, &blk, 0); break; }
                    r &= ~3;
                    msg_t d = { .type = MSG_DATA, .blk = blk, .n_iq = (uint32_t)(r / 4) };
                    if (!send_msg(&d)) { xQueueSend(s_free_q, &blk, 0); break; }
                    remaining -= (uint64_t)r;

                    /* naechste Datei (Folgedatei oder naechster Listeneintrag) vorab auswerten:
                     * am Dateiende bleiben nur noch open + lseek */
                    if (!pre_tried && remaining <= PREFETCH_BYTES) {
                        pre_tried = true;
                        char cand[PL_PATH_MAX];
                        if (seq_peek(&seq, next_name, cand, sizeof cand)) {
                            pre = calloc(1, sizeof *pre);
                            char err[128];
                            if (pre && wav_parse(cand, &pre->info, err, sizeof err) != 0) { free(pre); pre = NULL; }   /* Fehler meldet spaeter der Hauptpfad */
                            else if (pre) strlcpy(pre_path, cand, sizeof pre_path);
                        }
                    }
                }
                close(fd);
            }
            free(m);
        }

        more = seq_advance(&seq, ok, next_name, exists_cb, NULL);
        if (seq.msg[0]) {
            if (strstr(seq.msg, "nicht gefunden") || strstr(seq.msg, "zyklisch") || strstr(seq.msg, "abspielbar"))
                ESP_LOGW(TAG, "%s", seq.msg);
            else ESP_LOGI(TAG, "%s", seq.msg);
        }
    }
    free(pre);
    msg_t e = { .type = MSG_END };
    send_msg(&e);
    xEventGroupSetBits(s_evt, EVT_READER_DONE);
    vTaskDelete(NULL);
}

/* ---- DSP-Task ----------------------------------------------------------- */
typedef struct { const wav_info_t *i; const char *path; const sdr_plan_t *pl; int bits; } st_file_t;
static void st_file(player_status_t *s, void *a)
{
    st_file_t *f = a;
    strlcpy(s->file, f->path, sizeof s->file);
    strlcpy(s->next, f->i->next_name, sizeof s->next);
    s->fs = f->i->sample_rate; s->shift_hz = f->i->shift_hz;
    s->n = f->pl->n; s->fs_out = f->pl->fs_out; s->bits = f->bits;
    s->exact = f->pl->exact; s->alias = f->pl->alias; s->nyquist_ok = f->pl->nyquist_ok;
    s->pos_iq = 0; s->total_iq = f->i->data_bytes / 4;
    s->files_played++;
}

static void drain_and_wait(void)
{
    /* nach dem letzten Datenblock: restliche (unabgespielte) Daten ausspielen lassen */
    if (!o.dma_started) { out_start_dma(); }
    for (;;) {
        int64_t lead = (int64_t)o.s_wr - played_now();
        if (lead <= 0 || s_stop) break;
        if (!sleep_ms_stoppable((int)(lead * 1000 / o.fs_out) + 2)) break;
    }
}

static void dsp_task(void *arg)
{
    (void)arg;
    bool dsp_valid = false;
    uint32_t dsp_fs = 0; int dsp_n = 0;
    int B = 1024;
    msg_t *m = malloc(sizeof *m);
    int64_t win_start = esp_timer_get_time();
    uint64_t busy64 = 0;
    int64_t last_rest = esp_timer_get_time();
    const uint32_t cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    bool natural_end = false;
    bool skip_file = false;          /* Datei nicht spielbar (Rate): Datenbloecke bis zur naechsten Datei verwerfen */
    bool session_first = true;       /* erste Datei einer Sitzung: Ausgang immer sauber neu starten */
    bool use2 = false;               /* Mischer auf zwei Cores */
    float split_local = 0.0f;        /* Mischer-Anteil auf dem Filter-Core */
    int hold = 1, bps = 1;
    s_dsp_hdl = xTaskGetCurrentTaskHandle();

    while (m) {
        if (xQueueReceive(s_msg_q, m, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (s_stop) break;
            continue;
        }
        if (m->type == MSG_END) { natural_end = true; break; }

        if (m->type == MSG_FILE) {
            sdr_plan_t pl;
            double shift = m->info.shift_hz;
            skip_file = false;
            if (sdr_plan_rate(m->info.sample_rate, shift, s_set.maxrate, &pl) != 0) {
                ESP_LOGW(TAG, "%s: fs=%u Hz zu hoch fuer maxrate=%u Hz (4*fs > maxrate) - uebersprungen",
                         m->path, (unsigned)m->info.sample_rate, (unsigned)s_set.maxrate);
                skip_file = true;
                continue;
            }
            const int bits = s_set.width;
            bool need_open = session_first || !o.unit || o.fs_out != pl.fs_out || o.bits != bits;
            session_first = false;
            if (need_open) {
                if (o.unit && o.dma_started) { drain_and_wait(); }
                if (s_stop) break;
                if (out_open(&pl, bits, s_set.offset) != ESP_OK) { s_stop = true; break; }
            }
            if (!dsp_valid || dsp_fs != m->info.sample_rate || dsp_n != pl.n || need_open) {
                const bool fast = m->info.sample_rate >= SDR_FAST_FS_HZ;
                sdr_dsp_cfg_t c = { .bits = bits, .offset_binary = s_set.offset, .hold = fast ? pl.n / 2 : pl.n / 4,
                                    .phase_inc = sdr_phase_inc(shift, pl.fs_out),
                                    .fast = fast };
                s_dsp->agc = s_set.agc; s_dsp->manual_gain = s_set.manual_gain;
                sdr_dsp_init(s_dsp, &c);
                dsp_valid = true; dsp_fs = m->info.sample_rate; dsp_n = pl.n;
                hold = c.hold; bps = bits / 8;
            } else {
                sdr_dsp_set_phase_inc(s_dsp, sdr_phase_inc(shift, pl.fs_out));
            }
            B = SDR_BLOCK_MAX;
            while (B > 16 && (size_t)B * pl.n > 8192) B >>= 1;
            size_t need = (size_t)B * pl.n * (bits / 8);
            if (need > s_stage_bytes) {
                free(s_stage);
                s_stage = heap_caps_aligned_alloc(64, need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                s_stage_bytes = s_stage ? need : 0;
                if (!s_stage) { ESP_LOGE(TAG, "kein Speicher fuer Stage"); s_stop = true; break; }
            }
            st_file_t sf = { &m->info, m->path, &pl, bits };
            status_update(st_file, &sf);

            ESP_LOGI(TAG, "%s", m->path);
            ESP_LOGI(TAG, "  fs=%u Hz, Shift=%.3f kHz (%s), N=%d -> PARLIO %.3f MHz, %d Bit%s",
                     (unsigned)m->info.sample_rate, shift / 1e3,
                     m->info.shift_from_name ? "Dateiname" : (m->info.shift_from_auxi ? "auxi" : "manuell/unbekannt"),
                     pl.n, pl.fs_out / 1e6, bits, pl.exact ? ", exakter Teiler" : ", BRUCHTEILER (Jitter, Rate evtl. ungenau!)");
            if (pl.alias)
                ESP_LOGW(TAG, "  ACHTUNG: Fs_out < 2*fmax - Nutzsignal liegt nur als Bild/Alias vor (Alias-Frequenz %.3f MHz); Bandpass noetig",
                         pl.alias_hz / 1e6);
            else if (!pl.nyquist_ok)
                ESP_LOGW(TAG, "  Hinweis: Abstand zu Nyquist gering (Fs_out < %.1f x fmax)", SDR_NYQ_FACTOR);
            {
                float est = sdr_est_load_pct(m->info.sample_rate, pl.fs_out, m->info.sample_rate >= SDR_FAST_FS_HZ, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
                ESP_LOGI(TAG, "  Filter: %s, geschaetzte DSP-Last %.0f %% eines Cores", m->info.sample_rate >= SDR_FAST_FS_HZ ? "FAST (1 Halbband)" : "HQ (2 Halbbaender)", est);
                bool fastm = m->info.sample_rate >= SDR_FAST_FS_HZ;
                use2 = (s_set.cores == 2) || (s_set.cores == 0 && est >= 85.0f);
                if (use2) {
                    split_local = sdr_est_split(m->info.sample_rate, pl.fs_out, fastm);
                    double fc_, mc_;
                    sdr_est_cycles(m->info.sample_rate, pl.fs_out, fastm, &fc_, &mc_);
                    float l1 = (float)((fc_ + split_local * mc_) / (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1e6) * 100.0);
                    float l0 = (float)(((1.0f - split_local) * mc_) / (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1e6) * 100.0);
                    ESP_LOGI(TAG, "  2 Cores: Core 1 = Filter + %.0f %% Mischer (~%.0f %%), Core 0 = %.0f %% Mischer (~%.0f %%)",
                             split_local * 100.0f, l1, (1.0f - split_local) * 100.0f, l0);
                    if (l1 > 90.0f || l0 > 90.0f) ESP_LOGW(TAG, "  Auch mit 2 Cores zu hohe Last - Underruns wahrscheinlich");
                } else if (est > 85.0f) {
                    ESP_LOGW(TAG, "  Last > 85 %%: Underruns sehr wahrscheinlich ('cores 2' oder niedrigeres 'maxrate' bzw. kleinere fs)");
                }
                portENTER_CRITICAL(&s_mux); s_st.cores = use2 ? 2 : 1; s_st.split_local = split_local; portEXIT_CRITICAL(&s_mux);
            }
            if (m->info.next_name[0]) ESP_LOGI(TAG, "  Folgedatei: %s", m->info.next_name);
            continue;
        }

        /* MSG_DATA */
        if (skip_file) { xQueueSend(s_free_q, &m->blk, 0); continue; }
        const int16_t *iq = (const int16_t *)s_blk[m->blk];
        size_t done = 0, n = m->n_iq;
        while (done < n && !s_stop) {
            size_t k = n - done < (size_t)B ? n - done : (size_t)B;
            size_t outn;
            uint32_t c0 = esp_cpu_get_cycle_count();
            if (!use2) {
                outn = sdr_dsp_process(s_dsp, iq + 2 * done, k, s_stage);
                busy64 += (uint32_t)(esp_cpu_get_cycle_count() - c0);    /* reine DSP-Zeit (ohne Pacing-Schlaf) */
            } else {
                size_t nbb = sdr_dsp_filter(s_dsp, iq + 2 * done, k);
                size_t j0 = nbb < 256 ? nbb : ((size_t)((float)nbb * split_local)) & ~(size_t)63;   /* 64er-Raster: Cacheline-Grenze im Stage-Puffer */
                if (j0 < nbb) {
                    s_job.d = s_dsp; s_job.first = j0; s_job.count = nbb - j0;
                    s_job.out = s_stage + j0 * (size_t)hold * (size_t)bps;
                    xTaskNotifyGive(s_mix_hdl);
                }
                if (j0) sdr_dsp_mix(s_dsp, 0, j0, s_stage);
                busy64 += (uint32_t)(esp_cpu_get_cycle_count() - c0);
                if (j0 < nbb) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    /* Worker fertig (Wartezeit zaehlt nicht als Last) */
                sdr_dsp_commit(s_dsp, nbb);
                outn = nbb * (size_t)hold;
            }
            ring_put(s_stage, outn);
            done += k;
        }
        xQueueSend(s_free_q, &m->blk, 0);

        int64_t now = esp_timer_get_time();
        if (now - last_rest > 100000) {        /* im Rueckstand nie schlafen -> IDLE/WDT aushungern verhindern */
            vTaskDelay(1);
            last_rest = esp_timer_get_time();
        }
        if (now - win_start > 500000) {
            float load = (float)((double)busy64 / ((double)(now - win_start) * cpu_mhz)) * 100.0f;
            float lead = o.dma_started ? (float)(((int64_t)o.s_wr - played_now()) * 1000.0 / o.fs_out) : 0.0f;
            portENTER_CRITICAL(&s_mux);
            s_st.dsp_load_pct = load;
            s_st.lead_ms = lead;
            s_st.underruns = o.underruns;
            s_st.gain = s_dsp->gain;
            s_st.peak = s_dsp->last_peak;
            s_st.output_running = o.dma_started;
            portEXIT_CRITICAL(&s_mux);
            win_start = now; busy64 = 0;
        }
        portENTER_CRITICAL(&s_mux);
        s_st.pos_iq += n;
        portEXIT_CRITICAL(&s_mux);
    }

    if (!natural_end) s_stop = true;           /* Fehler/Stop: Reader beenden */

    /* Ende: Rest ausspielen, danach Stille */
    if (o.unit) {
        if (!s_stop) {
            if (!o.dma_started) out_start_dma();
            /* Stille hinter den letzten Daten, restlichen Vorsprung noch abspielen lassen */
            int64_t pn = played_now();
            int64_t lead = (int64_t)o.s_wr - pn;
            uint64_t free_s = o.ring_samples - (lead > 0 ? lead : 0) - (o.fs_out / 500);
            ring_silence(o.s_wr, free_s);
        } else {
            ring_silence(0, o.ring_samples);       /* sofort Stille */
        }
        portENTER_CRITICAL(&s_mux);
        s_st.output_running = o.dma_started;
        portEXIT_CRITICAL(&s_mux);
    }
    free(m);
    xEventGroupSetBits(s_evt, EVT_DSP_DONE);
    vTaskDelete(NULL);
}

/* ---- API ---------------------------------------------------------------- */
esp_err_t player_init(void)
{
    sdr_dsp_global_init();
    s_lock = xSemaphoreCreateMutex();
    s_evt = xEventGroupCreate();
    s_ring_mem = heap_caps_aligned_alloc(64, RING_MAX_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    s_dsp = heap_caps_malloc(sizeof(sdr_dsp_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_ring_mem || !s_dsp || !s_lock || !s_evt) {
        ESP_LOGE(TAG, "Speicher fuer Ring/DSP fehlt (PSRAM aktiv?)");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < RAW_BLOCKS; i++) {
        s_blk[i] = heap_caps_aligned_alloc(64, RAW_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_blk[i]) { ESP_LOGE(TAG, "Rohblock %d: kein internes RAM", i); return ESP_ERR_NO_MEM; }
    }
    xTaskCreatePinnedToCore(mix_worker, "sdr_mix", 3072, NULL, 11, &s_mix_hdl, 0);
    memset(s_dsp, 0, sizeof *s_dsp);
    s_dsp->agc = true; s_dsp->manual_gain = 0.5f;
    ESP_LOGI(TAG, "Ring %u KiB PSRAM, %d Rohbloecke a %d KiB intern, DSP-Kontext %u KiB",
             (unsigned)(RING_MAX_BYTES / 1024), RAW_BLOCKS, RAW_BYTES / 1024, (unsigned)(sizeof(sdr_dsp_t) / 1024));
    return ESP_OK;
}

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }

static void stop_locked(void)
{
    if (!s_active) return;
    s_stop = true;
    xEventGroupWaitBits(s_evt, EVT_READER_DONE | EVT_DSP_DONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(5000));
    vQueueDelete(s_msg_q); vQueueDelete(s_free_q);
    s_msg_q = s_free_q = NULL;
    playlist_free(&s_pl);
    s_active = false;
    portENTER_CRITICAL(&s_mux);
    s_st.playing = false;
    portEXIT_CRITICAL(&s_mux);
}

void player_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stop_locked();
    xSemaphoreGive(s_lock);
}

esp_err_t player_play(const char *abs_path, double shift_override_hz, bool loop)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* laufende Wiedergabe beenden; ein bereits beendeter Lauf (Playlist-Ende) wird abgeraeumt */
    if (s_active) stop_locked();

    char err[128], why[96];
    if (playlist_is_name(abs_path)) {
        if (playlist_load(abs_path, BOARD_SD_MOUNT_POINT, &s_pl, err, sizeof err) != 0) {
            printf("Fehler: %s\n", err);
            xSemaphoreGive(s_lock);
            return ESP_FAIL;
        }
        printf("Playlist '%s': %d Eintraege%s%s\n", s_pl.name, s_pl.n,
               s_pl.skipped ? ", ungueltige Zeilen verworfen" : "", loop ? ", Endlosschleife" : "");
        if (s_pl.cohi[0]) player_apply_directives(s_pl.cohi);      /* wirken wie Konsolenbefehle zu Beginn der Sitzung */
    } else {
        /* einzelne Datei: sofort pruefen, damit Fehler direkt in der Konsole stehen */
        wav_info_t w;
        if (wav_parse(abs_path, &w, err, sizeof err) != 0) {
            printf("Fehler: %s\n", err);
            xSemaphoreGive(s_lock);
            return ESP_FAIL;
        }
        double shift = isnan(shift_override_hz) ? w.shift_hz : shift_override_hz;
        sdr_plan_t pl;
        if (sdr_plan_rate(w.sample_rate, shift, s_set.maxrate, &pl) != 0) {
            printf("Fehler: fs=%u Hz nicht machbar (4*fs > maxrate %u Hz). 'maxrate' erhoehen oder kleinere Rate.\n",
                   (unsigned)w.sample_rate, (unsigned)s_set.maxrate);
            xSemaphoreGive(s_lock);
            return ESP_ERR_NOT_SUPPORTED;
        }
        playlist_single(&s_pl, abs_path);
        if (s_pl.n != 1) { xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
    }

    if (!player_width_ok(s_set.width, why, sizeof why)) {
        printf("Fehler: %d Bit auf diesem Board nicht moeglich: %s\n", s_set.width, why);
        playlist_free(&s_pl);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_free_q = xQueueCreate(RAW_BLOCKS, sizeof(int));
    s_msg_q = xQueueCreate(RAW_BLOCKS + 8, sizeof(msg_t));
    if (!s_free_q || !s_msg_q) {
        if (s_free_q) vQueueDelete(s_free_q);
        if (s_msg_q) vQueueDelete(s_msg_q);
        s_free_q = s_msg_q = NULL;
        playlist_free(&s_pl);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < RAW_BLOCKS; i++) xQueueSend(s_free_q, &i, 0);

    s_loop = loop;
    s_shift_override = shift_override_hz;
    s_stop = false;
    xEventGroupClearBits(s_evt, EVT_READER_DONE | EVT_DSP_DONE);
    portENTER_CRITICAL(&s_mux);
    memset(&s_st, 0, sizeof s_st);
    s_st.playing = true;
    s_st.loop = loop;
    s_st.pl_total = s_pl.n;
    s_st.pl_index = 1;
    strlcpy(s_st.pl_name, s_pl.name, sizeof s_st.pl_name);
    portEXIT_CRITICAL(&s_mux);
    s_active = true;

    xTaskCreatePinnedToCore(dsp_task, "sdr_dsp", 6144, NULL, 10, NULL, 1);
    xTaskCreatePinnedToCore(reader_task, "sdr_rd", 8192, NULL, 6, NULL, 0);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void player_get_status(player_status_t *st)
{
    portENTER_CRITICAL(&s_mux);
    *st = s_st;
    portEXIT_CRITICAL(&s_mux);
    /* Lauf beendet (Playlist-Ende)? */
    if (st->playing && s_evt && (xEventGroupGetBits(s_evt) & EVT_DSP_DONE)) st->playing = false;
}

void player_set_width(int bits)        { s_set.width = (bits == 16) ? 16 : 8; }
void player_set_cores(int c)           { s_set.cores = (c == 1 || c == 2) ? c : 0; }
int  player_get_cores(void)            { return s_set.cores; }
int  player_get_width(void)            { return s_set.width; }
void player_set_offset(bool ob)        { s_set.offset = ob; }
bool player_get_offset(void)           { return s_set.offset; }
void player_set_gain(bool agc, float g)
{
    s_set.agc = agc; if (g > 0) s_set.manual_gain = g;
    if (s_dsp) { s_dsp->agc = agc; if (g > 0) s_dsp->manual_gain = g; }   /* live (blockweise gelesen) */
}
void player_get_gain(bool *agc, float *g) { *agc = s_set.agc; *g = s_set.manual_gain; }
void player_set_maxrate(uint32_t hz)   { s_set.maxrate = hz; }
uint32_t player_get_maxrate(void)      { return s_set.maxrate; }

/* ---- Selbsttest: Benchmark + Zwei-Core-Bitgleichheit ----------------------- */
typedef struct { SemaphoreHandle_t done; bool busy_at_start; } st_arg_t;

static void selftest_task(void *arg)
{
    st_arg_t *a = arg;
    sdr_dsp_t *A = heap_caps_malloc(sizeof(sdr_dsp_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    sdr_dsp_t *B = heap_caps_malloc(sizeof(sdr_dsp_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t OUTMAX = 4 * SDR_BLOCK_MAX * 4;               /* hold <= 4, 8 Bit */
    uint8_t *oA = heap_caps_aligned_alloc(64, OUTMAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *oB = heap_caps_aligned_alloc(64, OUTMAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *iq = heap_caps_malloc(4 * SDR_BLOCK_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!A || !B || !oA || !oB || !iq) { printf("selftest: kein Speicher\n"); goto out; }

    s_dsp_hdl = xTaskGetCurrentTaskHandle();
    static const struct { const char *name; bool fast; int hold; double m_in, m_out; } modes[] = {
        { "HQ   (2 Halbbaender, Rampe x4)", false, 4, 203 * 1.2, 22.8 },
        { "FAST (1 Halbband,    Rampe x2)", true,  2,  97 * 1.2, 22.8 },
    };
    const int BLOCKS = 200, N = SDR_BLOCK_MAX;
    bool all_ok = true;
    printf("Selbsttest auf %s, %d MHz, Core %d\n", BOARD_NAME, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, (int)xPortGetCoreID());

    for (int mi = 0; mi < 2; mi++) {
        sdr_dsp_cfg_t cfg = { .bits = 8, .offset_binary = false, .hold = modes[mi].hold,
                              .phase_inc = (uint32_t)(0.2371 * 4294967296.0), .fast = modes[mi].fast };
        memset(A, 0, sizeof *A); memset(B, 0, sizeof *B);
        A->agc = B->agc = true; A->manual_gain = B->manual_gain = 0.5f;
        sdr_dsp_init(A, &cfg); sdr_dsp_init(B, &cfg);

        uint64_t cf = 0, cm = 0, cs = 0, nin = 0, nout = 0;
        int bad = 0;
        uint32_t seed = 12345, ph = 0;
        for (int b = 0; b < BLOCKS; b++) {
            for (int i = 0; i < N; i++) {                          /* Rauschen + langsamer Ton, ~ -10 dBFS */
                ph += 0x00A00000u;
                int32_t tone = (int32_t)(((ph >> 20) & 0x7FF) - 1024) * 6;
                iq[2*i]   = (int16_t)(tone + (int32_t)(lcg(&seed) >> 22) - 512);
                iq[2*i+1] = (int16_t)(-tone / 2 + (int32_t)(lcg(&seed) >> 22) - 512);
            }
            /* Referenz: einteilig, mit Zeitmessung */
            uint32_t t0 = esp_cpu_get_cycle_count();
            size_t nbb = sdr_dsp_filter(A, iq, N);
            uint32_t t1 = esp_cpu_get_cycle_count();
            size_t outn = sdr_dsp_mix(A, 0, nbb, oA);
            uint32_t t2 = esp_cpu_get_cycle_count();
            sdr_dsp_commit(A, nbb);
            cf += t1 - t0; cm += t2 - t1; nin += N; nout += outn;

            /* geteilt: 40 % lokal, 60 % Worker auf Core 0 */
            uint32_t t3 = esp_cpu_get_cycle_count();
            size_t nbb2 = sdr_dsp_filter(B, iq, N);
            size_t j0 = ((size_t)((float)nbb2 * 0.4f)) & ~(size_t)63;
            s_job.d = B; s_job.first = j0; s_job.count = nbb2 - j0;
            s_job.out = oB + j0 * (size_t)modes[mi].hold;
            xTaskNotifyGive(s_mix_hdl);
            sdr_dsp_mix(B, 0, j0, oB);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            sdr_dsp_commit(B, nbb2);
            cs += esp_cpu_get_cycle_count() - t3;

            if (nbb2 != nbb || memcmp(oA, oB, outn) != 0) bad++;
        }
        double cin = (double)cf / nin, cout_ = (double)cm / nout;
        double single = (double)(cf + cm), dual = (double)cs;
        printf("%s\n", modes[mi].name);
        printf("   Filter : %6.1f Takte / Eingangssample  (Modell %.0f)\n", cin, modes[mi].m_in);
        printf("   Mischer: %6.1f Takte / Ausgangssample  (Modell %.1f)\n", cout_, modes[mi].m_out);
        printf("   2 Cores: %d Bloecke, %d Abweichungen -> %s;  Zeit/Block 1 Core %.0f, 2 Cores %.0f Takte (x%.2f)\n",
               BLOCKS, bad, bad ? "FEHLER" : "bit-identisch", single / BLOCKS, dual / BLOCKS, single / dual);
        if (bad) all_ok = false;
    }
    printf(all_ok ? "Ergebnis: OK - 'cores 2' ist auf diesem Board bedenkenlos nutzbar.\n"
                  : "Ergebnis: FEHLER - Zwei-Core-Ausgabe weicht ab (Cache-Koharenz?). 'cores 1' verwenden!\n");
out:
    free(A); free(B); free(oA); free(oB); free(iq);
    xSemaphoreGive(a->done);
    vTaskDelete(NULL);
}

esp_err_t player_selftest(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) {
        player_status_t st; portENTER_CRITICAL(&s_mux); st = s_st; portEXIT_CRITICAL(&s_mux);
        if (st.playing && !(xEventGroupGetBits(s_evt) & EVT_DSP_DONE)) {
            printf("selftest: bitte zuerst 'stop'\n");
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        stop_locked();
    }
    st_arg_t a = { .done = xSemaphoreCreateBinary() };
    xTaskCreatePinnedToCore(selftest_task, "sdr_test", 6144, &a, 10, NULL, 1);   /* Core 1 wie der DSP-Task */
    xSemaphoreTake(a.done, portMAX_DELAY);
    vSemaphoreDelete(a.done);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/* Datenpins + Takt ab dem Einschalten auf "Stille" legen (vor dem SD-Zugriff), damit der DAC nicht an
 * schwebenden Eingaengen haengt.  Pegel: Stillecode des Standardformats (menuconfig: Offset-Binary). */
void player_idle_pins(void)
{
    const int bits = s_set.width;
    if (!player_width_ok(bits, NULL, 0)) return;
    const uint16_t sil = sdr_silence_code(bits, s_set.offset);
    for (int i = 0; i < bits; i++) {
        gpio_reset_pin((gpio_num_t)k_pins[i]);
        gpio_set_direction((gpio_num_t)k_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)k_pins[i], (sil >> i) & 1);
    }
    gpio_reset_pin((gpio_num_t)DAC_CLK_PIN);
    gpio_set_direction((gpio_num_t)DAC_CLK_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)DAC_CLK_PIN, 0);
}

/* "#COHI"-Direktiven einer Playlist wie die entsprechenden Konsolenbefehle anwenden. */
void player_apply_directives(const char *text)
{
    cohi_cfg_t c;
    cohi_cfg_init(&c);
    cohi_cfg_parse(&c, text);
    for (int i = 0; i < c.nwarn && i < COHI_MAX_WARN; i++) printf("Playlist-Direktive: %s\n", c.warn[i]);
    if (c.nwarn > COHI_MAX_WARN) printf("Playlist-Direktive: %d weitere Warnungen\n", c.nwarn - COHI_MAX_WARN);
    if (c.has_width) {
        char why[96];
        if (player_width_ok(c.width, why, sizeof why)) s_set.width = c.width;
        else printf("Playlist-Direktive width=%d nicht moeglich: %s\n", c.width, why);
    }
    if (c.has_offset) s_set.offset = c.offset;
    if (c.has_gain) player_set_gain(c.agc, c.agc ? 0 : c.gain);
    if (c.has_maxrate) s_set.maxrate = c.maxrate_hz;
    if (c.has_cores) s_set.cores = c.cores;
    char d[160];
    cohi_cfg_describe(&c, d, sizeof d);
    if (d[0]) printf("Playlist-Direktiven: %s\n", d);
}
