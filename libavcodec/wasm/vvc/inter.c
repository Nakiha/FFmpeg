/*
 * VVC inter prediction helpers for WebAssembly SIMD128, 8- and 10-bit
 *
 * 16-bit intermediate prediction (put_*_pixels), bi-prediction averaging and
 * the block SAD used by decoder-side refinement, mirroring the 8-bit C
 * instantiation of h2656_inter_template.c.  All entry points keep the C
 * semantics bit-exact, including the int16 wrap-free averaging range.
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

#include "libavcodec/wasm/vvc/inter.h"
#include "libavcodec/vvc/data.h"

#define MAX_PB_SIZE 128

typedef uint8_t pixel;

void ff_vvc_put_pixels_8_simd128(int16_t *dst, const uint8_t *_src,
    ptrdiff_t _src_stride, const int height, const int8_t *hf, const int8_t *vf,
    const int width)
{
    const pixel *src           = (const pixel *)_src;
    const ptrdiff_t src_stride = _src_stride / sizeof(pixel);

    for (int y = 0; y < height; y++) {
        int x = 0;

        for (; x + 8 <= width; x += 8) {
            const v128_t v = wasm_u16x8_load8x8(src + x);
            wasm_v128_store(dst + x, wasm_i16x8_shl(v, 14 - 8));
        }
        for (; x < width; x++)
            dst[x] = src[x] << (14 - 8);
        src += src_stride;
        dst += MAX_PB_SIZE;
    }
}

// After rounding, both output ranges end at the result of 32767 >> shift.
// Saturating a signed sum in either direction therefore preserves clipping.
static av_always_inline v128_t rounded_sum(v128_t a, v128_t b, const int shift)
{
    const v128_t offset = wasm_i16x8_splat(1 << (shift - 1));
    return wasm_i16x8_shr(wasm_i16x8_add_sat(wasm_i16x8_add_sat(a, b), offset), shift);
}

void ff_vvc_avg_8_simd128(uint8_t *_dst, const ptrdiff_t _dst_stride,
    const int16_t *src0, const int16_t *src1, const int width, const int height)
{
    pixel *dst                  = (pixel *)_dst;
    const ptrdiff_t dst_stride  = _dst_stride / sizeof(pixel);
    const int shift             = FFMAX(3, 15 - 8);
    const int offset            = 1 << (shift - 1);

    for (int y = 0; y < height; y++) {
        int x = 0;

        for (; x + 16 <= width; x += 16) {
            const v128_t lo = rounded_sum(wasm_v128_load(src0 + x), wasm_v128_load(src1 + x), shift);
            const v128_t hi = rounded_sum(wasm_v128_load(src0 + x + 8), wasm_v128_load(src1 + x + 8), shift);
            wasm_v128_store(dst + x, wasm_u8x16_narrow_i16x8(lo, hi));
        }
        if (x + 8 <= width) {
            const v128_t v = rounded_sum(wasm_v128_load(src0 + x), wasm_v128_load(src1 + x), shift);
            wasm_v128_store64_lane(dst + x, wasm_u8x16_narrow_i16x8(v, v), 0);
            x += 8;
        }
        for (; x < width; x++)
            dst[x] = av_clip_uint8((src0[x] + src1[x] + offset) >> shift);
        src0 += MAX_PB_SIZE;
        src1 += MAX_PB_SIZE;
        dst  += dst_stride;
    }
}

static av_always_inline int i32x4_sum(v128_t v)
{
    v128_t t = wasm_i32x4_add(v, wasm_i32x4_shuffle(v, v, 2, 3, 0, 1));
    t = wasm_i32x4_add(t, wasm_i32x4_shuffle(t, t, 1, 0, 3, 2));
    return wasm_i32x4_extract_lane(t, 0);
}

int ff_vvc_sad_8_simd128(const int16_t *src0, const int16_t *src1, int dx, int dy,
    const int block_w, const int block_h)
{
    v128_t acc = wasm_i32x4_const_splat(0);
    int sad = 0;

    dx -= 2;
    dy -= 2;
    src0 += (2 + dy) * MAX_PB_SIZE + 2 + dx;
    src1 += (2 - dy) * MAX_PB_SIZE + 2 - dx;
    for (int y = 0; y < block_h; y += 2) {
        int x = 0;

        for (; x + 8 <= block_w; x += 8) {
            const v128_t a = wasm_v128_load(src0 + x);
            const v128_t b = wasm_v128_load(src1 + x);
            // The difference fits in unsigned 16 bits, including opposite
            // signed extremes. Widen before accumulating across the block.
            const v128_t d = wasm_i16x8_sub(wasm_i16x8_max(a, b), wasm_i16x8_min(a, b));
            acc = wasm_i32x4_add(acc, wasm_u32x4_extadd_pairwise_u16x8(d));
        }
        for (; x < block_w; x++)
            sad += FFABS(src0[x] - src1[x]);
        src0 += 2 * MAX_PB_SIZE;
        src1 += 2 * MAX_PB_SIZE;
    }
    return sad + i32x4_sum(acc);
}

/* The 8-bit DMVR intermediate samples are in [0, 1020]. With at most
 * eight sampled rows and two groups per row, each lane fits in 16 bits. */
