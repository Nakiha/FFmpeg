/*
 * VVC ALF filters for WebAssembly SIMD128, 8- and 10-bit
 *
 * Mirrors libavcodec/vvc/filter_template.c (8- and 10-bit instantiations) and the
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

#define ALF_BLOCK_SIZE        4
#define ALF_NUM_COEFF_LUMA    12
#define ALF_NUM_COEFF_CHROMA   6

#if BIT_DEPTH == 8
typedef uint8_t FUNC(pixel);
#else
typedef uint16_t FUNC(pixel);
#endif

static av_always_inline v128_t FUNC(alf_load8)(const FUNC(pixel) *p)
{
#if BIT_DEPTH == 8
    return wasm_u16x8_load8x8(p);
#else
    return wasm_v128_load(p);
#endif
}

/* av_clip(v0 - curr, -clip, clip) + av_clip(v1 - curr, -clip, clip) */
static av_always_inline v128_t FUNC(alf_clip_pair16)(v128_t v0, v128_t v1, v128_t curr, v128_t clipv)
{
    const v128_t negclip = wasm_i16x8_sub(wasm_i16x8_splat(0), clipv);
    v128_t d0 = wasm_i16x8_sub(v0, curr);
    v128_t d1 = wasm_i16x8_sub(v1, curr);

    d0 = wasm_i16x8_min(wasm_i16x8_max(d0, negclip), clipv);
    d1 = wasm_i16x8_min(wasm_i16x8_max(d1, negclip), clipv);
    return wasm_i16x8_add(d0, d1);
}

static av_always_inline void FUNC(alf_store_row)(FUNC(pixel) *dst, v128_t v16, int npix)
{
#if BIT_DEPTH == 8
    const v128_t v8 = wasm_u8x16_narrow_i16x8(v16, v16);
    if (npix == 8)
        wasm_v128_store64_lane(dst, v8, 0);
    else
        wasm_v128_store32_lane(dst, v8, 0);
#else
    const v128_t v = wasm_i16x8_min(wasm_i16x8_max(v16, wasm_i16x8_splat(0)),
                                     wasm_i16x8_splat(1023));
    if (npix == 8)
        wasm_v128_store(dst, v);
    else
        wasm_v128_store64_lane(dst, v, 0);
#endif
}

/*
 * One row of an eight-pixel span (two adjacent ALF blocks).  `fv`/`cv` hold the
 * per-tap coefficient/clip vectors already interleaved for the pair; rows
 * p[0..6] are expected to carry the virtual-boundary substitutions.
 */
static av_always_inline void FUNC(alf_luma_row8)(FUNC(pixel) *dst, const FUNC(pixel) *const p[7],
    const v128_t *fv, const v128_t *cv,
    const int is_near_vb, const int npix, const int clipped)
{
    const v128_t curr = FUNC(alf_load8)(p[0]);
    v128_t acc_lo = wasm_i32x4_splat(0);
    v128_t acc_hi = wasm_i32x4_splat(0);

#define ALF_TAP(k, a, ax, b, bx)                                                     \
    do {                                                                             \
        const v128_t a0 = FUNC(alf_load8)(p[a] + (ax));                         \
        const v128_t a1 = FUNC(alf_load8)(p[b] + (bx));                         \
        const v128_t t = clipped ? FUNC(alf_clip_pair16)(a0, a1, curr, cv[k]) : \
            wasm_i16x8_sub(wasm_i16x8_add(a0, a1), wasm_i16x8_add(curr, curr)); \
        const v128_t f = fv[k];                                        \
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
        FUNC(alf_store_row)(dst, v16, npix);
    }
}

static av_always_inline void FUNC(alf_chroma_row8)(FUNC(pixel) *dst, const FUNC(pixel) *const p[5],
    const v128_t *fv, const v128_t *cv,
    const int is_near_vb, const int npix)
{
    const v128_t curr = FUNC(alf_load8)(p[0]);
    v128_t acc_lo = wasm_i32x4_splat(0);
    v128_t acc_hi = wasm_i32x4_splat(0);

#define ALF_TAP(k, a, ax, b, bx)                                                     \
    do {                                                                             \
        const v128_t t = FUNC(alf_clip_pair16)(FUNC(alf_load8)(p[a] + (ax)),                       \
                                       FUNC(alf_load8)(p[b] + (bx)), curr,                 \
                                       cv[k]);                       \
        const v128_t f = fv[k];                                        \
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
        FUNC(alf_store_row)(dst, v16, npix);
    }
}

