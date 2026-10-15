Original-decoder measurements
=============================

Decoder baseline: n7.0.3, eaddd1d7140bab19e5a4403d3c0f61fe5f59cb75.
Tested build version: n7.0.3-1-g0af556a (only the investigation document was
committed after upstream at build time; libavcodec implementation is unchanged).
Hardware: AMD Ryzen 7 9700X, 8 cores / 16 logical processors.

windows.csv: Windows 11, MSYS2 UCRT64 GCC 14.2, native w32threads backend.
linux.csv: Ubuntu 24.04 in WSL2, GCC 13.3, pthreads backend.
Both use optimized code, debug symbols and assertion level 2.

Inputs: synthetic testsrc2, 960x540 YUV420P, 25 fps, 50 frames per file.
The external fixture generator was FFmpeg 9.0.2 with libx264/libx265. Decoding
and timing used the compiled 7.0.3 libraries exclusively. The exact same inputs
were used on both platforms. Fixture hashes are in fixtures.sha256.

The latency summaries use B=0 streams, after demuxer/decoder initialization.
The probe follows send_packet(), then receive_frame() until EAGAIN. It records
first-frame output time, submit-to-output time via COPY_OPAQUE stamps, and the
maximum observed duration of individual send and receive calls. Means include
the EOF drain tail; short-sample maxima are not worst-case latency bounds.

All 8 paced runs on each platform returned 50 stamped frames in input order.
Additional B=0 and B=3 framehash comparisons at 2 and 12 frame threads matched
in pixel hashes and PTS, including Windows-versus-Linux comparisons. Each B=3
sample contains 36 B pictures. These are baseline checks, not validation of an
asynchronous early-output patch.

Full generated fixtures, per-frame CSVs, frame hashes and compiler logs remain
in the checkout's sibling output directories. Run the scripts to regenerate
them. Timing values vary with scheduling and machine load and are not CI gates.