int ff_vvc_dmvr_sad_bound_8_simd128(const int16_t *src0, const int16_t *src1, int dx, int dy,
    int width, int height, int limit)
{
    v128_t acc = wasm_i16x8_splat(0);
    src0 += dy * MAX_PB_SIZE + dx;
    src1 += (4 - dy) * MAX_PB_SIZE + 4 - dx;

    for (int y = 0; y < height; y += 2) {
        const v128_t a = wasm_v128_load(src0);
        const v128_t b = wasm_v128_load(src1);
        acc = wasm_i16x8_add(acc, wasm_i16x8_abs(wasm_i16x8_sub(a, b)));
        if (width == 16) {
            const v128_t a1 = wasm_v128_load(src0 + 8);
            const v128_t b1 = wasm_v128_load(src1 + 8);
            acc = wasm_i16x8_add(acc, wasm_i16x8_abs(wasm_i16x8_sub(a1, b1)));
        }
        if (y == 6 && y + 2 < height &&
            i32x4_sum(wasm_u32x4_extadd_pairwise_u16x8(acc)) >= limit)
            return INT_MAX;

        src0 += 2 * MAX_PB_SIZE;
        src1 += 2 * MAX_PB_SIZE;
    }
    return i32x4_sum(wasm_u32x4_extadd_pairwise_u16x8(acc));
}

static av_always_inline v128_t load_pix8(const pixel *p)
{
    return wasm_u16x8_load8x8(p);
}

static av_always_inline v128_t filter8(const pixel *src, ptrdiff_t stride,
    int taps, const int8_t *filter)
{
    v128_t sum = wasm_i16x8_const_splat(0);
    for (int k = 0; k < taps; k++)
        sum = wasm_i16x8_add(sum, wasm_i16x8_mul(wasm_i16x8_splat(filter[k]), load_pix8(src + k * stride)));
    return sum;
}

static av_always_inline int filter_scalar(const pixel *src, ptrdiff_t stride,
    int taps, const int8_t *filter)
{
    int sum = 0;
    for (int k = 0; k < taps; k++)
        sum += filter[k] * src[k * stride];
    return sum;
}

static av_always_inline void store_uni8(pixel *dst, v128_t value)
{
    wasm_v128_store64_lane(dst, wasm_u8x16_narrow_i16x8(value, value), 0);
}

static av_always_inline void put_uni_filter8(pixel *dst, ptrdiff_t dst_stride,
    const pixel *src, ptrdiff_t src_stride, int height, int width, ptrdiff_t tap_stride,
    int first, int taps, const int8_t *filter)
{
    const v128_t round = wasm_i16x8_const_splat(32);
    src += first * tap_stride;
    for (int y = 0; y < height; y++, dst += dst_stride, src += src_stride) {
        int x = 0;
        for (; x + 8 <= width; x += 8)
            store_uni8(dst + x, wasm_i16x8_shr(wasm_i16x8_add(filter8(src + x, tap_stride, taps, filter), round), 6));
        for (; x < width; x++)
            dst[x] = av_clip_uint8((filter_scalar(src + x, tap_stride, taps, filter) + 32) >> 6);
    }
}

