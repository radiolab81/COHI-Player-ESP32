/* Test: Playlist-Parser + Ablaufsteuerung (Playlist-Eintrag -> auxi-Kette -> naechster Eintrag). */
#include "../main/playlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---- Fake-Dateisystem: Pfad -> Folgedatei ("" keine), "!" = defekt/nicht lesbar, fehlende Pfade = nicht vorhanden ---- */
typedef struct { const char *path, *next; } ff_t;
static const ff_t *g_fs; static int g_nfs;
static const ff_t *find(const char *p) { for (int i = 0; i < g_nfs; i++) if (!strcmp(g_fs[i].path, p)) return &g_fs[i]; return NULL; }
static bool exists_cb(const char *p, void *c) { (void)c; return find(p) != NULL; }

/* simuliert den Reader: spielt max. maxn Dateien, liefert Reihenfolge als String */
static void run(const playlist_t *pl, bool loop, int maxn, char *out, size_t on, char *events, size_t en)
{
    seq_t s; seq_init(&s, pl, loop);
    out[0] = events[0] = 0;
    for (int played = 0; played < maxn; ) {
        const ff_t *f = find(seq_current(&s));
        bool ok = f && strcmp(f->next, "!") != 0;
        if (ok) {
            const char *b = strrchr(seq_current(&s), '/'); b = b ? b + 1 : seq_current(&s);
            strncat(out, b, on - strlen(out) - 2); strncat(out, " ", on - strlen(out) - 1);
            played++;
        } else {
            const char *b = strrchr(seq_current(&s), '/'); b = b ? b + 1 : seq_current(&s);
            strncat(out, "[", on - strlen(out) - 1); strncat(out, b, on - strlen(out) - 1); strncat(out, "!] ", on - strlen(out) - 1);
        }
        const char *nn = ok ? f->next : "";
        char peek[PL_PATH_MAX], cp[PL_PATH_MAX] = "";
        bool chain_missing = false;
        if (ok && nn[0]) {
            const char *sl = strrchr(seq_current(&s), '/');
            snprintf(cp, sizeof cp, "%.*s/%s", (int)(sl - seq_current(&s)), seq_current(&s), nn);
            chain_missing = !find(cp);
        }
        bool pk = seq_peek(&s, nn, peek, sizeof peek);
        bool more = seq_advance(&s, ok, nn, exists_cb, NULL);
        /* seq_peek gilt fuer den Fall 'cur wurde erfolgreich abgespielt' (Vorabauswertung waehrend des Streamens);
         * ausgenommen: Kettendatei fehlt (das kennt nur seq_advance) */
        if (ok && !chain_missing) { assert(pk == more); if (more) assert(!strcmp(peek, seq_current(&s))); }
        if (s.msg[0]) { strncat(events, s.msg, en - strlen(events) - 2); strncat(events, "; ", en - strlen(events) - 1); }
        if (!more) break;
        if (strlen(out) > on - 80) break;
    }
}
static int fails;
#define CHECK(name, got, want) do { if (strcmp(got, want)) { fails++; printf("FEHLER %s\n   ist:  %s\n   soll: %s\n", name, got, want); } else printf("ok  %s\n", name); } while (0)

static void write_file(const char *p, const char *content) { FILE *f = fopen(p, "wb"); fputs(content, f); fclose(f); }

