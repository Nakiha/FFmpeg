/*
 * VVC ALF filters for WebAssembly SIMD128, 8-bit
 *
 * Mirrors libavcodec/vvc/filter_template.c (8-bit instantiation) and the
 * aarch64 NEON driver layout: two ALF blocks (8 pixels) are filtered per
 * vector row.  Clipped tap pairs stay in 16-bit lanes; products are widened
 * to 32-bit with extending multiplies so the accumulator matches the C
 * `int sum` no matter how large the transmitted coefficients are.
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

#include <stddef.h>
#include <stdint.h>
#include <wasm_simd128.h>

#include "libavutil/attributes.h"
#include "libavutil/common.h"

#include "libavcodec/wasm/vvc/alf.h"

#define ALF_BLOCK_SIZE        4
#define ALF_NUM_COEFF_LUMA    12
#define ALF_NUM_COEFF_CHROMA   6

typedef uint8_t pixel;

static av_always_inline v128_t alf_load8(const pixel *p)
{
    return wasm_u16x8_extend_low_u8x16(wasm_v128_load(p));
}

/* av_clip(v0 - curr, -clip, clip) + av_clip(v1 - curr, -clip, clip) */
static av_always_inline v128_t alf_clip_pair16(v128_t v0, v128_t v1, v128_t curr, v128_t clipv)
{
    const v128_t negclip = wasm_i16x8_sub(wasm_i16x8_splat(0), clipv);
    v128_t d0 = wasm_i16x8_sub(v0, curr);
    v128_t d1 = wasm_i16x8_sub(v1, curr);

    d0 = wasm_i16x8_min(wasm_i16x8_max(d0, negclip), clipv);
    d1 = wasm_i16x8_min(wasm_i16x8_max(d1, negclip), clipv);
    return wasm_i16x8_add(d0, d1);
}

static av_always_inline void alf_store_row(pixel *dst, v128_t v8, int npix)
{
    if (npix == 8)
        wasm_v128_store64_lane(dst, v8, 0);
    else
        wasm_v128_store32_lane(dst, v8, 0);
}

/*
 * One row of an 8-pixel span (two adjacent ALF blocks).  `fv`/`cv` hold the
 * per-tap coefficient/clip vectors already interleaved for the pair; rows
 * p[0..6] are expected to carry the virtual-boundary substitutions.
 */
static av_always_inline void alf_luma_row8(pixel *dst, const pixel *const p[7],
    const int16_t (*fv)[8], const int16_t (*cv)[8],
    const int is_near_vb, const int npix)
{
    const v128_t curr = alf_load8(p[0]);
    v128_t acc_lo = wasm_i32x4_splat(0);
    v128_t acc_hi = wasm_i32x4_splat(0);

#define ALF_TAP(k, a, ax, b, bx)                                                     \
    do {                                                                             \
        const v128_t t = alf_clip_pair16(alf_load8(p[a] + (ax)),                       \
                                       alf_load8(p[b] + (bx)), curr,                 \
                                       wasm_v128_load(cv[k]));                       \
        const v128_t f = wasm_v128_load(fv[k]);                                        \
        acc_lo = wasm_i32x4_add(acc_lo, wasm_i32x4_extmul_low_i16x8(t, f));          \
        acc_hi = wasm_i32x4_add(acc_hi, wasm_i32x4_extmul_high_i16x8(t, f));         \
    } while (0)

    ALF_TAP( 0, 5,  0, 6,  0);
    ALF_TAP( 1, 3, +1, 4, -1);
    ALF_TAP( 2, 3,  0, 4,  0);
    ALF_TAP( 3, 3, -1, 4, +1);
    ALF_TAP( 4, 1, +2, 2, -2);
    ALF_TAP( 5, 1, +1, 2, -1);
    ALF_TAP( 6, 1,  0, 2,  0);
    ALF_TAP( 7, 1, -1, 2, +1);
    ALF_TAP( 8, 1, -2, 2, +2);
    ALF_TAP( 9, 0, +3, 0, -3);
    ALF_TAP(10, 0, +2, 0, -2);
    ALF_TAP(11, 0, +1, 0, -1);

#undef ALF_TAP
    {
        const int shift  = is_near_vb ? 10 : 7;
        const v128_t off   = wasm_i32x4_splat(1 << (shift - 1));
        const v128_t lo    = wasm_i32x4_shr(wasm_i32x4_add(acc_lo, off), shift);
        const v128_t hi    = wasm_i32x4_shr(wasm_i32x4_add(acc_hi, off), shift);
        const v128_t v16   = wasm_i16x8_add(wasm_i16x8_narrow_i32x4(lo, hi), curr);
        const v128_t v8    = wasm_u8x16_narrow_i16x8(v16, v16);

        alf_store_row(dst, v8, npix);
    }
}

