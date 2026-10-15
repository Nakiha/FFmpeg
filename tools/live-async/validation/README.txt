Validated asynchronous early-output implementation
==================================================

Base decoder: official n7.0.3, eaddd1d7140bab19e5a4403d3c0f61fe5f59cb75.
Fork ABI: libavcodec 61.4.100, AVCODEC_ASYNC_API_VERSION 1.
Hardware: Ryzen 7 9700X, 8 cores / 16 logical processors.
Windows: MSYS2 UCRT64 GCC 14.2, native w32threads backend.
Linux: Ubuntu 24.04/WSL2, GCC 13.3, pthreads backend.

linux.csv / windows.csv compare the SAME probe in paced legacy and async modes:
50 synthetic 960x540 YUV420P frames, 25 fps, no B pictures, after demux/open.
Columns legacy_first_frame_ms and first_frame_ms are control/new first output;
the corresponding means include the EOF-drain tail. Max call times describe
these short runs, not hard latency bounds. Bulk callback peaks come from a
separate unpaced run and include reference waits.

12-thread Linux control -> async (milliseconds):
    H.264 first output: 440.797 -> 0.963; mean packet latency: 388.756 -> 0.826.
    HEVC  first output: 440.507 -> 4.645; mean packet latency: 388.816 -> 1.392.
12-thread Windows control -> async:
    H.264 first output: 442.263 -> 0.780.
    HEVC  first output: 444.645 -> 1.744.

These are single-run observations on this machine, not production guarantees.
The frozen unmodified implementation's measurements remain in ../baseline/.
Legacy output and new output also match those original saved pixel hashes/PTS.

linux.log / windows.log / asan.log capture successful complete test runs:
* H.264 and HEVC with B=0/B=3, 2/12 frame threads, exact frame hashes, PTS,
  best-effort timestamps, packet DTS, duration, dimensions, pixel format, SAR,
  range/primaries/transfer/colorspace.
* Actual decoder-callback overlap (Linux bulk peaks 11/12, Windows 12/12);
  occupancy alone is not used as proof of multiple active tasks.
* No future input required after the first eligible frame; a worker deliberately
  held behind a closed get_buffer2 gate cannot block frontend data calls.
* Capacity-one retry, three drain/flush/replay cycles, flush with active work,
  five active-work close repetitions per codec, corrupt-packet recovery,
  non-refcounted packet rejection, invalid capacity and already-draining owner
  rejection, empty drain, input-after-close rejection.
* 10-bit decoding, midstream 640x360 -> 960x540 changes, delayed independent
  intra pictures retaining ordered output.

The two invalid-NAL messages in each log are intentionally injected errors;
they are followed by a successful flush and full correct decode.

asan.csv is diagnostic only. AddressSanitizer plus leak detection passed all
cases. That build uses the C implementation (--disable-asm) because FFmpeg 7.0.3
misdetects GCC ASan's ODR-indicator symbol as the external assembly prefix.
No sanitizer timing is used to quantify the optimized implementation's speed.

ThreadSanitizer is NOT validated: its configure test containing only an empty
main function segfaulted in this WSL environment. ASan is not a race detector.

Not established: production multi-stream capacity, large-stream scaling,
interlaced corpus correctness, actual network/filter/encode/mux latency or a
hard real-time upper bound. Validate those on the deployment OS and real streams.

Full media, per-frame CSVs and hashes are in the offline validation bundle.
Reproduce with tools/live-async/test-async.sh; see the Chinese migration guide.
