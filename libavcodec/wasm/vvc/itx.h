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

#ifndef AVCODEC_WASM_VVC_ITX_H
#define AVCODEC_WASM_VVC_ITX_H

#include <stddef.h>

#define VVC_MTS_DECL(type, size) \
void ff_vvc_inv_ ## type ## _ ## size ## _simd128(int *coeffs, ptrdiff_t stride, size_t nz)

VVC_MTS_DECL(dct8, 4);
VVC_MTS_DECL(dct8, 8);
VVC_MTS_DECL(dct8, 16);
VVC_MTS_DECL(dct8, 32);
VVC_MTS_DECL(dst7, 4);
VVC_MTS_DECL(dst7, 8);
VVC_MTS_DECL(dst7, 16);
VVC_MTS_DECL(dst7, 32);

#undef VVC_MTS_DECL
#endif
