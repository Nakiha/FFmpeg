> **实验版，最终验收未完成，不建议生产启用**
> 历史文档归档：正文中的“当前”、未推送状态及验收结论仅属于原阶段；以 `doc/early-output-20261022/REPORT.md` 的版本核对为准。`out/` 路径指本机保留证据，不随公开分支发布。

# Phase 3：H.264 提前输出修复独立复审

2026-10-22。审查者未参与实现，只读检查源码、规范及已有日志/原始帧摘要；没有修改实现、构建、样片或推送，没有重跑矩阵。本轮仅写本报告。遵循 `NakiLiveTranscode/AGENTS.md`。Phase 2 原报告保留，不改写已被否决版本的结论。

**结论：F1/F2 专项修复通过；在下述有限范围内，未发现剩余的发布前守卫、额外 DPB 提交原子性或引用生命周期阻断。** 两 tick 时槽证明的条件仍然成立，没有扩张为任意 H.264 流的无条件顺序证明。当前多 slice AU 保守回到旧 DPB/线程 setup 路径。严格模式遇到未来坏 AU 时，已经提前交付的正常前缀可能比关闭模式长，这不是当前坏 AU 的额外发布；不能承诺任意未来损坏流全字段、前缀长度都精确一致。

最终源码与二进制以 `out/phase3/build-identity.json` 为准。本审查独立重算了其中全部 source、patch、test binary 和 build file 的 SHA-256，均匹配。关键身份：

- FFmpeg 基线：`94bd940f1c4682dfce13e8ae43891df01621e16e`。
- `research/phase3/ffmpeg-final.patch`：`8da6856d143c24252103f4d3babd7119cccbb872150b9e1405d875b8020f2034`。
- `h264dec.c`：`8cd76b2b60cd3d64124901b27e4850e13b3532d5d5fe839f323bae4ed9cfdd7a`。
- `h264_slice.c`：`6b7593bb14c604bd4f0321b5a8b80a055bc343ee7fd8b68e4149d311f9cd21d5`。
- 最终 libavcodec：`cb2d65e540871e9d9fcaf84d9bc5227b3ca1f975a4e17a7d520f0557c9dc5f59`。

## F1/F2 的关闭依据

`h264_select_output_frame()` 的函数体已与基线逐字比较，相同。首 slice 只执行原来的单帧选择，不再摘除额外 delayed 图。自动首 anchor 仍来自旧 DPB 已选中的 KEY；IDR 建立 HRD clock 时绑定 SPS/PPS 快照，但不因此启用自动输出。外部 verified-even 的单独初始化逻辑仍有不同的调用者契约，不混同为直播自动证明。

`h264dec.c:607` 的 `live_check_au()` 在正常 NAL 处理前扫描整个已 split 的 AU。它记录重复 timing/BP、late SEI/参数、SEI 外壳截断、BP ID 及 payload 位数，错误标记在当前 AU 内不可恢复。当前扫描保守地拒绝第二个 VCL slice NAL，因此正常和损坏的多 slice AU 都不进入扩展，也不延迟真实线程 setup。

运行时 SEI/PPS/SPS/slice 错误会继续设置同一个 invalid 标记；扩展不改变原解码器的 lenient/strict 返回策略。`h264_slice.c:1419` 后核对当前 decode_error_flags、参数内容、timing 最小位数、BP 最小位数/SPS ID/成功解析状态，再计算 proof。`present` 已不能单独为短 timing payload 提供资格。缺 timing、重复 counter、MMCO5、矛盾或参数变化清 active/ready，直到新的有效 IDR 才重建 HRD clock。

参数快照在 IDR 成功建立 clock 时即保存（`h264_slice.c:1464`），覆盖首次 anchor 之前的 SPS A→B→A 路径；旧 KEY 作为 anchor 前还比较它自己的 PPS/SPS 内容，不能把旧参数的 KEY 套到当前 clock。之后 pending 图间和当前图相对已输出 watermark 的矛盾检查均在额外选择前执行。

