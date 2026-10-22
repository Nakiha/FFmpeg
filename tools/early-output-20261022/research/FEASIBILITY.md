> **实验版，最终验收未完成，不建议生产启用**
> 历史文档归档：正文中的“当前”、未推送状态及验收结论仅属于原阶段；以 `doc/early-output-20261022/REPORT.md` 的版本核对为准。`out/` 路径指本机保留证据，不随公开分支发布。

# Feasibility checkpoint before implementation

Baseline identity and build are recorded in CHECKPOINT.md. Source pin is 94bd940f.

## Finding

The HEVC integer-prefix rule does not prove H.264 safety. In H264, POC0 permits
wrap-derived lower POC; POC1 adds signed offsets; each frame has two field POCs;
MMCO5 changes the origin. IDR is a reference refresh, not a promise of no leading
pictures. Constructed 16x16 I_PCM fixture pcm-poc0-leading-negative.h264 decodes
without errors on the exact pinned build, 5/5 frames. The frame after IDR in decode
order (luma45, POC -2 relative to IDR) is output BEFORE IDR (luma32). Ordered SHA256
is 4919ac6f..., 7fba7a54..., ... . A generic IDR-first rule would already be wrong.
See generator, pcm-manifest.json, negative-baseline.log/framehash and trace_headers.
No media uploaded. Fixture conformance is not certified by this decode test; it is
an accepted-bitstream counterexample to equivalence with the required baseline.

Official H264 06/2026: 8.2.1 permits signed POC and describes field constraints;
8.2.1.3 NOTE3 explicitly describes POC2 output order as decode order. This makes
POC2 a possible narrow rule, unlike guessing POC0 +2 or transplanting HEVC +1.
Ordinary POC2 encoders usually already declare reorder0, so real-world gain may be
zero. Overdeclared reorder can provide a reproducible upper-bound opportunity.

## Proposed minimal candidate / proof obligations

Default-off native software option, initially only a POC2 IDR with no earlier
pending pictures, complete progressive frame-only SPS, default discard, no chunks,
valid PTS and no error flags. Use existing delayed_pic removal / next_output_pic
machinery before finish_setup; actual delivery remains after complete NAL decoding,
field_end (including deblocking) and finalize_frame. Do not reduce has_b_frames or
change subsequent sequence heuristics. All other picture types and POC0/1 fall back.
Do not enable in application or change pins. A candidate is experimental until
ordered pixels, metadata, timestamps AND error returns match; pkt_dts differences
must remain visible and cannot silently inherit HEVC's exception.

## Matrix (planned, not yet passed)

| Case | Source | Candidate behavior |
|---|---|---|
| POC0 lower-after-IDR / gaps | reproducible PCM | must fall back |
| POC1 signed reorder | reproducible PCM | must fall back |
| POC2 reorder0 / declared2 / frame_num wrap | reproducible PCM | narrow startup opportunity only |
| IDR vs non-IDR I, B0/B3/B7, ref B, open GOP | fixed libx264 generated files | POC0/normal rules retained |
| field pairs / PAFF / MBAFF / two fields per packet | 5 indexed FATE files | frame-only gate rejects |
| recovery point | FATE intra_refresh | non-IDR retained |
| SPS/PPS and extradata change | FATE tiny MOV, generated concat | no extension across changed origin |
| MMCO5, POC/frame_num wrap | PCM and generated long GOP | POC0 retained; POC2 only startup |
| malformed/truncated/missing headers | reproducible mutation | preserve return and ordered prefix, inspect early leak |
| flush / seek / drain / async / sync / 1,2,12 threads | portable per-AU harness | same queue retirement and pixels |
| HEVC IDR/CRA/RASL/W_RADL and POC gaps | generated fixed media + existing tests | regress current options, report failures |

No claims about safety or performance yet. Mac app service isn't supported; decoder
library is built on Darwin. Linux-only eventfd probes need a portable local adapter.
