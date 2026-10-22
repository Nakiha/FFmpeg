> **实验版，最终验收未完成，不建议生产启用**
> 历史文档归档：正文中的“当前”、未推送状态及验收结论仅属于原阶段；以 `doc/early-output-20261022/REPORT.md` 的版本核对为准。`out/` 路径指本机保留证据，不随公开分支发布。

# H.264 提前输出独立审查

审查日期：2026-10-22。审查者未参与实现；只读核对源码、规范及已有结果，未改源码、构建或样片，未推送，未重跑大矩阵。本报告是本轮唯一写入。遵循 `NakiLiveTranscode/AGENTS.md`。基线为 FFmpeg `94bd940f1c4682dfce13e8ae43891df01621e16e`，范围为六个 H264 文件和应用初始化开关。

**结论：有条件的时槽定理成立；当前实现不予安全验收。** 未发现满足本模式声明约束的合法流能够在已建立的连续输出前缀中插入更早图像的反例。否决点是当前 AU 已可见异常的保守回退没有贯穿选择到发布全过程，不能把正常矩阵通过写成“矛盾/损坏 SEI 均在额外输出前拦截”。这不是已复现的合法流像素或 PTS 错序，也不是对所有流的全面否决。可以保留为默认关闭的研究原型；修复后需要针对性再审。

## 发现

### F1 / P1（安全验收阻断）：当前 AU 的 SEI 异常不能可靠撤销额外选择

位置：`FFmpeg/libavcodec/h264_slice.c:1836`（首 slice header 中选择）；`h264_slice.c:1371`（只信任 picture_timing.present）；`h264dec.c:714`、`:723`（late SEI 忽略及非 EXPLODE 错误继续）；`h264dec.c:1098`、`:1102`（field_end 后直接 finalize pending）。

`h264_select_output_frame()` 在首 slice 头解析完即更新 watermark、摘除 delayed_pic 并建立 pending。完整 AU 的 NAL 循环尚未结束。最终 finalize 前没有重新确认本 AU 的 SEI 唯一性、解析失败记录或证明状态。`decode_nal_units()` 在非 `AV_EF_EXPLODE` 模式可忽略已返回的 SEI 错误；frame threading 的 `setup_finished` 后则直接忽略 late SEI。`field_end` 成功只说明其本身未返回错误，不能替代完整的证明有效性检查。

精确触发配方（源码可达性验证，遵守只读要求未生成/运行新坏样片）：选择已有 HRD 样片中会释放额外前缀的 AU，保留其合法 timing SEI，在首 slice 之前再加入一个 SEI NAL，RBSP payload header 声明的 payloadSize 大于实际剩余数据（例如 NAL 字节 `06 05 10 00 80`，Annex B 时外加 start code；AVCC 时更新长度）。`h264_sei.c:258` 会返回 INVALIDDATA，但 `h264dec.c:723` 在默认非 EXPLODE 设置下继续；先前的有效 timing.present 仍可用于额外选择。另一个配方是在首 slice 后加入与前面不同的 picture-timing SEI：多线程路径可将其作为 late SEI 忽略，单线程路径也不会重算已选出的 live_tick。两者都没有“本 AU 证明失效，因此只走旧 DPB”的持久标记。

影响：当前已可观察到证明材料异常，扩展仍可能输出。若要求损坏输入保持旧 DPB 前缀/发布边界，现状没有建立该保证。规范符合性假设不能解释掉当前已解析出的错误。此问题不证明正常 HRD 流有错序。

建议：在任何首 slice 选择前对整 AU 作保守资格筛查，并保留 AU 级不可逆失效标记；重复/late timing、SEI 解析失败或证明参数不一致均禁止该 AU 的扩展。不能只在 finalize 时清空 pending，因为 delayed_pic/watermark 已修改，且可能已被下一 frame thread 复制。若仍采用投机选择，必须证明回滚和跨线程状态传播。

### F2 / P2：timing.present 不等于 timing payload 足够长

位置：`FFmpeg/libavcodec/h264_sei.c:132`、`:143`、`:145`，以及 `h264_sei.c:69`、`:74`、`:75`、`:129`；新增信任点为 `h264_slice.c:1371`。

