/*
 * cohi_cfg.h - "#COHI"-Direktiven in Playlists (Kommentarzeilen, andere m3u-Programme ignorieren sie).
 *
 *   #COHI width=8 offset=on gain=0.6 maxrate=10 cores=auto
 *   #COHI once
 *
 *   width=8|16          Busbreite                      offset=on|off   Offset-Binary-DAC
 *   gain=auto|0.01..4   AGC bzw. fester Gain           maxrate=1..40   max. PARLIO-Takt in MHz
 *   cores=auto|1|2      Mischer auf zwei Cores         once            Autoplay nur einmal (statt Endlosschleife)
 *
 * Trenner: Leerzeichen, Tab oder Komma.  Mehrere Zeilen sind erlaubt, spaeter ueberschreibt frueher.
 * Unbekannte Schluessel oder ungueltige Werte werden gemeldet und ignoriert.
 * Reines C ohne IDF-Abhaengigkeit (Host-Test).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define COHI_MAX_WARN 4

typedef struct {
    bool     has_width;   int      width;
    bool     has_offset;  bool     offset;
    bool     has_gain;    bool     agc;  float gain;
    bool     has_maxrate; uint32_t maxrate_hz;
    bool     has_cores;   int      cores;            /* 0 = auto */
    bool     once;
    int      nwarn;
    char     warn[COHI_MAX_WARN][96];
} cohi_cfg_t;

void cohi_cfg_init(cohi_cfg_t *c);
/* text: Direktiven (mit oder ohne fuehrendes "#COHI"), beliebig viele Zeilen/Tokens. */
void cohi_cfg_parse(cohi_cfg_t *c, const char *text);
/* Kurzbeschreibung der gesetzten Direktiven fuer die Konsole, z. B. "width=8 offset=an gain=0.60 once". */
void cohi_cfg_describe(const cohi_cfg_t *c, char *out, unsigned n);
