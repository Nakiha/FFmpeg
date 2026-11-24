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
#endif
