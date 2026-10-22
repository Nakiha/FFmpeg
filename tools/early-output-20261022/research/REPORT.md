> **实验版，最终验收未完成，不建议生产启用**
> 历史文档归档：正文中的“当前”、未推送状态及验收结论仅属于原阶段；以 `doc/early-output-20261022/REPORT.md` 的版本核对为准。`out/` 路径指本机保留证据，不随公开分支发布。

# H.264 安全提前出帧评估结果（2026-10-22）

本轮结论：**不接受新的 H.264 提前出帧实现**。已经完成精确基线构建、最小候选、反例、实际 FATE/生成码流回归与交错性能测量。候选在正常 POC2 输入上改变输出触发 DTS，在一个基线接受的坏流上从 5 帧变成 4 帧。它不满足本任务要求，不能作为安全快出交付。

候选源码已从 FFmpeg 工作树撤回，两个仓库当前均干净。供复核的补丁保留为 `h264-poc2-experimental.patch`，实验二进制保留在 `../out/install/candidate/`，明确禁止作为验收 SDK 使用。本轮没有推送、合并、公开上传样片、更新应用 pin 或修改生产默认。没有恢复旧编码 tick-tock 优化。

## 1. 仓库和实际构建身份

独立工作区 `<WORKSPACE>`，两个仓库均为新克隆，没有复用其他代理的工作区。

- 应用 main：`67f5e07798575e20e1f3fe1dcc6e37b40f3d64fa`；gitlink：`94bd940f1c4682dfce13e8ae43891df01621e16e`。
- FFmpeg 没有 main，默认 master 为 `e9b3c55fe0321915a70f44f30b276ef2afdc3a0c`；`live/n9.0.2-async-early-output=d72c0d44...`；实际应用 pin 位于 `codex/hevc-idr-early-output=94bd940f...`。完整远端 heads 在 `../out/evidence/ffmpeg-remote-heads.txt`。
- 原生 Mac arm64 基线就是 `94bd940f`，不是系统 FFmpeg 8.1.2。基线 libavcodec 为 63.4.108；上游 n9.0.2 来源见 fork 的 `doc/live-n9-port-zh.txt`。候选是该 pin 加 3 文件、23 行增加/3 行删除的本地补丁，默认关闭；没有发布版本号或伪装成干净 pin。
- `../out/evidence/build-identity.json` 保存 compiler、完整 configure 选项、两套库/probe SHA256、otool 动态链接路径。基线和候选各自链接自己的 dylib，没有混用 Homebrew 库。系统 8.1.2 仅生成媒体及作为独立像素 oracle。
- 已读应用 AGENTS.md、两份本机 memory_summary.md、FFmpeg live async guide/auto/n9 文档。精确 pin 内没有 AGENTS.md 或 `.agents/skills`。应用是 Linux-only；没有声称在 Mac 完成服务运行/SDK 默认身份验收。

已核对线索对应真实提交：`26dee063` 是 n9 receive-frame worker 的异步移植；`8eabad8e` 清理共享预算争用时预取的 next_pkt；`42dd642a` 为软件单层有 PTS 的 HEVC IDR_N_LP 提前选择；`94bd940f` 为 HEVC 连续 POC 前缀及引用持有。提交摘要和文件清单在 `../out/evidence/verified-commits.json`。

应用源码核对：`3c9ba3c` 开 HEVC IDR，`a0bfa0c` 开连续 POC 并调整 dispatch，`bb4ebf6` 是包含有界异步交接的较大媒体/质量控制提交，`25e6c28` 在编码包之间让出并审计 CPU；不能把它们都称为同一种 codec 提前输出。当前 pipeline.c 仅为 HEVC 开两个选项。

## 2. 哪些等待可以减少，为什么不能直接移植

异步 API 减少的是已完成 worker 结果等待后续无关包/前台调用的调度驻留，仍按提交顺序退休、经过通用属性处理。它不会自动越过 H.264 的 delayed_pic/DPB 重排。codec 提前放行则修改“哪一帧可以先选择”，证明义务不同。

H.264 有两路场 POC、POC0 的 LSB wrap、POC1 的有符号偏移、POC2 的 frame_num 推导，以及 MMCO5 原点重置、恢复点、场配对和新参数集边界。不能用看见普通 I/IDR、POC+1 或常见逐行 POC+2 来推断不存在未来较小 POC。

