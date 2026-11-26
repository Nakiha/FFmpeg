/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef AVCODEC_VVENC_LIVE_H
#define AVCODEC_VVENC_LIVE_H
#include "avcodec.h"

/* Caller-owned mux accounting for the live-r28-tier encoder option. After
 * each receive, report the final video packet bits (including any changes
 * by media hooks) and the DTS on the mux timeline in microseconds. Report
 * the same DTS/bits after a successful write and flush. Calls are serialized
 * with send/receive. An aborted stream is discarded by avcodec_free_context. */
typedef struct AVVvencLiveDecision {
    int qp, role;
    int64_t predicted_bits;
    double complexity;
    int model_kind, saturated;
    int64_t decision_us, media_us;
    double quality_qp, allowance_bits, coefficient, room_bits, followup_bits;
} AVVvencLiveDecision;
int av_vvenc_live_coded(AVCodecContext *ctx, int64_t pts90k, int64_t dts_us,
                        int64_t bits, AVVvencLiveDecision *decision);
int av_vvenc_live_written(AVCodecContext *ctx, int64_t dts_us, int64_t wall_us, int64_t bits);
int av_vvenc_live_pending(AVCodecContext *ctx);

/* Inclusive wall-clock intervals in microseconds. GOP contains picture slots,
 * which include thread resources. The three wrapper intervals are disjoint. */
typedef struct AVVvencOpenStats {
    int64_t setup_us, library_us, post_us;
    int64_t thread_pool_us, preprocess_us, mctf_us, gop_us, picture_slots_us, thread_resources_us;
} AVVvencOpenStats;
int av_vvenc_get_open_stats(AVCodecContext *ctx, AVVvencOpenStats *stats);

/* Notify the caller when a final-pass picture finishes reconstruction. The
 * callback may run on a worker thread and must be fast, non-blocking and
 * must not re-enter FFmpeg or VVenC. Serialize registration with codec API
 * calls. Passing NULL for fn clears it and waits for active calls to return. */
typedef void (*AVVvencLiveWakeFn)(void *opaque);
int av_vvenc_live_set_wake(AVCodecContext *ctx, AVVvencLiveWakeFn fn, void *opaque);
#endif
