#!/usr/bin/env python3
import csv
import pathlib
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

root = pathlib.Path(sys.argv[1])
def read(path):
    with path.open() as file: return [{k:int(v) for k,v in r.items()} for r in csv.DictReader(file)]
stems = ("h264-1080p-b7", "hevc-4k-b7", "hevc-1080p-b3")
fig, axes = plt.subplots(3,1,figsize=(12,11),layout="constrained")
for ax, stem in zip(axes, stems):
    trace = read(root / f"{stem}-steps-auto.control.csv")
    frames = read(root / f"{stem}-steps-auto-s0.csv")
    passes = read(root / f"{stem}-steps-auto-s0.passes.csv")
    ax.step([r["time_us"]/1e6 for r in trace], [r["target"] for r in trace], where="post", label="Target parallelism", color="#15805e")
    ax.plot([r["time_us"]/1e6 for r in trace], [r["busy"] for r in trace], label="Unfinished tasks (includes waits)", color="#8796a9", alpha=.55, linewidth=.7)
    for phase in range(1,len(passes)-1):
        first = next(r for r in frames if r["phase"] == phase)
        begin = (first["received_us"] - passes[phase]["first_us"])/1e6
        end = begin + passes[phase]["elapsed_us"]/1e6
        ax.axvspan(begin,end,color="#e4a647",alpha=.16,label="Unlimited input" if phase==1 else None)
    ax.set_title(stem); ax.set_xlabel("Wall-clock seconds"); ax.set_ylabel("Frame tasks")
    ax.set_yticks(range(0,13,2)); ax.grid(alpha=.2); ax.legend(loc="upper right")
fig.suptitle("Automatic dispatch: 30 fps → unlimited input → 30 fps\n12 allocated workers, minimum 2, shared budget 12")
fig.savefig(root/"adaptive-control.png",dpi=160); fig.savefig(root/"adaptive-control.pdf"); plt.close(fig)
fig, axes = plt.subplots(3,1,figsize=(12,11),layout="constrained")
for ax, stem in zip(axes, stems):
    for mode, offset, color in (("fixed",-.18,"#2764a4"),("auto",.18,"#15805e")):
        passes = read(root/f"{stem}-steps-{mode}-s0.passes.csv")
        selected = [r for r in passes if r["fps"] == 0]
        ax.bar([r["phase"]+offset for r in selected], [r["frames"]*1e6/r["elapsed_us"] for r in selected],
               width=.36,label=mode,color=color)
    ax.set_title(stem); ax.set_xlabel("Unlimited-input replay"); ax.set_ylabel("Frames / second"); ax.set_xticks(range(1,len(selected)+1)); ax.legend(); ax.grid(axis="y",alpha=.2)
fig.suptitle("High-pressure replays: fixed 12 vs adaptive 2–12\n480 frames/replay for B7 samples; 192 for HEVC 1080p B3")
fig.savefig(root/"adaptive-throughput.png",dpi=160); fig.savefig(root/"adaptive-throughput.pdf")
