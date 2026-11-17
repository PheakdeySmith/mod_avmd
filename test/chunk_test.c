#define _GNU_SOURCE
/* feed odd chunk sizes and 16 kHz to check buffering gives identical verdicts */
#include "../engine/amvd_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
int main(void){
  int rates[]={8000,16000,48000}; amvd_config_t c; amvd_config_defaults(&c); c.wait_message_end=1;
  for(int ri=0;ri<3;ri++){ int R=rates[ri]; size_t n=R*10; int16_t *x=calloc(n,2); unsigned s=1;
    for(size_t i=0;i<n;i++){ double t=(double)i/R; double v=0;
      if(t>0.5&&t<4.5) { double f0=140+20*sin(6*t), ph=2*M_PI*f0*t; for(int h=1;h<=20;h++) v+=0.03*sin(h*ph)/h; v*=(0.55+0.45*sin(2*M_PI*4*t)); }
      if(t>5.0&&t<5.4) v=0.3*sin(2*M_PI*1000*t);
      s=s*1103515245+12345; v+=((int)(s>>16&0x7fff)-16384)/16384.0*0.0005; x[i]=(int16_t)(v*32767);}
    int sizes[]={1,7,160,333,4096}; char ref[64]="";
    for(int si=0;si<5;si++){ amvd_engine_t*e=amvd_engine_create(&c,R,NULL); int ev=0; size_t p=0;
      while(p<n && !(ev&AMVD_EV_DONE)){ size_t k=sizes[si]; if(k>n-p)k=n-p; ev|=amvd_engine_feed(e,x+p,k); p+=k;}
      const amvd_report_t*r=amvd_engine_report(e); char out[64]; snprintf(out,64,"%s/%s/%d/%s/%d",amvd_result_str(r->result),r->reason,r->decision_ms,amvd_end_str(r->end_type),r->end_ms);
      printf("rate=%5d chunk=%4d %s\n",R,sizes[si],out); if(si==0) snprintf(ref,64,"%s",out); else if(strcmp(ref,out)) {printf("MISMATCH\n"); return 1;}
      amvd_engine_destroy(e);} free(x);}
  puts("chunking/rate invariance OK"); return 0;}
