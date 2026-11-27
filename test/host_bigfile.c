#include "../main/wav_info.h"
#include <stdio.h>
#include <assert.h>
int main(int c, char **v){
  /* Kernfall: Datei mit 2.147.483.655 Byte, st_size bei 32-Bit-off_t als negative Zahl */
  assert(wav_size_from_stat(-2147483641LL, 4) == 2147483655ULL);
  assert(wav_size_from_stat(2147483655LL, 8) == 2147483655ULL);
  assert(wav_size_from_stat(2146435296LL, 4) == 2146435296ULL);
  assert(wav_size_from_stat(4294967295LL, 8) == 4294967295ULL);
  assert(wav_size_from_stat(-1LL, 4) == 4294967295ULL);   /* 4 GiB - 1 */
  puts("wav_size_from_stat OK");
  for (int i = 1; i < c; i++) { wav_info_t w; char e[128]; int r = wav_parse(v[i], &w, e, sizeof e);
    printf("%s\n  r=%d %s fs=%u data_offset=%u data_bytes=%llu file_size=%llu next='%s'\n", v[i], r, r ? e : "ok", w.sample_rate, w.data_offset,
           (unsigned long long)w.data_bytes, (unsigned long long)w.file_size, w.next_name); }
}
