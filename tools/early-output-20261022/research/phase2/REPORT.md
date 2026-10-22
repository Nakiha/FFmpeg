> **实验版，最终验收未完成，不建议生产启用**
> 历史文档归档：正文中的“当前”、未推送状态及验收结论仅属于原阶段；以 `doc/early-output-20261022/REPORT.md` 的版本核对为准。`out/` 路径指本机保留证据，不随公开分支发布。

# H.264/H.265 提前交付：第二阶段本地交付（2026-10-22）

本阶段证实：在明确的逐行 HRD/CFR 约束下，H.264 常规 B3、B7、参考 B、open GOP 和动态 B 可以减少 DPB 等待。实现和独立开关已落地并实际运行。它不是对所有普通 H.264 输入都有效的自动模式：没有所需 timing 声明的普通 x264 输入全部回退。当前源码仍是**未获完整安全验收的研究原型**，独立审查发现当前 AU 已可见的异常 SEI 尚未在额外选择前全部 fail closed；这与两个 tick 的合法流证明是否成立是两件事。不得按正常矩阵通过就把这个版本部署到任意来源输入。

没有推送、合并、修改 SDK pin、安装全局二进制或修改生产默认。样片仅保存在本机。第一阶段 POC2 候选仍被否决，旧 acceptance.json 保持不变。

## 身份与范围

隔离目录为本报告上两级的 task-2；两个 fresh clone 未复用其他代理工作区。应用 main/HEAD 为 `67f5e07798575e20e1f3fe1dcc6e37b40f3d64fa`，其 FFmpeg gitlink 为 `94bd940f1c4682dfce13e8ae43891df01621e16e`；目标 FFmpeg 在该 pin detached HEAD。远端 FFmpeg 默认是 master（检查时 `e9b3c55fe0321915a70f44f30b276ef2afdc3a0c`），没有把用户所说 main 当已存在分支。目标构建是 Apple clang 21、arm64、libavcodec 63.4.108；Homebrew 8.1.2 仅用于编码和独立像素 oracle。

源码线索逐一核实：26dee063 是 n9 异步 receive 移植，8eabad8e 是 next_pkt/flush 处理，42dd642a 是 HEVC IDR_N_LP 选择，94bd940f 是 HEVC 连续 POC 前缀。应用 3c9ba3c/a0bfa0c 是接入，bb4ebf6 包含更广媒体/质量控制，25e6c28 是编码 yield/CPU 审计；本轮没有恢复旧编码 tick-tock 工作。详细提交证据在 `../../out/evidence/verified-commits.json`。

`../../out/phase2/build-identity.json` 记录三个构建的配置、dylib/probe SHA256、链接目标、编译器、patch SHA256。`ffmpeg-early-output.patch` 与 `application-switch.patch` 对应当前未提交源码；被否决的早期 HRD v1 patch 单独保留，不混入最终固定构建结果。

## 两种证明来源和直播限制

1. **自动 HRD/CFR**：无需未来 GOP 或全片扫描。限定软件 Main/High、POC0、frame_mbs_only、上下场 POC 相同、PPS 无 bottom POC offset、无 film grain、whole AU/default discard、有有效 picture PTS；timing_info/fixed_frame_rate=1、pic_struct absent、单 CPB NAL HRD、非 VCL HRD、low_delay_hrd=0、合法延迟位宽和当前 picture-timing。先由旧 DPB 规则选出 IDR anchor，保留启动等待。C.2.2/C-12 给出 output tick=removal tick+dpb_output_delay；E.2.1/E-78/Table E-6 在此范围约束连续输出恰隔两 tick；D.2.3 要求 timing 与 POC 顺序一致。因此最小待输出 POC 恰在下一 tick 槽时可以扩展前缀，未来符合声明的画面不能插入该槽。奇数 POC 专项也通过，这不是偶数 POC 经验规律。CPB 回绕、IDR 新 serial、非 IDR buffering-period 的 C-8/C-9 origin 有实现；缺失 timing、重复 counter、MMCO5、参数内容变化、局部 POC/timing 矛盾会禁用。

