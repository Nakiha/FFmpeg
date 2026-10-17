# Native analysis execution results (2026-10-18)

First measurement of the new VVC no-pixels pruning (and the existing HEVC one)
on the native analysis driver: same `vpa_*` code path as the WASM core, same
fixtures, `mode 0` (full) vs `mode 1` (no-pixels), Apple M5, single thread.

| fixture | decoder | mode 0 (full) | mode 1 (no-pixels) | speedup | records |
| --- | --- | ---: | ---: | ---: | :---: |
| `h264_9s_1920x1080.mp4` (600 pictures) | H.264 | 1645.9 ms | 1424.2 ms | — (no separate path) | equal |
| `mhw_hevc_fullrange_bt709_3s.mp4` (180 pictures) | HEVC | 3163.1 ms | 1761.5 ms | **1.80x** | equal |
| `h266_10s_1920x1080.mp4` (600 pictures) | VVC | 7546.6 ms | 2579.9 ms | **2.93x** | equal |

- `path=1` in mode 1 confirms VVC/HEVC no-pixels execution was selected; H.264
  keeps its hook-level reconstruction skip in both modes (`path=0`).
- Record dumps (per-picture header + blocks + references) are byte-identical
  between modes for all three codecs, which is the acceptance contract used by
  `scripts/test-analysis-execution.mjs`.
- H.264 shows no mode difference because its hook already omits macroblock
  pixel reconstruction in analysis builds (`skip_loop_filter=AVDISCARD_ALL`).

Retry semantics (in-band parameter set after pruning started, film grain SEI,
decoded-picture-hash SEI, malformed parameter set) are covered by the JS
acceptance harness extension, not by this driver.

## Repro

```sh
ANALYSIS_STREAM_DIR=<FFmpeg-Build worktree with analysis/stream.c> \
    experiments/native-analysis/build-native-analysis.sh
experiments/native-analysis/run-native-analysis.sh
```

The driver needs the local `intptr_t` portability fix for `vpa_blocks()`/
`vpa_refs()` (wasm32 `int` truncates pointers on 64-bit hosts).
