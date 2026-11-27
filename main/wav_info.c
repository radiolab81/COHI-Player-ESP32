#include "wav_info.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static const char *basename_of(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

bool wav_shift_from_name(const char *path, double *hz)
{
    const char *b = basename_of(path);
    const char *k = NULL;
    for (const char *p = b; *p; p++)
        if (strncasecmp(p, "khz", 3) == 0) { k = p; break; }
    if (!k) return false;
    const char *e = k;
    while (e > b && (e[-1] == ' ')) e--;
    const char *s = e;
    while (s > b && (isdigit((unsigned char)s[-1]) || s[-1] == '.' || s[-1] == ',')) s--;
    if (s == e) return false;
    char tmp[32];
    size_t n = (size_t)(e - s);
    if (n >= sizeof tmp) return false;
    memcpy(tmp, s, n); tmp[n] = 0;
    for (char *q = tmp; *q; q++) if (*q == ',') *q = '.';
    char *end;
    double v = strtod(tmp, &end);
    if (end == tmp) return false;
    *hz = v * 1000.0;
    return true;
}

static void parse_auxi(const uint8_t *a, size_t len, wav_info_t *w)
{
    w->has_auxi = true;
    if (len >= 36) w->auxi_center_hz = rd32(a + 32);
    if (len >= 68 + 1) {
        char raw[97];
        size_t n = len - 68 < 96 ? len - 68 : 96;
        memcpy(raw, a + 68, n); raw[n] = 0;
        raw[n < 96 ? n : 96] = 0;
        size_t l = strlen(raw);
        while (l > 0 && (unsigned char)raw[l - 1] <= 0x20) raw[--l] = 0;
        const char *bn = basename_of(raw);
        strncpy(w->next_name, bn, sizeof w->next_name - 1);
        w->next_name[sizeof w->next_name - 1] = 0;
    }
}

/* st_size ist hier ein 32-Bit-off_t (picolibc/riscv32): Dateien ab 2 GiB erscheinen negativ.  FAT32-Dateien sind
 * hoechstens 4 GiB-1 gross, daher als vorzeichenlose 32-Bit-Zahl lesen.  (ftell/fseek(SEEK_END) scheitern ab 2 GiB!) */
uint64_t wav_size_from_stat(long long st_size, unsigned off_bytes)
{
    return off_bytes == 4 ? (uint64_t)(uint32_t)(int32_t)st_size : (uint64_t)st_size;
}

#define SETERR(...) do { if (err && errlen) snprintf(err, errlen, __VA_ARGS__); } while (0)

int wav_parse(const char *path, wav_info_t *w, char *err, size_t errlen)
{
    memset(w, 0, sizeof *w);
    struct stat sb;
    if (stat(path, &sb) != 0) { SETERR("'%s' nicht gefunden", path); return -1; }
    w->file_size = wav_size_from_stat((long long)sb.st_size, (unsigned)sizeof sb.st_size);
    FILE *f = fopen(path, "rb");
    if (!f) { SETERR("kann '%s' nicht oeffnen", path); return -1; }

    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f); SETERR("keine RIFF/WAVE-Datei"); return -2;
    }
    bool have_fmt = false, have_data = false;
    uint64_t pos = 12;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t ch[8];
        if (pos > 0x7FFFFFF0ull || fseek(f, (long)pos, SEEK_SET) != 0 || fread(ch, 1, 8, f) != 8) break;   /* nie Offsets >= 2 GiB anfahren */
        uint32_t csz = rd32(ch + 4);
        uint64_t body = pos + 8;
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t b[40] = {0};
            size_t n = csz < sizeof b ? csz : sizeof b;
            if (fread(b, 1, n, f) != n || n < 16) { fclose(f); SETERR("fmt-Chunk defekt"); return -3; }
            w->format = rd16(b); w->channels = rd16(b + 2);
            w->sample_rate = rd32(b + 4); w->bits = rd16(b + 14);
            have_fmt = true;
        } else if (!memcmp(ch, "auxi", 4)) {
            uint8_t b[164] = {0};
            size_t n = csz < sizeof b ? csz : sizeof b;
            if (fread(b, 1, n, f) == n) parse_auxi(b, n, w);
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_data) {
                w->data_offset = (uint32_t)body;
                uint64_t avail = w->file_size > body ? w->file_size - body : 0;
                w->data_bytes = (csz == 0 || csz == 0xFFFFFFFFu || csz > avail) ? avail : csz;
                have_data = true;
            }
            /* hinter den Daten weitersuchen (auxi kann am Ende stehen), falls Laenge plausibel */
            if (w->has_auxi || csz == 0 || csz == 0xFFFFFFFFu || body + csz >= w->file_size) break;
        }
        pos = body + csz + (csz & 1);
        if (pos + 8 > w->file_size) break;
    }
    fclose(f);

    if (!have_fmt)  { SETERR("kein fmt-Chunk"); return -3; }
    if (!have_data) { SETERR("kein data-Chunk"); return -4; }
    if ((w->format != 1 && w->format != 0xFFFE) || w->channels != 2 || w->bits != 16) {
        SETERR("Format nicht unterstuetzt (format=%u, %u Kanaele, %u Bit) - erwartet: 16-Bit-PCM I/Q (2 Kanaele)",
               w->format, w->channels, w->bits);
        return -5;
    }
    if (w->sample_rate == 0) { SETERR("Samplerate 0"); return -6; }

    double hz;
    if (wav_shift_from_name(path, &hz)) { w->shift_hz = hz; w->shift_from_name = true; }
    else if (w->auxi_center_hz) { w->shift_hz = w->auxi_center_hz; w->shift_from_auxi = true; }
    return 0;
}