2. **外部 exact-input 文件证明**：`live_poc_verified_even` 是独立、默认 false 的 caller assertion，不是自动开关。verify_contract.py 扫全片所有 slice header（含非首 slice MMCO），检查逐行 POC0、每 IDR 段唯一非负偶数 POC、稳定参数、无 MMCO5、相容 PTS；证书绑定 extradata/有序 packet bytes/PTS/DTS/duration。probe 只在实际预载 packet hash 匹配后设置 assertion；错误文件证书在解码前回退。此模式需要整片可用，**不计为直播收益**。应用不设置该 assertion；不能凭 x264 标识、历史采样或用户打开快出开关合成证明。头部检查也不认证 entropy/pixel 正确。

上述自动定理以码流遵守其当前规范声明为前提，并不是完整未来码流 conformance validator。已知当前 AU 守卫缺口及未覆盖分支见独立审查。官方依据是 [H.264 06/2026](https://www.itu.int/rec/T-REC-H.264-202606-I/en)，下载 PDF/文本与 hash 已本地保存；报告为推导摘要，不摘录规范大段原文。

实现将 H.264 decode callback 包装为 receive-frame，允许一个 AU 释放多帧；有界 H264Picture refs/私有 AVFrame FIFO 保存非参考 B 等已选帧，NAL 和 field_end 完成后交付，不跳过滤波/重建。原 n9 worker 和 async retirement 顺序保留。async 解决“已完成结果如何收到”，这里处理“DPB 是否可提前选择”，不能把两项收益混称 CPU 降低。

## 独立开关

应用新增可选布尔 `decoder_early_output`，在 avcodec_open2 前设置，独立于 async owner/frontend pump。省略使用内部 -1 sentinel，保留原默认 HEVC 开、H264 关；显式 false 同时关闭 HEVC 的 live_idr/live_poc 并让 H264 用原 DPB；显式 true 只请求受安全条件限制的路径。旧 pinned SDK 没有 H264 option 时回退，不报假命中。HEVC 原两个底层开关对应用合并为一个策略开关；没有第二个应用 assertion 开关。

这是初始化配置，不承诺热切换。当前应用并无 async-off 配置，decoder_pump 只是前端派发，不能冒充 async 开关。同步/异步 × early off/on 四组合在真实 codec decode 跑过；配置/default/type/fingerprint/四组合初始化测试在 prototype 和旧 pinned SDK 都通过。完整服务是 Linux 目标，本机 Mac 没有构建/启动全服务，不能声称已完成线上集成验收。

## 样片和逐帧回归

普通目标 28 文件（x264/x265 各14）：fixed3/fixed7 × pyramid0/1 × open0/1；B-dominant16、adaptive7、adaptive16 × open0/1。另14 个 x264 HRD 输入覆盖同结构，编码显式增加 `force-cfr=1:nal-hrd=vbr:vbv-maxrate=1200:vbv-bufsize=2400`，实际头部已检查 fixed=1、NAL HRD=1、low_delay=0、pic_struct=0、singleCPB。不能把这一受约束 recipe 外推到所有普通 x264 输出。

每片192帧、320x180、60fps、keyint64。B-dominant 的实测结构为177/192（closed）或179/192（open）B，必要 I/P/参考锚点仍存在；“全B”按此 B 主导定义。adaptive7 实际 B run 为1..7，adaptive16 实际为1..9，不把最大请求16写成实测16。H264 slice/ref_idc 与 HEVC NAL/slice_type trace 都有保存；例如 HEVC pyramid off 的参考 B 为0、pyramid on B3为48、B7为24、B-dominant为12，open GOP 的 CRA/RASL 也实际出现。

| 检查集 | 结果与限制 |
|---|---|
| 28普通目标×5同步/异步线程路径×2开关 | 280比较通过；H264用全片证书，HEVC用已有实现；early off 140全字段精确，on仅pkt_dts变化 |
| 14 HRD自动目标×相同5路径×2开关 | 140比较通过；无证书；off70精确，on70仅pkt_dts变化 |
| 上述420比较 | 241920个显示位置，按显示位置逐个比较而非hash集合；每个候选跑3轮，基线1轮逐段重复作参照 |
| 17特殊/回退文件×5路径×2开关 | 170比较，165精确；5项截断输入仅宽容模式decode_error_flags 0/12波动，原基线也观察过；不抹去该差异 |
| 当前HRD边界7文件×5路径 | 35 off/on对，3轮；wrap、奇数POC、fixed0、missing-timing、MMCO5、重复CPB、negative-leading，像素/PTS/顺序/字段属性保持，fixed0无提前差异 |
| H264 HRD动态B/HEVC动态B，2/12线程生命周期 | 20对 gate/flush/error/close/api，3轮，除触发DTS外逐帧匹配；包含实际in-flight flush和drain |
| ASan | 12线程自动HRD动态B和wrap/in-flight flush各3轮，terminal0且无ASan错误；未跑TSan，Mac未开LeakSanitizer |

特殊 FATE 下载仅8个：`h264-conformance/{camp_mot_fld0_full.26l,camp_mot_mbaff0_full.26l,camp_mot_picaff0_full.26l,Sharp_MP_PAFF_1r2.jvt,BA1_FT_C.264}`、`h264/{intra_refresh.h264,extradata-reload-multi-stsd.mov,twofields_packet.mp4}`。来源 URL/字节数/完整 SHA256 在 `../../out/evidence/fate-download.json`，未全量下载/公开上传。覆盖 field/互补场/PAFF/MBAFF、POC1/2、recovery point、参数重载；生成 I_PCM 覆盖 POC0/1/2、frame_num/POC wrap、MMCO5/负leading/损坏边界。独立 oracle 曾因音轨未map、autoscale影响分辨率变化而假失败，修正-map0:v:0/-noautoscale后全部匹配，初始失败记录保留。

逐帧CSV含像素SHA256、PTS、best_effort、pkt_dts、duration、尺寸/format/SAR/色彩、AVFrame flags、repeat_pict、decode_error_flags、pict_type和source AU id。未声称任意side-data字节逐个认证。错误返回独立记录；最新20次截断ledger里严格模式基线/原型都0帧、返回INVALIDDATA(-1094995529)，宽容模式都1帧/成功，本次20次均errorflags0，不否认此前0/12波动。开关关闭时不以DTS豁免掩盖其他行为。

曾发现并修复原型自己的IDR水位重置错误：旧前缀已完全排空时，新IDR仍与旧POC水位比较，导致丢GOP帧。最初HRD run35/140失败（387/576），v1 patch/日志保留；修复后固定构建140/140和280/280通过。这是被测试抓到的真实失败，不得遗漏。

## 第一阶段反例与HEVC边界（保留结论）

POC0 I_PCM反例：解码相对POC `[0(IDR),-2(nonref I),2,4,6]`，显示 `[-2,0,2,4,6]`，基线和独立8.1.2都5帧。16x16/Main77/CAVLC、fn/POClsb4bit、reorder2。8.2.1.1允许LSB14相对0推导负MSB；frame_num按preceding ref推进。没有完整HRD/conformance认证，故精确结论是：否定对所有基线接受流“只见IDR POC0就先输出”的等价规则，不否定更强合法流条件。本阶段HRD负leading也保持AU显示ID `[1,0,2,3,4]`。

旧POC2候选条件是progressive/no offset cycle/noMMCO5/presentPTS等，**不是本阶段已验收的自动模式**。正常POC2只改变AVFrame.pkt_dts：首帧96000→0（raw时间基1/1200000），PTS/best_effort/像素/帧数/顺序相同。decode.c wrapper把触发当前receive的packet DTS写给frame，提前交付会改变它；这是可解释的交付触发元数据差异，不能误报呈现时间戳错误。旧严格合同仍将其列为差异，已在REVIEW-ADDENDUM澄清。

真正的旧失败是坏POC2第二AU复用preceding ref fn0，推得POC-1：关输出 `[-1,0,2,4,6]`，开只 `[0,2,4,6]`；5种线程路径都5→4，terminal0/errorflags0。先交付IDR把水位设0，后来的-1走out_of_order被丢。当前AU无法预知共享首IDR的未来违规，不能事后回退撤回；这不证明所有合法POC2优化无解。新默认无证明路径对此回退，已恢复5→5。

HEVC代码本轮未修改。原生PTS closed/open/实际IDR_W_RADL+RADL/RASL边界无新增像素或显示顺序退化；2/12线程、flush/gate/error/checksum测试保留。已有94bd策略对“按解码序强行PTS=DTS”的不一致raw时间轴会改变best_effort，不能仅豁免pkt_dts就宣称精确：最小 `x265-b7-open0.hevc`、SYNTHETIC_PTS=1、async2/capacity1/3cycles，双方576帧，351处best_effort差异；显示位置4/sourceAU2 PTS均80000us，best_effort288000→96000，pkt_dts288000→240000（native units）。这来自已有guess_correct_pts/触发DTS交互，没证明正常PTS像素错序，也不在本轮无关重构。坏HEVC suffix checksum双方都返回INVALIDDATA、坏帧未发布、flush/replay匹配。

第一阶段50次perf-v2原样保留：微型POC2@25fps async2首帧81.193→0.065ms，全段平均70.403→63.655ms；sync1 pacedCPU0.04799→0.05025ms/frame；无pace微型CPU0.0019347→0.0018416ms/frame。普通无证明x264B7@60fps async2平均33.678→33.697ms；sync1无paceCPU0.075785→0.075481ms/frame、吞吐13005→13060fps。旧候选已否决，不能拿这些微型收益作为当前实现或生产吞吐承诺。

## 固定构建测量解释

最新162次：9输入×3交错重复×同步/async×early0/1=108 paced，另9×3×early0/1=54无pace单线程5轮CPU/吞吐；全部完成、无命令失败。媒体和codec/probe二进制固定，编码/ASan/其他解码不与测量并行。延时表先计算每轮全部显示帧的mean，再取3次重复的中位数，**不是逐帧P50**；首帧格是3次first-frame的中位数。完整命令、环境、逐帧AU关联和原始数值在 `../../out/phase2/performance/results.json`及相邻CSV/log，汇总见 `MEASUREMENTS.md`。

paced为60fps、2解码线程；延时是实际AU被接受提交到对应完整显示帧，也另记计划AU ready到帧，均含EOF尾部。文件预载只便于可重复输入，不扫描未来POC来给自动模式证明；文件open/probe/预载不计该codec时间窗。它不是网络摄入到屏幕，也不是解码新文件完整启动SLA。H264首次anchor保留旧等待，不能承诺首帧快出；HEVC原IDR快出另有首帧收益。

实际提前交付比例用相同显示帧的trigger pkt_dts相对原DPB变化计数（是可观察效果的保守下界，不是内部branch counter）：HRD B3 pyramid75%，B7 pyramid62.5%，B-dominant open58.85%，adaptive16 open73.96%；无pyramid B3/B7为25%/12.5%。14文件完整计数见 `effective-early-delivery.json`。普通无HRD控制片从guard明确回退，不能用时钟噪声称命中。

单线程CPU只对send/receive调用线程计时，含wrapper/时钟，不能完全剥离业务开销；async process CPU另存但含driver/demux/open，不能称纯解码CPU。结果没有一致CPU节省；有些文件小幅变慢。减少的是等待，新增引用/FIFO也有开销。缓冲只量化output时accept-submit减delivered的上层 outstanding proxy：paced同步最大3、async最大2，两开关相同，未隔离DPB字节/内存峰值，不能宣称减小该max或节约DPB内存。

## 未覆盖与验收限制

POC1/2、场/PAFF/MBAFF、pic_struct重复场、VFR、不充分HRD、多CPB、VCL-HRD、硬件、filmgrain、扩展profile等均不自动推广。缺口有FMO/ASO/SP/SI、SPS/PPS所有编号组合、完整多slice错误穷举、丢场、长时多路压力、HEVC多层/field_seq/10bit/filmgrain全边界；明确回退比声称无条件安全更准确。非IDR buffering-period只有源码/规范核对，暂无专项fixture，不能写成实测覆盖。

最重要的当前实现缺口是独立审查指出的当前AU可见异常：首slice header时已选择额外前缀，late SEI在setup_finished被忽略，宽容SEI解析错误可被忽略，picture-timing payload下界/重复矛盾检查不足，field_end前未完整重新验证这些资格。私有FIFO延迟发布不等于撤回已修改DPB选择。尚无此缺口导致合法流错序的实证，但**异常场景安全回退合同还未完成**。因此本阶段只交付受限定理/研究原型、开关、测试及准确缺口，不标为任意输入可发布。修补要在任何额外选择前完成整AU资格判定，或能可靠回滚选择；不能靠最终hash集合或输出后发现异常补救。未来非法AU也不能仅靠过去声明完全预知。

独立审查全文 `INDEPENDENT_REVIEW.md` 为验收依据之一；机器可读 `../../out/phase2/acceptance.json` 与第一阶段拒绝记录分开。继续上线前需完成上述守卫、相应最小反例回归及Linux服务集成；本轮按父任务要求不再扩展无界实验，不改生产默认。
