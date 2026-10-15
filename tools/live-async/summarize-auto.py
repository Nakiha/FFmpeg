#!/usr/bin/env python3
"""Performance and control summary for API 2; keep API 1's report separate."""
import csv
import html
import json
import pathlib
import statistics
import sys

root = pathlib.Path(sys.argv[1])
assert (root / "FUNCTIONAL_COMPLETE.json").exists()
assert (root / "PERFORMANCE_COMPLETE.json").exists()

def read(path):
    with path.open() as file:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(file)]

def p(values, percentage):
    values = sorted(values); index = (len(values) - 1) * percentage / 100
    low = int(index)
    return values[low] + (values[min(low + 1, len(values) - 1)] - values[low]) * (index - low)

def metrics(name):
    info = json.loads((root / f"{name}.json").read_text())
    assert not info["verify"], name
    rows = read(root / f"{name}-s0.csv")
    passes = read(root / f"{name}-s0.passes.csv")
    trace = read(root / f"{name}.control.csv")
    # Last-packet submission is earlier than EOF acceptance. Restrict to outputs
    # before that point as well as the fixed startup/tail exclusion window.
    last_submission = max(r["submitted_us"] for r in rows)
    steady = [r for i, r in enumerate(rows) if 32 <= i < len(rows)-48 and r["received_us"] < last_submission]
    assert len(steady) >= 48, name
    result = dict(first_ms=passes[0]["first_us"]/1000,
                  burst_fps=passes[0]["frames"]*1e6/passes[0]["elapsed_us"],
                  logical_cpu=info["cpu_us"]/info["elapsed_us"],
                  max_send_ms=info["max_send_us"]/1000, max_receive_ms=info["max_receive_us"]/1000,
                  observed_peak_target=max(t["peak_target"] for t in trace),
                  observed_peak_busy=max(t["busy"] for t in trace))
    for label, selected, field in (("packet", rows, "packet_latency_us"),
                                   ("steady_packet", steady, "packet_latency_us"),
                                   ("steady_arrival", steady, "arrival_latency_us")):
        for percentile in (50, 95, 99):
            result[f"{label}_p{percentile}_ms"] = p([r[field]/1000 for r in selected], percentile)
    return result

stems = [f"{codec}-{res}-b{b}" for res in ("1080p", "4k") for codec in ("h264", "hevc") for b in (3, 7)]
summary = []
for stem in stems:
    for profile in ("paced30", "stress120", "bulk"):
        for mode in ("fixed", "auto"):
            samples = [metrics(f"{stem}-{profile}-r{repeat}-{mode}") for repeat in range(1, 4 if profile == "bulk" else 2)]
            combined = {key: statistics.median(s[key] for s in samples) for key in samples[0]}
            combined.update(burst_fps_min=min(s["burst_fps"] for s in samples), burst_fps_max=max(s["burst_fps"] for s in samples))
            summary.append(dict(stem=stem, profile=profile, mode=mode, repeats=len(samples), **combined))
with (root / "performance.csv").open("w", newline="") as file:
    writer = csv.DictWriter(file, fieldnames=list(summary[0])); writer.writeheader(); writer.writerows(summary)
lookup = {(r["stem"], r["profile"], r["mode"]): r for r in summary}
steps = []
for stem in ("h264-1080p-b7", "hevc-4k-b7", "hevc-1080p-b3"):
    for mode in ("fixed", "auto"):
        name = f"{stem}-steps-{mode}"
        trace = read(root / f"{name}.control.csv")
        passes = read(root / f"{name}-s0.passes.csv")
        end_phase = len(passes) - 1
        late_high = [r["frames"]*1e6/r["elapsed_us"] for r in passes if r["phase"] in (end_phase-2,end_phase-1)]
        high = [r for r in trace if 1 <= r["phase"] < end_phase]
        low = [r for r in trace if r["phase"] == end_phase]
        # Report an explicit late observation window, not the middle of a
        # six-second phase that needs about five seconds to descend from 12.
        end = low[-1]["time_us"]
        settled = [r for r in low if end-1000000 <= r["time_us"] <= end-100000]
        steps.append(dict(stem=stem, mode=mode, late_high_fps=statistics.mean(late_high),
                          high_peak_target=max(r["target"] for r in high), end_low_target=low[-1]["target"],
                          steady_low_target=statistics.mode(r["target"] for r in settled),
                          steady_low_minimum_fraction=sum(r["target"] == 2 for r in settled)/len(settled),
                          late_low_window_ms=(settled[-1]["time_us"]-settled[0]["time_us"])/1000,
                          rollbacks=max(r["rollbacks"] for r in trace),
                          increases=max(r["increases"] for r in trace), decreases=max(r["decreases"] for r in trace),
                          high_fps_by_pass=[r["frames"]*1e6/r["elapsed_us"] for r in passes if 1 <= r["phase"] < end_phase]))
(root / "steps-summary.json").write_text(json.dumps(steps, indent=2) + "\n")

