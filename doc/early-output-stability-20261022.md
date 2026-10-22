# H264 / HEVC early output: bounded local acceptance

**实验版，最终验收未完成，不建议生产启用。**

This fork's native software decoder options remain initialization-only and off
by default. An enabled option is a request within the eligibility rules, not a
promise that every input can be emitted early. Ordinary decoding remains the
fallback. The enclosing Linux transcode service is still experimental.

## H264 presentation and reference decoding

The historical phase3 implementation (`e515265` through `487ae0c`) held frame
setup until full decode and output commit. Eligible consecutive frames could
therefore serialize despite `thread_count=12`. Frozen phase2 patches precede
that hardening and are not the current algorithm or its performance evidence.

The current implementation leaves MMCO, reference DPB membership, ordinary
one-picture selection and frame setup publication on their ordinary paths.
Input-order tickets are acquired before setup. After reconstruction and full AU
validation, workers retire complete AVFrame references through a separate shared
presentation owner, in ticket order. Later workers can reconstruct concurrently;
only their presentation retirement waits for earlier tickets. This is bounded
ordered completion, not an absence of reference dependencies or queueing.

The owner stores at most `H264_MAX_DPB_FRAMES+2` complete frame references and
`2*H264_MAX_PICTURE_COUNT` duplicate-suppression identities. It never retains
worker-local H264Picture pointers. Presentation IDs are monotonically assigned
per input packet, independent of DPB slot reuse. Multi-picture/gap AUs cannot
qualify. Ordinary selection of an already promoted identity is suppressed once;
IDR progression purges obsolete tokens. Suppression during draining cannot be
mistaken for EOF: ordinary delayed frames are drained to exhaustion.

Every proposed prefix is referenced into a private queue before its watermark
or suppression identities are committed. Preparation failure clears the proof
and retains ordinary output. External flush parks workers first, clears pending
presentation state and requires all issued tickets to have retired. Statistics
are lifetime counters and deliberately survive flush.

Automatic AVC eligibility remains conservative: progressive POC0, single slice,
Main/High HRD/CFR, one NAL HRD CPB, complete and unique picture timing, compatible
buffering period, valid PTS, no decoded errors, no incompatible SPS/PPS change,
no MMCO reset or unsupported frame packing/field/hardware/skip policy. HRD
output ticks must continue a proved presentation prefix by exactly two ticks.
No-HRD, normal multi-slice and other ineligible inputs fall back. Recent POC
patterns, encoder names and timestamps alone do not establish the proof.
`live_poc_verified_even` remains a separate exact-input caller contract; the
service never silently enables it. This is not universal AVC early output.

## HEVC and independent async control

HEVC selection policy is unchanged here. It can anchor a progressive single-layer
software sequence at eligible `IDR_N_LP/POC0`, then extend a prefix only when the
minimum pending POC is watermark+1. CRA, leading pictures, fields, multilayer,
hardware, missing PTS and unsupported cases retain ordinary selection. Completed
AU validation and pending-output ownership are retained. H264 cannot safely
copy this +1 rule because valid AVC display POCs can have gaps.

Async controls the bounded frontend/coordinator and task dispatch. Codec early
output controls selection from presentation reorder state. Both are independent
of the optional application's decoder pump and automatic thread-budget control.
Ordinary API calls still use FFmpeg frame threads and can block. Disabling early
output does not disable async; disabling async does not disable codec early
output. Neither feature removes B-picture reference dependencies.

## Observations and verification

`libavcodec/early_output.h` API v1 provides shared atomic lifetime snapshots for
native h264/hevc, with an async-owner wrapper. It distinguishes ordinary, H264
HRD, externally verified H264, HEVC IDR and HEVC prefix selections. Counts are
codec selections committed to output queues, not public receives or service
mux writes; flush/cancel can discard them. Picture decisions count completed
AUs, not individual output fallbacks. Snapshots while workers run are approximate;
HEVC's last observed decision need not be input ordered. Unsupported codecs
return ENOSYS. libavcodec micro is 109; async API remains version 2.

Active tests live in `tools/early-output/`; frozen `early-output-20261022`
materials remain historical evidence. Every normal test uses declared cycles
and independently generated frame counts. No zero-frame, partial-cycle or
exceptional exit can pass by shortening the reference. Ordered pixels, source
IDs, display metadata and full counts are required. Only packet DTS may differ
for early comparisons. Error tests require an explicit failure and a bounded
valid prefix of an independently complete oracle. Close tests check lifetime,
not pixel equality. SDK/probe/input hashes and actual loaded libavcodec paths
are checked. New output directories prevent stale result reuse.

Local environment: macOS ARM host, Colima Ubuntu 24.04 amd64/Rosetta, 8 vCPU,
12 GiB. Normal codec checks: 83/83 representatives and 80/80 boundaries/fault
cases. The latter include strict/lenient malformed SEI, SPS/HRD changes and
wraps, multi-slice fallback, and 12 observed ENOMEM injections confined to the
new presentation preparation. Every injected complete decode still returns
all 192 declared frames in the same display/pixel order.

Sanitizer, sustained replay and exact build/result hashes are recorded in the
companion stability evidence JSON. Service-SDK and end-to-end results belong to
the pinned parent application's stability report; codec checks cannot substitute
for service integration. Old phase2/phase3 timings cannot be relabeled as current
results. This local environment cannot establish native Linux production
capacity, race freedom, arbitrary damaged-stream safety or a production SLO.
Native multi-stream soak, target hardware CPU/throughput/queue/P95/P99 and
independent final review remain rollout work. No deployment or default change
is part of this change.
