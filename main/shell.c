/*
 * shell.c - serielle Konsole (esp_console): ls, cd, pwd, play, stop, status, info,
 *           gain, width, offset, maxrate, mount, umount
 *
 * ESP-IDF kennt keinen Prozess-Arbeitsordner (chdir), daher wird "cwd" hier
 * selbst gefuehrt.  "/" ist die Wurzel der SD-Karte (= /sdcard im VFS).
 */
#include "shell.h"
#include "player.h"
#include "sdcard.h"
#include "board.h"
#include "wav_info.h"
#include "sdr_plan.h"
#include "version.h"
#include "autoplay.h"
#include "sdr_dsp.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_console.h"
#include "esp_log.h"

static char s_cwd[256] = "/";               /* virtueller Pfad, immer mit fuehrendem '/' */

/* ---- Pfad-Auflosung ------------------------------------------------------ */
/* in (relativ zu cwd oder absolut) -> normalisierter virtueller Pfad in vout, realer Pfad in rout */
static bool resolve(const char *in, char *vout, size_t vn, char *rout, size_t rn)
{
    char tmp[512];
    if (in[0] == '/') snprintf(tmp, sizeof tmp, "%s", in);
    else snprintf(tmp, sizeof tmp, "%s/%s", s_cwd, in);

    char *comp[64]; int nc = 0;
    char *save = NULL;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { if (nc) nc--; continue; }
        if (nc >= 64) return false;
        comp[nc++] = t;
    }
    size_t l = 0;
    vout[0] = 0;
    if (nc == 0) { snprintf(vout, vn, "/"); }
    else for (int i = 0; i < nc; i++) {
        int w = snprintf(vout + l, vn - l, "/%s", comp[i]);
        if (w < 0 || (size_t)w >= vn - l) return false;
        l += (size_t)w;
    }
    int w = snprintf(rout, rn, "%s%s", BOARD_SD_MOUNT_POINT, strcmp(vout, "/") ? vout : "");
    return w > 0 && (size_t)w < rn;
}

static bool need_sd(void)
{
    if (!sdcard_is_mounted()) { printf("Keine SD-Karte gemountet - 'mount' versuchen.\n"); return false; }
    return true;
}

static bool is_wav(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".wav");
}

/* ---- ls ------------------------------------------------------------------ */
typedef struct { char name[130]; bool dir; uint32_t size; } ent_t;

static int ent_cmp(const void *a, const void *b)
{
    const ent_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static int cmd_ls(int argc, char **argv)
{
    if (!need_sd()) return 0;
    bool lng = false;
    const char *arg = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) lng = true; else arg = argv[i];
    }
    char v[256], r[300];
    if (!resolve(arg, v, sizeof v, r, sizeof r)) { printf("Pfad zu lang\n"); return 0; }
    DIR *d = opendir(r);
    if (!d) { printf("ls: '%s': nicht gefunden\n", v); return 0; }

    size_t cap = 64, n = 0;
    ent_t *e = malloc(cap * sizeof *e);
    struct dirent *de;
    while (e && (de = readdir(d))) {
        if (n == cap) { cap *= 2; ent_t *t = realloc(e, cap * sizeof *e); if (!t) break; e = t; }
        ent_t *x = &e[n++];
        strlcpy(x->name, de->d_name, sizeof x->name);
        char full[600];
        snprintf(full, sizeof full, "%s/%s", r, de->d_name);
        struct stat st;
        x->dir = (de->d_type == DT_DIR);
        x->size = 0;
        if (stat(full, &st) == 0) { x->dir = S_ISDIR(st.st_mode); x->size = (uint32_t)st.st_size; }
    }
    closedir(d);
    if (!e) { printf("kein Speicher\n"); return 0; }
    qsort(e, n, sizeof *e, ent_cmp);

    printf("%s:\n", v);
    for (size_t i = 0; i < n; i++) {
        if (!lng) { printf("%s%s\n", e[i].name, e[i].dir ? "/" : ""); continue; }
        if (e[i].dir) { printf("  <DIR>        %s/\n", e[i].name); continue; }
        printf("%12u  %s", (unsigned)e[i].size, e[i].name);
        if (is_wav(e[i].name)) {
            char full[600]; wav_info_t w; char err[64];
            snprintf(full, sizeof full, "%s/%s", r, e[i].name);
            if (wav_parse(full, &w, err, sizeof err) == 0) {
                printf("   [fs=%u kHz, shift=%.1f kHz%s", (unsigned)(w.sample_rate / 1000), w.shift_hz / 1e3,
                       w.shift_from_auxi ? " (auxi)" : "");
                if (w.next_name[0]) printf(", next=%s", w.next_name);
                printf("]");
            } else printf("   [%s]", err);
        }
        printf("\n");
    }
    printf("%u Eintraege\n", (unsigned)n);
    free(e);
    return 0;
}

