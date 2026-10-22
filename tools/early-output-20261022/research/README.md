> **实验版，最终验收未完成，不建议生产启用**
> 历史文档归档：正文中的“当前”、未推送状态及验收结论仅属于原阶段；以 `doc/early-output-20261022/REPORT.md` 的版本核对为准。`out/` 路径指本机保留证据，不随公开分支发布。

# Local H264 early-output investigation

Phase 1's POC2 candidate remains rejected. REPORT.md and REVIEW-ADDENDUM.md preserve
its decision and failures. Phase 2 continues in phase2/REPORT.md: independent switch,
offline verified mode, and an in-band HRD/CFR research prototype with bounded tests.
Read phase2/INDEPENDENT_REVIEW.md and ../out/phase2/acceptance.json for release limits;
passing ordinary-stream tests is not an unrestricted malformed-input guarantee.
This folder and ../out are private local research, not a release or default SDK.

Inputs / reproducibility:

- generate_pcm.py: deterministic minimal I_PCM Annex B fixtures, POC0/1/2, MMCO5,
  wrap and malformed counterexample. ../out/samples/pcm-manifest.json has hashes.
- download_fate.py: eight selectively chosen official FATE samples, size bounded;
  ../out/evidence/fate-download.json records exact URLs, sizes and SHA256.
- generate_media.py and prepare_boundaries.py: fixed x264/x265 streams and mutations.
- adapt_probe.py: derive portable_probe.c from the exact pin's existing async_probe.c.
  Changes: macOS nonblocking pipe notification, 1-thread synchronous test support,
  optional synthetic PTS and actual demux seek, exact source-id/error/field flags,
  preserve terminal_return. Number of packets is not assumed to equal output frames.
- run_matrix.py, run_boundaries.py, run_final_checks.py: ordered per-frame comparisons,
  exact statuses, multi-thread/capacity1/lifecycle/native PTS/seek/HEVC checks.
- audit_sources.py: independent installed Homebrew 8.1.2 pixel oracle / header trace.
- make_perf_probe.py, run_perf_v2.py: no-hash paced and throughput interleaved repeats.
  Raw thread CPU brackets send/receive; only synchronous thread1 encompasses all
  decode work. Process CPU includes driver/demux/open, not a pure decoder metric.

Baseline build: ../out/install/baseline at exact FFmpeg 94bd940f.
Experimental rejected build: ../out/install/candidate, patch h264-poc2-experimental.patch.
Neither build is installed globally. Phase 1 left the application clone unmodified;
phase 2 now holds local independent-switch changes, without changing the SDK pin.
The rejected patch is retained for reproduction and review, not deployment.

The full commands and per-run environment are in matrix/commands.json and the
summary/results JSON of each later stage. Build configure invocation is captured
by ffmpeg -version and configure logs, source and binaries by evidence/build-identity.json.

All media remain local. No push, merge, SDK replacement or public upload performed.
