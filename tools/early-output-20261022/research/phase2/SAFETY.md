> **实验版，最终验收未完成，不建议生产启用**
> 历史文档归档：正文中的“当前”、未推送状态及验收结论仅属于原阶段；以 `doc/early-output-20261022/REPORT.md` 的版本核对为准。`out/` 路径指本机保留证据，不随公开分支发布。

# Two proof modes; neither is a recent-POC heuristic

Final independent review: the conforming-stream theorem below was supported, but
the current implementation was NOT safety accepted. Current-AU late/malformed/
short or contradictory SEI eligibility is incomplete (F1/F2). Read REPORT.md and
INDEPENDENT_REVIEW.md before interpreting any normal-matrix pass as acceptance.

## Automatic in-band mode (usable without a future GOP scan)

Initialization option live_poc_early_output enables the policy independently of
avcodec_async_create. Default is false in the H264 codec. Supported initial scope:
native software Main/High, POC0, progressive frame_mbs_only, no per-frame bottom
POC offset, no film grain, default discard, whole AUs, present picture PTS.
Additional in-band eligibility: timing_info + fixed_frame_rate=1; pic_struct absent;
NAL-HRD only, one CPB, low_delay_hrd=0; picture-timing SEI on the current AU;
cpb/dpb delays nonnegative with lengths1..30. Other cases use existing DPB policy.
MMCO5, parameter-content changes, missing timing, repeated removal counters or
locally checked pending POC/timing contradictions disable the extension before
its selection; this does not cover every later NAL/SEI in the current AU.
Identical repeated SPS/PPS data are allowed. Non-IDR buffering-period changes
carry their removal origin using C-8/C-9; IDR starts a new clock serial. Counter
wrap and arithmetic bounds are explicit.

Proof, for a conforming stream satisfying those declared constraints:

- H264 C.2.2 computes DPB output time from CPB removal time plus dpb_output_delay.
  With low_delay_hrd=0, removal follows the nominal clock, not a late-arrival shift.
- E.2.1 (E-78), Table E-6 fixes consecutive progressive outputs exactly two clock
  ticks apart when fixed_frame_rate=1 and pic_struct is absent.
- D.2.3 requires the output order defined by these delays to agree with POC order.
- The first selected IDR is obtained by the OLD DPB rule. This mode does not assume
  IDR is the earliest decoded picture in presentation order, and does not remove
  the existing startup wait. Negative leading pictures are retained.
- If the smallest pending POC is at the next output tick after the watermark, no
  unseen conforming picture can be inserted between them or at the same time.
  Select that prefix, repeating while it stays contiguous. POC itself may be odd.

Selection/DELAYED flags are updated before frame-thread setup; H264Picture refs
hold pending frames, including non-reference B, until all NAL decoding and field
end validate. Output uses a receive-frame FIFO so one AU can release several
pictures. n9 workers/async still retire tasks in order. No filtering/reconstruction
is skipped and no additional future AU is requested for this proof.

This is a proof under the CURRENT bitstream's normative HRD/CFR declarations,
not a complete HRD conformance validator. A malicious or damaged future AU can
violate declarations; no streaming decoder can prove all unseen bytes conform.
Recognized unsupported/counter/field/recovery cases are not generalized to a
claim that arbitrary malformed streams always retain identical prefixes. Error
status and ordered output-prefix evidence are recorded separately. The original
unrestricted POC0/POC2 counterexamples do not satisfy this mode's eligibility.

Ordinary x264 without HRD and with fixed_frame_rate=0 remains a real unsupported
normal class. The lab's automatic target encoder recipe explicitly adds:
force-cfr=1:nal-hrd=vbr:vbv-maxrate=1200:vbv-bufsize=2400.
Their actual SPS/SEI are inspected, not inferred from x264 branding. Other producers
may satisfy the same intrinsic constraints; they need not be identified as x264.
An unproven non-HRD live source is never silently treated as eligible.

## Externally verified exact-input mode (not automatic live proof)

live_poc_verified_even is a separate CALLER ASSERTION, default false. It is not
set by the application switch. It requires the entire actual input to have unique,
nonnegative even POCs per IDR sequence, progressive POC0 with stable parameters,
no MMCO5 and consistent presentation timestamps. The local verifier checks all
slice headers, not just the first slice's MMCO, and creates a certificate binding
extradata, ordered packet bytes and PTS/DTS/duration. The probe hashes the exact
preloaded immutable packet references and only supplies the assertion on a match.
Missing/mismatched certificates fall back before decoding/output.

This file mode needs whole-input availability and an upfront verification cost;
its timings are NOT claimed as live-source latency. Header checking proves the
ordering contract, not entropy/pixel correctness. The assertion is safe only when
its issuer/caller is trusted. It cannot be synthesized from an encoder SEI name,
recent even POCs, a boolean early switch or an unknown remote source.

## Application switch/default compatibility

Optional decoder_early_output is a boolean initialization override. Omitted is an
internal -1 sentinel, preserving HEVC enabled / H264 disabled as before. Explicit
false asks original DPB selection for both; explicit true permits only eligible
paths. The app NEVER sets the external assertion for arbitrary input. Missing
H264 extension in the old pinned SDK falls back, without silently selecting an
unavailable optimization. This switch does not change async ownership; the current
service has no async-off configuration, and decoder_pump is only frontend dispatch.
The codec test harness exercises synchronous/asynchronous x early off/on.
No hot switching is promised or implemented. No SDK pin or production binary is
changed by this local prototype.
