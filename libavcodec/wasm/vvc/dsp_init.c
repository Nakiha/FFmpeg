/*
 * WebAssembly SIMD128 DSP init for the VVC decoder
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

#include "libavutil/attributes.h"
#include "libavutil/cpu_internal.h"

#include "libavcodec/vvc/dsp.h"
#include "libavcodec/wasm/vvc/alf.h"

av_cold void ff_vvc_dsp_init_wasm(VVCDSPContext *c, const int bit_depth)
{
    int cpu_flags = av_get_cpu_flags();

    if (!CPUEXT(cpu_flags, SIMD128))
        return;

#if HAVE_SIMD128
    if (bit_depth == 8) {
        c->alf.filter[0] = ff_vvc_alf_filter_luma_8_simd128;
        c->alf.filter[1] = ff_vvc_alf_filter_chroma_8_simd128;
    }
#endif
}
