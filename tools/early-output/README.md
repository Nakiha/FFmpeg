# Active early-output validation

**实验版，最终验收未完成，不建议生产启用**

This is the active replacement for the frozen phase3 representative runner in
`tools/early-output-20261022/`. Historical scripts, reports and results remain
unchanged. The current H264 implementation separates completed-picture
presentation from reference decoding, restoring ordinary frame setup release.
See `doc/early-output-stability-20261022.md` for the current implementation and
evidence; the phase3 setup deferral describes the historical implementation.

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

Continuous replay and bounded preparation-failure checks (Linux):

```sh
python3 tools/early-output/run_boundaries.py --workspace /lab \
  --identity /lab/probes/identity.json --results /lab/out/UNIQUE-boundaries
python3 tools/early-output/build_probes.py --probe load.c \
  --baseline-prefix /lab/baseline --candidate-prefix /lab/candidate \
  --output /lab/out/UNIQUE-load-probes
python3 tools/early-output/run_load.py --workspace /lab \
  --identity /lab/out/UNIQUE-load-probes/identity.json \
  --results /lab/out/UNIQUE-load
```

`--sanitize address,undefined` links correctness probes for a matching instrumented
SDK. The Linux `fail_presentation.c` interposer is test-only. Build it as a shared
object with the matching SDK headers; pass its path with `--interposer` and the
matching unstripped libavcodec with `--unstripped-library`. It fails only writes
whose caller is within the presentation owner's symbol range. Every injection
must be observed exactly once, and all declared frames must still match. An
uninjected test cannot pass. Do not preload this library into a real service.

`run_load.py` validates 3 continuous replays with ordered pixels/metadata/source
identity, then times a separate no-hash workload. It alternates old/new build
order over at least 3 repetitions and records wall time and process CPU time.
The old build is a frozen SDK, not a hidden runtime switch. CPU parallelism and
sustained fps must be assessed together; peak workers includes waits and cannot
establish concurrent decoding. The small synthetic fixtures cannot establish
production capacity. Inputs, probes and selected shared libraries remain bound
to hashes. All output directories must be new.
