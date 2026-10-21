#define _GNU_SOURCE
#include <time.h>
#include <sys/timex.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
static int64_t ns(clockid_t id) { struct timespec t; clock_gettime(id,&t); return (int64_t)t.tv_sec*1000000000+t.tv_nsec; }
int main(void) {
    cpu_set_t allowed, one; sched_getaffinity(0,sizeof(allowed),&allowed);
    puts("mode,sample,mono_ns,raw_ns,cpu_ns,frequency_ppm,tick_us,cpu_begin,cpu_end");
    for(int mode=0;mode<3;mode++) {
        one=allowed;
        if(mode) { CPU_ZERO(&one); for(int c=0;c<CPU_SETSIZE;c++) if(CPU_ISSET(c,&allowed)) { CPU_SET(c,&one); if(mode==1) break; else { for(int k=c+1;k<CPU_SETSIZE;k++) if(CPU_ISSET(k,&allowed)) { CPU_CLR(c,&one); break; } } } }
        sched_setaffinity(0,sizeof(one),&one);
        for(int i=0;i<1000;i++) {
            struct timex tx={0}; adjtimex(&tx);
            int c0=sched_getcpu();
            int64_t m0=ns(CLOCK_MONOTONIC),r0=ns(CLOCK_MONOTONIC_RAW),c0t=ns(CLOCK_THREAD_CPUTIME_ID);
            volatile uint64_t x=1;
            while(ns(CLOCK_MONOTONIC_RAW)-r0<20000000) for(int j=0;j<200;j++) x=x*1664525+1013904223;
            int64_t c1t=ns(CLOCK_THREAD_CPUTIME_ID),r1=ns(CLOCK_MONOTONIC_RAW),m1=ns(CLOCK_MONOTONIC);
            printf("%d,%d,%lld,%lld,%lld,%.6f,%ld,%d,%d\n",mode,i,(long long)(m1-m0),(long long)(r1-r0),(long long)(c1t-c0t),tx.freq/65536.,tx.tick,c0,sched_getcpu());
        }
    }
}

