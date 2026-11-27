/*
 * playlist.h - Playlists (.m3u) und Ablaufsteuerung (Playlist-Eintrag -> auxi-Folgedatei-Kette -> naechster Eintrag).
 * Reines C (ohne IDF/FreeRTOS), auf dem Host testbar.
 *
 * Playlist-Format: eine Datei pro Zeile.
 *   /Ordner1/datei_2.wav       absolut: "/" ist die Wurzel der SD-Karte
 *   unterordner/datei.wav      relativ zum Ordner der Playlist
 *   # Kommentar, leere Zeilen, UTF-8-BOM, CRLF, '\' als Trenner werden toleriert
 *   #COHI width=8 offset=on ...  Direktiven (siehe cohi_cfg.h); werden in pl->cohi gesammelt
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PL_MAX_ENTRIES  2048
#define PL_PATH_MAX     160            /* realer Pfad inkl. NUL (z. B. "/sdcard/Ordner/datei.wav") */
#define PL_FILE_MAX     (256 * 1024)
#define SEQ_SEEN_MAX    128

typedef struct {
    char **e;                          /* reale Pfade (mount + virtueller Pfad) */
    int    n;
    char  *pool;
    char   name[48];                   /* Dateiname der Liste, fuer die Anzeige */
    int    skipped;                    /* beim Laden verworfene Zeilen */
    char   cohi[256];                  /* gesammelte "#COHI"-Direktiven (ohne Schluesselwort, eine Zeile je Direktivenzeile) */
} playlist_t;

bool playlist_is_name(const char *name);                       /* .m3u / .m3u8 / .lst / .txt */
int  playlist_load(const char *real_path, const char *mount, playlist_t *pl, char *err, size_t errlen);
void playlist_single(playlist_t *pl, const char *real_path);   /* "Liste" mit einem Eintrag (play datei.wav) */
void playlist_free(playlist_t *pl);

/* Eintrag -> realer Pfad. base_vdir: virtueller Ordner der Playlist (z. B. "/Listen"). 0 = ok. */
int  playlist_resolve(const char *entry, const char *base_vdir, const char *mount, char *real_out, size_t n);

/* ---- Ablauf ---- */
typedef struct {
    const playlist_t *pl;
    bool     loop;
    int      idx;                      /* aktueller Listeneintrag */
    int      pass_ok;                  /* seit Durchlaufbeginn erfolgreich gestartete Dateien */
    unsigned passes;                   /* abgeschlossene Durchlaeufe (loop) */
    uint32_t seen[SEQ_SEEN_MAX];       /* Hashes der Dateien der aktuellen Kette (Zyklenschutz) */
    int      nseen;
    char     cur[PL_PATH_MAX];
    char     msg[240];                 /* Ereignistext der letzten Entscheidung, "" = keiner */
} seq_t;

typedef bool (*seq_exists_fn)(const char *path, void *ctx);

void        seq_init(seq_t *s, const playlist_t *pl, bool loop);
const char *seq_current(const seq_t *s);
/* Vorschau (ohne Zustandsaenderung) auf das Ziel nach erfolgreichem Abspielen von cur; fuer die Vorabauswertung. */
bool        seq_peek(const seq_t *s, const char *next_name, char *out, size_t n);
/* ok = cur wurde abgespielt (false: nicht oeffnbar/defekt). next_name = Folgedatei aus auxi ("" = keine).
 * true: seq_current() ist das naechste Ziel; false: Ende. */
bool        seq_advance(seq_t *s, bool ok, const char *next_name, seq_exists_fn exists, void *ctx);