/* Keep a rolling window of horizontally filtered rows for each eight-column
 * stripe, so the vertical pass consumes the SIMD values directly. */
static av_always_inline void put_uni_hv8(pixel *dst, ptrdiff_t dst_stride,
    const pixel *src, ptrdiff_t src_stride, int height, int width,
    int first, int taps, const int8_t *hf, const int8_t *vf)
{
    const v128_t round = wasm_i32x4_const_splat(2048);
    v128_t weights[8];
    int x = 0;

    src += first * (src_stride + 1);
    for (int k = 0; k < taps; k++)
        weights[k] = wasm_i16x8_splat(vf[k]);

    for (; x + 8 <= width; x += 8) {
        const pixel *row = src + x;
        pixel *out = dst + x;
        v128_t window[8];

        for (int k = 0; k < taps - 1; k++, row += src_stride)
            window[k] = filter8(row, 1, taps, hf);
        for (int y = 0; y < height; y++, row += src_stride, out += dst_stride) {
            v128_t lo = round, hi = round;
            window[taps - 1] = filter8(row, 1, taps, hf);
            for (int k = 0; k < taps; k++) {
                lo = wasm_i32x4_add(lo, wasm_i32x4_extmul_low_i16x8(window[k], weights[k]));
                hi = wasm_i32x4_add(hi, wasm_i32x4_extmul_high_i16x8(window[k], weights[k]));
            }
            store_uni8(out, wasm_i16x8_narrow_i32x4(wasm_i32x4_shr(lo, 12), wasm_i32x4_shr(hi, 12)));
            for (int k = 0; k < taps - 1; k++)
                window[k] = window[k + 1];
        }
    }

    for (int y = 0; x < width && y < height; y++) {
        pixel *out = dst + y * dst_stride;
        for (int col = x; col < width; col++) {
            int sum = 2048;
            for (int k = 0; k < taps; k++)
                sum += vf[k] * filter_scalar(src + (y + k) * src_stride + col, 1, taps, hf);
            out[col] = av_clip_uint8(sum >> 12);
        }
    }
}

/* One separable filter pass over 8-bit samples.  `first` is the offset of the
 * first tap relative to the output position and `stride` the distance between
 * taps (1 for horizontal, the line size for vertical).  The running sum of
 * the spec filters stays within int16: max sum |f| * 255 <= 29580 for luma
 * and <= 24990 for chroma. */
static av_always_inline void put_filter_pass_8(int16_t *dst, const pixel *src,
    const ptrdiff_t stride, const int width, const int first, const int ntaps,
    const int8_t *filter)
{
    int x = 0;

    for (; x + 8 <= width; x += 8) {
        v128_t acc = wasm_i16x8_splat(0);
        for (int k = 0; k < ntaps; k++)
            acc = wasm_i16x8_add(acc, wasm_i16x8_mul(wasm_i16x8_splat(filter[k]),
                                                      load_pix8(src + (first + k) * stride + x)));
        wasm_v128_store(dst + x, acc);
    }
    for (; x < width; x++) {
        int sum = 0;
        for (int k = 0; k < ntaps; k++)
            sum += filter[k] * src[(first + k) * stride + x];
        dst[x] = sum;
    }
}

/* Vertical pass over 16-bit intermediates: products of int8 taps with the
 * first-pass sums need 32-bit accumulation. */