/* ---- cd / pwd ------------------------------------------------------------- */
static int cmd_cd(int argc, char **argv)
{
    if (!need_sd()) return 0;
    const char *arg = argc > 1 ? argv[1] : "/";
    char v[256], r[300];
    if (!resolve(arg, v, sizeof v, r, sizeof r)) { printf("Pfad zu lang\n"); return 0; }
    struct stat st;
    if (strcmp(v, "/") && (stat(r, &st) != 0 || !S_ISDIR(st.st_mode))) {
        printf("cd: '%s': kein Verzeichnis\n", v);
        return 0;
    }
    strlcpy(s_cwd, v, sizeof s_cwd);
    printf("%s\n", s_cwd);
    return 0;
}

static int cmd_pwd(int argc, char **argv) { (void)argc; (void)argv; printf("%s\n", s_cwd); return 0; }

/* ---- play / stop / info ---------------------------------------------------- */
static int cmd_play(int argc, char **argv)
{
    if (!need_sd()) return 0;
    bool loop = false;
    int a = 1;
    if (argc > 1 && !strcasecmp(argv[1], "loop")) { loop = true; a = 2; }
    if (argc <= a) {
        printf("Aufruf: play [loop] <datei.wav | liste.m3u> [shift_kHz]\n"
               "  Playlist: eine Datei pro Zeile; Eintrag -> Folgedateien (auxi) -> naechster Eintrag; loop = Endlosschleife\n");
        return 0;
    }
    char v[256], r[300];
    if (!resolve(argv[a], v, sizeof v, r, sizeof r)) { printf("Pfad zu lang\n"); return 0; }
    double shift = NAN;
    if (argc > a + 1) shift = atof(argv[a + 1]) * 1000.0;
    autoplay_user_override();                /* Konsole hat Vorrang vor dem Autoplay */
    if (player_play(r, shift, loop) == ESP_OK) printf("Wiedergabe gestartet: %s%s\n", v, loop ? " (Endlosschleife)" : "");
    return 0;
}

static int cmd_stop(int argc, char **argv) { (void)argc; (void)argv; autoplay_user_override(); player_stop(); printf("gestoppt\n"); return 0; }

