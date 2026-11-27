/*
 * wav_info.h - WAV-Header-Auswertung fuer Cohiradia-Aufnahmen.
 *
 *  - fmt : Samplerate, Kanaele (2 = I/Q), Bits (16)
 *  - auxi: Name der Folgedatei (Offset 68, 96 Bytes) und Mittenfrequenz (Offset 32)
 *  - data: Datenbeginn/-laenge
 *  - Shift-Frequenz: aus dem Dateinamen ("...1233kHz...", wie COHIRADIAStreamer)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint32_t sample_rate;
    uint16_t channels, bits, format;
    uint32_t data_offset;
    uint64_t data_bytes;         /* nutzbare Laenge (auf Dateigroesse begrenzt) */
    uint64_t file_size;
    bool     has_auxi;
    uint32_t auxi_center_hz;     /* CenterFreq aus auxi (0 = unbekannt) */
    char     next_name[97];      /* Folgedatei (nur Dateiname), "" = keine */
    bool     shift_from_name;
    double   shift_hz;           /* aus Dateiname, sonst auxi-CenterFreq, sonst 0 */
    bool     shift_from_auxi;
} wav_info_t;

/* 0 = ok, <0 = Fehler (err enthaelt Text). */
int wav_parse(const char *path, wav_info_t *out, char *err, size_t errlen);

/* Dateigroesse aus stat(): off_t=4 Byte -> vorzeichenlos (Dateien 2..4 GiB). Zum Testen exportiert. */
uint64_t wav_size_from_stat(long long st_size, unsigned off_bytes);

/* "..._1233kHz.wav" / "...1233.5 kHz..." -> Hz. Nur Basename wird ausgewertet. */
bool wav_shift_from_name(const char *path, double *hz);
