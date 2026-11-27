#include "../main/autoplay_logic.h"
#include <stdio.h>
#include <assert.h>
#define S(x) ((int64_t)(x) * 1000000LL)
static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; printf("FEHLER: %s\n", what); } else printf("ok  %s\n", what); } while (0)

/* simuliert: Lauf von dur_s Sekunden, danach endet er; liefert die Pause (ms) vor dem naechsten Start */
static int cycle(ap_t *a, int64_t *t, int dur_s, ap_action_t *act_out)
{
    int w; ap_action_t act;
    /* waehrend des Laufs: alle 0,5 s abfragen (hier grob alle 5 s) */
    for (int64_t x = 5; x <= dur_s; x += 5) { act = ap_decide(a, true, false, *t + S(x), &w); assert(act == AP_NONE); }
    *t += S(dur_s);
    act = ap_decide(a, false, false, *t, &w);
    *act_out = act;
    return w;
}

int main(void)
{
    ap_t a; int64_t t = 0; int w; ap_action_t act;

    /* Erster Start ohne Pause, dann Lauf endet nach 10 s: Pausen 5, 10, 30, 30 s */
    ap_init(&a, true);
    act = ap_decide(&a, false, false, t, &w); CHECK(act == AP_START && w == 0, "erster Start sofort");
    ap_started(&a, true, t);
    int expect[] = {5000, 10000, 30000, 30000};
    for (int i = 0; i < 4; i++) {
        int pause = cycle(&a, &t, 10, &act);
        char m[80]; snprintf(m, sizeof m, "Lauf %d endet nach 10 s -> Pause %d ms", i + 1, expect[i]);
        CHECK(act == AP_START_AFTER_WAIT && pause == expect[i], m);
        t += S(pause / 1000); ap_started(&a, true, t);
    }
    /* langer Lauf (60 s) setzt die Pausen zurueck */
    int pause = cycle(&a, &t, 60, &act);
    CHECK(act == AP_START_AFTER_WAIT && pause == 5000, "nach 60 s Lauf beginnt die Pause wieder bei 5 s");
    t += S(5); ap_started(&a, true, t);

    /* Startversuche scheitern (Karte gezogen): 5, 10, 30, 30 */
    ap_init(&a, true);
    act = ap_decide(&a, false, false, t, &w); CHECK(act == AP_START, "scheiternder Start: erster Versuch sofort");
    ap_started(&a, false, t);
    int e2[] = {5000, 10000, 30000, 30000};
    for (int i = 0; i < 4; i++) {
        act = ap_decide(&a, false, false, t, &w);
        char m[80]; snprintf(m, sizeof m, "Fehlversuch %d -> Pause %d ms", i + 1, e2[i]);
        CHECK(act == AP_START_AFTER_WAIT && w == e2[i], m);
        ap_started(&a, false, t);
    }
    ap_started(&a, true, t);
    pause = cycle(&a, &t, 5, &act);
    CHECK(act == AP_START_AFTER_WAIT && pause == 30000, "kurzer Lauf nach Fehlversuchen: Pause bleibt lang (kein Zuruecksetzen)");

    /* "#COHI once": ein Durchlauf, dann Ende (kein Neustart) */
    ap_init(&a, false);
    ap_decide(&a, false, false, t, &w); ap_started(&a, true, t);
    cycle(&a, &t, 20, &act); CHECK(act == AP_DONE, "once: nach dem Lauf Ende");

    /* Konsolenbefehl 'autoplay' waehrend etwas anderes spielt: sofort starten */
    ap_init(&a, true);
    ap_decide(&a, false, false, t, &w); ap_started(&a, true, t);
    act = ap_decide(&a, true, false, t + S(3), &w); CHECK(act == AP_NONE, "laeuft: nichts tun");
    act = ap_decide(&a, true, true, t + S(4), &w); CHECK(act == AP_START && w == 0, "kick waehrend Wiedergabe: sofort neu starten");
    ap_started(&a, true, t + S(4));
    cycle(&a, &t, 8, &act);
    CHECK(a.attempt == 1, "nach kick zaehlt der Fehlerzaehler wieder von 0");

    /* kick nach Fehlversuchen setzt den Zaehler zurueck */
    ap_init(&a, true); ap_started(&a, false, t); ap_started(&a, false, t); ap_started(&a, false, t);
    act = ap_decide(&a, false, true, t, &w); CHECK(act == AP_START && a.attempt == 0, "kick setzt Fehlerzaehler zurueck");

    printf(fails ? "\n%d FEHLER\n" : "\nalle Tests bestanden\n", fails);
    return fails != 0;
}
