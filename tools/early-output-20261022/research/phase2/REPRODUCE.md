> **实验版，最终验收未完成，不建议生产启用**
> 历史文档归档：正文中的“当前”、未推送状态及验收结论仅属于原阶段；以 `doc/early-output-20261022/REPORT.md` 的版本核对为准。`out/` 路径指本机保留证据，不随公开分支发布。

# Reproduction map

These are local research artifacts, not a release. Read acceptance.json first.
Run from task-2. The current source trees already contain the exported patches;
do not reapply patches or rerun implement.py/add_hrd.py over the modified tree.

Builds used `bash research/build-macos.sh phase2` and
`FFMPEG_TEST_SANITIZE=address bash research/build-macos.sh phase2-asan`.
The exact configure/version and binary hashes are in out/phase2/build-identity.json;
build-proof-final.log / build-asan-final.log contain compiler output. The baseline
is the separate out/install/baseline build of unmodified94bd940f.

The standalone probes derive from research/phase2/probe.c and perf_probe.c.
They link only the explicitly selected local prefix (see recorded otool output).
The local phase2 binary is not installed system-wide.

Generation and source provenance:

- generate_targets.py: 28 x264/x265 ordinary targets; generate_hrd_targets.py:
  14 H264 HRD targets. Manifests record complete encoder commands and hashes.
- generate_hrd_boundaries.py: deterministic I_PCM HRD/POC/counter boundaries.
  delivery-inventory.json additionally hashes every resulting media fixture,
  including odd-poc and negative-leading.
- ../download_fate.py and out/evidence/fate-download.json: eight official samples.
- verify_contract.py: full-input header verifier for the separate offline mode;
  certificates and exact packet digests retained alongside target proof results.

Executed regression entry points and outputs:

- run_hrd_targets_proof_final.py → out/phase2/hrd-proof-final/summary.json
- run_targets_proof_final.py → out/phase2/target-proof-final/summary.json
- fallback_checks_final.py → out/phase2/fallback-final/summary.json
- run_lifecycle.py → out/phase2/lifecycle/summary.json
- post_measurement_checks.py → out/phase2/post-checks/{boundaries,error-return-ledger,actual-header-structures}.json
- run_performance.py → out/phase2/performance/results.json (162 runs, every exact
  command and environment included). Do not run other encoders/decoders in parallel
  with paced measurement. NO_HASH=1; H264_CERTIFICATE absent.
- build_config_test.sh phase2 / baseline → config-test-final.log and
  config-test-pinned-sdk.log; full Linux service is not a Mac target.
- summarize_delivery.py → medians,MEASUREMENTS.md and explicit rejection status.

Each comparison record points to raw per-display-position .sha256, per-event .csv
and .log. Baseline single-cycle output is repeated per cycle before comparison;
frame order/count and statuses are independent of pixel set equality. Native
PTS/best_effort and metadata must match; only the documented output-trigger
pkt_dts change is allowed for supported early-on runs. Early-off requires exact
metadata, with pre-existing lenient error-flag races recorded separately.

Output directories are evidence from this run. Copy this workspace or use new
output directories before rerunning scripts that currently use fixed filenames.
Never replace the first-stage rejected evidence or the independent review with
later results. Any guard fix must receive a new build identity and targeted review;
the existing162 measurements must not be relabelled as that future binary.
