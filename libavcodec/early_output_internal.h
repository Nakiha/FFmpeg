#ifndef AVCODEC_EARLY_OUTPUT_INTERNAL_H
#define AVCODEC_EARLY_OUTPUT_INTERNAL_H
#include <stdatomic.h>
#include "early_output.h"
#include "internal.h"

typedef struct FFEarlyOutputState {
    int requested_poc, requested_idr;
    atomic_int last_reason;
    atomic_uint_fast64_t pictures, candidate_pictures;
    atomic_uint_fast64_t frames[AV_EARLY_MODE_COUNT], suppressed_frames;
    atomic_uint_fast64_t decisions[AV_EARLY_REASON_COUNT];
} FFEarlyOutputState;

int ff_early_output_init(AVCodecContext *ctx);

static inline void ff_early_output_picture(const AVCodecContext *ctx, int reason, int candidate)
{
    FFEarlyOutputState *s = ctx->internal->early_output;
    if (!s)
        return;
    if (reason < 0 || reason >= AV_EARLY_REASON_COUNT)
        reason = AV_EARLY_UNSUPPORTED;
    atomic_fetch_add_explicit(&s->pictures, 1, memory_order_relaxed);
    if (candidate)
        atomic_fetch_add_explicit(&s->candidate_pictures, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->decisions[reason], 1, memory_order_relaxed);
    atomic_store_explicit(&s->last_reason, reason, memory_order_relaxed);
}

static inline void ff_early_output_frames(const AVCodecContext *ctx, int mode, uint64_t count)
{
    FFEarlyOutputState *s = ctx->internal->early_output;
    if (s && count)
        atomic_fetch_add_explicit(&s->frames[mode], count, memory_order_relaxed);
}

static inline void ff_early_output_suppressed(const AVCodecContext *ctx)
{
    FFEarlyOutputState *s = ctx->internal->early_output;
    if (s)
        atomic_fetch_add_explicit(&s->suppressed_frames, 1, memory_order_relaxed);
}
#endif
