#!/usr/bin/env python3
"""Summarize performance-only runs; never use correctness/hash runs for timing."""
import argparse
import csv
import html
import json
import pathlib
import statistics

parser = argparse.ArgumentParser()
parser.add_argument("results", type=pathlib.Path)
args = parser.parse_args()
root = args.results
bulk_repeats = json.loads((root / "environment.json").read_text())["bulk_repeats"]

def quantile(values, p):
    values = sorted(values)
    index = (len(values) - 1) * p
    low = int(index)
    return values[low] + (values[min(low + 1, len(values) - 1)] - values[low]) * (index - low)

def analyze(path):
    info = json.loads(path.read_text())
    with path.with_suffix(".csv").open() as file:
        rows = [{k: int(v) for k, v in row.items()} for row in csv.DictReader(file)]
    # Same fixed presentation-order window for both implementations. Explicitly
    # remove EOF drain as well, even if queue sizes or codec reordering change.
    steady = [r for r in rows if 32 <= r["output"] < len(rows) - 48
              and r["received_us"] < info["eof_at_us"]]
    assert len(steady) >= 48, (path, "too few steady-state samples", len(steady))
    out = dict(first_ms=info["first_us"] / 1000, frames=info["frames"],
               fps=info["frames"] * 1e6 / info["elapsed_us"],
               steady_fps=(len(steady) - 1) * 1e6 / (steady[-1]["received_us"] - steady[0]["received_us"]),
               effective_cpu_cores=info["cpu_us"] / info["elapsed_us"],
               cpu_ms_per_frame=info["cpu_us"] / info["frames"] / 1000,
               max_send_ms=info["max_send_us"] / 1000, max_receive_ms=info["max_receive_us"] / 1000,
               peak_active_workers=info["peak_active_workers"], peak_inflight=info["peak_inflight"],
               send_eagain=info["send_eagain"], steady_frames=len(steady),
               steady_arrival_growth_ms=(statistics.mean(r["arrival_latency_us"] for r in steady[-16:]) -
                                         statistics.mean(r["arrival_latency_us"] for r in steady[:16])) / 1000)
    for name, selected, key in (("packet", rows, "packet_latency_us"),
                                 ("steady_packet", steady, "packet_latency_us"),
                                 ("steady_arrival", steady, "arrival_latency_us")):
        values = [row[key] / 1000 for row in selected]
        out[f"{name}_mean_ms"] = statistics.mean(values)
        for p in (50, 95, 99):
            out[f"{name}_p{p}_ms"] = quantile(values, p / 100)
    out["submit_lateness_p95_ms"] = quantile([(r["submitted_us"] - r["scheduled_us"]) / 1000 for r in rows], .95)
    return out

fixtures = [json.loads(path.read_text()) for path in sorted(root.glob("*-fixture.json"))]
all_rows = []
aggregate = []
for fixture in fixtures:
    stem = fixture["stem"]
    # Every configuration must have a complete pixel comparison with the frozen
    # original as well as successful completion markers for performance runs.
    reference = (root / f"{stem}-golden.sha256").read_bytes()
    assert len(reference.splitlines()) == fixture["frames"]
    for threads in range(2, 13):
        for version in ("original", "async"):
            pixel = root / f"{stem}-t{threads:02}-verify-r1-{version}.sha256"
            assert pixel.read_bytes() == reference, pixel
            for profile in ("paced30", "stress120", "bulk"):
                paths = sorted(root.glob(f"{stem}-t{threads:02}-{profile}-r*-{version}.json"))
                expected = bulk_repeats if profile == "bulk" else 1
                assert len(paths) == expected, (stem, threads, profile, version, len(paths))
                measurements = []
                for path in paths:
                    assert path.with_suffix(".ok").exists(), path
                    measurements.append(analyze(path))
                    all_rows.append(dict(stem=stem, codec=fixture["codec"], resolution=fixture["resolution"],
                                         bframes=fixture["bframes"], threads=threads, version=version,
                                         profile=profile, run=path.stem, **measurements[-1]))
                combined = {key: statistics.median(m[key] for m in measurements) for key in measurements[0]}
                combined.update(fps_min=min(m["fps"] for m in measurements), fps_max=max(m["fps"] for m in measurements),
                                peak_active_workers_max=max(m["peak_active_workers"] for m in measurements))
                aggregate.append(dict(stem=stem, codec=fixture["codec"], resolution=fixture["resolution"],
                                      bframes=fixture["bframes"], threads=threads, version=version,
                                      profile=profile, repeats=len(measurements), **combined))

