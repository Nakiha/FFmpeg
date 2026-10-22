# Next H264 experiment: decode-state / output-state separation

**实验版，最终验收未完成，不建议生产启用**

This is a design boundary for the next codec change, not an implemented or
accepted optimization. The active validation tools are in `tools/early-output`.
The codec remains the phase3 snapshot with deferred frame setup.

The current `ff_h264_update_thread_context()` copies delayed_pic, output
watermark, recovery flags and live HRD proof state alongside reference decoding
state. Therefore moving `ff_thread_finish_setup()` earlier, or merely moving
the output FIFO to another thread, would publish an incomplete output decision
to the next worker. A faster version must explicitly resolve this coupling.

| State | Required owner / lifetime question |
| --- | --- |
| Reference DPB, MMCO, POC prediction, SPS/PPS | Preserve ordinary decode setup/progress dependencies and reference marking exactly once |
| Complete AU proof and decode status | Worker supplies immutable results after all NAL/slice processing and field_end; observable errors disable extension |
| Presentation pending list, output watermark, HRD clock | One ordered output owner; stop copying speculative output decisions between frame workers |
| DELAYED_PIC_REF, recovery state, non-reference B lifetime | Explicit ownership independent of worker-local H264Picture addresses and DPB recycling |
| Prepared AVFrames and FIFO | Prepare everything before atomic output apply; allocation failure cannot remove extra pictures |
| Frame properties and errors | Retain n9 common frame-property/postprocessing/cropping and ordered error retirement; do not bypass public receive validation |

A proposed completion record needs a generation/epoch, immutable parameter
identity, per-picture POC/timing facts, AU validity/error state and held frame
references. Flush/reinit must cancel stale generations before accepting later
completion records. A failed proof must never invalidate a reference needed for
decoding, publish partially prepared output, or be silently restored by a later
NAL of the same AU. This still does not prove unseen future damaged AUs conform.

Before choosing a record layout or adding a callback at worker retirement,
establish where common frame properties are finalized, how ordinary DPB
selection changes recovery state, and which reference flags the next worker
needs. Complete record ownership and cancellation proofs are prerequisites to
implementing early setup publication; this file does not assert they are solved.

Required comparison gates include all four sync/async x early modes, early-off
compatibility, single/multiple slice fallback, parameter epochs, late/duplicate/
short/malformed SEI, negative leading POC, MMCO5, strict/lenient errors, complete
and in-flight flush, output-allocation failures and close. In addition to ordered
pixels/PTS, measure actual reconstruction overlap and setup/reference wait
separately from active-worker counters. Sustained throughput and queue age must
remain acceptable while complete-service tail latency improves; a shorter
codec output delay alone cannot establish that result.
