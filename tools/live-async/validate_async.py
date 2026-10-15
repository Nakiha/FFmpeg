"""Validate exact legacy/async output equality and summarize actual measurements."""
import csv
import pathlib
import sys
from fractions import Fraction

root = pathlib.Path(sys.argv[1])
baseline = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None

def summary(stem):
    lines = (root / (stem + ".csv")).read_text().splitlines()
    line = next(line for line in lines if line.startswith("summary,"))
    fields = dict(item.split("=", 1) for item in line.split(",")[1:])
    assert len([line for line in lines if line.startswith("frame,")]) == int(fields["frames"])
    return fields

rows = []
for codec in ("h264", "hevc"):
    for bframes in (0, 3):
        for threads in (2, 12):
            stem = f"{codec}-b{bframes}-t{threads}"
            legacy = (root / (stem + "-legacy.sha256")).read_text()
            actual = (root / (stem + "-async.sha256")).read_text()
            assert actual == legacy, stem
            assert len(actual.splitlines()) == 50, stem
            if baseline:
                old_lines = (baseline / f"{codec}-b{bframes}-t{threads}.sha256").read_text().splitlines()
                tb = Fraction(next(line.split(": ", 1)[1] for line in old_lines if line.startswith("#tb 0:")))
                old_rows = [line.split(",") for line in old_lines if line and not line.startswith("#")]
                new_rows = [line.split(",") for line in actual.splitlines()]
                assert [r[-1].strip() for r in old_rows] == [r[-1] for r in new_rows], stem
                assert [int(int(r[2]) * tb * 1_000_000) for r in old_rows] == [int(r[0]) for r in new_rows], stem
    golden = (root / f"{codec}-b0-t12-legacy.sha256").read_text()
    reordered = (root / f"{codec}-b3-t12-legacy.sha256").read_text()
    for name in ("slow", "pause", "error", "gate", "flush-inflight", "api"):
        assert (root / f"{codec}-{name}.sha256").read_text() == golden, (codec, name)
    assert (root / f"{codec}-flush.sha256").read_text() == reordered * 3
    assert int(summary(f"{codec}-flush")["send_eagain"]) > 0
    assert int(summary(f"{codec}-pause")["first_frame_us"]) < 1_000_000
    # Verify real decoder-callback overlap, not just occupied queue slots.
    bulk = summary(f"{codec}-b0-t12-async")
    assert int(bulk["peak_active_workers"]) >= 2, bulk
    for threads in (2, 4, 8, 12):
        fields = summary(f"{codec}-paced-t{threads}")
        control = summary(f"{codec}-paced-legacy-t{threads}")
        assert int(fields["frames"]) == 50
        assert (root / f"{codec}-paced-t{threads}.sha256").read_text() == golden
        rows.append({
            "codec": codec, "threads": threads,
            "legacy_first_frame_ms": round(int(control["first_frame_us"]) / 1000, 3),
            "legacy_mean_latency_ms": round(int(control["mean_latency_us"]) / 1000, 3),
            **{key.replace("_us", "_ms"): round(int(fields[key]) / 1000, 3)
               for key in ("first_frame_us", "mean_latency_us", "max_send_us", "max_receive_us")},
            "bulk_peak_active_workers": int(bulk["peak_active_workers"]),
        })
    for variant, count in (("resize", 40), ("10bit", 20)):
        actual = (root / f"{codec}-{variant}-async.sha256").read_text()
        assert actual == (root / f"{codec}-{variant}-legacy.sha256").read_text()
        assert len(actual.splitlines()) == count
        dimensions = {tuple(line.split(",")[4:6]) for line in actual.splitlines()}
        if variant == "resize":
            assert dimensions == {("640", "360"), ("960", "540")}, dimensions
assert (root / "intra-legacy.sha256").read_text() == (root / "intra-async.sha256").read_text()
assert int(summary("intra-async")["peak_active_workers"]) >= 2
with (root / "summary.csv").open("w", newline="") as output:
    writer = csv.DictWriter(output, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
for row in rows:
    print(row)
print("PASS: H.264/HEVC, B=0/B=3, 2/12 threads: hashes, PTS, DTS, duration, format and color match legacy.")
print("PASS: input pause, capacity-one retry, 3 drain/flush cycles, slow callbacks, corrupt-packet recovery, close-inflight.")
print("PASS: multiple decoder callbacks overlapped on both codecs (includes dependency waits).")
print("PASS: blocked-decoder gate, input validation, empty drain, resolution changes, 10-bit and delayed intra frames.")
if baseline:
    print("PASS: pixel hashes and PTS match the saved, unmodified 7.0.3 baseline.")
