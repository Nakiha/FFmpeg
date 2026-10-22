/* Fork-only native decoder observations, LGPL-2.1-or-later. */
#include <string.h>
#include "early_output_internal.h"
#include "libavutil/opt.h"
#include "libavutil/refstruct.h"

int ff_early_output_init(AVCodecContext *ctx)
{
    FFEarlyOutputState *s;
    int64_t poc = 0, idr = 0;

    if (!ctx->codec || (strcmp(ctx->codec->name, "h264") &&
                        strcmp(ctx->codec->name, "hevc")))
        return 0;
    s = av_refstruct_alloc_ext(sizeof(*s), 0, NULL, NULL);
    if (!s)
        return AVERROR(ENOMEM);
    ctx->internal->early_output = s;
    av_opt_get_int(ctx->priv_data, "live_poc_early_output", 0, &poc);
    av_opt_get_int(ctx->priv_data, "live_idr_early_output", 0, &idr);
    s->requested_poc = !!poc;
    s->requested_idr = !!idr;
    atomic_init(&s->last_reason, (poc || idr) ? AV_EARLY_AWAIT_ANCHOR : AV_EARLY_DISABLED);
    atomic_init(&s->pictures, 0);
    atomic_init(&s->candidate_pictures, 0);
    atomic_init(&s->suppressed_frames, 0);
    for (int i = 0; i < AV_EARLY_MODE_COUNT; i++)
        atomic_init(&s->frames[i], 0);
    for (int i = 0; i < AV_EARLY_REASON_COUNT; i++)
        atomic_init(&s->decisions[i], 0);
    return 0;
}

int avcodec_get_early_output_stats(const AVCodecContext *ctx, AVCodecEarlyOutputStats *out)
{
    const FFEarlyOutputState *s;

    if (!ctx || !ctx->internal || !out)
        return AVERROR(EINVAL);
    s = ctx->internal->early_output;
    if (!s)
        return AVERROR(ENOSYS);
    memset(out, 0, sizeof(*out));
    out->version = AVCODEC_EARLY_OUTPUT_API_VERSION;
    out->requested_poc = s->requested_poc;
    out->requested_idr = s->requested_idr;
    out->last_reason = atomic_load_explicit(&s->last_reason, memory_order_relaxed);
    out->pictures = atomic_load_explicit(&s->pictures, memory_order_relaxed);
    out->candidate_pictures = atomic_load_explicit(&s->candidate_pictures, memory_order_relaxed);
    out->suppressed_frames = atomic_load_explicit(&s->suppressed_frames, memory_order_relaxed);
    for (int i = 0; i < AV_EARLY_MODE_COUNT; i++)
        out->frames[i] = atomic_load_explicit(&s->frames[i], memory_order_relaxed);
    for (int i = 0; i < AV_EARLY_REASON_COUNT; i++)
        out->decisions[i] = atomic_load_explicit(&s->decisions[i], memory_order_relaxed);
    return 0;
}

const char *avcodec_early_output_reason_name(int reason)
{
    static const char *const names[] = {
        "disabled", "unsupported_picture", "missing_hrd_cfr_timing",
        "invalid_access_unit", "parameter_change", "awaiting_anchor",
        "presentation_gap", "output_allocation", "bounded_capacity", "eligible"
    };
    return reason >= 0 && reason < AV_EARLY_REASON_COUNT ? names[reason] : "unknown";
}
