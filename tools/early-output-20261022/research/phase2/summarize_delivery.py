from pathlib import Path
import json, statistics, hashlib

p = Path(__file__).resolve().parents[2]
q = p / 'out/phase2'
rows = json.loads((q / 'performance/results.json').read_text())
keys = list(dict.fromkeys((x['source_folder'], x['file']) for x in rows))
medians = []
for folder, file in keys:
    for mode, fps in [('legacy',60), ('async',60), ('legacy',0)]:
        for early in [0,1]:
            runs = [x for x in rows if (x['source_folder'],x['file'],x['mode'],x['fps'],x['early']) == (folder,file,mode,fps,early)]
            result = dict(folder=folder,file=file,mode=mode,fps=fps,early=early,repeats=len(runs))
            for k in ['submit_latency_mean_ms','planned_latency_mean_ms','first_frame_ms','throughput_fps','codec_api_cpu_ms_per_frame','max_outstanding_at_output']:
                result[k] = statistics.median(x[k] for x in runs) if runs[0][k] is not None else None
            medians.append(result)
(q/'performance/medians.json').write_text(json.dumps(medians,indent=2)+'\n')
def find(folder,file,mode,fps,early):
    return next(x for x in medians if (x['folder'],x['file'],x['mode'],x['fps'],x['early'])==(folder,file,mode,fps,early))
text = ['# 固定构建量化','', '162次均returncode0。延时均值格先对每轮全部显示帧算mean，再取3次交错重复的中位数；不是逐帧P50。首帧格是每轮first-frame的3次中位数；CPU/吞吐格同样先按轮计算再取中位数。完整命令/环境及单次结果见 out/phase2/performance/results.json。此处关闭/开启均为同一phase2二进制，不含文件证书。','',
        '## AU提交到对应显示帧：60fps、2解码线程','',
        '|输入|同步 off→on 均值ms|async off→on 均值ms|async计划ready→帧 off→on ms|async首帧 off→on ms|',
        '|---|---:|---:|---:|---:|']
for folder,file in keys:
    a,b = [find(folder,file,'legacy',60,e) for e in [0,1]]
    c,d = [find(folder,file,'async',60,e) for e in [0,1]]
    label = ('HRD ' if folder=='hrd-targets' else '')+file
    def pair(x,y,k): return f"{x[k]:.3f}→{y[k]:.3f}"
    text.append('|'+label+'|'+pair(a,b,'submit_latency_mean_ms')+'|'+pair(c,d,'submit_latency_mean_ms')+'|'+pair(c,d,'planned_latency_mean_ms')+'|'+pair(c,d,'first_frame_ms')+'|')
text += ['', 'H264启动等待保留；HEVC首IDR走已有快出。普通无HRD控制片全回退，on/off微差是时钟/调度噪声。outstanding-at-output上层proxy最大值同步3、async2，均不随early开关下降；不是DPB内存计量。', '',
         '## 单线程无pace：codec调用CPU与吞吐','',
         '|输入|调用CPU off→on ms/frame|吞吐 off→on fps|', '|---|---:|---:|']
for folder,file in keys:
    a,b = [find(folder,file,'legacy',0,e) for e in [0,1]]
    text.append(f"|{'HRD ' if folder=='hrd-targets' else ''}{file}|{a['codec_api_cpu_ms_per_frame']:.6f}→{b['codec_api_cpu_ms_per_frame']:.6f}|{a['throughput_fps']:.1f}→{b['throughput_fps']:.1f}|")
text += ['', '单线程计时包含send/receive wrapper和时钟开销；不含像素hash，循环5次。未看到一致CPU降低，不把等待缩短称为少算。320x180结果不外推1080p或生产容量。async进程CPU原值另存，包含driver/demux/open，不当纯解码CPU。输入文件预载，codec测量窗不含网络、文件open/probe及屏幕显示。', '']
(p/'research/phase2/MEASUREMENTS.md').write_text('\n'.join(text))
acceptance = {
    'status':'research_prototype_not_safety_accepted',
    'scope':'H264 in-band HRD/CFR automatic prototype and external verified file mode; independent H264/HEVC application initialization switch',
    'automatic_legal_stream_theorem':'Supported progressive same-CVS HRD/CFR consecutive two-tick output-slot proof independently reviewed; no recent-POC heuristic or future scan.',
    'release_blockers':[
        {'id':'F1','priority':'P1','reason':'Current AU known SEI errors/late or contradictory SEI need not invalidate speculative extra DPB selection before publication; complete rollback not implemented.', 'evidence_kind':'independent source review with minimal byte recipes; new malformed fixtures not executed'},
        {'id':'F2','priority':'P2','reason':'picture_timing present does not guarantee payload length meets SPS-declared CPB/DPB field widths.', 'evidence_kind':'independent source review; short-payload recipe not executed'}
    ],
    'normal_matrix':{'certified_h264_existing_hevc':280,'automatic_hrd_h264':140,'ordered_display_positions':241920,'all_pixels_count_order_pts_other_recorded_metadata_match':True,'early_off_all_fields_exact':210,'early_on_only_trigger_pkt_dts_differences':210,'independently_recomputed_raw_sha256':True},
    'fallback_matrix':{'comparisons':170,'exact':165,'known_error_flag_race_comparisons':5,'note':'lenient first-IDR truncation decode_error_flags 0/12 variation previously reproduced on old baseline; new 20-run ledger matches strict status/0 frames and lenient 1 frame/0 flags'},
    'hrd_boundary_pairs':35,'lifecycle_pairs':20,'performance_runs':len(rows),'performance_failures':sum(x['returncode']!=0 for x in rows),
    'four_combinations_real_codec_decode':True,'full_linux_service_run':False,
    'ordinary_without_hrd_automatic':'fallback; not a solution for arbitrary unconstrained normal H264 live sources',
    'external_certificate_mode':'offline exact-input assertion only; application never asserts it; not counted as live latency gain',
    'asan':'two 12-thread normal/wrap/inflight-flush runs, three cycles each; no ASan errors; LeakSanitizer disabled; no TSan',
    'non_idr_buffering_period_fixture':False,
    'independent_review':'research/phase2/INDEPENDENT_REVIEW.md',
    'build_identity':'out/phase2/build-identity.json',
    'source_patches':['research/phase2/ffmpeg-early-output.patch','research/phase2/application-switch.patch'],
    'pushed':False,'merged':False,'production_defaults_changed':False,'sdk_pin_changed':False,'media_uploaded':False,
    'phase1_rejected_candidate_unchanged':True,
    'environment_blocker':'Transient exec-server transport outage recovered; postchecks and exports confirmed complete; no pending/denied/uncertain local action remains.',
    'remaining_action':'Whole-AU fail-closed eligibility or proven rollback before extra selection/publication; targeted malformed/contradictory SEI and non-IDR BP coverage; rebuild/re-review and Linux service integration before release.'
}
(q/'acceptance.json').write_text(json.dumps(acceptance,indent=2)+'\n')
print('wrote measurement medians/report and explicit safety rejection')
