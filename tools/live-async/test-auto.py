#!/usr/bin/env python3
"""Functional tests or serial performance measurements for the opt-in governor."""
import argparse
import csv
import hashlib
import json
import os
import pathlib
import select
import signal
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--work", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[3])
parser.add_argument("--install", default="install-linux-auto")
parser.add_argument("--results", default="results-auto")
parser.add_argument("--performance", action="store_true")
parser.add_argument("--validate-performance-only", action="store_true", help="recheck recorded data and source/library fingerprints without new timing runs")
parser.add_argument("--quick", action="store_true", help="ASan: two large fixtures and one replay per budget")
args = parser.parse_args()
work = args.work.resolve(); root = work / args.results; root.mkdir(exist_ok=True)
install = work / args.install
environment = dict(os.environ, LD_LIBRARY_PATH=str(install / "lib"))
probe = install / "bin/auto_probe"
completion = root / ("PERFORMANCE_COMPLETE.json" if args.performance or args.validate_performance_only else "FUNCTIONAL_COMPLETE.json")
completion.unlink(missing_ok=True)
core = ["async.c", "async.h", "async_budget.c", "async_budget.h", "pthread_frame.c",
        "thread.h", "version.h", "Makefile", "decode.c", "decode.h", "internal.h"]
provenance = dict(started=time.strftime("%Y-%m-%dT%H:%M:%S%z"), install=args.install,
                  sanitizer_options=environment.get("ASAN_OPTIONS"),
                  library_sha256=hashlib.sha256((install / "lib/libavcodec.so").read_bytes()).hexdigest(),
                  probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),
                  working_core_sha256={name: hashlib.sha256((work / "ffmpeg-7.0.3/libavcodec" / name).read_bytes()).hexdigest() for name in core},
                  logical_processors=os.cpu_count(), quick=args.quick,
                  source_base_commit=subprocess.check_output(["git", "-C", str(work / "ffmpeg-7.0.3"), "rev-parse", "HEAD"], text=True).strip())
if args.validate_performance_only:
    previous = json.loads((root / "performance-environment.json").read_text())
    for key in ("library_sha256", "probe_sha256", "working_core_sha256"):
        assert provenance[key] == previous[key], ("recorded performance no longer matches", key)
else:
    (root / ("performance-environment.json" if args.performance else "functional-environment.json")).write_text(json.dumps(provenance, indent=2)+"\n")
fixtures = [f"{codec}-{res}-b{b}" for res in ("1080p", "4k") for codec in ("h264", "hevc") for b in (3, 7)]

def rows(path):
    with path.open() as file:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(file)]

def command(stem, name, rates="0", streams=1, slots=12, minimum=2, mode="auto", verify=True, group="-", gate=0, long=False):
    return [str(probe), str(work / ("fixtures-auto" if long else "fixtures-load") / (stem + ".mp4")),
            "12", str(minimum), str(slots), group, mode, rates, str(streams), str(root / name),
            str(int(verify)), str(gate)]

def validate(stem, name, rates, streams, verify, slots, minimum, long=False):
    count = 480 if long else 192
    phases = len(rates.split(","))
    data = json.loads((root / f"{name}.json").read_text())
    assert data["frames"] == count * phases * streams, name
    for stream in range(streams):
        if verify:
            expected = ((root / f"{stem}-long-original.sha256") if long else
                        (work / f"results-load/{stem}-golden.sha256")).read_bytes()
            assert (root / f"{name}-s{stream}.sha256").read_bytes() == expected * phases, (name, stream)
        actual = rows(root / f"{name}-s{stream}.csv")
        canonical = rows((root / f"{stem}-long-original.csv") if long else
                         (work / f"results-load/{stem}-t12-verify-r1-original.csv"))
        keys = ("pts", "best_effort", "dts", "duration", "width", "height", "format")
        expected_metadata = [tuple(row[k] for k in keys) for row in canonical] * phases
        assert [tuple(row[k] for k in keys) for row in actual] == expected_metadata, (name, stream, "metadata")
    if data["auto"]:
        for row in rows(root / f"{name}.control.csv"):
            assert minimum <= row["target"] <= 12, (name, row)
            assert 0 <= row["busy"] <= row["leased"] <= (slots or 256), (name, row)

def run(stem, name, rates="0", streams=1, slots=12, minimum=2, mode="auto", verify=True, group="-", gate=0, long=False, extra=None):
    cmd = command(stem, name, rates, streams, slots, minimum, mode, verify, group, gate, long)
    began = time.monotonic()
    if not args.validate_performance_only:
        with (root / f"{name}.log").open("w") as log:
            subprocess.run(cmd, env=dict(environment, **(extra or {})), stdout=log, stderr=log, check=True, timeout=210)
    validate(stem, name, rates, streams, verify, slots, minimum, long)
    info = json.loads((root / f"{name}.json").read_text())
    assert info["auto"] == int(mode == "auto") and info["slots"] == slots and info["verify"] == int(verify), name
    assert [r["fps"] for r in rows(root / f"{name}-s0.passes.csv")] == list(map(int, rates.split(","))), name
    print(f"{'VALIDATED' if args.validate_performance_only else 'PASS'} {name} ({time.monotonic()-began:.1f}s)", flush=True)
    return info

