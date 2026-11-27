#include "autoplay.h"
#include "autoplay_logic.h"
#include "player.h"
#include "playlist.h"
#include "cohi_cfg.h"
#include "sdcard.h"
#include "board.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "driver/uart.h"

#ifndef CONFIG_COHI_AUTOPLAY_FILE
#define CONFIG_COHI_AUTOPLAY_FILE "/autoplay.m3u"
#endif
#ifndef CONFIG_COHI_AUTOPLAY_DELAY_S
#define CONFIG_COHI_AUTOPLAY_DELAY_S 3
#endif

static const char *TAG = "autoplay";
static char          s_real[PL_PATH_MAX];
static volatile bool s_enabled;              /* Ueberwachung aktiv */
static volatile bool s_kick;                 /* sofort (ohne Pause) starten */
static bool          s_loop = true;
static volatile int  s_attempt;               /* nur fuer die Anzeige (Kopie aus ap_t) */
static TaskHandle_t  s_task;

static void build_path(void)
{
    const char *f = CONFIG_COHI_AUTOPLAY_FILE;
    snprintf(s_real, sizeof s_real, "%s%s%s", BOARD_SD_MOUNT_POINT, f[0] == '/' ? "" : "/", f);
}

/* Datei vorhanden und brauchbar? Liest Direktiven (once). */
static bool probe(bool verbose, int *entries)
{
    build_path();
    struct stat sb;
    if (!sdcard_is_mounted() || stat(s_real, &sb) != 0) {
        if (verbose) ESP_LOGI(TAG, "keine %s auf der SD-Karte - kein Autoplay", CONFIG_COHI_AUTOPLAY_FILE);
        return false;
    }
    playlist_t pl;
    char err[128];
    if (playlist_load(s_real, BOARD_SD_MOUNT_POINT, &pl, err, sizeof err) != 0) {
        ESP_LOGW(TAG, "%s unbrauchbar: %s", CONFIG_COHI_AUTOPLAY_FILE, err);
        return false;
    }
    cohi_cfg_t c;
    cohi_cfg_init(&c);
    cohi_cfg_parse(&c, pl.cohi);
    s_loop = !c.once;
    if (entries) *entries = pl.n;
    playlist_free(&pl);
    return true;
}

/* Abbruchfenster: beliebige Taste an der Konsole-UART bricht ab.  Aufruf zwischen esp_console_new_repl_uart()
 * (installiert den UART-Treiber) und esp_console_start_repl() (ab dann liest die REPL-Task die Eingabe). */
static bool __attribute__((unused)) countdown_aborted(int seconds)
{
#if CONFIG_ESP_CONSOLE_UART
    const uart_port_t port = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
    uart_flush_input(port);                  /* Reste aus dem Reset/Terminalaufbau verwerfen */
    printf("Autoplay: Start in %d s - beliebige Taste = abbrechen\n", seconds);
    for (int s = seconds; s > 0; s--) {
        printf("  %d ...\n", s);
        for (int i = 0; i < 10; i++) {
            uint8_t b;
            int r = uart_read_bytes(port, &b, 1, pdMS_TO_TICKS(100));
            if (r > 0) return true;
            if (r < 0) vTaskDelay(pdMS_TO_TICKS(100));   /* Treiber nicht verfuegbar: wenigstens warten */
        }
    }
    return false;
#else
    printf("Autoplay: Start in %d s (Tasterkennung nur bei UART-Konsole)\n", seconds);
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    return false;
#endif
}

bool autoplay_boot_check(void)
{
#if CONFIG_COHI_AUTOPLAY
    int n = 0;
    if (!probe(true, &n)) return false;
    printf("Autoplay: %s gefunden (%d Eintraege, %s)\n", CONFIG_COHI_AUTOPLAY_FILE, n, s_loop ? "Endlosschleife" : "einmal");
    if (CONFIG_COHI_AUTOPLAY_DELAY_S > 0 && countdown_aborted(CONFIG_COHI_AUTOPLAY_DELAY_S)) {
        printf("Autoplay abgebrochen - 'autoplay' startet ihn, 'play ...' spielt manuell.\n");
        return false;
    }
    return true;
#else
    return false;
#endif
}

