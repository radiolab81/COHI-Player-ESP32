/* Prueft: filter + (mix in 2 Teilen, beliebige 64er-Teilung) + commit == process, ueber viele Bloecke. */
#include "../main/sdr_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static sdr_dsp_t A, B;
static uint16_t outA[8*4*1024*64], outB[8*4*1024*64];
int main(void)
{
    srand(7);
    int fails = 0, cases = 0;
    struct { int bits, fast, N; } cfgs[] = {{8,0,4},{16,0,16},{8,0,20},{16,0,64},{8,1,4},{16,1,8},{8,1,16}};
    for (unsigned c = 0; c < sizeof cfgs / sizeof *cfgs; c++) {
        sdr_dsp_cfg_t cfg = { cfgs[c].bits, c & 1, cfgs[c].fast ? cfgs[c].N/2 : cfgs[c].N/4,
                              (uint32_t)(0.2371 * 4294967296.0), cfgs[c].fast };
        memset(&A, 0, sizeof A); memset(&B, 0, sizeof B);
        A.agc = B.agc = 1; A.manual_gain = B.manual_gain = 0.5f;
        sdr_dsp_init(&A, &cfg); sdr_dsp_init(&B, &cfg);
        int bps = cfgs[c].bits / 8;
        for (int blk = 0; blk < 60; blk++) {
            size_t n = (blk % 5 == 4) ? 37 + (size_t)(rand() % 900) : 128 << (blk % 4);   /* auch ungerade Laengen */
            if (n > SDR_BLOCK_MAX) n = SDR_BLOCK_MAX;
            static int16_t iq[2 * SDR_BLOCK_MAX];
            for (size_t i = 0; i < n; i++) {
                double t = (double)(blk * 1000 + i);
                iq[2*i]   = (int16_t)(9000 * sin(t * 0.05) + (rand() % 2000 - 1000));
                iq[2*i+1] = (int16_t)(9000 * cos(t * 0.05) + (rand() % 2000 - 1000));
            }
            size_t mA = sdr_dsp_process(&A, iq, n, outA);
            size_t nbb = sdr_dsp_filter(&B, iq, n);
            size_t j0 = ((nbb * (size_t)(rand() % 51)) / 100) & ~(size_t)63;       /* 0..50 % */
            if (blk % 7 == 0) j0 = 0;                                              /* Extremfall: alles auf Worker */
            size_t m1 = sdr_dsp_mix(&B, 0, j0, outB);
            size_t m2 = sdr_dsp_mix(&B, j0, nbb - j0, (uint8_t *)outB + m1 * bps);  /* "Worker" */
            sdr_dsp_commit(&B, nbb);
            cases++;
            if (mA != m1 + m2 || memcmp(outA, outB, mA * bps) || A.phase != B.phase || A.prevI != B.prevI) {
                fails++; printf("MISMATCH cfg=%u blk=%d n=%zu j0=%zu\n", c, blk, n, j0);
            }
        }
    }
    printf("%d Faelle, %d Fehler -> %s\n", cases, fails, fails ? "FEHLER" : "bit-identisch");
    return fails != 0;
}
