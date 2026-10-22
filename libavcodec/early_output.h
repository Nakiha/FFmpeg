/* Fork-only early-output observations, LGPL-2.1-or-later. */
#ifndef AVCODEC_EARLY_OUTPUT_H
#define AVCODEC_EARLY_OUTPUT_H
#include <stdint.h>
#include "avcodec.h"
#define AVCODEC_EARLY_OUTPUT_API_VERSION 1

enum AVCodecEarlyOutputMode {
    AV_EARLY_ORDINARY,
    AV_EARLY_H264_HRD,
    AV_EARLY_H264_VERIFIED,
    AV_EARLY_HEVC_IDR,
    AV_EARLY_HEVC_PREFIX,
    AV_EARLY_MODE_COUNT
};
enum AVCodecEarlyOutputReason {
    AV_EARLY_DISABLED,
    AV_EARLY_UNSUPPORTED,
    AV_EARLY_NO_TIMING,
    AV_EARLY_INVALID_AU,
    AV_EARLY_PARAMETERS,
    AV_EARLY_AWAIT_ANCHOR,
    AV_EARLY_PRESENTATION_GAP,
    AV_EARLY_OUTPUT_ALLOCATION,
    AV_EARLY_BOUNDED_CAPACITY,
    AV_EARLY_ELIGIBLE,
    AV_EARLY_REASON_COUNT
};
/* Approximate atomic snapshots, shared by all native frame workers.
 * Lifetime counters survive flush. Frames count codec selections committed to
 * output queues, not frontend receives or muxed frames: cancel/flush can discard
 * them. decisions count completed picture AUs, not output-frame fallback causes.
 * last_reason is the last observed worker decision, not necessarily input order.
 * This v1 structure/layout is fixed; it is not an extensible caller buffer. */
typedef struct AVCodecEarlyOutputStats {
    int version, requested_poc, requested_idr, last_reason;
    uint64_t pictures, candidate_pictures;
    uint64_t frames[AV_EARLY_MODE_COUNT];
    uint64_t suppressed_frames;
    uint64_t decisions[AV_EARLY_REASON_COUNT];
} AVCodecEarlyOutputStats;
/* Native software h264/hevc only. Caller owns the context lifetime and must
 * serialize this API with flush/free. No decoding, allocation or queue lock. */
int avcodec_get_early_output_stats(const AVCodecContext *ctx, AVCodecEarlyOutputStats *stats);
const char *avcodec_early_output_reason_name(int reason);
#endif