static av_always_inline void put_filter_v_from16(int16_t *dst, const int16_t *tmp,
    const int width, const int first, const int ntaps, const int8_t *filter)
{
    int x = 0;

    for (; x + 8 <= width; x += 8) {
        v128_t lo = wasm_i32x4_splat(0);
        v128_t hi = wasm_i32x4_splat(0);
        for (int k = 0; k < ntaps; k++) {
            const v128_t t = wasm_v128_load(tmp + (first + k) * MAX_PB_SIZE + x);
            const v128_t f = wasm_i16x8_splat(filter[k]);
            lo = wasm_i32x4_add(lo, wasm_i32x4_extmul_low_i16x8(t, f));
            hi = wasm_i32x4_add(hi, wasm_i32x4_extmul_high_i16x8(t, f));
        }
        lo = wasm_i32x4_shr(lo, 6);
        hi = wasm_i32x4_shr(hi, 6);
        wasm_v128_store(dst + x, wasm_i16x8_narrow_i32x4(lo, hi));
    }
    for (; x < width; x++) {
        int sum = 0;
        for (int k = 0; k < ntaps; k++)
            sum += filter[k] * tmp[(first + k) * MAX_PB_SIZE + x];
        dst[x] = sum >> 6;
    }
}

void ff_vvc_put_pixels_10_simd128(int16_t *dst, const uint8_t *_src,
    ptrdiff_t src_stride, int height, const int8_t *hf, const int8_t *vf, int width)
{
    const uint16_t *src = (const uint16_t *)_src;
    src_stride /= sizeof(*src);
    for (int y = 0; y < height; y++) {
        int x = 0;
        for (; x + 8 <= width; x += 8)
            wasm_v128_store(dst + x, wasm_i16x8_shl(wasm_v128_load(src + x), 4));
        for (; x < width; x++)
            dst[x] = src[x] << 4;
        src += src_stride;
        dst += MAX_PB_SIZE;
    }
}

void ff_vvc_avg_10_simd128(uint8_t *_dst, ptrdiff_t dst_stride,
    const int16_t *src0, const int16_t *src1, int width, int height)
{
    uint16_t *dst = (uint16_t *)_dst;
    const v128_t zero = wasm_i16x8_splat(0);
    const v128_t max = wasm_i16x8_splat(1023);
    dst_stride /= sizeof(*dst);
    for (int y = 0; y < height; y++) {
        int x = 0;
        for (; x + 8 <= width; x += 8) {
            const v128_t v = rounded_sum(wasm_v128_load(src0 + x), wasm_v128_load(src1 + x), 5);
            wasm_v128_store(dst + x, wasm_i16x8_min(wasm_i16x8_max(v, zero), max));
        }
        for (; x < width; x++)
            dst[x] = av_clip_uintp2((src0[x] + src1[x] + 16) >> 5, 10);
        dst  += dst_stride;
        src0 += MAX_PB_SIZE;
        src1 += MAX_PB_SIZE;
    }
}

/* Unlike the 8-bit path, a 10-bit first-pass tap sum can exceed int16.
 * Apply the reference shift before narrowing to the 14-bit intermediate. */
static av_always_inline void put_filter_pass_10(int16_t *dst, const uint16_t *src,
    ptrdiff_t stride, int width, int first, int ntaps, const int8_t *filter)
{
    int x = 0;
    for (; x + 8 <= width; x += 8) {
        v128_t lo = wasm_i32x4_splat(0);
        v128_t hi = wasm_i32x4_splat(0);
        for (int k = 0; k < ntaps; k++) {
            const v128_t tap = wasm_i16x8_splat(filter[k]);
            const v128_t v = wasm_v128_load(src + (first + k) * stride + x);
            lo = wasm_i32x4_add(lo, wasm_i32x4_extmul_low_i16x8(v, tap));
            hi = wasm_i32x4_add(hi, wasm_i32x4_extmul_high_i16x8(v, tap));
        }
        wasm_v128_store(dst + x, wasm_i16x8_narrow_i32x4(
            wasm_i32x4_shr(lo, 2), wasm_i32x4_shr(hi, 2)));
    }
    for (; x < width; x++) {
        int sum = 0;
        for (int k = 0; k < ntaps; k++)
            sum += filter[k] * src[(first + k) * stride + x];
        dst[x] = sum >> 2;
    }
}

#define LUMA_FIRST   (-3)
#define CHROMA_FIRST (-1)
#define LUMA_TAPS     8
#define CHROMA_TAPS   4