if args.performance or args.validate_performance_only:
    # Run only when fixture encoders, builds and functional/sanitizer jobs have
    # finished. No processes are measured concurrently except intentional tests.
    for stem in fixtures:
        for profile, rates, repeats in (("paced30", "30", 1), ("stress120", "120", 1), ("bulk", "0", 3)):
            for repeat in range(repeats):
                for mode in (("auto", "fixed") if repeat % 2 else ("fixed", "auto")):
                    run(stem, f"{stem}-{profile}-r{repeat+1}-{mode}", rates, mode=mode, verify=False)
    for stem, long in (("h264-1080p-b7", True), ("hevc-4k-b7", True), ("hevc-1080p-b3", False)):
        rates = "30,0,0,0,0,30" if long else "30,0,0,0,0,0,0,0,0,30"
        end_phase = len(rates.split(",")) - 1
        for mode in ("fixed", "auto"):
            name = f"{stem}-steps-{mode}"
            run(stem, name, rates, mode=mode, verify=False, long=long)
            trace = rows(root / f"{name}.control.csv")
            if mode == "auto":
                high = [r for r in trace if 1 <= r["phase"] < end_phase]
                low = [r for r in trace if r["phase"] == end_phase]
                assert max(r["target"] for r in high) > 2, name
                # Observe settled low demand, excluding the final EOF/tail
                # packet burst. A single terminal snapshot is not steady state.
                settled = low[len(low)//2:-max(1,len(low)//8)]
                fraction = sum(r["target"] == 2 for r in settled) / len(settled)
                high_peak = max(r["target"] for r in high)
                assert sum(r["target"] < high_peak for r in settled) / len(settled) >= .8, (name, "did not shrink")
                # Minimum is a floor, not a required operating point for every
                # codec/burst pattern. The clearly light H.264 case also checks
                # that the controller can actually return to its floor.
                if stem == "h264-1080p-b7":
                    assert fraction >= .8, (name, "failed to settle at minimum", fraction)
    (root / "PERFORMANCE_COMPLETE.json").write_text(json.dumps(dict(finished=time.time())) + "\n")
    print("PASS all serial auto/fixed performance and grow/shrink checks", flush=True)
    raise SystemExit(0)

if not args.quick:
    # Compile against the original headers/libs instead of depending on a
    # previously generated (possibly different-platform) benchmark executable.
    baseline = work / "install-linux-baseline"
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    f"-I{baseline}/include", str(work / "ffmpeg-7.0.3/tools/live-async/load_probe.c"),
                    "-o", str(root / "reference_probe_original"), f"-L{baseline}/lib",
                    "-lavformat", "-lavcodec", "-lavutil"], check=True)
subprocess.run([str(install / "bin/auto_lifetime_test"), str(work / "fixtures-load/h264-1080p-b3.mp4")],
               env=environment, check=True, timeout=90)
for stem in (["h264-1080p-b7", "hevc-1080p-b7"] if args.quick else fixtures):
    for slots in (1, 2, 4, 12):
        run(stem, f"{stem}-budget{slots}-correct", slots=slots, minimum=1)
if args.quick:
    run("hevc-4k-b7", "asan-large", slots=12)
stress_stem = "h264-1080p-b7" if args.quick else "hevc-4k-b7"
run(stress_stem, "same-process", "0,0", streams=3, slots=2)
run("h264-1080p-b3", "default-process-budget", streams=2, slots=0, group="default")
run("h264-1080p-b7", "output-stall", streams=2, slots=4, extra={"PROBE_HOLD_READ_MS": "1200"})
stalled = rows(root / "output-stall.control.csv")
for stream in range(2):
    full = [r for r in stalled if r["stream"] == stream and r["time_us"] < 1200000 and r["output"] == 12]
    assert len(full) >= 2
    assert len({r["increases"] for r in full}) == 1, "grew while downstream queue was full"