intro = """FFmpeg 7.0.3 API 2 自动调度测试

本报告对照同一套 libavcodec 61.5.100：固定 async 上限 12 与 auto（最低 2、
已分配 12、匿名共享预算 12）。官方原版/61.4.100 的 2–12 线程矩阵另见
results-load/report.html，不能把那组数字当作 auto 的实测值。

平台：Ryzen 7 9700X（8 核/16 逻辑处理器），Ubuntu 24.04 / WSL2，GCC 13.3。
只有性能测量进程运行；没有并行编译、编码或消毒器任务。性能轮无像素哈希、
像素拷贝或逐帧磁盘写出。压缩包预读、解封装、初始化和线程创建不计时。
30/120 fps 各一次；192 帧不限速短片各三次，中位数附 min/max。
单帧 packet：实际提交对应压缩包→完整帧返回；steady arrival：计划到达→帧，
含前台提交滞后。稳态窗口剔除首 32/末 48，并仅取最后一个包实际提交前的输出，
以保守排除 drain。以上不是网络→filter/编码的端到端时延。

短片 burst_fps 包含控制器扩张的启动成本，不能当作收敛后的持续容量。
另有两个 480 帧长样本：一次 30 fps 回放、四次连续不限速回放、一次 30 fps
回放；每轮完整 drain/flush，保留控制状态与计数。高压力后两轮均值用于观察
扩张后的吞吐，同时给出各轮数值。另加 HEVC 1080p B=3 的 192 帧八次高压回放，
观察非单调线程扩展配置。它仍是有限合成负载，不是长期容量保证。
target 是在途任务许可，busy 含 setup/参考等待；都不是物理 CPU 核数。
logical_cpu 是全进程 CPU 时间/墙钟时间，单位为逻辑核当量。

功能证据：原版 n7.0.3 的整帧 SHA256 和元数据对照，预算 1/2/4/12，多 owner、
下游阻塞、API 参数、并发默认预算生命周期、独立预算句柄、跨进程额度排他、
容量冲突拒绝、SIGKILL 后额度回收、两个进程并发以及长样本正确性。
Linux ASan/Leak 为独立禁 asm 构建；Windows 支持进程内自动模式，命名预算
仅支持 Linux 同 UID/共享 /dev/shm。TSan 未验证；ASan 不是竞态证明。

下面每对数值均为“固定 async 12 → auto”。时延 ms，吞吐 fps。
"""
text = [intro]
page = ["<!doctype html><html lang=zh-CN><meta charset=utf-8><title>FFmpeg 自动调度测试</title>",
        "<style>body{font:15px system-ui;margin:32px;line-height:1.65;color:#172032}pre{white-space:pre-wrap;max-width:1050px}table{border-collapse:collapse;font-size:13px}td,th{padding:8px;border:1px solid #cbd3df;text-align:right}th{background:#edf2f8}img{max-width:1100px;width:100%;height:auto}</style>",
        "<h1>FFmpeg 自动调度测试</h1><pre>" + html.escape(intro) + "</pre>",
        "<h2>192 帧完整对照</h2><table><tr><th>样本</th><th>30fps 首帧</th><th>单帧 p50 / p95</th><th>稳态到达 p95</th><th>短片不限速 fps</th><th>auto 短片 fps 范围</th><th>auto 短片峰值目标</th></tr>"]
for stem in stems:
    def pair(profile, key):
        return f"{lookup[(stem, profile, 'fixed')][key]:.2f} → {lookup[(stem, profile, 'auto')][key]:.2f}"
    a = lookup[(stem,"bulk","auto")]
    fields = [stem, pair("paced30","first_ms"), pair("paced30","packet_p50_ms") + " / " + pair("paced30","packet_p95_ms"),
              pair("paced30","steady_arrival_p95_ms"), pair("bulk","burst_fps"),
              f"{a['burst_fps_min']:.1f}–{a['burst_fps_max']:.1f}", str(a["observed_peak_target"])]
    text.append(" | ".join(fields)); page.append("<tr>" + "".join("<td>"+html.escape(v)+"</td>" for v in fields)+"</tr>")
page.append("</table><h2>低→高→低压力过程</h2><pre>")
text.append("\n低→高→低压力过程：")
for r in steps:
    line = (f"{r['stem']} {r['mode']}: 后两轮高压均值 {r['late_high_fps']:.2f} fps；"
            f"高压峰值目标 {r['high_peak_target']}，低压末段目标 {r['steady_low_target']}"
            f"（观察窗 {r['late_low_window_ms']:.0f}ms，最低目标占比 {r['steady_low_minimum_fraction']:.0%}，最后快照 {r['end_low_target']}）；"
            f"回退 {r['rollbacks']} 次；各轮高压 fps=" + ", ".join(f"{v:.1f}" for v in r["high_fps_by_pass"]))
    text.append(line); page.append(html.escape(line)+"\n")
page.append('</pre><p>可选图：<a href="adaptive-control.png">并行度轨迹</a>、<a href="adaptive-throughput.png">高压各轮吞吐</a>。</p></html>')
(root / "report-zh.txt").write_text("\n".join(text)+"\n", encoding="utf-8")
(root / "report.html").write_text("\n".join(page), encoding="utf-8")
print("PASS auto summary and independent fixed/auto comparisons")
