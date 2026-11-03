/*
 * VoidPlayer VVC runtime tuning
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef AVCODEC_VVC_TUNING_H
#define AVCODEC_VVC_TUNING_H

#include "avcodec.h"

/* Call between decoder operations. Return the active count or a negative
 * AVERROR. A count of zero queries the active count. */
int avcodec_vvc_grow_frame_contexts(AVCodecContext *avctx, int count);

#endif