void ALF_EXPORT(alf_filter_luma)(uint8_t *_dst, ptrdiff_t dst_stride,
    const uint8_t *_src, ptrdiff_t src_stride, const int width, const int height,
    const int16_t *filter, const int16_t *clip, const int vb_pos)
{
    const FUNC(pixel) *src = (const FUNC(pixel) *)_src;
    FUNC(pixel) *dst       = (FUNC(pixel) *)_dst;
    src_stride /= sizeof(*src);
    dst_stride /= sizeof(*dst);

    for (int y = 0; y < height; y += ALF_BLOCK_SIZE) {
        const int far = (y + ALF_BLOCK_SIZE - 1 < vb_pos - 3) || (y > vb_pos + 2);

        for (int x = 0; x < width; x += 2 * ALF_BLOCK_SIZE) {
            const FUNC(pixel) *s0 = src + y * src_stride + x;
            const FUNC(pixel) *s1 = s0 + src_stride;
            const FUNC(pixel) *s2 = s0 - src_stride;
            const FUNC(pixel) *s3 = s1 + src_stride;
            const FUNC(pixel) *s4 = s2 - src_stride;
            const FUNC(pixel) *s5 = s3 + src_stride;
            const FUNC(pixel) *s6 = s4 - src_stride;
            const int npix = FFMIN(width - x, 2 * ALF_BLOCK_SIZE);
            v128_t fv[ALF_NUM_COEFF_LUMA];
            v128_t cv[ALF_NUM_COEFF_LUMA];
            const int second = npix == 2 * ALF_BLOCK_SIZE ? ALF_NUM_COEFF_LUMA : 0;
            int clipped = 0;

            for (int k = 0; k < ALF_NUM_COEFF_LUMA; k++) {
                const v128_t f0 = wasm_i16x8_splat(filter[k]);
                const v128_t f1 = wasm_i16x8_splat(filter[second + k]);
                const v128_t c0 = wasm_i16x8_splat(clip[k]);
                const v128_t c1 = wasm_i16x8_splat(clip[second + k]);
                fv[k] = wasm_i32x4_shuffle(f0, f1, 0, 1, 4, 5);
                cv[k] = wasm_i32x4_shuffle(c0, c1, 0, 1, 4, 5);
                clipped |= (clip[k] < (1 << BIT_DEPTH)) | (clip[second + k] < (1 << BIT_DEPTH));
            }

            for (int i = 0; i < ALF_BLOCK_SIZE; i++) {
                FUNC(pixel) *drow = dst + (y + i) * dst_stride + x;
                const FUNC(pixel) *p[7];
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
                // A full-range clip never changes a difference between two samples.
                if (clipped)
                    FUNC(alf_luma_row8)(drow, p, fv, cv, is_near_vb, npix, 1);
                else
                    FUNC(alf_luma_row8)(drow, p, fv, cv, is_near_vb, npix, 0);
            }
            filter += (npix / ALF_BLOCK_SIZE) * ALF_NUM_COEFF_LUMA;
            clip   += (npix / ALF_BLOCK_SIZE) * ALF_NUM_COEFF_LUMA;
        }
    }
}