`decode_picture_timing()` 仅检查 payload 不过大，允许零长/短 payload 并设置 present；`ff_h264_sei_process_picture_timing()` 读取 cpb/dpb 延迟后未检查剩余位数是否为负。完整 NAL 外壳也可能含不足以容纳 SPS 声明字段的 payload。以本轮真实 HRD 样片的 12-bit cpb + 8-bit dpb 为例，payloadSize=2 只有 16 bit，低于需要的 20 bit，却不会因此使自动模式失效。

精确触发配方：将某个 picture-timing SEI 的 payloadSize 和实际 payload 一并缩短到 2 byte，其他 AU 内容保持有效。它能通过外层 size 检查；present 仍为真，延迟由超出有效 payload 的读取产生。重复 counter 或时间矛盾检查有时会碰巧回退，但并非长度验证，不能作为必然守卫。此处也未生成新样片或声称测得输出差异。

建议：把当前 SPS 所需最小 timing 位数检查纳入扩展的资格条件；BP payload 也应核对 SPS ID 一致性、长度及解析成功。保留普通解码器的既有容错策略可以，但扩展不能继续使用未验证的 timing。

## 规范与源码证明核对

规范依据为本地完整提取 `out/evidence/H264-202606.txt`（ITU-T H.264 V16，06/2026），不是仅接受 SAFETY.md 的描述。

- C.1.2 C-8/C-9 定义非初始化 buffering period 的 removal origin；C-10 在 low_delay_hrd=0 时使实际 removal 等于 nominal removal。C.2.2 C-12 给出 output time = removal + tc × dpb_output_delay。
- E.2.1 E-78 及 Table E-6 对 pic_struct_present=0、field_pic=0 给出 divisor=2；fixed_frame_rate=1 时，同 CVS 的连续输出间隔因此为两个 tc。跨 CVS 只有规范规定的同 clock 条件才有此间隔约束；实现用 IDR serial 和边界限制，不能任意跨 CVS 外推。
- POC 与 dpb_output_delay 顺序一致条款实际位于 **D.2.3**，D.2.2 是 buffering-period 语义。以正确公式得到的上一输出 tick 为 t，当前最小 pending POC 的 tick 为 t+2 时，不能在二者之间或同一 tick 插入另一合规 progressive 输出。这是空槽排除证明，未使用 POC 奇偶性或未来扫描。
- 自动模式首次 active 只能在 `h264_slice.c:1491` 由旧 DPB 已选中的 KEY picture 建立；此时之前 early 为 false，选择仍要求旧的 pics > has_b_frames。`:1447` 的立即 IDR 激活专属于外部 verified-even 模式。负 leading picture 不被自动当成不合法；已有 negative-leading 数据按 [1,0,2,3,4] 输出。
- 非 IDR BP 的代码在 `h264_slice.c:1397` 先用旧 origin 算当前图，再把当前 removal 加入 base、将后续 counter origin 重置，符合 C-8/C-9 的相对时间结构。但本轮没有覆盖非 IDR BP 的实际 fixture，不将源码推导等同实跑。
- counter 下降加一个 modulus，重复值禁用 ready；相等但实际跨整圈的合法情形会保守回退。已有 wrap fixture 覆盖常规回绕。更长间隔/多整圈以及长度变更未有专门实测。算术 margin 能在达到 int64 上界前停止，未发现可在现有受控状态推进中先发生溢出的路径。
- SPS/PPS 内容比较、MMCO5、场/不支持 profile、pic_struct、film grain、缺 PTS、非默认 discard、CHUNKS/hwaccel 等在额外选择判断之前存在。缺当前 timing 会清 ready 并禁用 active。相同参数重发按内容允许。参数切换、MMCO5 的所有后续损坏 slice 组合尚未证明；F1 说明为何不能把首 slice 检查推广为整个 AU 的验证。
- pending 中有效 timing 的两两 POC/tick 顺序矛盾在选择前检查，但没有涵盖重复 SEI 的历史值，也没有完整对照已输出 watermark 的矛盾检查。因此“所有可观察矛盾均失效”属于过强描述。

未来 IDR 的 `no_output_of_prior_pics_flag`：C.2.2 明确允许未来 IDR 取消尚未到 HRD 输出时间的图像，所以本证明不能宣称完整 HRD 输出时刻/抑制语义一致。当前基线 `h264_refs.c:839` 已直接跳过该 bit，未实现它的抑制语义；本次也未找到因此相对该基线新增的 PTS/像素错序反例。不得把这个基线既存限制虚报成已复现的本补丁回归。若目标升级为完整规范的输出抑制一致性，必须另立范围并验证，现有空槽定理仅证明顺序空槽。

