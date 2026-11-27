#include "cohi_cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

void cohi_cfg_init(cohi_cfg_t *c) { memset(c, 0, sizeof *c); }

static void warn(cohi_cfg_t *c, const char *fmt, const char *tok)
{
    if (c->nwarn < COHI_MAX_WARN) snprintf(c->warn[c->nwarn], sizeof c->warn[0], fmt, tok);
    c->nwarn++;
}

static bool parse_bool(const char *v, bool *out)
{
    if (!strcasecmp(v, "on") || !strcasecmp(v, "an") || !strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "ja")) { *out = true; return true; }
    if (!strcasecmp(v, "off") || !strcasecmp(v, "aus") || !strcmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "nein")) { *out = false; return true; }
    return false;
}

static bool parse_num(const char *v, double *out)
{
    char *end;
    for (const char *p = v; *p; p++) if (*p == ',') return false;      /* Komma ist Trenner, kein Dezimalzeichen */
    double d = strtod(v, &end);
    if (end == v || *end) return false;
    *out = d;
    return true;
}

static void apply_token(cohi_cfg_t *c, char *tok)
{
    char *eq = strchr(tok, '=');
    if (!eq) {
        if (!strcasecmp(tok, "once")) c->once = true;
        else if (!strcasecmp(tok, "loop")) c->once = false;
        else warn(c, "unbekannte Direktive '%s'", tok);
        return;
    }
    *eq = 0;
    const char *k = tok, *v = eq + 1;
    double d; bool b;
    if (!strcasecmp(k, "width")) {
        if (!strcmp(v, "8") || !strcmp(v, "16")) { c->has_width = true; c->width = atoi(v); }
        else warn(c, "width: '%s' ungueltig (8 oder 16)", v);
    } else if (!strcasecmp(k, "offset")) {
        if (parse_bool(v, &b)) { c->has_offset = true; c->offset = b; }
        else warn(c, "offset: '%s' ungueltig (on/off)", v);
    } else if (!strcasecmp(k, "gain")) {
        if (!strcasecmp(v, "auto")) { c->has_gain = true; c->agc = true; }
        else if (parse_num(v, &d) && d >= 0.01 && d <= 4.0) { c->has_gain = true; c->agc = false; c->gain = (float)d; }
        else warn(c, "gain: '%s' ungueltig (auto oder 0.01..4.0)", v);
    } else if (!strcasecmp(k, "maxrate")) {
        if (parse_num(v, &d) && d >= 1.0 && d <= 40.0) { c->has_maxrate = true; c->maxrate_hz = (uint32_t)(d * 1e6 + 0.5); }
        else warn(c, "maxrate: '%s' ungueltig (1..40 MHz)", v);
    } else if (!strcasecmp(k, "cores")) {
        if (!strcasecmp(v, "auto")) { c->has_cores = true; c->cores = 0; }
        else if (!strcmp(v, "1") || !strcmp(v, "2")) { c->has_cores = true; c->cores = atoi(v); }
        else warn(c, "cores: '%s' ungueltig (auto, 1, 2)", v);
    } else {
        char t[48]; snprintf(t, sizeof t, "%.40s", k);
        warn(c, "unbekannte Direktive '%s'", t);
    }
}

void cohi_cfg_parse(cohi_cfg_t *c, const char *text)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", text);
    char *p = buf;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
        if (*p) *p++ = 0;
        if (!strcasecmp(start, "#COHI")) continue;                 /* fuehrendes Schluesselwort */
        apply_token(c, start);
    }
}

void cohi_cfg_describe(const cohi_cfg_t *c, char *out, unsigned n)
{
    out[0] = 0;
    size_t l = 0;
#define ADD(...) do { int w = snprintf(out + l, n - l, __VA_ARGS__); if (w > 0 && (size_t)w < n - l) l += (size_t)w; } while (0)
    if (c->has_width)   ADD("%swidth=%d", l ? " " : "", c->width);
    if (c->has_offset)  ADD("%soffset=%s", l ? " " : "", c->offset ? "an" : "aus");
    if (c->has_gain)    { if (c->agc) ADD("%sgain=auto", l ? " " : ""); else ADD("%sgain=%.2f", l ? " " : "", c->gain); }
    if (c->has_maxrate) ADD("%smaxrate=%.1f", l ? " " : "", c->maxrate_hz / 1e6);
    if (c->has_cores)   { if (c->cores) ADD("%scores=%d", l ? " " : "", c->cores); else ADD("%scores=auto", l ? " " : ""); }
    if (c->once)        ADD("%sonce", l ? " " : "");
#undef ADD
}
