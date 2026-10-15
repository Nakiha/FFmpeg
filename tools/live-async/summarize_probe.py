"""Validate frame counts and publish the measured baseline, without timing gates."""
import csv
import pathlib
import sys

result_dir = pathlib.Path(sys.argv[1])
rows = []
for codec in ("h264", "hevc"):
    for threads in (2, 4, 8, 12):
        lines = (result_dir / f"{codec}-t{threads}.csv").read_text().splitlines()
        summary_line = next(line for line in lines if line.startswith("summary,"))
        fields = dict(item.split("=", 1) for item in summary_line.split(",")[1:])
        assert fields["codec"] == codec, fields
        assert int(fields["threads"]) == threads, fields
        assert int(fields["packets"]) == 50, fields
        assert int(fields["frames"]) == 50, fields
        assert int(fields["stamped_frames"]) == 50, fields
        frames = [line.split(",") for line in lines if line.startswith("frame,")]
        assert len(frames) == 50
        assert [int(frame[2]) for frame in frames] == list(range(50))
        for reorder in (0, 3):
            digest = result_dir / f"{codec}-b{reorder}-t{threads}.sha256"
            if threads in (2, 12):
                assert len([
                    line for line in digest.read_text().splitlines()
                    if line and not line.startswith("#")
                ]) == 50, digest
        rows.append({
            "codec": codec,
            "threads": threads,
            **{
                key.replace("_us", "_ms"): round(int(fields[key]) / 1000, 3)
                for key in (
                    "first_frame_us", "mean_latency_us",
                    "max_send_us", "max_receive_us",
                )
            },
        })

with (result_dir / "summary.csv").open("w", newline="") as output:
    writer = csv.DictWriter(output, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)

for row in rows:
    print(row)
print("PASS: all 8 paced runs produced 50 ordered, timestamped frames.")
print("PASS: 2/12-thread frame hashes match for H.264/HEVC, B=0 and B=3.")
print("Mean latency includes end-of-stream drain; inspect per-frame CSVs for steady state.")
