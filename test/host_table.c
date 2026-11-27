#include "../main/sdr_plan.h"
#include "../main/sdr_dsp.h"
#include <stdio.h>
int main(void){
  struct{uint32_t fs;double fc;uint32_t mx;}t[]={
   {250000,531e3,10000000},{250000,1233e3,10000000},{250000,198e3,10000000},{250000,3950e3,10000000},{250000,6090e3,10000000},{250000,6090e3,16000000},
   {500000,1233e3,10000000},{625000,1000e3,10000000},{1250000,1250e3,10000000},{1250000,1800e3,10000000},{2500000,1000e3,10000000}};
  printf("%8s %8s %6s | %3s %9s %5s | %-12s | %s\n","fs","fc","maxrate","N","Fs_out","Alias","1 Core","2 Cores (Core1 / Core0)");
  for(unsigned i=0;i<sizeof t/sizeof*t;i++){sdr_plan_t p; if(sdr_plan_rate(t[i].fs,t[i].fc,t[i].mx,&p)){printf("%8u %7.0fk | nicht machbar\n",t[i].fs,t[i].fc/1e3);continue;}
   bool fast=t[i].fs>=SDR_FAST_FS_HZ; double f,m; sdr_est_cycles(t[i].fs,p.fs_out,fast,&f,&m); float s=sdr_est_split(t[i].fs,p.fs_out,fast);
   printf("%8u %7.0fk %5.0fM | %3d %7.3f M %5s | %3.0f%% (%s) | %3.0f%% / %3.0f%%\n",t[i].fs,t[i].fc/1e3,t[i].mx/1e6,p.n,p.fs_out/1e6,p.alias?"JA":"nein",
     (f+m)/400e6*100,fast?"FAST":"HQ  ",(f+s*m)/400e6*100,(1-s)*m/400e6*100);}
}