static av_always_inline void alf_chroma_row8(pixel *dst, const pixel *const p[5],
    const int16_t (*fv)[8], const int16_t (*cv)[8],
    const int is_near_vb, const int npix)
{
    const v128_t curr = alf_load8(p[0]);
    v128_t acc_lo = wasm_i32x4_splat(0);
    v128_t acc_hi = wasm_i32x4_splat(0);

#define ALF_TAP(k, a, ax, b, bx)                                                     \
    do {                                                                             \
        const v128_t t = alf_clip_pair16(alf_load8(p[a] + (ax)),                       \
                                       alf_load8(p[b] + (bx)), curr,                 \
                                       wasm_v128_load(cv[k]));                       \
        const v128_t f = wasm_v128_load(fv[k]);                                        \
        acc_lo = wasm_i32x4_add(acc_lo, wasm_i32x4_extmul_low_i16x8(t, f));          \
        acc_hi = wasm_i32x4_add(acc_hi, wasm_i32x4_extmul_high_i16x8(t, f));         \
    } while (0)

    ALF_TAP(0, 3,  0, 4,  0);
    ALF_TAP(1, 1, +1, 2, -1);
    ALF_TAP(2, 1,  0, 2,  0);
    ALF_TAP(3, 1, -1, 2, +1);
    ALF_TAP(4, 0, +2, 0, -2);
    ALF_TAP(5, 0, +1, 0, -1);

#undef ALF_TAP
    {
        const int shift  = is_near_vb ? 10 : 7;
        const v128_t off   = wasm_i32x4_splat(1 << (shift - 1));
        const v128_t lo    = wasm_i32x4_shr(wasm_i32x4_add(acc_lo, off), shift);
        const v128_t hi    = wasm_i32x4_shr(wasm_i32x4_add(acc_hi, off), shift);
        const v128_t v16   = wasm_i16x8_add(wasm_i16x8_narrow_i32x4(lo, hi), curr);
        const v128_t v8    = wasm_u8x16_narrow_i16x8(v16, v16);

        alf_store_row(dst, v8, npix);
    }
}

void ff_vvc_alf_filter_luma_8_simd128(uint8_t *_dst, ptrdiff_t dst_stride,
    const uint8_t *_src, ptrdiff_t src_stride, const int width, const int height,
    const int16_t *filter, const int16_t *clip, const int vb_pos)
{
    const pixel *src = (const pixel *)_src;
    pixel *dst       = (pixel *)_dst;

    for (int y = 0; y < height; y += ALF_BLOCK_SIZE) {
        const int far = (y + ALF_BLOCK_SIZE - 1 < vb_pos - 3) || (y > vb_pos + 2);

        for (int x = 0; x < width; x += 2 * ALF_BLOCK_SIZE) {
            const pixel *s0 = src + y * src_stride + x;
            const pixel *s1 = s0 + src_stride;
            const pixel *s2 = s0 - src_stride;
            const pixel *s3 = s1 + src_stride;
            const pixel *s4 = s2 - src_stride;
            const pixel *s5 = s3 + src_stride;
            const pixel *s6 = s4 - src_stride;
            const int npix = FFMIN(width - x, 2 * ALF_BLOCK_SIZE);
            int16_t fv[ALF_NUM_COEFF_LUMA][8];
            int16_t cv[ALF_NUM_COEFF_LUMA][8];

            for (int k = 0; k < ALF_NUM_COEFF_LUMA; k++) {
                for (int j = 0; j < 4; j++) {
                    fv[k][j]     = filter[k];
                    fv[k][4 + j] = filter[ALF_NUM_COEFF_LUMA + k];
                    cv[k][j]     = clip[k];
                    cv[k][4 + j] = clip[ALF_NUM_COEFF_LUMA + k];
                }
            }

            for (int i = 0; i < ALF_BLOCK_SIZE; i++) {
                pixel *drow = dst + (y + i) * dst_stride + x;
                const pixel *p[7];
                int is_near_vb = 0;

                p[0] = s0 + i * src_stride;
                p[1] = s1 + i * src_stride;
                p[2] = s2 + i * src_stride;
                p[3] = s3 + i * src_stride;
                p[4] = s4 + i * src_stride;
                p[5] = s5 + i * src_stride;
                p[6] = s6 + i * src_stride;

                if (!far) {
                    const int r = y + i;

                    is_near_vb = (r == vb_pos - 1) || (r == vb_pos);
                    if (is_near_vb) {
                        p[1] = p[0];
                        p[2] = p[0];
                    }
                    if (r >= vb_pos - 2 && r <= vb_pos + 1) {
                        p[3] = p[1];
                        p[4] = p[2];
                    }
                    if (r >= vb_pos - 3 && r <= vb_pos + 2) {
                        p[5] = p[3];
                        p[6] = p[4];
                    }
                }
                alf_luma_row8(drow, p, fv, cv, is_near_vb, npix);
            }
            filter += (npix / ALF_BLOCK_SIZE) * ALF_NUM_COEFF_LUMA;
            clip   += (npix / ALF_BLOCK_SIZE) * ALF_NUM_COEFF_LUMA;
        }
    }
}