/* wartet ms; true = durchgelaufen, false = Ueberwachung ausgeschaltet oder Sofortstart angefordert */
static bool wait_ms(int ms)
{
    for (int t = 0; t < ms; t += 100) {
        if (!s_enabled || s_kick) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

static void autoplay_task(void *arg)
{
    (void)arg;
    ap_t ap;
    ap_init(&ap, s_loop);

    for (;;) {
        if (!s_enabled) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        player_status_t st;
        player_get_status(&st);
        ap.loop = s_loop;
        int wait = 0;
        ap_action_t act = ap_decide(&ap, st.playing, s_kick, esp_timer_get_time(), &wait);
        s_attempt = ap.attempt;

        if (act == AP_NONE) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        if (act == AP_DONE) {
            ESP_LOGI(TAG, "Wiedergabe (einmal) beendet");
            s_enabled = false;
            continue;
        }
        if (act == AP_START_AFTER_WAIT) {
            ESP_LOGW(TAG, "Wiedergabe beendet - Neustart in %d s", wait / 1000);
            wait_ms(wait);
            if (!s_enabled) continue;
            if (!s_kick) {                       /* Pause lief ab: Session abraeumen, Karte neu einbinden (gezogen/gesteckt) */
                player_stop();
                sdcard_unmount();
                sdcard_mount();
            }
        }
        s_kick = false;

        int n = 0;
        bool ok = probe(false, &n) && player_play(s_real, NAN, s_loop) == ESP_OK;
        ap_started(&ap, ok, esp_timer_get_time());
        s_attempt = ap.attempt;
        if (!ok && ap.attempt == 1) ESP_LOGW(TAG, "%s nicht startbar - weitere Versuche mit Pause", CONFIG_COHI_AUTOPLAY_FILE);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void ensure_task(void)
{
    if (!s_task)
        xTaskCreatePinnedToCore(autoplay_task, "autoplay", 10240, NULL, 4, &s_task, 0);
}

void autoplay_start(void)
{
    s_enabled = true;
    s_kick = true;
    ensure_task();
}

void autoplay_user_override(void)
{
    if (s_enabled) ESP_LOGI(TAG, "Konsole hat uebernommen - Autoplay-Ueberwachung aus ('autoplay' startet sie wieder)");
    s_enabled = false;
}

esp_err_t autoplay_request(void)
{
#if CONFIG_COHI_AUTOPLAY
    int n = 0;
    if (!probe(true, &n)) {
        printf("Autoplay: %s nicht gefunden/brauchbar\n", CONFIG_COHI_AUTOPLAY_FILE);
        return ESP_ERR_NOT_FOUND;
    }
    printf("Autoplay: starte %s (%d Eintraege, %s)\n", CONFIG_COHI_AUTOPLAY_FILE, n, s_loop ? "Endlosschleife" : "einmal");
    autoplay_start();
    return ESP_OK;
#else
    printf("Autoplay ist in der Konfiguration abgeschaltet (menuconfig -> COHI-Player)\n");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void autoplay_print_status(void)
{
#if CONFIG_COHI_AUTOPLAY
    int n = 0;
    bool have = probe(false, &n);
    printf("Autoplay: Datei %s %s", CONFIG_COHI_AUTOPLAY_FILE, have ? "vorhanden" : "nicht vorhanden/brauchbar");
    if (have) printf(" (%d Eintraege, %s)", n, s_loop ? "Endlosschleife" : "einmal");
    printf("\n  Ueberwachung: %s, Fehlversuche in Folge: %d, Abbruchfenster %d s\n",
           s_enabled ? "aktiv" : "aus", s_attempt, CONFIG_COHI_AUTOPLAY_DELAY_S);
#else
    printf("Autoplay: in der Konfiguration abgeschaltet\n");
#endif
}