#define PUT_FILTER_FUNCS(depth, pixel, name, first, ntaps, before, extra)                       \
void ff_vvc_put_##name##_h_##depth##_simd128(int16_t *dst, const uint8_t *_src,           \
    ptrdiff_t _src_stride, const int height, const int8_t *hf, const int8_t *vf,  \
    const int width)                                                              \
{                                                                                 \
    const pixel *src           = (const pixel *)_src;                             \
    const ptrdiff_t src_stride = _src_stride / sizeof(pixel);                     \
                                                                                  \
    for (int y = 0; y < height; y++) {                                            \
        put_filter_pass_##depth(dst, src + y * src_stride, 1, width, first, ntaps, hf);   \
        dst += MAX_PB_SIZE;                                                       \
    }                                                                             \
}                                                                                 \
                                                                                  \
void ff_vvc_put_##name##_v_##depth##_simd128(int16_t *dst, const uint8_t *_src,           \
    ptrdiff_t _src_stride, const int height, const int8_t *hf, const int8_t *vf,  \
    const int width)                                                              \
{                                                                                 \
    const pixel *src           = (const pixel *)_src;                             \
    const ptrdiff_t src_stride = _src_stride / sizeof(pixel);                     \
                                                                                  \
    for (int y = 0; y < height; y++) {                                            \
        put_filter_pass_##depth(dst, src + y * src_stride, src_stride, width, first,      \
                        ntaps, vf);                                               \
        dst += MAX_PB_SIZE;                                                       \
    }                                                                             \
}                                                                                 \
                                                                                  \
void ff_vvc_put_##name##_hv_##depth##_simd128(int16_t *dst, const uint8_t *_src,          \
    ptrdiff_t _src_stride, const int height, const int8_t *hf, const int8_t *vf,  \
    const int width)                                                              \
{                                                                                 \
    int16_t tmp_array[(MAX_PB_SIZE + extra) * MAX_PB_SIZE];                       \
    int16_t *tmp                = tmp_array;                                      \
    const pixel *src            = (const pixel *)_src;                            \
    const ptrdiff_t src_stride  = _src_stride / sizeof(pixel);                    \
                                                                                  \
    src -= before * src_stride;                                                   \
    for (int y = 0; y < height + extra; y++) {                                    \
        put_filter_pass_##depth(tmp, src + y * src_stride, 1, width, first, ntaps, hf);   \
        tmp += MAX_PB_SIZE;                                                       \
    }                                                                             \
                                                                                  \
    tmp = tmp_array + before * MAX_PB_SIZE;                                       \
    for (int y = 0; y < height; y++) {                                            \
        put_filter_v_from16(dst, tmp, width, first, ntaps, vf);                   \
        tmp += MAX_PB_SIZE;                                                       \
        dst += MAX_PB_SIZE;                                                       \
    }                                                                             \
}

PUT_FILTER_FUNCS(8, uint8_t, luma, LUMA_FIRST, LUMA_TAPS, 3, 7)
PUT_FILTER_FUNCS(8, uint8_t, chroma, CHROMA_FIRST, CHROMA_TAPS, 1, 3)
PUT_FILTER_FUNCS(10, uint16_t, luma, LUMA_FIRST, LUMA_TAPS, 3, 7)
PUT_FILTER_FUNCS(10, uint16_t, chroma, CHROMA_FIRST, CHROMA_TAPS, 1, 3)


#define PUT_UNI_8(name, first, taps)                                             \
void ff_vvc_put_uni_ ## name ## _h_8_simd128(uint8_t *dst, ptrdiff_t dst_stride,  \
    const uint8_t *src, ptrdiff_t src_stride, int height,                        \
    const int8_t *hf, const int8_t *vf, int width)                               \
{                                                                               \
    put_uni_filter8(dst, dst_stride, src, src_stride, height, width, 1, first, taps, hf); \
}                                                                               \
void ff_vvc_put_uni_ ## name ## _v_8_simd128(uint8_t *dst, ptrdiff_t dst_stride,  \
    const uint8_t *src, ptrdiff_t src_stride, int height,                        \
    const int8_t *hf, const int8_t *vf, int width)                               \
{                                                                               \
    put_uni_filter8(dst, dst_stride, src, src_stride, height, width, src_stride, first, taps, vf); \
}                                                                               \
void ff_vvc_put_uni_ ## name ## _hv_8_simd128(uint8_t *dst, ptrdiff_t dst_stride, \
    const uint8_t *src, ptrdiff_t src_stride, int height,                        \
    const int8_t *hf, const int8_t *vf, int width)                               \
{                                                                               \
    put_uni_hv8(dst, dst_stride, src, src_stride, height, width, first, taps, hf, vf); \
}

