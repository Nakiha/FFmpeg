/*
 * VVC ALF filters for WebAssembly SIMD128, 8- and 10-bit
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

#define ALF_CAT(name, depth) name ## _ ## depth
#define ALF_NAME(name, depth) ALF_CAT(name, depth)
#define ALF_SUFFIX(name) name ## _simd128
#define ALF_SUFFIX_EXPAND(name) ALF_SUFFIX(name)
#define FUNC(name) ALF_NAME(name, BIT_DEPTH)
#define ALF_EXPORT(name) ALF_SUFFIX_EXPAND(FUNC(ff_vvc_ ## name))

#define BIT_DEPTH 8
#include "alf_template.c"
#undef BIT_DEPTH

#define BIT_DEPTH 10
#include "alf_template.c"
#undef BIT_DEPTH
