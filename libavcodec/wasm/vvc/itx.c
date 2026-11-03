/*
 * VVC inverse DCT-VIII and DST-VII for WebAssembly SIMD128
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

#include <wasm_simd128.h>

#include "libavutil/attributes.h"
#include "libavcodec/vvc/data.h"
#include "itx.h"

static av_always_inline void inverse_mts(int *coeffs, ptrdiff_t stride, size_t nz,
                                          const int8_t *matrix, int size)
{
    v128_t sums[8];

    // Each vector accumulates four output coefficients. Keep full precision
    // until the caller applies the existing transform rounding and clipping.
    for (int x = 0; x < size / 4; x++)
        sums[x] = wasm_i32x4_const_splat(0);

    for (int y = 0; y < (int)nz; y++, matrix += size) {
        const v128_t input = wasm_i32x4_splat(coeffs[y * stride]);
        for (int x = 0; x < size / 4; x++) {
            const v128_t row = wasm_v128_load32_zero(matrix + x * 4);
            const v128_t weights = wasm_i32x4_extend_low_i16x8(wasm_i16x8_extend_low_i8x16(row));
            sums[x] = wasm_i32x4_add(sums[x], wasm_i32x4_mul(input, weights));
        }
    }

    for (int x = 0; x < size / 4; x++) {
        int *dst = coeffs + 4 * x * stride;
        const v128_t output = sums[x];
        if (stride == 1) {
            wasm_v128_store(dst, output);
        } else {
            dst[0 * stride] = wasm_i32x4_extract_lane(output, 0);
            dst[1 * stride] = wasm_i32x4_extract_lane(output, 1);
            dst[2 * stride] = wasm_i32x4_extract_lane(output, 2);
            dst[3 * stride] = wasm_i32x4_extract_lane(output, 3);
        }
    }
}

#define VVC_MTS(type, size)                                                     \
void ff_vvc_inv_ ## type ## _ ## size ## _simd128(int *coeffs, ptrdiff_t stride,  \
                                                 size_t nz)                    \
{                                                                              \
    inverse_mts(coeffs, stride, nz, &ff_vvc_ ## type ## _ ## size ## x ## size[0][0], size); \
}

VVC_MTS(dct8, 4)
VVC_MTS(dct8, 8)
VVC_MTS(dct8, 16)
VVC_MTS(dct8, 32)
VVC_MTS(dst7, 4)
VVC_MTS(dst7, 8)
VVC_MTS(dst7, 16)
VVC_MTS(dst7, 32)