void ff_vvc_alf_filter_chroma_8_simd128(uint8_t *_dst, ptrdiff_t dst_stride,
    const uint8_t *_src, ptrdiff_t src_stride, const int width, const int height,
    const int16_t *filter, const int16_t *clip, const int vb_pos)
{
    const pixel *src = (const pixel *)_src;
    pixel *dst       = (pixel *)_dst;

    for (int y = 0; y < height; y += ALF_BLOCK_SIZE) {
        const int far = (y + ALF_BLOCK_SIZE - 1 < vb_pos - 2) || (y > vb_pos + 1);

        for (int x = 0; x < width; x += 2 * ALF_BLOCK_SIZE) {
            const pixel *s0 = src + y * src_stride + x;
            const pixel *s1 = s0 + src_stride;
            const pixel *s2 = s0 - src_stride;
            const pixel *s3 = s1 + src_stride;
            const pixel *s4 = s2 - src_stride;
            const int npix = FFMIN(width - x, 2 * ALF_BLOCK_SIZE);
            int16_t fv[ALF_NUM_COEFF_CHROMA][8];
            int16_t cv[ALF_NUM_COEFF_CHROMA][8];

            /* Chroma ALF uses one filter set for the whole subblock. */
            for (int k = 0; k < ALF_NUM_COEFF_CHROMA; k++) {
                for (int j = 0; j < 4; j++) {
                    fv[k][j]     = filter[k];
                    fv[k][4 + j] = filter[k];
                    cv[k][j]     = clip[k];
                    cv[k][4 + j] = clip[k];
                }
            }

            for (int i = 0; i < ALF_BLOCK_SIZE; i++) {
                pixel *drow = dst + (y + i) * dst_stride + x;
                const pixel *p[5];
                int is_near_vb = 0;

                p[0] = s0 + i * src_stride;
                p[1] = s1 + i * src_stride;
                p[2] = s2 + i * src_stride;
                p[3] = s3 + i * src_stride;
                p[4] = s4 + i * src_stride;

                if (!far) {
                    const int r = y + i;

                    is_near_vb = (r == vb_pos - 1) || (r == vb_pos);
                    if (is_near_vb) {
                        p[1] = p[0];
                        p[2] = p[0];
                    }
                    if (r >= vb_pos - 2 && r <= vb_pos + 1) {
                        p[3] = p[1];
                        p[4] = p[2];
                    }
                }
                alf_chroma_row8(drow, p, fv, cv, is_near_vb, npix);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* ALF classification: scalar gradient pass (as C) plus a SIMD sum      */
/* over the gradient window.                                            */

static av_always_inline void alf_get_idx_8(int *class_idx, int *transpose_idx,
    const int *sum, const int ac)
{
    static const int arg_var[] = { 0, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4 };
    int hv0, hv1, dir_hv, d0, d1, dir_d, hvd1, hvd0, sum_hv, dir1;

    dir_hv = sum[0] <= sum[1];
    hv1    = FFMAX(sum[0], sum[1]);
    hv0    = FFMIN(sum[0], sum[1]);
    dir_d  = sum[2] <= sum[3];
    d1     = FFMAX(sum[2], sum[3]);
    d0     = FFMIN(sum[2], sum[3]);
    dir1   = (uint64_t)d1 * hv0 <= (uint64_t)hv1 * d0;
    hvd1   = dir1 ? hv1 : d1;
    hvd0   = dir1 ? hv0 : d0;
    sum_hv = sum[1] + sum[0];
    *class_idx = arg_var[av_clip_uintp2(sum_hv * ac >> 7, 4)];
    if (hvd1 * 2 > 9 * hvd0)
        *class_idx += ((dir1 << 1) + 2) * 5;
    else if (hvd1 > 2 * hvd0)
        *class_idx += ((dir1 << 1) + 1) * 5;
    *transpose_idx = dir_d * 2 + dir_hv;
}

void ff_vvc_alf_classify_8_simd128(int *class_idx, int *transpose_idx,
    const uint8_t *_src, const ptrdiff_t _src_stride, const int width, const int height,
    const int vb_pos, int *gradient_tmp)
{
    const int h       = height + 4;                     /* ALF_GRADIENT_BORDER * 2 */
    const int w       = width + 4;
    const int size    = (ALF_BLOCK_SIZE + 4) / 2;       /* 4 */
    const int gstride = (w / 2) * 4;
    const pixel *src  = (const pixel *)_src;
    const ptrdiff_t src_stride = _src_stride / sizeof(pixel);

    src -= 3 * src_stride + 2;                          /* (BORDER + 1) * stride + BORDER */

    /* Gradient pass: identical to the C reference. */
    {
        int *grad = gradient_tmp;
        for (int y = 0; y < h; y += 2) {
            const pixel *s0 = src + y * src_stride;
            const pixel *s1 = s0 + src_stride;
            const pixel *s2 = s1 + src_stride;
            const pixel *s3 = s2 + src_stride;

            if (y == vb_pos)
                s3 = s2;
            else if (y == vb_pos + 2)
                s0 = s1;

            for (int x = 0; x < w; x += 2) {
                const pixel *a0 = s0 + x;
                const pixel *p0 = s1 + x;
                const pixel *b0 = s2 + x;
                const int val0  = (*p0) << 1;
                const pixel *a1 = s1 + x + 1;
                const pixel *p1 = s2 + x + 1;
                const pixel *b1 = s3 + x + 1;
                const int val1  = (*p1) << 1;

                grad[0] = FFABS(val0 - *a0 - *b0) + FFABS(val1 - *a1 - *b1);
                grad[1] = FFABS(val0 - *(p0 - 1) - *(p0 + 1)) + FFABS(val1 - *(p1 - 1) - *(p1 + 1));
                grad[2] = FFABS(val0 - *(a0 - 1) - *(b0 + 1)) + FFABS(val1 - *(a1 - 1) - *(b1 + 1));
                grad[3] = FFABS(val0 - *(a0 + 1) - *(b0 - 1)) + FFABS(val1 - *(a1 + 1) - *(b1 - 1));
                grad += 4;
            }
        }
    }

    /* Window sum per ALF block, four directions per i32x4 lane. */
    for (int y = 0; y < height; y += ALF_BLOCK_SIZE) {
        int start = 0;
        int end   = size;
        int ac    = 2;

        if (y + ALF_BLOCK_SIZE == vb_pos) {
            end -= 1;
            ac = 3;
        } else if (y == vb_pos) {
            start += 1;
            ac = 3;
        }
        for (int x = 0; x < width; x += ALF_BLOCK_SIZE) {
            const int xg = x / 2;
            const int yg = y / 2;
            const int *g = gradient_tmp + (yg + start) * gstride + xg * 4;
            v128_t acc = wasm_i32x4_splat(0);
            int sum[4];

            for (int i = start; i < end; i++) {
                for (int j = 0; j < size; j++) {
                    acc = wasm_i32x4_add(acc, wasm_v128_load(g));
                    g += 4;
                }
                g += gstride - size * 4;
            }
            sum[0] = wasm_i32x4_extract_lane(acc, 0);
            sum[1] = wasm_i32x4_extract_lane(acc, 1);
            sum[2] = wasm_i32x4_extract_lane(acc, 2);
            sum[3] = wasm_i32x4_extract_lane(acc, 3);
            alf_get_idx_8(class_idx, transpose_idx, sum, ac);
            class_idx++;
            transpose_idx++;
        }
    }
}
