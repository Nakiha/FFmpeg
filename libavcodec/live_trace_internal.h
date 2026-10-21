/* Fork-only opt-in per-picture telemetry, LGPL-2.1-or-later. */
#ifndef AVCODEC_LIVE_TRACE_INTERNAL_H
#define AVCODEC_LIVE_TRACE_INTERNAL_H
#include <time.h>
#include "libavutil/frame.h"
#include "libavutil/time.h"
/* CPU accounting uses an unslewed scheduler clock on Linux. Keep a nested
 * RAW elapsed interval for its audit; MONOTONIC remains the timeline clock. */
static inline int64_t ff_live_raw_us(void) {
#ifdef CLOCK_MONOTONIC_RAW
    struct timespec t;
    if(!clock_gettime(CLOCK_MONOTONIC_RAW,&t))return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;
#endif
    return -1;
}
static inline int64_t ff_live_cpu_us(void) {
#ifdef CLOCK_THREAD_CPUTIME_ID
    struct timespec t;
    if(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t))return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;
#endif
    return -1;
}
static inline AVLiveDecodeTrace *ff_live_trace(AVFrame *f) {
    AVFrameSideData *sd=av_frame_get_side_data(f,AV_FRAME_DATA_LIVE_DECODE_TRACE);
    return sd&&sd->size==sizeof(AVLiveDecodeTrace)?(AVLiveDecodeTrace*)sd->data:NULL;
}
#define FF_LIVE_SET(t,field,value) __atomic_store_n(&(t)->field,(int64_t)(value),__ATOMIC_RELEASE)
/* Only timed when the fast readiness check proves that a reference may wait. */
extern _Thread_local int64_t *ff_live_reference_wait_us;
#endif
