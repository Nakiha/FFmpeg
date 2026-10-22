# H.264/H.265 提前出帧实验：公开归档与版本核对

> **实验版，最终验收未完成，不建议生产启用**

2026-10-22，本次仅接手、核对、整理和公开已有源码、测试脚本与技术说明。没有运行新测试、编译、生成样片、修改算法或部署。以下“读回”指读取既有文件、计算哈希、比较原有逐帧摘要；不是新的解码或性能实测。公开分支用于研究留存，不代表安全、性能或服务验收通过。

## 源码与补丁的版本关系

FFmpeg 基线为 `94bd940f1c4682dfce13e8ae43891df01621e16e`，应用基线为 `67f5e07798575e20e1f3fe1dcc6e37b40f3d64fa`。接手时 FFmpeg 位于该 pin 的 detached HEAD，六个 H.264 源文件有未提交修改；应用位于 main，六个相关文件有修改，其中新策略头为 intent-to-add。原目录的工作内容、暂存区和 HEAD 保留，发布在独立工作树整理。

本轮发布分支：FFmpeg `experiment/h264-early-output-archive-20261022`；[应用独立实验分支](https://github.com/Nakiha/NakiLiveTranscode/tree/experiment/decoder-early-output-archive-20261022)。不合入默认分支，不修改 SDK gitlink 或生产默认值。FFmpeg 远端默认分支为 master，应用为 main；发布不以默认分支最新代码替换实验基线。

| 阶段 / 材料 | 本次确认的对应关系 | 结论范围 |
| --- | --- | --- |
| phase1 `research/REPORT.md`、`REVIEW-ADDENDUM.md`、POC2 patch | 早期被否决候选的历史记录，独立于本次发布的 phase3 源码 | POC2 损坏流丢帧、无条件 IDR/POC 推断的反例必须保留；不重新验收 |
| phase2 `REPORT.md`、`INDEPENDENT_REVIEW.md`、`MEASUREMENTS.md` | 记录加固前实现和 F1/F2 否决；不是当前源码说明 | 正常矩阵与 162 次性能记录属于加固前版本，不能冒充加固后结果 |
| phase2 `ffmpeg-early-output.patch` | SHA256 `b138d0e9bb5dd21e9e9f6211d261f3ef84ebf3e3c3e87444c094cdc2e094654d`；匹配 phase2 冻结身份，也与 phase3 `pre-fix-source.patch` 逐字节相同 | 首 slice 投机摘队、AU 级守卫不完整的加固前版本；**不同于当前 FFmpeg diff** |
| phase2 / phase3 `application-switch.patch` | 两份逐字节相同；SHA256 `38c1333654d8e93b698fb20099be62f6ab61e967e89a8e927cff49eb8d1f637e`；等于接手时应用的完整 diff | 应用开关没有随 phase3 更改；补丁一致不意味着 FFmpeg 两个阶段也一致 |
| phase3 `ffmpeg-final.patch` | SHA256 `8da6856d143c24252103f4d3babd7119cccbb872150b9e1405d875b8020f2034`；等于接手时 FFmpeg 完整 diff；六个源码 SHA256 全匹配 phase3 build-identity | 本次公开的算法源码快照，未再修改算法 |
| phase3 `INDEPENDENT_REVIEW.md` | 当时的专项复审称 F1/F2 已关闭，并列出性能仍待汇总等限制 | 只保留其历史专项结论；本次发现下述原始证据路径冲突，不能把其中“83 对全部 raw 验证”当作本次已确认事实 |

历史材料位于 [归档 research](../../tools/early-output-20261022/research/README.md)，每份 Markdown 顶部添加版本警示。原始补丁及脚本逐字节复制；Markdown 仅增加警示并隐去个人工作区绝对路径。完整原始/公开哈希及排除项见 [ARCHIVE_MANIFEST.json](ARCHIVE_MANIFEST.json)。历史正文中的“未推送”“当前”只描述原阶段。

当前源码的六个文件、冻结库、probe 和故障注入程序均在本次读回时匹配既有 build-identity 的 SHA256。加固后 `h264dec.c` 为 `8cd76b2b60cd3d64124901b27e4850e13b3532d5d5fe839f323bae4ed9cfdd7a`，`h264_slice.c` 为 `6b7593bb14c604bd4f0321b5a8b80a055bc343ee7fd8b68e4149d311f9cd21d5`；对应 libavcodec 为 `cb2d65e540871e9d9fcaf84d9bc5227b3ca1f975a4e17a7d520f0557c9dc5f59`。这些是冻结文件身份，不是新运行的验收结果；也不能单凭事后磁盘哈希证明每次历史运行从未替换过库。

## 加固前与加固后的改动

phase2 引入 H.264 receive-frame/FIFO，使一个 AU 能交付多个已选画面；自动模式通过 HRD/CFR 时槽证明选择前缀，另有 caller 提供的全文件 verified-even 契约。phase2 的额外 DPB 选择发生在首 slice 阶段，完整 AU 的 late、重复、截断 SEI 和不足位宽的 timing 不能可靠使其失效；独立审查据此以 F1/F2 拒绝安全验收。

phase3 保留原单帧选择器，将额外路径移到完整 AU/NAL/slice 解码与 field_end 成功之后。完整 AU 预筛和不可恢复的 invalid 标记、timing/BP 长度与 SPS ID 核对、参数快照和水位矛盾检查先于扩展。额外输出按 plan → AVFrame/FIFO 全部准备 → 无失败点 apply → 真实 frame-thread setup 发布的顺序提交；准备失败保留额外 DPB 项并回退。外部 flush 与内部参数 reinit 的 packet 释放区分，错误清 active/ready，close/flush 释放私有引用。多 VCL slice AU 保守回原 DPB/setup 路径。HEVC 本轮没有算法改动，沿用基线现有实现。

这是源码对照与旧专项复审所支持的改动说明，不是本次重新证明全部线程、引用或坏流安全。中间版本已有真实多 slice 损坏帧像素差异的历史，不能与既存的 lenient error_flags 波动混同；phase3 文档保留该事实。

## 自动路径、回退与独立开关

H.264 自动路径仅限原生软件 Main/High、POC0、逐行 frame_mbs_only、上下场 POC 相同、无 PPS bottom POC offset、无 film grain、整 AU、default discard、有 picture PTS 的范围。还要求有效 timing_info、fixed_frame_rate=1、pic_struct absent、单 CPB 的 NAL HRD、无 VCL HRD、low_delay_hrd=0，以及当前 AU 合法 picture-timing/BP 和受支持的延迟位宽。多 slice、场及其他未支持路径回退。

自动模式先等待原 DPB 规则选出 IDR anchor，**不消除 H.264 原有启动等待**。C.2.2/C-12 的 output=removal+delay、E.2.1/E-78/Table E-6 的连续两 tick 间隔及 D.2.3 的 POC/timing 顺序约束构成受限时槽推导；不是最近 POC 的经验猜测，也不扫描未来 GOP。本归档转述已有规范分析，未重新下载或独立认证标准全文。该推导仍以输入遵守规范声明为前提，不是完整 HRD 合规验证器，也不能证明未来所有字节都不损坏。

**无 HRD/CFR/timing 声明或不满足条件时回到旧 DPB；普通未声明的 x264 自动输入没有已确认收益。** 缺 timing、重复 counter、MMCO5、参数变化、可见矛盾等禁用扩展，不能靠编码器名字、近期偶数 POC 或一个用户布尔值生成证明。完整 HRD 输出时机和未来 IDR `no_output_of_prior_pics_flag` 抑制语义不在已证明范围；基线也有相应限制。

应用 `decoder_early_output` 是初始化前的可选布尔值，独立于 async owner 与 `decoder_pump`：

| 配置 | 语义 |
| --- | --- |
| 省略 | 内部 -1 sentinel；维持原默认 HEVC 开、H.264 关；省略字段不改变旧 fingerprint |
| false | 同时关闭 HEVC 的 live_idr/live_poc；H.264 走原 DPB 策略 |
| true | 请求受资格限制的路径；不会绕开证明条件 |

旧 pinned SDK 无 H.264 option 时保守回退。配置在 avcodec_open2 前设置，不承诺热切换。应用没有 async-off 配置，`decoder_pump` 不是 async 开关；codec harness 的同步/异步 × early off/on 不能写成完整服务的四组合实跑。

`live_poc_verified_even` 是库中独立、默认 false 的 caller assertion；它要求可信调用者核验完整精确输入并绑定 extradata、packet bytes、PTS/DTS/duration。测试 probe 需匹配全文件证书才设置。应用从不为任意输入设置它；它需要整片可用，不是自动直播证明或直播收益。头部契约也不认证 entropy/pixel 正确性。

## 既有日志的本次读回结果

[RECONCILIATION.json](evidence/RECONCILIATION.json) 记录逐项比对、异常、原始证据文件哈希与大小；公开的是技术索引，不含原始日志、媒体或二进制。`out/` 路径指原实验本机证据，并非公开仓库中可用的文件。原始记录的保留不等于独立公众可用媒体重现；仅靠哈希不能恢复未公开样片。

| 既有集合 | 本次读回可确认的内容 | 不可扩张的结论 |
| --- | --- | --- |
| phase2 HRD 140 对、普通/证书/HEVC 280 对 | 按显示位置重读原 `.sha256`，帧数、像素、PTS 和已记录 metadata 除允许的 pkt_dts 外匹配；off 210 对全字段匹配 | 属于加固前版本；证书模式不是直播自动模式 |
| phase2 fallback 170 对 | 165 对全字段相同；5 对既有 lenient 截断 error_flags 差异仍保留 | 不能写成 170 对精确通过 |
| phase2 生命周期 20 对、HRD 边界 35 对 | 原摘要和逐帧记录吻合，除触发 DTS 外匹配 | 不是加固后新测量 |
| phase3 pre-fix 12 对 | 加固前复现记录存在并与原逐帧摘要对应 | 用来保留 F1/F2 历史，不代表当前源码失败 |
| phase3 checks-final2 88 对 | 88 对共同前缀除 DTS 外匹配；82 对同长度；6 个 strict parser-error/truncated 记录为 off/on 10/11 帧 | 未来坏 AU 到来前已交付的正常前缀可更长，不是全长度相同；15 多 slice 对全字段精确，9 次 FIFO 注入有实际命中日志 |
| phase3 representatives-final2 83 对 | **63 对没有跨输入路径冲突，可读回对应原始记录；其余 20 对涉及共用前缀，证据归属标为未知** | 不确认“83 对独立原始记录全部通过” |
| phase3 extra-boundaries-final 10 对 | 原始记录除 DTS 外匹配；anchor 前 SPS A→B→A 的两组合无差异回退 | 不能据此声称穷举所有参数切换 |
| phase3 ASan 5 项 | 原有日志无 ASan 错误标记、正常终止，冻结程序身份匹配 | detect_leaks=0；没有 LeakSanitizer/TSan 完整验收 |
| 应用初始化日志 | phase2、旧 SDK、phase3 均存在 PASS 配置日志 | 完整 Linux 服务未构建/运行验收 |

**新增证据限制（本次整理发现）：** `run_representatives.py` 的输出前缀只有 file.stem，没有输入目录。`hrd-targets/x264-fixed7-pyr1-open1.mp4` 与 `targets/x264-fixed7-pyr1-open1.mp4` 对应 summary 的 0-based 行 10–19、50–59，共 20 行，引用同一组输出前缀；脚本后写普通输入。因此较早 HRD 原始记录可能被后写覆盖，现存文件不能独立支持这些 HRD 行。5 个 early-on 行（11、13、15、17、19）的 summary 记 differing_columns=[2]，而现存 raw 为 []，直接证实记录对应有冲突。这里保守标记所有 20 个共用前缀条目归属未知；未补跑、未修改 summary 或日志。未发现路径冲突的 63 行不因此作废，但也不弥补该缺口。

AVFrame.pkt_dts 是触发交付的 packet DTS，提前交付可改变它；不能误报成像素/PTS 错序。既有 HEVC 在人为 PTS=DTS 的不一致时间轴下还有 best_effort 变化，不能仅豁免 pkt_dts 就算精确通过。未验证任意 side-data 字节等价。

## 性能记录与尚未确认的结果

**加固前：** phase2 `MEASUREMENTS.md` 的 162 次记录已存在，记录 returncode=0，原日志 terminal 和 CSV 帧数与摘要吻合；perf_probe 与冻结身份匹配。例如受限 HRD B3 async2 平均提交到帧为 33.677→17.349 ms，而普通无 HRD 控制片 33.665→33.684 ms。它们是 320×180、60fps 下逐轮 mean 的三轮中位数，不是逐帧 P50，也不是源到屏延时或生产容量。未观察到一致 CPU 降低。**这些数字不得作为 phase3 的性能结论。**

**加固后尚未确认：** 现有 phase3 performance/results.json 有 156 条记录，符合脚本预期：72 次 paced（含同步/async）、36 次单线程无 pace、48 次 phase2/phase3 双线程无 pace 对照。记录 returncode/terminal 均为 0，读回 CSV 帧数和日志 terminal 与摘要吻合；其中24条使用旧 phase2构建，不能统称156条均为新二进制。当前磁盘 perf_probe 匹配 phase3 build-identity。原专项复审写作时这些测量仍在进行；未找到其后的完整最终性能复审和最终 acceptance 文件。本次仅归档这些数值记录，**不发布新的加固后收益、吞吐或并行开销结论**。逐条运行时的库身份、测量隔离状态、统计解释及完整服务影响尚未最终确认。

两阶段的去个人路径数值表分别留在 [phase2-performance-records.json](evidence/phase2-performance-records.json) 与 [phase3-performance-records.json](evidence/phase3-performance-records.json)。两份 build-identity 也公开为技术元数据；其中历史 pushed=false 只属于原记录，不能当成本次推送状态。

## 公开范围与仍未完成的验收

本提交只包含六个相关 H.264 源文件、已有研究脚本/补丁/技术报告和本次整理的技术索引。视频、FATE 样片、原始 out/ 日志、库/对象/可执行文件、dSYM、缓存、下载标准全文、内网资料、凭据均不纳入提交；不清理本机原件。源码生成/修改 helper（phase2 implement/add_hrd、phase3 implement_commit）和过程 CHECKPOINT 不公开，最终源码和阶段补丁保留。

尚未完成的项目：

- 解决代表性测试的跨输入输出前缀冲突，重新建立受影响记录与原始证据的可靠对应。此次禁止补跑，故保留未知。
- 加固后性能的完整最终复审、运行身份与隔离状态核验、setup 延迟发布对并行重叠的影响解释；不得沿用 phase2 数值代替。
- 最终综合验收与结论签署；原有 phase1 rejected、phase2 not-safety-accepted 不改写，phase3 专项修复结论不等于最终验收。
- Linux 完整服务、匹配 SDK 的显式实验集成、真实流与完整 filter/编码/封装链；当前应用 gitlink 仍指向原 pin，H.264 新路径不会仅因发布分支而进入默认构建。
- 全部分配失败点、恶意/损坏输入、重配置、packet side-data、32-bit 时长字段、超长间隔、硬件与多 slice 等未证明范围；TSan/LeakSanitizer 及生产场景容量也未完成。

上述项目是后续验收缺口，不是本次获准执行的测试/部署事项。提交文件完整清单见 [PUBLICATION_FILES.txt](PUBLICATION_FILES.txt)。提交前逐文件比对暂存清单、源码 diff、归档哈希和完整 diff；非 patch 文件的 whitespace 检查干净。归档 `.patch` 中正常的空白 context 行会被外层 `git diff --check` 报为 trailing whitespace，保留这些字节以维持原补丁哈希，不修剪或改写历史补丁。
