/* Host-Test: liest int16 I/Q (stdin), schreibt Ausgabesamples (stdout).
 * usage: host_dsp bits hold shift_hz fs_out agc(0/1) gain */
#include "../main/sdr_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static sdr_dsp_t d;
int main(int argc, char **argv)
{
    
    int bits = atoi(argv[1]), hold = atoi(argv[2]);
    double shift = atof(argv[3]), fso = atof(argv[4]);
    d.agc = atoi(argv[5]); d.manual_gain = atof(argv[6]);
    sdr_dsp_cfg_t c = { bits, 0, hold, (uint32_t)llround(shift / fso * 4294967296.0), argc > 7 && atoi(argv[7]) };
    sdr_dsp_init(&d, &c);
    static int16_t in[2 * SDR_BLOCK_MAX];
    static uint16_t out[4 * 64 * SDR_BLOCK_MAX];
    size_t n;
    while ((n = fread(in, 4, SDR_BLOCK_MAX, stdin)) > 0) {
        size_t m = sdr_dsp_process(&d, in, n, out);
        if (bits == 8) { for (size_t i = 0; i < m; i++) fputc(((uint8_t *)out)[i], stdout); }
        else fwrite(out, 2, m, stdout);
    }
    return 0;
}
