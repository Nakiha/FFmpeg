/*
 * H.26L/H.264/AVC/JVT/14496-10/... encoder/decoder
 * Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_VOIDPLAYER_CABAC_BITS_H
#define AVCODEC_VOIDPLAYER_CABAC_BITS_H

#include "libavutil/common.h"
#include "libavutil/intmath.h"
#include "cabac.h"

static inline uint32_t ff_voidplayer_cabac_bit_position(const CABACContext *c,
                                                        const uint8_t *start)
{
    const uint32_t low = c->low;
    int64_t position;

    if (!start || !low)
        return 0;

    // The lowest set bit in low is the refill marker. Renormalization shifts
    // it left by one per consumed bit. A refill advances the byte pointer by
    // CABAC_BITS and moves the marker back by the same number of bits.
    position = (int64_t)(c->bytestream - start) * 8 + ff_ctz(low) - CABAC_BITS;
    // A final refill can read padding beyond the end of the CABAC data.
    return av_clip64(position, 0, FFMIN((int64_t)(c->bytestream_end - start) * 8,
                                       UINT32_MAX));
}

#endif /* AVCODEC_VOIDPLAYER_CABAC_BITS_H */