整个额外输出路径是：完整 AU NAL/slice 解码结束 → `ff_h264_field_end()` 成功 → plan → prepare → apply → 真实 `ff_thread_finish_setup()`。保留原 `setup_finished` 布尔设置时点，使原 late-NAL 容错路径不因延迟真实 setup 而被意外改写。真实 setup 被 hold 的候选在 invalid 时也先清 proof，再发布给下一 worker；其他 AU 沿原 setup 路径。

## 提交原子性与线程/引用复核

本轮初版复审发现并反馈了三个问题：逐张摘队后的后续分配失败可能丢图；真实 setup 在输出分配前发布；负错误返回只清 active、未清 ready。另反馈了 anchor 前参数快照和正在解析的 packet 在内部 reinit 中释放的问题。最终版本已修复，检查结果如下：

- `ff_h264_live_output_plan()` 只把 delayed 指针复制到局部数组模拟额外前缀。它会更新当前 proof clock/anchor 状态，但不摘除额外 DPB 项，不提前清额外图的 DELAYED 标记。准备失败后 active/ready 与当前 tick 被清，已修改的 clock 不再供后续非 IDR 使用。
- `live_prepare_outputs()`（`h264dec.c:1097`）先完成全部 AVFrame finalize 和私有 FIFO staging。准备中使用局部 H264Picture view 模拟 recovery，不预先修改额外图的 recovery 状态。任意分配/finalize/FIFO 写失败，私有 staging 全部释放，额外 delayed 项仍在，退回原单帧 finalize。
- 已成功准备后，从 staging 读第一帧使用 `flags=0`。独立检查 `libavutil/container_fifo.c:206` 证实该路径为 `av_frame_move_ref`，不分配；之后的 FIFO swap 和 apply 没有可返回失败的操作。再发布真实 setup，因此下一 worker 不会看到半个额外前缀。
- apply 清额外 DELAYED 标记、摘队、推进 watermark；旧单帧已经由原选择器摘除，不重复摘除。plan 指针在当前 DPB 持有且真实 setup 未发布期间存活；prepare 成功取得输出 AVFrame 引用后才允许释放额外 DPB 持有。移除旧 speculative H264Picture pending 队列后，未发现缺失的生命周期持有。
- 原 n9 worker 循环允许一个任务接收多帧，并按任务顺序退役。proof scalar/参数快照在 `ff_h264_update_thread_context` 复制；私有输出 FIFO 不复制到下一 worker。frame-thread reference marking 仍在原有 context-update 分工中执行，未额外做一次或跳过一次。
- decoder 硬错误清 active 和 ready；取包的 EAGAIN 保留正常 proof，其他硬错误清 proof。flush 清队列、clock、参数引用；packet unref 移到外部 `h264_decode_flush()`，内部参数 reinit 的 `flush_change()` 不再释放当前正在解析的 packet。close 释放 packet/FIFO/参数对象。

本轮 OOM 实测覆盖 FIFO staging 第1/2/3次写入失败，不能代表对所有 malloc/ref/output metadata 分配点的故障遍历。但源码中这些准备失败共享保留额外 DPB 的回退路径；未发现失败后额外前缀已传播或泄漏的剩余路径。原单帧自身的分配失败保留原错误语义，不承诺错误后仍可无损继续且不 flush。

## 独立证据核验

本审查重新读取 raw `.sha256` 逐行比较；正常基线单 cycle 按测试脚本重复到结果 cycles。未仅接受 summary 布尔值。