gated = run("h264-1080p-b3", "frontend-gate", slots=1, minimum=1, gate=500000)
assert gated["max_send_us"] < 50000 and gated["max_receive_us"] < 50000, gated
run("h264-1080p-b3", "api-checks", extra={"PROBE_CHECK_API": "1"})
for slots, group in ((257, "-"), (2, "../invalid")):
    rejected = subprocess.run(command("h264-1080p-b3", "invalid-budget", slots=slots, group=group),
                              env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
    assert rejected.returncode != 0

separate_group = f"handles-v2-{os.getpid()}-{int(time.time())}"
try:
    run("h264-1080p-b3", "separate-handles", streams=2, slots=1, minimum=1, group=separate_group,
        gate=500000, extra={"PROBE_SEPARATE_HANDLES": "1"})
    for stream in range(2):
        assert rows(root / f"separate-handles-s{stream}.csv")[0]["received_us"] >= 450000
finally:
    for path in pathlib.Path("/dev/shm").glob(f"ffmpeg-async-{os.getuid()}-{separate_group}*"): path.unlink()

group = f"test-v2-{os.getpid()}-{int(time.time())}"
processes = []
logs = []
try:
    first_log = (root / "crash-holder.log").open("w"); logs.append(first_log)
    first = subprocess.Popen(command("h264-1080p-b3", "crash-holder", slots=1, minimum=1, group=group, gate=10000000),
                             env=environment, stdout=subprocess.PIPE, stderr=first_log, text=True)
    processes.append(first)
    assert select.select([first.stdout], [], [], 10)[0], "holder never entered decoder"
    assert first.stdout.readline().strip() == "GATE_ENTERED"
    # Capacity mismatches must fail even while an existing group is active.
    mismatch = subprocess.run(command("h264-1080p-b3", "mismatch", slots=2, group=group), env=environment,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
    assert mismatch.returncode != 0, "accepted incompatible shared capacity"
    (root / "mismatch.log").write_bytes(mismatch.stderr)
    second_log = (root / "crash-waiter.log").open("w"); logs.append(second_log)
    second = subprocess.Popen(command("h264-1080p-b3", "crash-waiter", slots=1, minimum=1, group=group, gate=1),
                              env=environment, stdout=subprocess.PIPE, stderr=second_log, text=True)
    processes.append(second)
    assert not select.select([second.stdout], [], [], .5)[0], "second decoder exceeded shared one-slot budget"
    first.kill(); first.wait(timeout=10)
    assert select.select([second.stdout], [], [], 5)[0], "kernel did not recover killed owner's slot"
    assert second.stdout.readline().strip() == "GATE_ENTERED"
    assert second.wait(timeout=60) == 0
    validate("h264-1080p-b3", "crash-waiter", "0", 1, True, 1, 1)
    print("PASS cross-process one-slot exclusion, incompatible capacity rejection, SIGKILL recovery", flush=True)
finally:
    for process in processes:
        if process.poll() is None: process.kill(); process.wait()
    for log in logs: log.close()
    for path in pathlib.Path("/dev/shm").glob(f"ffmpeg-async-{os.getuid()}-{group}*"):
        path.unlink()  # All members are stopped; removing live lock files is forbidden.

group = f"pair-v2-{os.getpid()}-{int(time.time())}"
processes = []; logs = []
try:
    for index in range(2):
        log = (root / f"process-pair-{index}.log").open("w"); logs.append(log)
        processes.append(subprocess.Popen(command(stress_stem, f"process-pair-{index}", "0,0", slots=4, group=group),
                                          env=environment, stdout=log, stderr=log))
    for index, process in enumerate(processes):
        assert process.wait(timeout=180) == 0
        validate(stress_stem, f"process-pair-{index}", "0,0", 1, True, 4, 2)
    print("PASS two simultaneous processes, shared four-slot budget, all pixels/metadata", flush=True)
finally:
    for process in processes:
        if process.poll() is None: process.kill(); process.wait()
    for log in logs: log.close()
    for path in pathlib.Path("/dev/shm").glob(f"ffmpeg-async-{os.getuid()}-{group}*"): path.unlink()
if not args.quick:
    for stem in ("h264-1080p-b7", "hevc-4k-b7"):
        name = f"{stem}-steps-correct"
        run(stem, name, "30,0,0,0,0,0,0,0,0,30")
        trace = rows(root / f"{name}.control.csv")
        high_peak = max(r["target"] for r in trace if 1 <= r["phase"] <= 8)
        low = [r for r in trace if r["phase"] == 9]
        assert high_peak > 2, (name, "did not grow")
        # SHA/copy work adds real downstream CPU/bursts. A later increase is
        # legitimate in this correctness workload. Require actual shrink here;
        # the uninstrumented performance scenario separately requires final 2.
        assert min(r["target"] for r in low) < high_peak, (name, "did not shrink", low[-1])
        print("PASS grow/shrink with every pixel/metadata checked", stem, flush=True)
    for stem in ("h264-1080p-b7", "hevc-4k-b7"):
        reference = root / f"{stem}-long-original"
        cmd = [str(root / "reference_probe_original"), str(work / f"fixtures-auto/{stem}.mp4"),
               "12", "0", "legacy", "verify", "12", str(reference)]
        baseline_env = dict(environment, LD_LIBRARY_PATH=str(work / "install-linux-baseline/lib"))
        subprocess.run(cmd, env=baseline_env, check=True, timeout=180)
        name = f"{stem}-long-correct"
        result = run(stem, name, long=True)
        assert result["frames"] == 480
        assert (root / f"{name}-s0.sha256").read_bytes() == reference.with_suffix(".sha256").read_bytes()
        keys = ("pts", "best_effort", "dts", "duration", "width", "height", "format")
        assert ([tuple(r[k] for k in keys) for r in rows(root / f"{name}-s0.csv")] ==
                [tuple(r[k] for k in keys) for r in rows(reference.with_suffix(".csv"))])
        print("PASS long-fixture pixels/metadata versus unmodified 7.0.3", stem, flush=True)
(root / "FUNCTIONAL_COMPLETE.json").write_text(json.dumps(dict(finished=time.time(), quick=args.quick)) + "\n")
print("PASS all automatic-dispatch functional tests", flush=True)
