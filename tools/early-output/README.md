# Active early-output validation

**实验版，最终验收未完成，不建议生产启用**

This is the active replacement for the frozen phase3 representative runner in
`tools/early-output-20261022/`. Historical scripts, reports and results remain
unchanged. This change does not alter the codec algorithm or restore H264 frame
parallelism; the phase3 setup deferral is still present.

The checker uses declared cycles and independent fixture frame counts. Every
normal cycle must have its expected frame events, ordered digests, source IDs,
completion marker and final summary. Process failures, missing/failed terminal
records, partial cycles, malformed records or unexpected metadata fail the run.
Only column 2 (pkt_dts) can be allowed to differ for early-on comparisons. A
close case has a separate in-flight-worker checkpoint and is never reported as
a pixel comparison. Certificate cases require actual verification, not fallback.

All result directories must be new. Case IDs include the input folder, avoiding
the historical HRD/non-HRD basename collision. The plan records the exact selected
scope; filtered runs cannot represent the entire matrix. The probe executable,
shared libraries and input fixture hashes are checked around execution. The
instrumented probe records the actual loaded libavcodec path, checked against the
selected SDK. Binary identity files and raw results are local build artifacts.

Run the judge's negative tests without decoding media:

```sh
python3 -m unittest discover -s tools/early-output -p test_evidence.py -v
```

Build instrumented probes against two existing POSIX SDK installations. Neither
SDK is rebuilt or replaced by this command:

```sh
python3 tools/early-output/build_probes.py \
  --baseline-prefix /lab/out/install/baseline \
  --candidate-prefix /lab/out/install/phase3 \
  --output /lab/out/phase4-UNIQUE/probes
python3 tools/early-output/run_representatives.py \
  --workspace /lab --identity /lab/out/phase4-UNIQUE/probes/identity.json \
  --results /lab/out/phase4-UNIQUE/representatives --full
```

The original lab's fixture/oracle manifests and existing media are prerequisites;
this runner does not download or generate fixtures. The default core matrix is
60 cases; `--full` explicitly selects 83 including three offline contracts, ten
fallbacks and ten lifecycle pairs. Of these, two in-flight close pairs check
lifetime only. A timeout or any failed selected case gives process exit status 1.

This correctness probe adds cycle/runtime identity diagnostics and is not a
latency or throughput benchmark. No codec-only result establishes full-service
latency, sustained capacity, race freedom or general malformed-stream safety.
