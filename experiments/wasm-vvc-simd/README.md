# WebAssembly SIMD128 ports of the VVC DSP

Isolated bit-exactness harnesses for the wasm SIMD128 kernels under
`libavcodec/wasm/vvc/`.  Both tests include the real C reference by
compiling `libavcodec/vvc/dsp.c` into the harness, then compare the SIMD
entry points against the C template functions over randomized inputs.

    emcc -O2 -msimd128 -sSTACK_SIZE=8388608 \
        -I<ffmpeg> -I<ffmpeg>/libavcodec/vvc -I<ffmpeg-wasm-build> \
        test-alf.c <ffmpeg>/libavcodec/wasm/vvc/alf.c \
        <ffmpeg>/libavcodec/wasm/vvc/dsp_init.c \
        <ffmpeg>/libavcodec/wasm/vvc/inter.c \
        <ffmpeg-wasm-build>/work/install/lib/libavutil.a -o test.js
    node test.js

`test-alf.c` sweeps luma/chroma ALF (widths 4..64, heights 4..32, 15
virtual-boundary positions, random coefficients) and the ALF
classification (same sizes, 10 virtual-boundary positions, comparing the
gradient buffer and the class/transpose outputs entry by entry).
`test-inter.c` sweeps put_pixels, avg, sad and the luma/chroma h/v/hv
quarter-pel filters (real filter phases, full-range 16-bit inputs for
avg/sad).

Decoder-level validation lives outside the tree: decode the pinned 1080p
VVC fixture with the pinned core and with the ported build and compare a
per-frame hash of `vp_pixels()` output.

## Measured notes

- The window sum of the classification (four directions per i32x4 lane)
  gives about 1.0% end to end on the pinned fixture (interleaved A/B).
- Hand-vectorizing the classification *gradient* pass measured 3.1% slower
  than the compiler's scalar code (two order-swapped interleaved rounds,
  round-to-round spread 1.9%..4.3%, within-round spread below 0.2% in the
  tight round), so it was reverted and the scalar gradient is kept.  An
  instruction-count audit of the compiled objects is not sufficient on
  its own: the classic 2x2 gradient has few cross-lane dependencies when
  left scalar, while the vector version pays for 12 widened loads and a
  4x4 lane transpose per eight columns.
