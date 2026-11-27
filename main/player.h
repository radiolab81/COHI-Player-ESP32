#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    bool     playing;
    bool     output_running;      /* PARLIO-Loop laeuft (auch bei Stille) */
    char     file[160];
    char     next[100];
    uint32_t fs;                  /* WAV-Samplerate                       */
    double   shift_hz;
    int      n;                   /* Interpolationsfaktor                  */
    uint32_t fs_out;              /* PARLIO-Takt                           */
    int      bits;
    bool     exact, alias, nyquist_ok;
    uint64_t pos_iq, total_iq;    /* Position in aktueller Datei           */
    float    lead_ms;             /* Vorsprung des Schreibzeigers          */
    uint32_t underruns;
    float    dsp_load_pct;        /* reine DSP-Rechenzeit in % eines Cores */
    float    gain;                /* aktuelle AGC-/manuelle Verstaerkung   */
    float    peak;                /* Eingangspeak 0..1                     */
    uint32_t files_played;
    int      cores;               /* 1 oder 2 (Mischer-Aufteilung) */
    bool     loop;                /* Endlosschleife ueber die Playlist */
    int      pl_total, pl_index;  /* Eintraege der Playlist / aktueller Eintrag (1-basiert) */
    unsigned pl_passes;           /* abgeschlossene Durchlaeufe (loop) */
    char     pl_name[48];
    float    split_local;         /* Anteil Mischer auf dem Filter-Core */
} player_status_t;

esp_err_t player_init(void);

/* abs_path: WAV-Datei oder Playlist (.m3u/.m3u8/.lst/.txt, eine Datei pro Zeile).
 * Ablauf: Eintrag -> auxi-Folgedateien -> naechster Eintrag; loop = Endlosschleife ueber die Liste.
 * shift_override_hz: NAN = aus Dateiname/auxi. Kehrt nach der Pruefung sofort zurueck. */
esp_err_t player_play(const char *abs_path, double shift_override_hz, bool loop);
void      player_stop(void);
void      player_get_status(player_status_t *st);

/* Einstellungen (width/offset/maxrate wirken ab dem naechsten play) */
void      player_set_width(int bits);
void      player_set_cores(int c);                        /* 0 = auto, 1, 2 */
int       player_get_cores(void);
bool      player_width_ok(int bits, char *why, size_t n); /* Pins frei von SD/Konsole/Versorgung? */
const char *player_board_name(void);
void      player_idle_pins(void);                         /* Pins sofort auf Stille (vor dem SD-Zugriff) */
void      player_apply_directives(const char *text);      /* "#COHI"-Direktiven einer Playlist anwenden */
esp_err_t player_selftest(void);                          /* Benchmark + Zwei-Core-Bitgleichheit */
int       player_get_width(void);
void      player_set_offset(bool ob);
bool      player_get_offset(void);
void      player_set_gain(bool agc, float manual);      /* live */
void      player_get_gain(bool *agc, float *manual);
void      player_set_maxrate(uint32_t hz);
uint32_t  player_get_maxrate(void);
