#!/usr/bin/env python3
"""Serial Linux benchmark. Never overlap measured processes or fixture encoding."""
import argparse
import csv
import hashlib
import json
import os
import pathlib
import random
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--work", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[3])
parser.add_argument("--results", default="results-load")
parser.add_argument("--bulk-repeats", type=int, default=3)
args = parser.parse_args()
work = args.work.resolve()
root = work / args.results
root.mkdir(exist_ok=True)
source = work / "ffmpeg-7.0.3/tools/live-async/load_probe.c"
prefixes = {"original": work / "install-linux-baseline", "async": work / "install-linux-async"}

def environment(version):
    return dict(os.environ, LD_LIBRARY_PATH=str(prefixes[version] / "lib"))

for version, prefix in prefixes.items():
    cmd = ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]
    if version == "async":
        cmd.append("-DLIVE_ASYNC")
    cmd += [f"-I{prefix}/include", str(source), "-o", str(root / f"load_probe_{version}"),
            f"-L{prefix}/lib", "-lavformat", "-lavcodec", "-lavutil"]
    subprocess.run(cmd, check=True)

fixtures = []
for resolution in ("1080p", "4k"):
    for codec in ("h264", "hevc"):
        for bframes in (3, 7):
            stem = f"{codec}-{resolution}-b{bframes}"
            path = work / f"fixtures-load/{stem}.mp4"
            cmd = [str(prefixes["original"] / "bin/ffprobe"), "-v", "error", "-threads", "1",
                   "-select_streams", "v:0", "-show_frames", "-show_streams", "-of", "json", str(path)]
            data = json.loads(subprocess.check_output(cmd, env=environment("original")))
            frames = data["frames"]
            types = "".join(frame["pict_type"] for frame in frames)
            # Probe returns display order. Check that the requested run really exists.
            max_b_run = max(map(len, types.replace("I", " ").replace("P", " ").split()), default=0)
            assert len(frames) == 192, (stem, len(frames))
            assert max_b_run == bframes, (stem, max_b_run)
            assert data["streams"][0]["pix_fmt"] == "yuv420p"
            fixture = dict(stem=stem, codec=codec, resolution=resolution, bframes=bframes,
                           frames=len(frames), max_b_run=max_b_run, types=types,
                           bytes=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                           stream=data["streams"][0])
            fixtures.append(fixture)
            (root / f"{stem}-fixture.json").write_text(json.dumps(fixture, indent=2) + "\n")
            print("FIXTURE", stem, len(frames), "max B run", max_b_run, flush=True)

metadata = dict(started=time.strftime("%Y-%m-%dT%H:%M:%S%z"), bulk_repeats=args.bulk_repeats,
                uname=subprocess.check_output(["uname", "-a"], text=True).strip(),
                cpu=subprocess.check_output(["lscpu"], text=True),
                compiler=subprocess.check_output(["gcc", "--version"], text=True),
                commit=subprocess.check_output(["git", "-C", str(work / "ffmpeg-7.0.3"), "rev-parse", "HEAD"], text=True).strip(),
                fixtures=fixtures,
                baseline_configuration=subprocess.check_output([str(prefixes["original"] / "bin/ffmpeg"), "-version"],
                                                                env=environment("original"), text=True),
                async_configuration=subprocess.check_output([str(prefixes["async"] / "bin/ffmpeg"), "-version"],
                                                             env=environment("async"), text=True))
(root / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")

def read_rows(path):
    with path.open() as file:
        return list(csv.DictReader(file))

def canonical(rows):
    keys = ("sequence", "pts", "best_effort", "dts", "duration", "width", "height", "format")
    return [tuple(row[k] for k in keys) for row in rows]

jobs = []
for fixture in fixtures:
    for threads in range(2, 13):
        jobs.append((fixture, threads))
random.Random(703).shuffle(jobs)
total = len(jobs) * (2 + 2 + args.bulk_repeats * 2 + 2)
completed = 0
started = time.monotonic()
for fixture, threads in jobs:
    stem = fixture["stem"]
    golden = None
    for profile, fps, verify, repeats in (("verify", 0, True, 1), ("paced30", 30, False, 1),
                                          ("stress120", 120, False, 1), ("bulk", 0, False, args.bulk_repeats)):
        for repeat in range(repeats):
            versions = ["original", "async"]
            if (threads + repeat + fixture["bframes"]) % 2:
                versions.reverse()
            results = {}
            for version in versions:
                name = f"{stem}-t{threads:02}-{profile}-r{repeat + 1}-{version}"
                target = root / name
                marker = target.with_suffix(".ok")
                # Only successful completed jobs are reusable after an interruption.
                if not marker.exists():
                    cmd = [str(root / f"load_probe_{version}"), str(work / f"fixtures-load/{stem}.mp4"),
                           str(threads), str(fps), "legacy" if version == "original" else "async",
                           "verify" if verify else "perf", "12", str(target)]
                    with target.with_suffix(".log").open("w") as log:
                        subprocess.run(cmd, env=environment(version), stdout=log, stderr=log, check=True, timeout=150)
                    marker.write_text("OK\n")
                info = json.loads(target.with_suffix(".json").read_text())
                assert info["frames"] == fixture["frames"]
                assert info["lavc"] == ((61 << 16) + ((3 if version == "original" else 4) << 8) + 100)
                results[version] = target
                completed += 1
                print(f"[{completed}/{total}] {name} first={info['first_us']/1000:.2f}ms "
                      f"fps={info['frames']*1e6/info['elapsed_us']:.1f} "
                      f"cores={info['cpu_us']/info['elapsed_us']:.2f} elapsed={time.monotonic()-started:.0f}s", flush=True)
            original = canonical(read_rows(results["original"].with_suffix(".csv")))
            actual = canonical(read_rows(results["async"].with_suffix(".csv")))
            assert actual == original, (stem, threads, profile, "output metadata mismatch")
            if verify:
                reference = results["original"].with_suffix(".sha256").read_bytes()
                assert results["async"].with_suffix(".sha256").read_bytes() == reference, (stem, threads, "pixel mismatch")
                reference_path = root / f"{stem}-golden.sha256"
                if reference_path.exists():
                    assert reference_path.read_bytes() == reference, (stem, threads, "thread-dependent output")
                else:
                    reference_path.write_bytes(reference)
                golden = original
            else:
                assert actual == golden, (stem, threads, profile, "metadata changed without hashing")

(root / "COMPLETE.json").write_text(json.dumps(dict(completed=completed, seconds=time.monotonic()-started,
                                                   finished=time.strftime("%Y-%m-%dT%H:%M:%S%z")), indent=2) + "\n")
print("PASS: entire matrix; all frames/pixels/metadata match original n7.0.3", flush=True)
