/*
 * VVC ALF SIMD128 (WebAssembly) entry points
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

#ifndef AVCODEC_WASM_VVC_ALF_H
#define AVCODEC_WASM_VVC_ALF_H

#include <stddef.h>
#include <stdint.h>

void ff_vvc_alf_filter_luma_8_simd128(uint8_t *dst, ptrdiff_t dst_stride,
    const uint8_t *src, ptrdiff_t src_stride, int width, int height,
    const int16_t *filter, const int16_t *clip, int vb_pos);

void ff_vvc_alf_filter_chroma_8_simd128(uint8_t *dst, ptrdiff_t dst_stride,
    const uint8_t *src, ptrdiff_t src_stride, int width, int height,
    const int16_t *filter, const int16_t *clip, int vb_pos);

void ff_vvc_alf_classify_8_simd128(int *class_idx, int *transpose_idx,
    const uint8_t *src, ptrdiff_t src_stride, int width, int height,
    int vb_pos, int *gradient_tmp);

#endif /* AVCODEC_WASM_VVC_ALF_H */