最小反例 `pcm-poc0-leading-negative.h264`：16×16 CAVLC I_PCM，解码顺序 IDR(luma32, POC0)、nonref I(luma45, 相对 POC-2)、后续 I。精确基线输出顺序为 luma45、luma32、后续，5/5 帧且零解码错误；独立 8.1.2 的顺序 hash 也相同。首两帧 SHA256 分别 `4919ac6f...`、`7fba7a54...`。所以通用 IDR-first 会直接违背基线。这里证明的是基线接受码流的等价性反例，不声称已完成该码流的全规范 conformance 认证。

官方 [H.264 06/2026](https://www.itu.int/rec/T-REC-H.264-202606-I/en) 8.2.1 描述场 POC 及 MMCO5，8.2.1.3 说明 POC2 的输出顺序等于解码顺序。PDF SHA256 为 `028cf2cf...`，本地 PDF/提取文本在 `../out/evidence/H264-202606.*`。

先写的证明边界与样片计划保留在 `FEASIBILITY.md`，之后才实现候选。候选只对：软件、逐行 frame_mbs_only、POC2、默认 discard、无 CHUNKS、有效 PTS、当前为 IDR、无先前待输出帧、输出水位 INT_MIN 的启动/flush 边界生效。沿用现有 delayed_pic 移除和 next_output_pic，在 finish_setup 前选择，仍在全部 NAL、field_end/滤波和 finalize_frame 完成后交付；未降低 has_b_frames，未跳过滤波/依赖，未增加未来帧或主动丢帧。

## 3. 实际结果：候选被证伪

主矩阵 24 个媒体、120 组：同步 1/2/12 线程、异步 2/12 线程、capacity=1。每组对照原基线、候选关闭、候选开启。

- 基线与候选关闭：**120/120 完全匹配，12,745 个逐帧位置**。
- 候选开启：105/120 完全匹配，**15 组失败**。正常声明重排2的 POC2 和 POC2 frame_num wrap 各 5 组只有 pkt_dts 不同；坏流各路径/线程共 5 组从 5 帧变成 4 帧。
- 正常 POC2 反例的首帧 pkt_dts（流时间基单位）关闭为 96000，开启为 0；像素、显示 PTS 等相同。不得偷偷继承旧 HEVC 测试对 pkt_dts 的豁免。
- `pcm-poc2-invalid-leading.h264` 故意违反 POC2 约束：IDR 后 nonref 复用 frame_num0，推得 POC-1。基线仍接受并输出其较早画面；候选先交付 IDR 后，原有 out-of-order 逻辑丢掉该画面，5→4。两边 terminal_return 都为0。故不能用“最终都成功”或 hash 集合掩盖它。
- 普通 x264 B0 的 POC2/reorder0 原本就可立即出帧；B3/B7 的 POC0 以及非 IDR I/open GOP 走回退，未发现该候选对它们有收益。

结果文件 `../out/matrix/summary-with-returns.json`；每组完整命令在 `commands.json`，原始 `.sha256/.csv/.log` 按路径对应保存。

逐帧记录：有序像素 SHA256、PTS、best_effort_timestamp、pkt_dts、duration、尺寸/格式、SAR、range/primaries/TRC/colorspace、AVFrame flags（含场/TFF）、repeat_pict、decode_error_flags、pict_type、输入 AU ID。**比较有序位置和帧数，不比较无序 hash 集合**。没有做全部任意 side-data 的逐字节相等认证。

## 4. 实际样片矩阵和生命周期

| 边界 | 实际输入和核查 | 结果/回退 |
|---|---|---|
| POC0 | PCM 负向回绕、gap、正向 LSB wrap；x264 B3/7 | 候选拒绝快出；匹配 |
| POC1 | PCM signed delta；FATE Sharp_MP_PAFF_1r2 | 候选拒绝快出；匹配 |
| POC2 | FATE BA1_FT_C(299帧)、x264 B0、PCM reorder0/2、frame_num 15→0 | 正常流仅过度声明重排时有首帧机会；该机会改DTS |
| IDR/non-IDR I/open GOP | x264 B0/3/7×closed/open，各192帧 | 实际 B3/7；参考 B slices分别48/24；open有3个non-IDR I；回退匹配 |
| 场/互补场/PAFF/MBAFF | FATE camp_mot_fld0_full(30帧/60field slices)、camp_mot_mbaff0_full(30)、camp_mot_picaff0_full(30)、Sharp_MP_PAFF_1r2(15) | 实际 frame/field/MBAFF header核查，逐行gate拒绝 |
| 两场同一包 | FATE twofields_packet.mp4，147帧/294 field slices | count/有序像素及字段匹配 |
| recovery point | FATE intra_refresh.h264，392帧，recovery_frame_cnt=21 | 非IDR回退匹配 |
| SPS/PPS/extradata变更 | FATE extradata-reload-multi-stsd.mov，256×128→128×128，4帧 | 像素匹配；异步flush错误标志存在原基线波动，见下 |
| MMCO5 | PCM header实际含operation5，5帧 | POC0回退匹配 |
| 损坏/缺失 | 缺PPS、首IDR截断、末帧截断、late bad SEI、坏POC2后续画面 | 缺PPS/首截断0帧且同错误；末截断9帧且同错误；late SEI被两边接受，不称严格拒绝通过；坏POC2是拒绝候选的关键失败 |
| seek/flush/drain | 实际 av_seek_frame 到1.2s，再3轮drain/flush；异步在途flush、gate、error-recovery、close、API | on/off顺序/像素基本相同；候选自身DTS问题保留 |
| 多线程/自动额度 | 同步1/2/12，异步2/12，capacity1；lifecycle设置auto min2 | 已跑，未做Linux命名共享预算 |

额外边界有 108 个 on/off 对，另有 10 个正向POC wrap/首IDR截断对（10/10精确一致）。native PTS 40 对中39对全字段一致，余1对只有下述已有错误标志波动。原始 `../out/boundaries/summary-with-returns.json` 与 `../out/last-regressions/summary.json` 不掩盖失败。

8个FATE样片是从当前 pin 的 `tests/fate/h264.mak` 和官方目录索引按需选择，未拉全库；URL、大小、SHA256在 `../out/evidence/fate-download.json`。其它媒体由固定参数 x264/x265 或 `generate_pcm.py` 可复现生成。`../out/source-audit/` 有独立 trace_headers、逐帧类型与field属性核查。

独立Homebrew oracle最初两个比较失败是测试端映射问题：双场MP4还带音轨，而CLI未限定视频；尺寸切换被CLI默认autoscale统一尺寸。保留最初失败；显式 `-map 0:v:0 -noautoscale` 后，两者有序像素分别147/147和4/4匹配。主矩阵其余22份原本匹配；另真实RADL样本也匹配。更正证据在 `../out/final-checks/corrected-oracles.json`，没有改解码器迎合oracle。

## 5. 现有 HEVC 边界回归和发现

使用精确 pin 的现有两个快出选项，H264补丁没有修改HEVC代码。

- x265 B7 closed：4 IDR_N_LP；open：1 IDR_N_LP、3 CRA、21 RASL。2/12线程、capacity1、3轮drain/flush的带原生PTS MP4：有序像素、显示PTS和记录元数据匹配；已知触发pkt_dts差异单独保留。
- 真实 `radl=3` 样本：1 IDR_N_LP、3 IDR_W_RADL、6 RADL_N、3 RADL_R，不是仅改NAL头冒充。12组（2/12线程，async/gate/flush/error/api/checksum）有序像素/显示元数据匹配，只有触发DTS差异。`../out/final-checks/summary-with-returns.json`。
- raw缺失PTS、仅改IDR头的W_RADL无leading控制、删除TRAIL_N的POC gap均执行。缺PTS路径保留原行为。
- **边界发现**：对raw B流故意分配“解码顺序PTS=DTS”（数字有效，presentation语义不一致）后，两选项还会改变 best_effort_timestamp。6组（closed/open/gap×2/12线程）不能按只豁免DTS判通过，原始字段1/2差异保留。它说明有数字PTS不等于有效显示时间轴；本轮不无关重构HEVC。
- 故意破坏首IDR后缀decoded-picture MD5后，开启/关闭都先报 `-1094995529`，然后flush/replay；错误帧未作为输出交付。H264坏包恢复也保留相同注入错误数值。日志在 `../out/error-returns/`。
- **已有问题**：FATE extradata reload在未改基线自身相同配置的5次 async12/三轮flush实验中，decode_error_flags在0/8/12之间变化；候选POC0 gate不可达却也出现同类波动。像素仍一致，不能把单次on/off标志差异直接归因给候选，也不能声称标志验收已完全通过。重复原始证据在 final-checks。

未执行：HEVC多层、field_seq、硬件、10bit、film-grain全边界及Linux服务全链路；H264 FMO/ASO、SP/SI、丢互补场穷举、所有SPS/PPS编号/声明切换组合、长时多路压力、TSan/ASan。候选被否决，不以这些缺口暗示可上线；原实现仍负责所有回退。没有把小尺寸测试外推到1080p或生产SLA。

## 6. 时延、CPU、吞吐、缓冲分别量化

`../out/perf-v2/results.json` 是同一候选库关闭/开启的5次交错重复（奇数轮换顺序），固定媒体和线程。性能轮不做像素拷贝/hash，生成/大矩阵已结束后才测。v1保留，v2额外记录计划AU到帧延迟，避免只看成功submit边界。

每个画面同时记录实际被接受的AU submit→对应完整显示帧、计划AU ready→帧；输入文件预载，**不含网络/打开/显示设备**。EOF尾部包含在全段均值，不把12帧小样本称为稳态。CPU用线程CPU钟包围同步send/receive；仅同步1线程时覆盖实际codec调用计算，含wrapper/时钟开销。async的进程CPU另存，但含driver/demux/open，不能冒充纯解码CPU。

下表为5轮的中位数，ms；吞吐为无节奏完整解码循环frames/s。

| 条件 | 关 | 开 | 解释 |
|---|---:|---:|---|
| PCM POC2声明2，25fps，async2 首帧 |81.193|0.065|只提前启动IDR；候选已被安全验收拒绝 |
| 同段全部12帧 submit驻留均值 |70.403|63.655|收益只摊到一帧，后续仍按原buffer阈值 |
| 同段计划ready→帧均值 |72.144|65.216|包括paced供包调度偏差 |
| PCM paced同步1 codec调用CPU/frame |0.04799|0.05025|没有计算节省，极小帧受计时/调度影响大 |
| PCM无节奏同步1，100轮 codec调用CPU/frame |0.00193|0.00184|微秒级，不能据此宣称计算加速 |
| 同无节奏PCM吞吐 |331583|349142|16×16微基准，非业务容量结论 |
| 普通x264 B7 open，60fps async2 submit均值 |33.678|33.697|POC0不走候选，无收益证据 |
| 同x264计划ready→帧均值 |35.561|35.465|微小运行波动 |
| x264无节奏同步1 codec调用CPU/frame |0.07579|0.07548|原样回退，无计算变更 |
| 同x264无节奏吞吐 |13005|13060|小媒体波动，不宣称提速 |

在上述1AU对应1帧的性能样本中，输出时“已接受但未交付的画面”峰值均为2（含codec/async驻留的计数，不是独立DPB引用计数），开启后稳态未减少。真实普通流没有证明降低缓冲；仅过度声明重排的构造样本启动等待有收益，却违反完整时间戳/坏流兼容合同。

## 7. 交付、复现和剩余限制

可审计入口：`../out/acceptance.json`（明确rejected）、本报告、`FEASIBILITY.md`、`h264-poc2-experimental.patch`、`../out/evidence/build-identity.json`、matrix/boundaries/final-checks/perf-v2 各summary和全部逐帧日志。build-identity 是结束时的 probe 身份；probe 在阶段间增加了 seek/错误记录等测试日志能力，两套 codec dylib 在矩阵及测量期间保持相同。后续复跑脚本会创建新时间戳目录，避免覆盖本次证据。测试源和生成器都在本目录；PCM/FATE/编码样片都在本地 `../out/samples/`。README列明入口。

重建基线的configure命令见 build-identity.json；`build-macos.sh`提供相同参数。复核候选须在隔离精确pin上显式应用experimental.patch，再构建独立candidate前缀；测试已否决，不能更新应用默认以试用。两个源树结束时均干净，候选二进制/补丁刻意保留作反证。

起初完整克隆期间Mac命令服务断连，恢复后未完成目录不存在，已改浅克隆并构建成功；没有持续连接阻塞。原Linux build脚本在macOS bash3因set-u空数组失败，同等直接configure成功，未修改脚本。向父任务发送检查点的工具返回thread not found；本地CHECKPOINT已保存，任务最终结果由平台回传。

当前剩余限制是上述验收失败和平台范围，**没有等待审批的本地操作，也没有被自动审批拒绝的修改**。不需要为失败候选请求发布批准；本轮停止在有证据的否决结论和可复现回归资产。