| 最终证据 | 结果与边界 |
| --- | --- |
| `out/phase3/checks-final2/summary.json`，88对 | 共同前缀像素/PTS/记录 metadata 全匹配，terminal 状态全匹配；异常 AU 无额外交付。 |
| 其中6个 strict parser-error/truncated | off/on 为10/11帧，多出的源 AU 是10；未来坏 AU 是12，错误返回同为 INVALIDDATA。保留该差异，不记作全长度一致。 |
| 其中15个 multislice | 正常、late、parser-error、short、后续 slice 损坏，在 legacy1/async2/async12 全列精确一致，包括 DTS。原中间版本的稳定损坏帧像素差异已由保守回退关闭。 |
| 其中9个 FIFO ENOMEM 注入 | 日志均有实际注入记录，192/192帧，像素/PTS/metadata 保持，仅允许触发 DTS 变化。未把未命中的注入算通过。 |
| 其中28个 BP/参数边界 | 非IDR BP、BP邻近/之后wrap、BP错SPS、clock/fixed-rate变化、参数相同重发；3cycles及同步/异步组合有序结果一致。 |
| `out/phase3/representatives-final2/summary.json`，83对 | raw 比较全部通过。含固定/长B/自适应HRD H264、HEVC、无HRD H264、单独证书模式、场/重载等回退、gate/flush/error/close/API生命周期。 |
| `out/phase3/extra-boundaries-final/summary.json`，10对 | anchor前SPS A→B→A完全回退；短/截断/重复BP和PPS内容切换保留错误前已发生的正常提前效果，之后禁用。raw 全部一致（DTS按已说明范围除外）。 |
| `out/phase3/asan-summary.json`，5项 | 正常、短timing、late、多slice损坏、BPwrap+in-flight flush；12线程、各3cycles，实际 ASan build，terminal=0且日志无ASan错误。`detect_leaks=0`，不声称通过LeakSanitizer。 |

代表测试中同步 legacy 1/2/12 thread，异步2/12 thread的 early off/on均有实跑。应用配置最终初始化测试通过，但服务自身仍没有 async-off配置；不能称完整 Linux 服务四组合实跑。旧 SDK 的 H264 option 缺失仍保守回退；应用不为任意输入设置 verified-even。

Phase 2 的原始配方已在被拒构建实跑：parser-error/truncated/duplicate/late 的坏 AU12 确实仍额外交付2帧，而关闭模式要等AU13/14。该证据证明原守卫缺口真实存在；当时未发现这些配方的合法流像素/PTS错序，不应追溯夸大。中间 phase3 多slice损坏 async2 的像素差异也保存在 `checks/` 与16次重复记录中；它与旧 lenient error_flags 0/12 波动不是同一问题，未将二者合并隐藏。

## 仍须保留的范围与未证明项

- 自动模式仅依靠当前输入的规范 HRD/CFR 声明，不扫描未来GOP。原规范推导见 phase2 独立报告：C.1.2 C-8/C-9/C-10、C.2.2 C-12、E.2.1 E-78/Table E-6，以及 **D.2.3** 的POC顺序约束。D.2.2仅为BP语义。没有发现满足这些约束的合法流更早插入反例。
- 当前多 VCL slice AU、场、pic_struct/重复显示等不支持路径保守回退。普通不带 HRD 的 x264自动模式没有收益；外部 verified-even仍须可信调用者对完整精确输入签发契约，不是直播证据。
- 流符合性仍是假设，不是完整 HRD 合规校验器。无法从已收到的字节证明未来永不损坏；严格未来错误可留下更长的已交付正常前缀。既有 `no_output_of_prior_pics_flag` 抑制语义范围限制保持，未据此声称完整HRD输出时机一致。
- 没有证明所有恶意/损坏构造均与基线有精确字段、同长度前缀。没有穷举所有重配置、packet side-data、32-bit时长字段、超长时间间隔和全部分配故障；硬件加速未在本build启用。结论仅覆盖原型限定范围及本轮证据。
- 延迟真实 frame setup会改变并行重叠，最终phase3性能测量在本报告完成时仍进行中。本报告不提供新延迟/吞吐收益数字；旧phase2的162次测量不能当作这版二进制的测量。Linux完整服务、部署、SDK pin更新和生产默认值变更均不在本轮验收内。

本轮专项复审可关闭F1/F2及过程中发现的提交/恢复问题。当前没有需要额外授权才能继续的阻塞；最终性能结论由同一冻结构建的新测量另行汇总。