def save_csv(path, records):
    with path.open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(records[0]))
        writer.writeheader(); writer.writerows(records)

save_csv(root / "runs.csv", all_rows)
save_csv(root / "summary.csv", aggregate)
lookup = {(r["stem"], r["threads"], r["profile"], r["version"]): r for r in aggregate}
comparisons = []
for fixture in fixtures:
    for threads in range(2, 13):
        stem = fixture["stem"]
        out = dict(stem=stem, threads=threads, pixels="PASS", frames=fixture["frames"])
        for version in ("original", "async"):
            for profile in ("paced30", "stress120", "bulk"):
                r = lookup[(stem, threads, profile, version)]
                for key in ("first_ms", "packet_p50_ms", "packet_p95_ms", "steady_packet_p50_ms",
                            "steady_packet_p95_ms", "steady_arrival_p50_ms", "steady_arrival_p95_ms",
                            "steady_arrival_growth_ms", "fps", "steady_fps", "effective_cpu_cores",
                            "cpu_ms_per_frame", "max_send_ms", "max_receive_ms", "peak_active_workers_max",
                            "fps_min", "fps_max"):
                    out[f"{profile}_{version}_{key}"] = r[key]
        out["bulk_async_vs_original"] = out["bulk_async_fps"] / out["bulk_original_fps"]
        for version in ("original", "async"):
            out[f"bulk_{version}_vs_t2"] = (out[f"bulk_{version}_fps"] /
                lookup[(stem, 2, "bulk", version)]["fps"])
        comparisons.append(out)
save_csv(root / "comparison.csv", comparisons)

intro = """本机 FFmpeg 7.0.3 多帧异步解码压力测试

对照：冻结的官方 n7.0.3 libavcodec 61.3.100；改造：665d09b，61.4.100。
平台：Ubuntu 24.04 / WSL2，Ryzen 7 9700X，8 核 16 逻辑处理器。
8 种固定种子合成样本：1920x1080 / 3840x2160，H.264 / HEVC，连续 B=3/7。
每段 192 帧，源时间基准 30 fps、8-bit YUV420P、medium/CRF20；testsrc2 加动态噪声。
实际 B 帧连续长度由原版解码器逐帧探测确认；细节、码率与文件 SHA256 见 fixture JSON。
2–12 线程每个整数均测；帧线程；异步输入/输出队列容量各 12；单路、串行运行。
192 帧是短期容量/排队测试，不能代表长期热稳定性或生产多路直播负载。

计时边界：压缩包预读、解封装/探测/打开/线程创建完成后，首次送包前起表。
首帧：起表到成功取得第一帧；非通知/入队耗时。
单帧 packet_latency：该帧对应压缩包实际提交到完整 AVFrame 返回；包含库内排队、
依赖和显示顺序等待，并非该帧自身 CPU 计算耗时；p50/p95 汇总所有帧，含首尾。
稳态：输出序号 [32,144)，且返回早于接受 EOF；固定窗口剔除启动和 drain。
steady_packet：稳态的实际送包→出帧；steady_arrival：计划到达→出帧，额外包含
应用送包滞后/背压。压力过载时 arrival 延迟不断增长，不应声称存在真正稳定稳态。
30 fps 为实时供包；120 fps 为压力供包。B 帧按 MP4 解封装的解码顺序等间隔供应，
未模拟网络抖动、源端编码/编码重排、播放 PTS 时钟、filter、编码或复用。
完整像素 SHA256 校验单独运行；所有性能轮禁止拷贝/哈希/逐帧磁盘写出。
bulk：不限速送包，3 次独立进程，报告中位数和 min/max；总吞吐包含填充和 drain。
steady_fps 仅中段输出速率；有效 CPU 核数 = 全进程 CPU 时间 / 测量墙钟时间，
单位是逻辑核当量，不是物理核心数量（本机 8 个物理核心、16 个逻辑处理器）。
peak_active_workers 是同时进入解码回调的峰值，含参考等待，不能单独证明 CPU 并行。
并行优势应结合 bulk 吞吐随线程数的变化及有效 CPU 核数判断。
未测原版回调峰值（记 0 代表无该统计，不代表无并行），没有额外修改原版库。
所有线程/编码/分辨率/B 配置均独立进行整段像素 SHA256、PTS、best_effort、DTS、
duration、格式、尺寸、色彩、SAR 和对应输入包序号校验；性能轮也校验输出元数据。
逐帧原始记录、各轮汇总、环境、生成参数和日志一并保留。

下面每对数值均为“原版 → 异步改造版”。时间 ms，吞吐 fps。
"""
intro = intro.replace("3 次独立进程", f"{bulk_repeats} 次独立进程")
at12 = [r for r in comparisons if r["threads"] == 12]
worst = sorted(comparisons, key=lambda r: r["bulk_async_vs_original"])[:2]
intro += (f"\n12 线程八种样本吞吐中位数的相对变化范围："
          f"{(min(r['bulk_async_vs_original'] for r in at12)-1)*100:+.1f}% 至 "
          f"{(max(r['bulk_async_vs_original'] for r in at12)-1)*100:+.1f}%。\n"
          "低线程退化项必须单独保留，不能从 12 线程结果推广到所有线程数：\n")
