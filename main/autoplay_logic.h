/* autoplay_logic.h - Entscheidungslogik des Autoplay-Ueberwachers (reines C, Host-Test). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AP_GOOD_RUN_US (30LL * 1000000)   /* ab dieser Laufzeit gilt der Lauf als erfolgreich -> Pausen beginnen wieder bei 5 s */

typedef struct {
    bool    loop;          /* Endlosschleife (false: "#COHI once") */
    bool    started;       /* seit dem letzten Start lief eine Wiedergabe */
    bool    first;         /* noch kein Startversuch */
    int     attempt;       /* Fehlversuche/Laeufe in Folge, bestimmt die Pause */
    int64_t run_start_us;
} ap_t;

typedef enum {
    AP_NONE,               /* nichts tun (spielt) */
    AP_START,              /* sofort starten */
    AP_START_AFTER_WAIT,   /* *wait_ms warten (abbrechbar), SD neu mounten, dann starten */
    AP_DONE                /* einmaliger Durchlauf beendet: Ueberwachung ausschalten */
} ap_action_t;

void        ap_init(ap_t *s, bool loop);
int         ap_backoff_ms(int attempt);                       /* attempt 1 -> 5 s, 2 -> 10 s, >= 3 -> 30 s */
ap_action_t ap_decide(ap_t *s, bool playing, bool kick, int64_t now_us, int *wait_ms);
void        ap_started(ap_t *s, bool ok, int64_t now_us);     /* Ergebnis des Startversuchs melden */
