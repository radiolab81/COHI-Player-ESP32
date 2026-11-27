#include "playlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>

bool playlist_is_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".m3u") || !strcasecmp(dot, ".m3u8") || !strcasecmp(dot, ".lst") || !strcasecmp(dot, ".txt");
}

/* ---- Pfade --------------------------------------------------------------- */
static bool normalize(const char *in, char *vout, size_t vn)
{
    char tmp[512];
    if (strlen(in) >= sizeof tmp) return false;
    strcpy(tmp, in);
    char *comp[96]; int nc = 0;
    char *save = NULL;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (nc) nc--; continue; }
        if (nc >= 96) return false;
        comp[nc++] = t;
    }
    if (nc == 0) return false;                 /* nur "/" -> kein Dateiname */
    size_t l = 0;
    for (int i = 0; i < nc; i++) {
        int w = snprintf(vout + l, vn - l, "/%s", comp[i]);
        if (w < 0 || (size_t)w >= vn - l) return false;
        l += (size_t)w;
    }
    return true;
}

int playlist_resolve(const char *entry, const char *base_vdir, const char *mount, char *real_out, size_t n)
{
    char s[512];
    if (strlen(entry) >= sizeof s) return -1;
    strcpy(s, entry);
    for (char *p = s; *p; p++) if (*p == '\\') *p = '/';
    const char *q = s;
    size_t ml = strlen(mount);
    if (!strncmp(q, mount, ml) && (q[ml] == '/' || q[ml] == 0)) q += ml;      /* "/sdcard/x" -> "/x" */
    char joined[640];
    if (q[0] == '/') snprintf(joined, sizeof joined, "%s", q);
    else snprintf(joined, sizeof joined, "%s/%s", base_vdir, q);
    char v[PL_PATH_MAX];
    if (!normalize(joined, v, sizeof v)) return -1;
    int w = snprintf(real_out, n, "%s%s", mount, v);
    return (w > 0 && (size_t)w < n && (size_t)w < PL_PATH_MAX) ? 0 : -2;
}

/* ---- Laden --------------------------------------------------------------- */
void playlist_free(playlist_t *pl)
{
    free(pl->e); free(pl->pool);
    memset(pl, 0, sizeof *pl);
}

static void set_name(playlist_t *pl, const char *real_path)
{
    const char *b = strrchr(real_path, '/');
    snprintf(pl->name, sizeof pl->name, "%s", b ? b + 1 : real_path);
}

void playlist_single(playlist_t *pl, const char *real_path)
{
    memset(pl, 0, sizeof *pl);
    pl->pool = malloc(PL_PATH_MAX);
    pl->e = malloc(sizeof(char *));
    if (!pl->pool || !pl->e) { playlist_free(pl); return; }
    snprintf(pl->pool, PL_PATH_MAX, "%s", real_path);
    pl->e[0] = pl->pool;
    pl->n = 1;
    set_name(pl, real_path);
}

#define LERR(...) do { if (err && errlen) snprintf(err, errlen, __VA_ARGS__); } while (0)

int playlist_load(const char *real_path, const char *mount, playlist_t *pl, char *err, size_t errlen)
{
    memset(pl, 0, sizeof *pl);
    FILE *f = fopen(real_path, "rb");
    if (!f) { LERR("Playlist '%s' nicht lesbar", real_path); return -1; }
    char *buf = malloc(PL_FILE_MAX + 1);
    if (!buf) { fclose(f); LERR("kein Speicher"); return -2; }
    size_t len = fread(buf, 1, PL_FILE_MAX + 1, f);
    fclose(f);
    if (len > PL_FILE_MAX) { free(buf); LERR("Playlist groesser als %d KiB", PL_FILE_MAX / 1024); return -3; }
    buf[len] = 0;

    /* virtueller Ordner der Playlist */
    char vdir[PL_PATH_MAX] = "";
    {
        size_t ml = strlen(mount);
        const char *v = real_path;
        if (!strncmp(v, mount, ml)) v += ml;
        snprintf(vdir, sizeof vdir, "%s", v);
        char *sl = strrchr(vdir, '/');
        if (sl) *sl = 0;                         /* "" = Wurzel */
    }

    pl->pool = malloc((size_t)PL_MAX_ENTRIES * PL_PATH_MAX);
    pl->e = malloc((size_t)PL_MAX_ENTRIES * sizeof(char *));
    if (!pl->pool || !pl->e) { free(buf); playlist_free(pl); LERR("kein Speicher"); return -2; }
    set_name(pl, real_path);

    char *p = buf;
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;   /* BOM */
    int lineno = 0;
    while (*p) {
        char *nl = strchr(p, '\n');
        char *line = p;
        if (nl) { *nl = 0; p = nl + 1; } else p += strlen(p);
        lineno++;
        size_t l = strlen(line);
        while (l && (unsigned char)line[l - 1] <= ' ') line[--l] = 0;            /* CR, Leerzeichen */
        while (*line && (unsigned char)*line <= ' ') line++;
        if (*line == '#') {                                  /* Kommentar - ausser "#COHI ..." (Direktiven) */
            if (!strncasecmp(line + 1, "COHI", 4) && (line[5] == 0 || isspace((unsigned char)line[5]))) {
                size_t cl = strlen(pl->cohi);
                int w = snprintf(pl->cohi + cl, sizeof pl->cohi - cl, "%s\n", line + 5);
                if (w < 0 || (size_t)w >= sizeof pl->cohi - cl) {
                    pl->cohi[cl] = 0;
                    printf("Playlist Zeile %d: zu viele #COHI-Direktiven - ignoriert\n", lineno);
                }
            }
            continue;
        }
        if (!*line) continue;
        if (pl->n >= PL_MAX_ENTRIES) { printf("Playlist: mehr als %d Eintraege - Rest ignoriert\n", PL_MAX_ENTRIES); break; }
        char *dst = pl->pool + (size_t)pl->n * PL_PATH_MAX;
        if (playlist_resolve(line, vdir, mount, dst, PL_PATH_MAX) != 0) {
            printf("Playlist Zeile %d: ungueltiger oder zu langer Pfad: %.60s\n", lineno, line);
            pl->skipped++;
            continue;
        }
        pl->e[pl->n++] = dst;
    }
    free(buf);
    if (pl->n == 0) { playlist_free(pl); LERR("Playlist enthaelt keine Eintraege"); return -4; }

    /* Speicher auf die tatsaechliche Groesse verkleinern (Pool und Zeigerfeld neu aufbauen) */
    char *pool2 = malloc((size_t)pl->n * PL_PATH_MAX);
    char **e2 = malloc((size_t)pl->n * sizeof(char *));
    if (pool2 && e2) {
        for (int i = 0; i < pl->n; i++) {
            char *d = pool2 + (size_t)i * PL_PATH_MAX;
            memcpy(d, pl->e[i], PL_PATH_MAX);
            e2[i] = d;
        }
        free(pl->pool); free(pl->e);
        pl->pool = pool2; pl->e = e2;
    } else { free(pool2); free(e2); }          /* kein Platz fuers Verkleinern: grosse Variante behalten */
    return 0;
}

