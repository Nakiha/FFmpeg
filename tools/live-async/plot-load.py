#!/usr/bin/env python3
"""Optional publication/export plots; requires matplotlib, not needed to test."""
import csv
import json
import pathlib
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = pathlib.Path(sys.argv[1])
repeats = json.loads((root / "environment.json").read_text())["bulk_repeats"]
with (root / "summary.csv").open() as file:
    rows = list(csv.DictReader(file))
stems = [f"{codec}-{resolution}-b{bframes}" for resolution in ("1080p", "4k")
         for codec in ("h264", "hevc") for bframes in (3, 7)]
colors = {"original": "#2764a4", "async": "#d06a16"}
labels = {"original": "Original 7.0.3", "async": "Async fork"}

def selected(stem, profile, version):
    return sorted([r for r in rows if r["stem"] == stem and r["profile"] == profile
                   and r["version"] == version], key=lambda r: int(r["threads"]))

for name, title, profile, keys, ylabel in (
    ("throughput", f"Unlimited-input decoding throughput (median of {repeats} runs)", "bulk",
     [("fps", "", "-")], "Frames / second"),
    ("latency-30fps", "30 fps input: first output and steady arrival-to-frame p95", "paced30",
     [("first_ms", " first output", "-"), ("steady_arrival_p95_ms", " steady p95", "--")], "Milliseconds"),
    ("pressure-120fps", "120 fps input: middle-window arrival latency p95", "stress120",
     [("steady_arrival_p95_ms", "", "-")], "Milliseconds"),
    ("cpu-parallelism", "Unlimited input: process CPU time / wall time", "bulk",
     [("effective_cpu_cores", "", "-")], "Logical CPU time equivalents"),
):
    fig, axes = plt.subplots(4, 2, figsize=(12, 13), layout="constrained")
    fig.suptitle(title + "\nRyzen 7 9700X / WSL2; 192-frame seeded noisy YUV420P samples", fontsize=14)
    for ax, stem in zip(axes.flat, stems):
        for version in ("original", "async"):
            values = selected(stem, profile, version)
            x = [int(r["threads"]) for r in values]
            for key, suffix, style in keys:
                ax.plot(x, [float(r[key]) for r in values], style, marker="o", markersize=3,
                        color=colors[version], label=labels[version] + suffix)
            if name == "throughput":
                ax.fill_between(x, [float(r["fps_min"]) for r in values],
                                [float(r["fps_max"]) for r in values], color=colors[version], alpha=.12)
        if name == "pressure-120fps":
            ax.set_yscale("symlog", linthresh=50)
        ax.set_title(stem.replace("hevc", "HEVC").replace("h264", "H.264"))
        ax.set_xticks(range(2, 13)); ax.set_xlabel("Decoder frame threads"); ax.set_ylabel(ylabel)
        ax.grid(alpha=.2)
        ax.legend(fontsize=8)
    fig.savefig(root / f"{name}.png", dpi=160)
    fig.savefig(root / f"{name}.pdf")
    plt.close(fig)
