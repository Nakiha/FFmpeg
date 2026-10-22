# Local readiness follow-up

**实验版，最终验收未完成，不建议生产启用。**

Base: `2673319b815fa6da6fccd0c3801e099b2e473066`.

H264's hardened output planner can return a plan containing only the ordinary
DPB selector's already selected picture. Applying this plan makes no additional
DPB change. That case now uses ordinary `finalize_frame` directly, saving the
temporary AVFrame and staging FIFO allocation. Actual prefix extensions still
prepare all outputs before apply. Complete-AU validation, frame-setup deferral,
the HRD/CFR contract and no-declaration/multiple-slice fallback remain in force.
HEVC and async scheduling are unchanged by this codec patch.

This reduces unnecessary allocation; it does **not** restore AVC frame
parallelism, widen eligible inputs or establish a throughput/latency gain.
The state-separation prerequisites in `early-output-state-separation.md` remain
open. Moving finish_setup earlier without resolving those dependencies is unsafe.

Local Linux amd64/Rosetta decoder-only build: full active matrix **83/83**,
81 ordered comparison/lifecycle cases and two in-flight close cases. Baseline
is the previous exact service SDK with early output off, candidate is this local
codec build. Checks use declared input/cycle counts, source identities, pixels,
PTS, terminal results and loaded-library hashes. Only early-on pkt_dts differs.

Local evidence in `/build/local-codec-83` and `/build/local-codec-probes` (dev
volume); no media/build outputs included in Git. Summary SHA256:
`f1d14fbbbaa055c6435ae83f1f85b91690f66dc13bce8aa4b14991886600f5d5`.
Probe identity SHA256:
`2ba646dd8277bff5e26587f49cfeb00a1d35b6c5f9d00c3c43cccaf629f3063c`.
This is new correctness evidence, separate from frozen phase2 performance.
