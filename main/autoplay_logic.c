#include "autoplay_logic.h"

void ap_init(ap_t *s, bool loop)
{
    s->loop = loop; s->started = false; s->first = true; s->attempt = 0; s->run_start_us = 0;
}

int ap_backoff_ms(int attempt)
{
    return attempt <= 1 ? 5000 : (attempt == 2 ? 10000 : 30000);
}

ap_action_t ap_decide(ap_t *s, bool playing, bool kick, int64_t now_us, int *wait_ms)
{
    *wait_ms = 0;
    if (kick) {                                   /* Konsolenbefehl "autoplay": sofort, auch wenn gerade etwas anderes spielt */
        s->started = false; s->attempt = 0;
        return AP_START;
    }
    if (playing) {
        if (s->started && s->attempt > 0 && now_us - s->run_start_us > AP_GOOD_RUN_US) s->attempt = 0;
        return AP_NONE;
    }
    if (s->started && !s->loop) { s->started = false; return AP_DONE; }
    if (s->started) { s->attempt++; s->started = false; }      /* Lauf endete von selbst */
    if (s->first) return AP_START;
    *wait_ms = ap_backoff_ms(s->attempt);
    return AP_START_AFTER_WAIT;
}

void ap_started(ap_t *s, bool ok, int64_t now_us)
{
    s->first = false;
    if (ok) { s->started = true; s->run_start_us = now_us; }
    else s->attempt++;
}
