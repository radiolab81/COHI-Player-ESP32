#include "../main/sdr_plan.h"
#include "../main/wav_info.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct { uint32_t fs; double fc; } t[] = {
        {250000, 1233e3}, {250000, 531e3}, {250000, 300e3}, {1250000, 1000e3}, {1250000, 6000e3},
        {250000, 18960e3}, {500000, 7000e3}, {192000, 1000e3}, {625000, 1000e3}, {2500000, 1000e3}, {6000000, 100e3}};
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        sdr_plan_t p;
        int r = sdr_plan_rate(t[i].fs, t[i].fc, 20000000, &p);
        if (r) { printf("fs=%7u fc=%8.0fk -> NICHT MACHBAR\n", t[i].fs, t[i].fc/1e3); continue; }
        printf("fs=%7u fc=%8.0fk -> N=%3d Fs_out=%6.3f MHz exact=%d nyq=%d alias=%d%s (phase_inc=%u)\n", t[i].fs, t[i].fc/1e3, p.n,
               p.fs_out/1e6, p.exact, p.nyquist_ok, p.alias, p.alias ? " (Bild/Alias)" : "", sdr_phase_inc(t[i].fc, p.fs_out));
    }
    const char *files[] = {"/tmp/rec_1233kHz_a.wav", "/tmp/rec_1233kHz_b.wav", "/tmp/noname_c.wav"};
    for (int i = 0; i < 3; i++) {
        wav_info_t w; char err[128];
        int r = wav_parse(files[i], &w, err, sizeof err);
        printf("%s: r=%d fs=%u ch=%u bits=%u off=%u bytes=%llu auxi=%d center=%u next='%s' shift=%.0f (name=%d auxi=%d)\n", files[i], r, w.sample_rate, w.channels,
               w.bits, w.data_offset, (unsigned long long)w.data_bytes, w.has_auxi, w.auxi_center_hz, w.next_name, w.shift_hz, w.shift_from_name, w.shift_from_auxi);
    }
    const char *names[] = {"x_549 kHz.wav", "/a/b_1233.5kHz.WAV", "foo.wav", "dir1kHz/foo_88kHz_x.wav"};
    for (int i = 0; i < 4; i++) { double hz = 0; bool ok = wav_shift_from_name(names[i], &hz); printf("name '%s' -> %d %.1f\n", names[i], ok, hz); }
    return 0;
}
