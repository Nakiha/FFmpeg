/*
 * VVC sample adaptive offset for WebAssembly SIMD128
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

#ifndef AVCODEC_WASM_VVC_SAO_H
#define AVCODEC_WASM_VVC_SAO_H

#include <stddef.h>
#include <stdint.h>

#define VVC_SAO_DECL(depth) \
void ff_vvc_sao_band_ ## depth ## _simd128(uint8_t *dst, const uint8_t *src, \
    ptrdiff_t dst_stride, ptrdiff_t src_stride, const int16_t *offset, int left, int width, int height); \
void ff_vvc_sao_edge_ ## depth ## _simd128(uint8_t *dst, const uint8_t *src, \
    ptrdiff_t dst_stride, const int16_t *offset, int eo, int width, int height)

VVC_SAO_DECL(8);
VVC_SAO_DECL(10);

#undef VVC_SAO_DECL
#endif