for r in worst:
    intro += (f"  {r['stem']} / {r['threads']} 线程：{r['bulk_original_fps']:.1f} → "
              f"{r['bulk_async_fps']:.1f} fps；三轮范围见 CSV/阴影。原因未单独定位。\n")
text = [intro]
page = ["<!doctype html><html lang=zh-CN><meta charset=utf-8><title>FFmpeg 7.0.3 压力测试</title>",
        "<style>body{font:15px system-ui;margin:32px;line-height:1.6;color:#172032}pre{white-space:pre-wrap;max-width:1000px}table{border-collapse:collapse;font-size:13px;margin-bottom:32px}th,td{padding:7px 10px;border:1px solid #ccd4df;text-align:right}th{background:#edf2f8}h2{margin-top:40px}a{color:#1764b7}</style>",
        "<h1>FFmpeg 7.0.3 多帧异步解码压力测试</h1><pre>" + html.escape(intro) + "</pre>",
        '<p><a href="comparison.csv">全部对照数据 CSV</a> · <a href="summary.csv">各配置汇总 CSV</a> · <a href="runs.csv">各轮数据 CSV</a></p>']
page += ['<p>可选绘图：运行 plot-load.py 后，<a href="throughput.png">吞吐曲线</a>、'
         '<a href="latency-30fps.png">实时供包时延曲线</a>、'
         '<a href="pressure-120fps.png">压力供包时延曲线</a>、'
         '<a href="cpu-parallelism.png">CPU 并行曲线</a>可用于对比线程扩展效果。</p>']
for fixture in fixtures:
    stem = fixture["stem"]
    text += ["\n" + stem, "线程 | 30fps首帧 | 单帧p50/p95 | 稳态到达p50/p95 | bulk吞吐 | 吞吐比 | async逻辑核当量 | 120fps稳态到达p95"]
    page += [f"<h2>{html.escape(stem)}</h2><table><tr><th>线程</th><th>30fps 首帧 ms</th><th>单帧 p50 / p95 ms</th><th>30fps 稳态到达 p50 / p95 ms</th><th>bulk fps</th><th>异步/原版</th><th>异步逻辑核当量</th><th>120fps 中段到达 p95 ms</th></tr>"]
    for r in (row for row in comparisons if row["stem"] == stem):
        def pair(profile, key):
            return f"{r[f'{profile}_original_{key}']:.2f} → {r[f'{profile}_async_{key}']:.2f}"
        packet = pair("paced30", "packet_p50_ms") + " / " + pair("paced30", "packet_p95_ms")
        steady = pair("paced30", "steady_arrival_p50_ms") + " / " + pair("paced30", "steady_arrival_p95_ms")
        fields = [str(r["threads"]), pair("paced30", "first_ms"), packet, steady,
                  pair("bulk", "fps"), f"{r['bulk_async_vs_original']:.3f}×",
                  f"{r['bulk_async_effective_cpu_cores']:.2f}", pair("stress120", "steady_arrival_p95_ms")]
        text.append(" | ".join(fields))
        page.append("<tr>" + "".join("<td>" + html.escape(field) + "</td>" for field in fields) + "</tr>")
    page.append("</table>")
page.append("<p>正确性：全部 88 对配置的完整像素和元数据一致；所有性能轮元数据一致。</p></html>")
(root / "report-zh.txt").write_text("\n".join(text) + "\n", encoding="utf-8")
(root / "report.html").write_text("\n".join(page), encoding="utf-8")
print(f"PASS: {len(comparisons)} configurations, {len(all_rows)} performance runs; reports saved in {root}")