PUT_UNI_8(luma, LUMA_FIRST, LUMA_TAPS)
PUT_UNI_8(chroma, CHROMA_FIRST, CHROMA_TAPS)


static av_always_inline v128_t dmvr_blend8(v128_t a, v128_t b,
    v128_t wa, v128_t wb, int shift)
{
    const v128_t round = wasm_i16x8_splat(1 << (shift - 1));
    const v128_t sum = wasm_i16x8_add(wasm_i16x8_mul(a, wa), wasm_i16x8_mul(b, wb));
    return wasm_u16x8_shr(wasm_i16x8_add(sum, round), shift);
}

static av_always_inline void dmvr_8(int16_t *dst, const pixel *src, ptrdiff_t stride,
    int height, intptr_t mx, intptr_t my, int width, int horizontal, int vertical)
{
    const int8_t *fx = ff_vvc_inter_luma_dmvr_filters[mx];
    const int8_t *fy = ff_vvc_inter_luma_dmvr_filters[my];
    const v128_t x0 = wasm_i16x8_splat(fx[0]), x1 = wasm_i16x8_splat(fx[1]);
    const v128_t y0 = wasm_i16x8_splat(fy[0]), y1 = wasm_i16x8_splat(fy[1]);
    int x = 0;

    if (horizontal && vertical) {
        for (; x + 8 <= width; x += 8) {
            const pixel *row = src + x;
            int16_t *out = dst + x;
            v128_t prev = dmvr_blend8(load_pix8(row), load_pix8(row + 1), x0, x1, 2);
            for (int y = 0; y < height; y++, out += MAX_PB_SIZE) {
                row += stride;
                const v128_t next = dmvr_blend8(load_pix8(row), load_pix8(row + 1), x0, x1, 2);
                wasm_v128_store(out, dmvr_blend8(prev, next, y0, y1, 4));
                prev = next;
            }
        }
        for (int y = 0; y < height; y++, src += stride, dst += MAX_PB_SIZE)
            for (int col = x; col < width; col++) {
                const int a = (fx[0] * src[col] + fx[1] * src[col + 1] + 2) >> 2;
                const int b = (fx[0] * src[col + stride] + fx[1] * src[col + stride + 1] + 2) >> 2;
                dst[col] = (fy[0] * a + fy[1] * b + 8) >> 4;
            }
        return;
    }

    const ptrdiff_t step = horizontal ? 1 : stride;
    const v128_t a = horizontal ? x0 : y0, b = horizontal ? x1 : y1;
    const int8_t *f = horizontal ? fx : fy;
    for (int y = 0; y < height; y++, src += stride, dst += MAX_PB_SIZE) {
        x = 0;
        for (; x + 8 <= width; x += 8) {
            const v128_t value = load_pix8(src + x);
            const v128_t result = horizontal || vertical
                ? dmvr_blend8(value, load_pix8(src + x + step), a, b, 2)
                : wasm_i16x8_shl(value, 2);
            wasm_v128_store(dst + x, result);
        }
        for (; x < width; x++)
            dst[x] = horizontal || vertical ? (f[0] * src[x] + f[1] * src[x + step] + 2) >> 2 : src[x] << 2;
    }
}

#define DMVR8(name, horizontal, vertical)                                       \
void ff_vvc_dmvr ## name ## _8_simd128(int16_t *dst, const uint8_t *src,          \
    ptrdiff_t stride, int height, intptr_t mx, intptr_t my, int width)           \
{                                                                              \
    dmvr_8(dst, src, stride, height, mx, my, width, horizontal, vertical);       \
}

DMVR8(, 0, 0)
DMVR8(_h, 1, 0)
DMVR8(_v, 0, 1)
DMVR8(_hv, 1, 1)