int main(void)
{
    char out[1024], ev[1024];
    /* ===== Parser ===== */
    system("rm -rf /tmp/sdroot && mkdir -p /tmp/sdroot/Listen /tmp/sdroot/Ordner1");
    write_file("/tmp/sdroot/Listen/liste.m3u",
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "/datei_xyz\r\n"
        "\r\n"
        "  /Ordner1/datei_2  \r\n"
        "# Kommentar\r\n"
        "/Ordner_x/Ordner_y/datei\r\n"
        "..\\Ordner1\\win_pfad.wav\r\n"       /* Windows-Trenner, relativ zur Playlist (/Listen) -> /Ordner1/win_pfad.wav */
        "unter/rel.wav\r\n"                  /* relativ -> /Listen/unter/rel.wav */
        "/tmp/sdroot/schon_voll.wav\r\n"    /* Mount-Praefix (hier /tmp/sdroot, auf dem Geraet /sdcard) wird toleriert */
        "/Ordner1/../Ordner1/./norm.wav\n"
        "/\r\n"                               /* kein Dateiname -> verworfen */
        "/datei mit leerzeichen.wav");
    playlist_t pl; char err[128];
    assert(playlist_load("/tmp/sdroot/Listen/liste.m3u", "/tmp/sdroot", &pl, err, sizeof err) == 0);
    const char *want[] = {"/tmp/sdroot/datei_xyz", "/tmp/sdroot/Ordner1/datei_2", "/tmp/sdroot/Ordner_x/Ordner_y/datei",
        "/tmp/sdroot/Ordner1/win_pfad.wav", "/tmp/sdroot/Listen/unter/rel.wav", "/tmp/sdroot/schon_voll.wav",
        "/tmp/sdroot/Ordner1/norm.wav", "/tmp/sdroot/datei mit leerzeichen.wav"};
    printf("Playlist '%s': %d Eintraege, %d verworfen\n", pl.name, pl.n, pl.skipped);
    assert(pl.n == 8 && pl.skipped == 1);
    for (int i = 0; i < pl.n; i++) { if (strcmp(pl.e[i], want[i])) { fails++; printf("FEHLER Eintrag %d: '%s' statt '%s'\n", i, pl.e[i], want[i]); } }
    printf("%s Parser\n", fails ? "FEHLER" : "ok ");
    playlist_free(&pl);
    write_file("/tmp/sdroot/leer.m3u", "# nur Kommentar\r\n\r\n");
    assert(playlist_load("/tmp/sdroot/leer.m3u", "/tmp/sdroot", &pl, err, sizeof err) != 0); printf("ok  leere Liste abgelehnt: %s\n", err);
    assert(playlist_load("/tmp/sdroot/gibtsnicht.m3u", "/tmp/sdroot", &pl, err, sizeof err) != 0); printf("ok  fehlende Liste abgelehnt: %s\n", err);
    assert(playlist_is_name("a.M3U") && playlist_is_name("x.m3u8") && !playlist_is_name("a.wav") && !playlist_is_name("m3u"));

    /* ===== Ablauf ===== */
    #define P "/sdcard/"
    static const ff_t fs1[] = {
        {P"A.wav", ""}, {P"B_1.wav", "B_2.wav"}, {P"B_2.wav", "B_3.wav"}, {P"B_3.wav", ""},
        {P"C.wav", "gibts_nicht.wav"}, {P"D.wav", "!"}, {P"E.wav", "Z1.wav"}, {P"Z1.wav", "Z2.wav"}, {P"Z2.wav", "Z1.wav"},
        {P"Ordner1/F.wav", "F2.wav"}, {P"Ordner1/F2.wav", ""},
    };
    g_fs = fs1; g_nfs = sizeof fs1 / sizeof *fs1;
    playlist_t L; memset(&L, 0, sizeof L);
    const char *e1[] = {P"A.wav", P"B_1.wav", P"C.wav"};
    L.e = (char **)e1; L.n = 3;

    run(&L, false, 50, out, sizeof out, ev, sizeof ev);
    CHECK("Kette B_1->B_2->B_3, danach naechster Eintrag", out, "A.wav B_1.wav B_2.wav B_3.wav C.wav ");
    printf("    Ereignisse: %s\n", ev);
    assert(strstr(ev, "gibts_nicht") && strstr(ev, "Ende der Playlist"));

    run(&L, true, 9, out, sizeof out, ev, sizeof ev);
    CHECK("loop: Wiederholung", out, "A.wav B_1.wav B_2.wav B_3.wav C.wav A.wav B_1.wav B_2.wav B_3.wav ");
    assert(strstr(ev, "Durchlauf 1"));

    const char *e2[] = {P"D.wav", P"A.wav", P"nicht_da.wav"}; L.e = (char **)e2;
    run(&L, false, 50, out, sizeof out, ev, sizeof ev);
    CHECK("defekte/fehlende Eintraege werden uebersprungen", out, "[D.wav!] A.wav [nicht_da.wav!] ");

    const char *e3[] = {P"D.wav", P"nicht_da.wav"}; L.e = (char **)e3; L.n = 2;
    run(&L, true, 50, out, sizeof out, ev, sizeof ev);
    CHECK("loop ohne spielbare Datei endet (keine Endlosschleife)", out, "[D.wav!] [nicht_da.wav!] ");
    assert(strstr(ev, "Endlosschleife beendet"));

    const char *e4[] = {P"E.wav", P"A.wav"}; L.e = (char **)e4;
    run(&L, false, 50, out, sizeof out, ev, sizeof ev);
    CHECK("zyklische Kette Z1->Z2->Z1 wird abgebrochen", out, "E.wav Z1.wav Z2.wav A.wav ");
    assert(strstr(ev, "zyklisch"));

    const char *e5[] = {P"B_1.wav"}; L.e = (char **)e5; L.n = 1;
    run(&L, true, 7, out, sizeof out, ev, sizeof ev);
    CHECK("einzelne Datei mit Kette im loop", out, "B_1.wav B_2.wav B_3.wav B_1.wav B_2.wav B_3.wav B_1.wav ");
    run(&L, false, 50, out, sizeof out, ev, sizeof ev);
    CHECK("einzelne Datei ohne loop: nur die Kette", out, "B_1.wav B_2.wav B_3.wav ");

    const char *e6[] = {P"Ordner1/F.wav", P"A.wav"}; L.e = (char **)e6; L.n = 2;
    run(&L, false, 50, out, sizeof out, ev, sizeof ev);
    CHECK("Folgedatei liegt im Ordner der aktuellen Datei", out, "F.wav F2.wav A.wav ");

    printf(fails ? "\n%d FEHLER\n" : "\nalle Tests bestanden\n", fails);
    return fails != 0;
}
