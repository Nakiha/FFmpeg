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

#if BIT_DEPTH == 8
#define SAO_PIXEL uint8_t
#define SAO_LOAD(p) wasm_u16x8_load8x8(p)
#define SAO_STORE(p, v) wasm_v128_store64_lane(p, wasm_u8x16_narrow_i16x8(v, v), 0)
#else
#define SAO_PIXEL uint16_t
#define SAO_LOAD(p) wasm_v128_load(p)
#define SAO_STORE(p, v) wasm_v128_store(p, wasm_i16x8_min(wasm_i16x8_max(v, \
    wasm_i16x8_const_splat(0)), wasm_i16x8_const_splat(1023)))
#endif
#define SAO_MAX ((1 << BIT_DEPTH) - 1)
#define SAO_SHIFT (BIT_DEPTH - 5)

void SAO_NAME(band, BIT_DEPTH)(uint8_t *dst_bytes, const uint8_t *src_bytes,
    ptrdiff_t dst_stride, ptrdiff_t src_stride, const int16_t *offset, int left, int width, int height)
{
    const SAO_PIXEL *src = (const SAO_PIXEL *)src_bytes;
    SAO_PIXEL *dst = (SAO_PIXEL *)dst_bytes;
    // In VVC the transmitted offset magnitude is at most 31 at these depths.
    const v128_t table = wasm_i8x16_make(offset[1], offset[2], offset[3], offset[4],
                                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    const v128_t start = wasm_i16x8_splat(left);
    const v128_t mask = wasm_i16x8_const_splat(31);

    src_stride /= sizeof(*src);
    dst_stride /= sizeof(*dst);
    for (int y = 0; y < height; y++) {
        int x = 0;
        for (; x + 8 <= width; x += 8) {
            const v128_t samples = SAO_LOAD(src + x);
            const v128_t bands = wasm_u16x8_shr(samples, SAO_SHIFT);
            const v128_t index = wasm_v128_and(wasm_i16x8_sub(bands, start), mask);
            const v128_t packed = wasm_u8x16_narrow_i16x8(index, index);
            const v128_t delta = wasm_i16x8_extend_low_i8x16(wasm_i8x16_swizzle(table, packed));
            SAO_STORE(dst + x, wasm_i16x8_add(samples, delta));
        }
        for (; x < width; x++) {
            const int index = ((src[x] >> SAO_SHIFT) - left) & 31;
            dst[x] = av_clip(src[x] + (index < 4 ? offset[index + 1] : 0), 0, SAO_MAX);
        }
        src += src_stride;
        dst += dst_stride;
    }
}

void SAO_NAME(edge, BIT_DEPTH)(uint8_t *dst_bytes, const uint8_t *src_bytes,
    ptrdiff_t dst_stride, const int16_t *offset, int eo, int width, int height)
{
    static const uint8_t edge_index[] = { 1, 2, 0, 3, 4 };
    const SAO_PIXEL *src = (const SAO_PIXEL *)src_bytes;
    SAO_PIXEL *dst = (SAO_PIXEL *)dst_bytes;
    const ptrdiff_t src_stride = (2 * MAX_PB_SIZE + AV_INPUT_BUFFER_PADDING_SIZE) / sizeof(*src);
    const ptrdiff_t step[] = { 1, src_stride, src_stride + 1, src_stride - 1 };
    const ptrdiff_t distance = step[eo];
    const v128_t table = wasm_i8x16_make(offset[1], offset[2], offset[0], offset[3], offset[4],
                                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    const v128_t center = wasm_i16x8_const_splat(2);

    dst_stride /= sizeof(*dst);
    for (int y = 0; y < height; y++) {
        int x = 0;
        for (; x + 8 <= width; x += 8) {
            const v128_t samples = SAO_LOAD(src + x);
            const v128_t a = SAO_LOAD(src + x - distance);
            const v128_t b = SAO_LOAD(src + x + distance);
            const v128_t da = wasm_i16x8_sub(wasm_i16x8_lt(samples, a), wasm_i16x8_gt(samples, a));
            const v128_t db = wasm_i16x8_sub(wasm_i16x8_lt(samples, b), wasm_i16x8_gt(samples, b));
            const v128_t index = wasm_i16x8_add(center, wasm_i16x8_add(da, db));
            const v128_t packed = wasm_u8x16_narrow_i16x8(index, index);
            const v128_t delta = wasm_i16x8_extend_low_i8x16(wasm_i8x16_swizzle(table, packed));
            SAO_STORE(dst + x, wasm_i16x8_add(samples, delta));
        }
        for (; x < width; x++) {
            const int sample = src[x], a = src[x - distance], b = src[x + distance];
            const int index = 2 + (sample > a) - (sample < a) + (sample > b) - (sample < b);
            dst[x] = av_clip(sample + offset[edge_index[index]], 0, SAO_MAX);
        }
        src += src_stride;
        dst += dst_stride;
    }
}

#undef SAO_PIXEL
#undef SAO_LOAD
#undef SAO_STORE
#undef SAO_MAX
#undef SAO_SHIFT