/* ---- Ablauf -------------------------------------------------------------- */
static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)tolower((unsigned char)*s); h *= 16777619u; }   /* FAT: Gross/klein egal */
    return h;
}

static bool seen_has(const seq_t *s, uint32_t h)
{
    for (int i = 0; i < s->nseen; i++) if (s->seen[i] == h) return true;
    return false;
}

static bool chain_path(const char *cur, const char *next_name, char *out, size_t n)
{
    if (!next_name || !next_name[0]) return false;
    const char *bn = next_name;
    for (const char *p = next_name; *p; p++) if (*p == '/' || *p == '\\') bn = p + 1;
    if (!*bn) return false;
    const char *sl = strrchr(cur, '/');
    size_t dl = sl ? (size_t)(sl - cur) : 0;
    int w = snprintf(out, n, "%.*s/%s", (int)dl, cur, bn);
    return w > 0 && (size_t)w < n;
}

/* Ereignismeldungen anhaengen (mehrere Ereignisse in einer Entscheidung moeglich) */
static void note(seq_t *s, const char *fmt, ...)
{
    size_t l = strlen(s->msg);
    if (l && l + 3 < sizeof s->msg) { strcat(s->msg, "; "); l += 2; }
    va_list ap; va_start(ap, fmt);
    vsnprintf(s->msg + l, sizeof s->msg - l, fmt, ap);
    va_end(ap);
}

static void set_cur(seq_t *s, const char *path)
{
    snprintf(s->cur, sizeof s->cur, "%s", path);
}

void seq_init(seq_t *s, const playlist_t *pl, bool loop)
{
    memset(s, 0, sizeof *s);
    s->pl = pl; s->loop = loop;
    if (pl && pl->n > 0) { set_cur(s, pl->e[0]); s->seen[0] = fnv(s->cur); s->nseen = 1; }
}

const char *seq_current(const seq_t *s) { return s->cur; }

bool seq_peek(const seq_t *s, const char *next_name, char *out, size_t n)
{
    char c[PL_PATH_MAX];
    if (chain_path(s->cur, next_name, c, sizeof c) && !seen_has(s, fnv(c)) && s->nseen < SEQ_SEEN_MAX) {
        snprintf(out, n, "%s", c);
        return true;
    }
    int ni = s->idx + 1;
    if (ni >= s->pl->n) { if (!s->loop) return false; ni = 0; }
    snprintf(out, n, "%s", s->pl->e[ni]);
    return true;
}

bool seq_advance(seq_t *s, bool ok, const char *next_name, seq_exists_fn exists, void *ctx)
{
    s->msg[0] = 0;
    if (ok) s->pass_ok++;
    char c[PL_PATH_MAX];
    if (ok && chain_path(s->cur, next_name, c, sizeof c)) {
        if (seen_has(s, fnv(c)) || s->nseen >= SEQ_SEEN_MAX) {
            note(s, "Folgedatei-Kette zyklisch bei '%s' - weiter mit der Playlist", c);
        } else if (exists && !exists(c, ctx)) {
            note(s, "Folgedatei '%s' nicht gefunden - Ende der Kette", c);
        } else {
            set_cur(s, c);
            s->seen[s->nseen++] = fnv(c);
            return true;
        }
    }
    /* Kette zu Ende -> naechster Listeneintrag */
    int ni = s->idx + 1;
    if (ni >= s->pl->n) {
        if (!s->loop) { note(s, "Ende der Playlist"); return false; }
        if (s->pass_ok == 0) { note(s, "kein Eintrag der Playlist abspielbar - Endlosschleife beendet"); return false; }
        ni = 0; s->passes++; s->pass_ok = 0;
        note(s, "Durchlauf %u beendet - von vorn", s->passes);
    }
    s->idx = ni;
    set_cur(s, s->pl->e[ni]);
    s->nseen = 0;
    s->seen[s->nseen++] = fnv(s->cur);
    return true;
}