static int cmd_info(int argc, char **argv)
{
    if (!need_sd()) return 0;
    if (argc < 2) { printf("Aufruf: info <datei.wav>\n"); return 0; }
    char v[256], r[300], err[128];
    if (!resolve(argv[1], v, sizeof v, r, sizeof r)) { printf("Pfad zu lang\n"); return 0; }
    wav_info_t w;
    if (wav_parse(r, &w, err, sizeof err)) { printf("Fehler: %s\n", err); return 0; }
    printf("%s\n  Samplerate : %u Hz\n  Format     : %u Kanaele, %u Bit (fmt=%u)\n",
           v, (unsigned)w.sample_rate, w.channels, w.bits, w.format);
    printf("  Daten      : Offset %u, %llu Bytes = %.2f s\n", (unsigned)w.data_offset,
           (unsigned long long)w.data_bytes, (double)w.data_bytes / 4.0 / w.sample_rate);
    printf("  Shift      : %.3f kHz (%s)\n", w.shift_hz / 1e3,
           w.shift_from_name ? "Dateiname" : (w.shift_from_auxi ? "auxi-CenterFreq" : "unbekannt"));
    printf("  auxi       : %s, CenterFreq %u Hz\n", w.has_auxi ? "ja" : "nein", (unsigned)w.auxi_center_hz);
    printf("  Folgedatei : %s\n", w.next_name[0] ? w.next_name : "(keine)");
    sdr_plan_t p;
    if (sdr_plan_rate(w.sample_rate, w.shift_hz, player_get_maxrate(), &p) == 0) {
        printf("  PARLIO-Plan: N=%d -> %.3f MHz (%s)%s%s\n", p.n, p.fs_out / 1e6,
               p.exact ? "exakter Teiler" : "BRUCHTEILER", p.alias ? ", ALIAS/Bild-Betrieb" : "",
               (!p.alias && !p.nyquist_ok) ? ", wenig Nyquist-Abstand" : "");
        bool fast = w.sample_rate >= SDR_FAST_FS_HZ;
        float est = sdr_est_load_pct(w.sample_rate, p.fs_out, fast, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
        printf("  DSP        : %s, geschaetzte Last %.0f %% eines Cores%s\n", fast ? "FAST (1 Halbband)" : "HQ (2 Halbbaender)", est,
               est > 85.0f ? "  <-- zu hoch, Underruns wahrscheinlich" : "");
    } else printf("  PARLIO-Plan: nicht machbar (4*fs > maxrate)\n");
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    player_status_t s; player_get_status(&s);
    bool agc; float mg; player_get_gain(&agc, &mg);
    printf("Version: %s (%s), gebaut %s\n", COHI_STAGE, COHI_VERSION, COHI_BUILD_DATE);
    printf("Board: %s\n", player_board_name());
    printf("Einstellungen: width=%d Bit, offset=%s, gain=%s, maxrate=%.1f MHz, cores=%s\n", player_get_width(),
           player_get_offset() ? "an (Offset-Binary)" : "aus (signed)",
           agc ? "auto" : "manuell", player_get_maxrate() / 1e6,
           player_get_cores() == 0 ? "auto" : (player_get_cores() == 1 ? "1" : "2"));
    if (!s.playing) { printf("Player: %s\n", s.output_running ? "Stopp (Ausgang laeuft, Stille)" : "Stopp"); }
    else {
        printf("Player: spielt %s\n", s.file);
        if (s.pl_total > 1 || s.loop)
            printf("  Playlist '%s': Eintrag %d/%d%s, Durchlaeufe: %u\n", s.pl_name, s.pl_index, s.pl_total, s.loop ? ", Endlosschleife" : "", s.pl_passes);
        printf("  fs=%u Hz, Shift=%.3f kHz, N=%d, PARLIO %.3f MHz %d Bit%s%s\n", (unsigned)s.fs, s.shift_hz / 1e3, s.n,
               s.fs_out / 1e6, s.bits, s.exact ? "" : " [Bruchteiler]", s.alias ? " [ALIAS]" : "");
        double t = s.fs ? (double)s.pos_iq / s.fs : 0, tt = s.fs ? (double)s.total_iq / s.fs : 0;
        printf("  Position %.1f / %.1f s, Folgedatei: %s, Dateien: %u\n", t, tt, s.next[0] ? s.next : "(keine)", (unsigned)s.files_played);
        printf("  Vorsprung %.1f ms, Underruns %u, DSP-Last %.0f %% (Core 1), Gain %.2f, Eingangspeak %.2f\n",
               s.lead_ms, (unsigned)s.underruns, s.dsp_load_pct, s.gain, s.peak);
        if (s.cores == 2) printf("  Mischer auf 2 Cores: Core 1 rechnet %.0f %%, Core 0 %.0f %% (Last Core 1 oben gemessen)\n", s.split_local * 100.0f, (1.0f - s.split_local) * 100.0f);
    }
    return 0;
}

/* ---- Einstellungen ---------------------------------------------------------- */
static int cmd_gain(int argc, char **argv)
{
    if (argc < 2) { printf("Aufruf: gain auto | gain <0.01..4.0>  (1.0 = Eingangs-Vollaussteuerung)\n"); return 0; }
    if (!strcasecmp(argv[1], "auto")) { player_set_gain(true, 0); printf("Gain: auto (AGC, Ziel 50%%)\n"); }
    else {
        float g = (float)atof(argv[1]);
        if (g < 0.01f || g > 4.0f) { printf("Bereich 0.01..4.0\n"); return 0; }
        player_set_gain(false, g); printf("Gain: manuell %.2f\n", g);
    }
    return 0;
}

static int cmd_width(int argc, char **argv)
{
    if (argc < 2 || (atoi(argv[1]) != 8 && atoi(argv[1]) != 16)) { printf("Aufruf: width 8|16 (wirkt ab naechstem play)\n"); return 0; }
    char why[96];
    if (!player_width_ok(atoi(argv[1]), why, sizeof why)) {
        printf("%d Bit auf diesem Board (%s) nicht moeglich: %s\n", atoi(argv[1]), player_board_name(), why);
        return 0;
    }
    player_set_width(atoi(argv[1])); printf("width = %d Bit\n", player_get_width());
    return 0;
}

static int cmd_cores(int argc, char **argv)
{
    if (argc < 2) {
        int c = player_get_cores();
        printf("cores = %s\n", c == 0 ? "auto (2 Cores nur bei geschaetzter Last >= 85 %)" : (c == 1 ? "1" : "2"));
        return 0;
    }
    if (!strcasecmp(argv[1], "auto")) player_set_cores(0);
    else if (!strcmp(argv[1], "1")) player_set_cores(1);
    else if (!strcmp(argv[1], "2")) player_set_cores(2);
    else { printf("Aufruf: cores auto|1|2\n"); return 0; }
    printf("cores = %s (wirkt ab naechstem play)\n", argv[1]);
    return 0;
}

static int cmd_autoplay(int argc, char **argv)
{
    if (argc > 1 && !strcasecmp(argv[1], "status")) { autoplay_print_status(); return 0; }
    if (argc > 1) { printf("Aufruf: autoplay [status]   (ohne Argument: Autoplay jetzt starten)\n"); return 0; }
    if (!need_sd()) return 0;
    autoplay_request();
    return 0;
}

static int cmd_selftest(int argc, char **argv) { (void)argc; (void)argv; player_selftest(); return 0; }

static int cmd_offset(int argc, char **argv)
{
    if (argc < 2) { printf("Aufruf: offset on|off (Offset-Binary fuer unipolare DACs; ab naechstem play)\n"); return 0; }
    player_set_offset(!strcasecmp(argv[1], "on") || !strcmp(argv[1], "1"));
    printf("offset = %s\n", player_get_offset() ? "an" : "aus");
    return 0;
}

static int cmd_maxrate(int argc, char **argv)
{
    if (argc < 2) { printf("maxrate = %.1f MHz\n", player_get_maxrate() / 1e6); return 0; }
    double m = atof(argv[1]);
    if (m < 1 || m > 40) { printf("Bereich 1..40 MHz\n"); return 0; }
    player_set_maxrate((uint32_t)(m * 1e6)); printf("maxrate = %.1f MHz\n", m);
    return 0;
}

static int cmd_mount(int argc, char **argv) { (void)argc; (void)argv; if (sdcard_mount() == ESP_OK) printf("SD gemountet\n"); return 0; }
static int cmd_umount(int argc, char **argv) { (void)argc; (void)argv; autoplay_user_override(); player_stop(); sdcard_unmount(); strcpy(s_cwd, "/"); printf("SD ausgehaengt\n"); return 0; }

static esp_console_repl_t *s_repl;

/* Konsole anlegen und Befehle registrieren (der UART-Treiber ist danach installiert; gelesen wird erst ab shell_run()). */
void shell_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "sdr> ";
    rc.task_stack_size = 8192;
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rc, &repl));

    esp_console_register_help_command();
    const esp_console_cmd_t cmds[] = {
        { .command = "ls",      .help = "Verzeichnis anzeigen: ls [-l] [pfad]  (-l: WAV-Header-Infos)", .func = cmd_ls },
        { .command = "cd",      .help = "Verzeichnis wechseln: cd [pfad]  (ohne Argument: Wurzel)",     .func = cmd_cd },
        { .command = "pwd",     .help = "aktuelles Verzeichnis",                                         .func = cmd_pwd },
        { .command = "play",    .help = "Wiedergabe: play [loop] <datei.wav|liste.m3u> [shift_kHz]",     .func = cmd_play },
        { .command = "autoplay",.help = "autoplay.m3u jetzt starten und Ueberwachung einschalten; autoplay status", .func = cmd_autoplay },
        { .command = "stop",    .help = "Wiedergabe stoppen",                                            .func = cmd_stop },
        { .command = "status",  .help = "Player-Status, Vorsprung, Underruns, DSP-Last",                 .func = cmd_status },
        { .command = "info",    .help = "WAV-Header + PARLIO-Plan anzeigen: info <datei.wav>",           .func = cmd_info },
        { .command = "gain",    .help = "Verstaerkung: gain auto | gain <0.01..4.0>",                    .func = cmd_gain },
        { .command = "width",   .help = "Busbreite: width 8|16",                                         .func = cmd_width },
        { .command = "cores",   .help = "Mischer auf 2 Cores: cores auto|1|2",                          .func = cmd_cores },
        { .command = "selftest",.help = "Benchmark + Pruefung Zwei-Core-Bitgleichheit (nicht waehrend play)", .func = cmd_selftest },
        { .command = "offset",  .help = "Offset-Binary: offset on|off",                                  .func = cmd_offset },
        { .command = "maxrate", .help = "max. PARLIO-Takt in MHz: maxrate [MHz]",                        .func = cmd_maxrate },
        { .command = "mount",   .help = "SD-Karte mounten",                                              .func = cmd_mount },
        { .command = "umount",  .help = "SD-Karte aushaengen",                                           .func = cmd_umount },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));

    s_repl = repl;
}

void shell_run(void)
{
    printf("\nCOHI-Player-ESP32 %s (%s) - 'help' fuer Befehle.  ls / cd / play <datei.wav|liste.m3u> / stop\n\n", COHI_STAGE, COHI_VERSION);
    ESP_ERROR_CHECK(esp_console_start_repl(s_repl));
}

void shell_start(void) { shell_init(); shell_run(); }
