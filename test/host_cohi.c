#include "../main/cohi_cfg.h"
#include "../main/playlist.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; printf("FEHLER: %s\n", what); } else printf("ok  %s\n", what); } while (0)
int main(void)
{
    cohi_cfg_t c; char d[160];
    cohi_cfg_init(&c);
    cohi_cfg_parse(&c, " width=8 offset=on gain=0.6 maxrate=10 cores=auto\n");
    cohi_cfg_describe(&c, d, sizeof d);
    CHECK(c.has_width && c.width == 8 && c.has_offset && c.offset && c.has_gain && !c.agc && c.gain > 0.59f && c.gain < 0.61f &&
          c.has_maxrate && c.maxrate_hz == 10000000 && c.has_cores && c.cores == 0 && !c.once && c.nwarn == 0, "Beispielzeile aus dem Konzept");
    printf("    -> %s\n", d);
    cohi_cfg_init(&c); cohi_cfg_parse(&c, "#COHI once"); CHECK(c.once && c.nwarn == 0, "#COHI once (mit Schluesselwort)");
    cohi_cfg_init(&c); cohi_cfg_parse(&c, "ONCE, WIDTH=16, Offset=AUS,gain=AUTO\n cores=2"); CHECK(c.once && c.width == 16 && !c.offset && c.agc && c.cores == 2 && c.nwarn == 0, "Gross/klein, Kommas, mehrere Zeilen");
    cohi_cfg_init(&c); cohi_cfg_parse(&c, "width=12 gain=9 maxrate=0 offset=vielleicht cores=3 foo=1 bar"); 
    CHECK(c.nwarn == 7 && !c.has_width && !c.has_gain && !c.has_maxrate && !c.has_offset && !c.has_cores && !c.once, "ungueltige Werte/Schluessel: gemeldet, nichts gesetzt");
    for (int i = 0; i < c.nwarn && i < COHI_MAX_WARN; i++) printf("    Warnung: %s\n", c.warn[i]);
    cohi_cfg_init(&c); cohi_cfg_parse(&c, "gain=0.3 gain=auto width=8\nwidth=16 once loop"); CHECK(c.agc && c.width == 16 && !c.once, "spaeter ueberschreibt frueher (auch once/loop)");
    cohi_cfg_init(&c); cohi_cfg_parse(&c, "gain=0,5"); CHECK(!c.has_gain && c.nwarn >= 1, "Komma ist Trenner, kein Dezimalzeichen");

    /* Playlist sammelt #COHI-Zeilen, andere Kommentare nicht */
    system("mkdir -p /tmp/sdroot");
    FILE *f = fopen("/tmp/sdroot/autoplay.m3u", "wb");
    fputs("#EXTM3U\r\n#COHI width=8 offset=on\r\n# normaler Kommentar\r\n#cohi gain=0.6 maxrate=10\r\n#COHIX nicht\r\n/a.wav\r\n#COHI once\r\n/b.wav\r\n", f); fclose(f);
    playlist_t pl; char err[128];
    assert(playlist_load("/tmp/sdroot/autoplay.m3u", "/tmp/sdroot", &pl, err, sizeof err) == 0);
    CHECK(pl.n == 2, "Playlist: 2 Dateien trotz Direktiven");
    cohi_cfg_init(&c); cohi_cfg_parse(&c, pl.cohi); cohi_cfg_describe(&c, d, sizeof d);
    CHECK(c.width == 8 && c.offset && c.has_gain && !c.agc && c.has_maxrate && c.once && c.nwarn == 0, "Playlist: Direktiven (auch kleingeschrieben, auch mitten in der Datei) gesammelt, '#COHIX' ignoriert");
    printf("    -> %s\n", d);
    playlist_free(&pl);
    printf(fails ? "\n%d FEHLER\n" : "\nalle Tests bestanden\n", fails);
    return fails != 0;
}
