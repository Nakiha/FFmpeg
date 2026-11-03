/*
 * WebAssembly SIMD128 VVC inter prediction entry points
 * (see wasm/vvc/inter.c)
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

#ifndef AVCODEC_WASM_VVC_INTER_H
#define AVCODEC_WASM_VVC_INTER_H

#include <stddef.h>
#include <stdint.h>

void ff_vvc_put_pixels_8_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);

void ff_vvc_avg_8_simd128(uint8_t *dst, ptrdiff_t dst_stride,
    const int16_t *src0, const int16_t *src1, int width, int height);

int ff_vvc_sad_8_simd128(const int16_t *src0, const int16_t *src1, int dx, int dy,
    int block_w, int block_h);

void ff_vvc_put_luma_h_8_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);
void ff_vvc_put_luma_v_8_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);
void ff_vvc_put_luma_hv_8_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);
void ff_vvc_put_chroma_h_8_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);
void ff_vvc_put_chroma_v_8_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);
void ff_vvc_put_chroma_hv_8_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);

void ff_vvc_put_pixels_10_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);

void ff_vvc_avg_10_simd128(uint8_t *dst, ptrdiff_t dst_stride,
    const int16_t *src0, const int16_t *src1, int width, int height);

void ff_vvc_put_luma_h_10_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);
void ff_vvc_put_luma_v_10_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);
void ff_vvc_put_luma_hv_10_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);
void ff_vvc_put_chroma_h_10_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);
void ff_vvc_put_chroma_v_10_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);
void ff_vvc_put_chroma_hv_10_simd128(int16_t *dst, const uint8_t *src, ptrdiff_t src_stride,
    int height, const int8_t *hf, const int8_t *vf, int width);

#define VVC_UNI_DECL(name) \
void ff_vvc_put_uni_ ## name ## _8_simd128(uint8_t *dst, ptrdiff_t dst_stride, \
    const uint8_t *src, ptrdiff_t src_stride, int height, const int8_t *hf, const int8_t *vf, int width)
VVC_UNI_DECL(luma_h);
VVC_UNI_DECL(luma_v);
VVC_UNI_DECL(luma_hv);
VVC_UNI_DECL(chroma_h);
VVC_UNI_DECL(chroma_v);
VVC_UNI_DECL(chroma_hv);
#undef VVC_UNI_DECL

#define VVC_DMVR8_DECL(name) \
void ff_vvc_dmvr ## name ## _8_simd128(int16_t *dst, const uint8_t *src, \
    ptrdiff_t stride, int height, intptr_t mx, intptr_t my, int width)
VVC_DMVR8_DECL();
VVC_DMVR8_DECL(_h);
VVC_DMVR8_DECL(_v);
VVC_DMVR8_DECL(_hv);
#undef VVC_DMVR8_DECL

#endif /* AVCODEC_WASM_VVC_INTER_H */