## 接口、引用及生命周期

`h264dec.c:1140` receive wrapper 先排空 FIFO，再取下一包；`h264dec.c:1103` 把同一 AU 的多帧结果入队，保持选中顺序。额外选中的非参考图由 `ff_h264_ref_picture()` 持有 AVFrame、thread frame、PPS、motion/error 等引用；`h264_picture.c:89` 后复制新增 timing 字段。选择在 `ff_thread_finish_setup` 前完成，`:432` 后复制 scalar proof 状态，pending/FIFO 由本 worker 私有持有，不需把同一输出队列复制到下一 worker。n9 的 `pthread_frame.c:310` 后循环收取多帧，并按任务顺序退役。此路径未发现明确的正常输入引用悬挂、输出顺序或重复释放错误。

`ff_h264_flush_change()` 清 ready/active/serial、pending、FIFO、packet 和参数引用，close 释放新增对象。返回负错误时清 pending/FIFO，并停止 active；但不回滚已被选择摘除的好图及已传播状态，异常前缀等价性仍受 F1 的边界限制。现有 error 注入是独立坏包后 flush，不能证明“含有效首 slice、又在后续 NAL 失败”的同 AU 路径。

应用 `decode_output_policy.h` 默认 sentinel -1 保留 HEVC 开、H264 关；显式 false/true 在 avcodec_open2 前应用。未设置 verified-even；缺旧 SDK 的 H264 option 保守回退。fingerprint 仅显式字段参与，初始化测试在 prototype 和 pinned SDK 均有 PASS。当前完整 Linux 服务未在本 Mac 构建/跑通，应用没有 async-off 配置，不应将 codec harness 四组合写成服务端四组合实跑。

## 实测证据核验与未证明项

本审查独立读取并重算了 raw `.sha256`，基线单 cycle 按原脚本重复为三 cycle 后比较，未只信 summary 的布尔值。

- 自动 HRD：140 行全部像素、帧数、显示顺序、PTS 和其余记录 metadata 相等；70 行仅 column 2（输出触发 pkt_dts）不同。环境无 H264_CERTIFICATE。
- 外部证书 H264 / 原 HEVC：280 行同样通过；140 行仅 pkt_dts 不同。H264 的证书绑定 extradata、每包字节及 PTS/DTS/duration，probe 在设置断言前对实际预载包重新摘要。它需要全文件并信任证书签发方；不能称为自动直播证明或直播收益。
- 两套均覆盖 legacy（同步）1/2/12 thread，async 2/12 thread，early off/on；因此线程数2和12的四组合确有实跑。off 无差异。
- post-checks/boundaries.json 的35对通过（7类 × 5调度组合，3 cycles）：odd POC、wrap、negative-leading、missing timing、fixed0、MMCO5、重复 CPB。普通无 HRD x264 不属于自动资格。
- fallback-final 170行有5行非 exact，集中于既存 lenient 截断 error_flags；不得隐去。新的 error-return-ledger 20行中 baseline/phase2 strict 都返回 -1094995529、0帧；lenient 各5次为正常终止、1帧、error_flags=0。这不能否定先前已保留的0/12波动。
- lifecycle 20项有序像素/PTS一致，实际 ASan 的 hrd/wrap日志正常退出；未将 macOS未启用的 LeakSanitizer当作已验证。
- performance/results.json 162次全部 returncode=0，覆盖四组合，环境只设置 EARLY_OUTPUT/NO_HASH，没有证书。它能支持对应自动HRD/HEVC测量，不能支持所有H264输入或线到屏延迟；本审查没有重新测量性能。

仍需有界补证：非IDR BP（含counter reset邻接回绕）、BP引用SPS不匹配、SPS/PPS在pending期间切换、重复/矛盾/短payload/late SEI、首slice成功而后续NAL或slice失败、输出队列已积累时OOM，以及未来IDR抑制语义的明确范围。针对F1/F2修复后的二进制需记录新身份；旧162性能结果不能静默冒充新二进制测量。

审查时主要源码 SHA-256：h264dec.c `798ec8425caab018735cb03953b244b3f41207139f595505e5a2069812b5f0f1`；h264_slice.c `7aa0bcb8480d8d1c54ce78b12a859caff78902d44790f9f0a6a178475ce71d19`。后续修复应作为新版本审查，不改写本轮结论。