void ALF_EXPORT(alf_filter_chroma)(uint8_t *_dst, ptrdiff_t dst_stride,
    const uint8_t *_src, ptrdiff_t src_stride, const int width, const int height,
    const int16_t *filter, const int16_t *clip, const int vb_pos)
{
    const FUNC(pixel) *src = (const FUNC(pixel) *)_src;
    FUNC(pixel) *dst       = (FUNC(pixel) *)_dst;
    src_stride /= sizeof(*src);
    dst_stride /= sizeof(*dst);

    v128_t fv[ALF_NUM_COEFF_CHROMA];
    v128_t cv[ALF_NUM_COEFF_CHROMA];

    // Chroma ALF uses one filter set for the whole subblock.
    for (int k = 0; k < ALF_NUM_COEFF_CHROMA; k++) {
        fv[k] = wasm_i16x8_splat(filter[k]);
        cv[k] = wasm_i16x8_splat(clip[k]);
    }

    for (int y = 0; y < height; y += ALF_BLOCK_SIZE) {
        const int far = (y + ALF_BLOCK_SIZE - 1 < vb_pos - 2) || (y > vb_pos + 1);

        for (int x = 0; x < width; x += 2 * ALF_BLOCK_SIZE) {
            const FUNC(pixel) *s0 = src + y * src_stride + x;
            const FUNC(pixel) *s1 = s0 + src_stride;
            const FUNC(pixel) *s2 = s0 - src_stride;
            const FUNC(pixel) *s3 = s1 + src_stride;
            const FUNC(pixel) *s4 = s2 - src_stride;
            const int npix = FFMIN(width - x, 2 * ALF_BLOCK_SIZE);

            for (int i = 0; i < ALF_BLOCK_SIZE; i++) {
                FUNC(pixel) *drow = dst + (y + i) * dst_stride + x;
                const FUNC(pixel) *p[5];
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
                FUNC(alf_chroma_row8)(drow, p, fv, cv, is_near_vb, npix);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* ALF classification: scalar gradient pass (as C) plus a SIMD sum      */
/* over the gradient window.                                            */

static av_always_inline void FUNC(alf_get_idx_8)(int *class_idx, int *transpose_idx,
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
    *class_idx = arg_var[av_clip_uintp2(sum_hv * ac >> (BIT_DEPTH - 1), 4)];
    if (hvd1 * 2 > 9 * hvd0)
        *class_idx += ((dir1 << 1) + 2) * 5;
    else if (hvd1 > 2 * hvd0)
        *class_idx += ((dir1 << 1) + 1) * 5;
    *transpose_idx = dir_d * 2 + dir_hv;
}

#if BIT_DEPTH == 8
/* Combine the even samples of the first row and odd samples of the second. */
static av_always_inline v128_t FUNC(alf_alternate8)(const uint8_t *even, const uint8_t *odd)
{
    return wasm_i16x8_shuffle(wasm_u16x8_load8x8(even), wasm_u16x8_load8x8(odd),
        0, 9, 2, 11, 4, 13, 6, 15);
}

static av_always_inline v128_t FUNC(alf_gradient8)(v128_t center, v128_t a, v128_t b)
{
    const v128_t gradient = wasm_i16x8_abs(wasm_i16x8_sub(center, wasm_i16x8_add(a, b)));
    return wasm_i32x4_dot_i16x8(gradient, wasm_i16x8_splat(1));
}

static av_always_inline void FUNC(alf_gradient_groups8)(int *out,
    const uint8_t *s0, const uint8_t *s1, const uint8_t *s2, const uint8_t *s3)
{
    const v128_t p = FUNC(alf_alternate8)(s1, s2);
    const v128_t center = wasm_i16x8_add(p, p);
    const v128_t v = FUNC(alf_gradient8)(center, FUNC(alf_alternate8)(s0, s1),
        FUNC(alf_alternate8)(s2, s3));
    const v128_t h = FUNC(alf_gradient8)(center, FUNC(alf_alternate8)(s1 - 1, s2 - 1),
        FUNC(alf_alternate8)(s1 + 1, s2 + 1));
    const v128_t d0 = FUNC(alf_gradient8)(center, FUNC(alf_alternate8)(s0 - 1, s1 - 1),
        FUNC(alf_alternate8)(s2 + 1, s3 + 1));
    const v128_t d1 = FUNC(alf_gradient8)(center, FUNC(alf_alternate8)(s0 + 1, s1 + 1),
        FUNC(alf_alternate8)(s2 - 1, s3 - 1));

    const v128_t vh0 = wasm_i32x4_shuffle(v, h, 0, 4, 1, 5);
    const v128_t vh1 = wasm_i32x4_shuffle(v, h, 2, 6, 3, 7);
    const v128_t d01 = wasm_i32x4_shuffle(d0, d1, 0, 4, 1, 5);
    const v128_t d23 = wasm_i32x4_shuffle(d0, d1, 2, 6, 3, 7);

    wasm_v128_store(out,      wasm_i32x4_shuffle(vh0, d01, 0, 1, 4, 5));
    wasm_v128_store(out + 4,  wasm_i32x4_shuffle(vh0, d01, 2, 3, 6, 7));
    wasm_v128_store(out + 8,  wasm_i32x4_shuffle(vh1, d23, 0, 1, 4, 5));
    wasm_v128_store(out + 12, wasm_i32x4_shuffle(vh1, d23, 2, 3, 6, 7));
}
#endif

#if BIT_DEPTH == 8
static av_always_inline v128_t FUNC(alf_sum_column_pair)(const int *g, int stride, int rows)
{
    v128_t sum = wasm_i32x4_splat(0);
    for (int i = 0; i < rows; i++, g += stride) {
        sum = wasm_i32x4_add(sum, wasm_v128_load(g));
        sum = wasm_i32x4_add(sum, wasm_v128_load(g + 4));
    }
    return sum;
}
#endif

void ALF_EXPORT(alf_classify)(int *class_idx, int *transpose_idx,
    const uint8_t *_src, const ptrdiff_t _src_stride, const int width, const int height,
    const int vb_pos, int *gradient_tmp)
{
    const int h       = height + 4;                     /* ALF_GRADIENT_BORDER * 2 */
    const int w       = width + 4;
    const int size    = (ALF_BLOCK_SIZE + 4) / 2;       /* 4 */
    const int gstride = (w / 2) * 4;
    const FUNC(pixel) *src  = (const FUNC(pixel) *)_src;
    const ptrdiff_t src_stride = _src_stride / sizeof(FUNC(pixel));

    src -= 3 * src_stride + 2;                          /* (BORDER + 1) * stride + BORDER */

    /* Gradient pass, with each group storing the four directions together. */
    {
        int *grad = gradient_tmp;
        for (int y = 0; y < h; y += 2) {
            const FUNC(pixel) *s0 = src + y * src_stride;
            const FUNC(pixel) *s1 = s0 + src_stride;
            const FUNC(pixel) *s2 = s1 + src_stride;
            const FUNC(pixel) *s3 = s2 + src_stride;

            if (y == vb_pos)
                s3 = s2;
            else if (y == vb_pos + 2)
                s0 = s1;

            int x = 0;
#if BIT_DEPTH == 8
            for (; x + 8 <= w; x += 8) {
                FUNC(alf_gradient_groups8)(grad, s0 + x, s1 + x, s2 + x, s3 + x);
                grad += 16;
            }
#endif
            for (; x < w; x += 2) {
                const FUNC(pixel) *a0 = s0 + x;
                const FUNC(pixel) *p0 = s1 + x;
                const FUNC(pixel) *b0 = s2 + x;
                const int val0  = (*p0) << 1;
                const FUNC(pixel) *a1 = s1 + x + 1;
                const FUNC(pixel) *p1 = s2 + x + 1;
                const FUNC(pixel) *b1 = s3 + x + 1;
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
#if BIT_DEPTH == 8
        const int rows = end - start;
        const int *columns = gradient_tmp + (y / 2 + start) * gstride;
        v128_t previous = FUNC(alf_sum_column_pair)(columns, gstride, rows);
#endif
        for (int x = 0; x < width; x += ALF_BLOCK_SIZE) {
            int sum[4];
#if BIT_DEPTH == 8
            // Consecutive blocks share the middle two columns.
            columns += 8;
            const v128_t next = FUNC(alf_sum_column_pair)(columns, gstride, rows);
            const v128_t acc = wasm_i32x4_add(previous, next);
            previous = next;
#else
            const int xg = x / 2;
            const int yg = y / 2;
            const int *g = gradient_tmp + (yg + start) * gstride + xg * 4;
            v128_t acc = wasm_i32x4_splat(0);

            for (int i = start; i < end; i++) {
                for (int j = 0; j < size; j++) {
                    acc = wasm_i32x4_add(acc, wasm_v128_load(g));
                    g += 4;
                }
                g += gstride - size * 4;
            }
#endif
            sum[0] = wasm_i32x4_extract_lane(acc, 0);
            sum[1] = wasm_i32x4_extract_lane(acc, 1);
            sum[2] = wasm_i32x4_extract_lane(acc, 2);
            sum[3] = wasm_i32x4_extract_lane(acc, 3);
            FUNC(alf_get_idx_8)(class_idx, transpose_idx, sum, ac);
            class_idx++;
            transpose_idx++;
        }
    }
}

#undef ALF_BLOCK_SIZE
#undef ALF_NUM_COEFF_LUMA
#undef ALF_NUM_COEFF_CHROMA
