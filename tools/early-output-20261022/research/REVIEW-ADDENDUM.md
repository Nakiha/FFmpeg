> **实验版，最终验收未完成，不建议生产启用**
> 历史文档归档：正文中的“当前”、未推送状态及验收结论仅属于原阶段；以 `doc/early-output-20261022/REPORT.md` 的版本核对为准。`out/` 路径指本机保留证据，不随公开分支发布。

# 父任务复核补充：区分语义失败与严格等价失败

不重开实验，不改变 acceptance.json 的 rejected 结论，不改变默认或推送。

1. POC0 反例是基线接受的、独立8.1.2也按同序输出的码流，不是经过完整HRD/conformance工具认证的码流。结构为Main77、8bit420、16x16逐行CAVLC I_PCM，frame_num/POC LSB都是4bit，refs4，reorder2/maxDPB4；解码顺序相对POC为0(IDR ref fn0), -2(nonref I fn1,LSB14), 2(ref I fn1),4(ref I fn2),6(ref I fn3)，显示顺序为-2,0,2,4,6。POC0公式8.2.1.1允许MSB向负方向推导；8.2.1对IDR规定自身min field POC为0，对后续场POC的有序/唯一性约束并不等同于“后续POC必须正”；7.4.3的frame_num约束在该序列中按preceding reference推进。此解释不替代完整规范认证。它直接否定的是对所有基线接受输入“仅因当前NAL为IDR且当前POC=0就先输出”的等价规则；不能由此泛化否定任意更强前提的合法流优化。

2. 正常POC2样本的唯一差异是AVFrame.pkt_dts（首帧96000→0，输入time_base为1/1200000，即80ms→0）。像素、顺序、帧数、PTS、best_effort_timestamp均相同。decode.c将当前触发codec返回的packet DTS赋给未自行设置DTS的decoder输出，所以这是可解释的输出触发字段变化，不是呈现时间轴错误。原验收按“所有时间戳匹配”计为失败；若用户明确允许该字段变更，它本身不再是像素/显示安全否决理由。此前10组正常POC2失败应归类为此严格字段差异，另5组坏流才是实际丢帧。

3. 坏POC2流与正常流的首IDR相同，但第二AU nonref frame_num=0，违反7.4.3（非IDR逐行画面不能复用preceding ref的frame_num）；POC2公式推得-1。关：[-1,0,2,4,6]，开：[0,2,4,6]。candidate先将水位设0，第二AU到达时原out_of_order分支移除-1而不交付。五种同步/异步线程配置都5→4，terminal_return=0，画面decode_error_flags也未报告这项违规。这证明当前候选不保留已接受坏流行为，不证明所有合法POC2流不能提前。当前AU的检查无法预知与正常流共享完全相同首AU的未来违规；第二AU才发现已不能撤回IDR。加一帧前视仅覆盖这个具体反例且不符合本任务不增加未来等待的要求；本轮没有实现/证明完整可靠防护。

4. HEVC新增H264补丁不改变任何HEVC代码；原生PTS closed/open及真实RADL测试没发现像素/显示顺序退化。已有94bd940策略在“数值有效但按解码序赋PTS”的输入上改变best_effort_timestamp。最小现有复现是x265-b7-open0.hevc，SYNTHETIC_PTS=1，async2/capacity1/3cycles，对比两个HEVC开关都关/都开。576帧两边相同；第4个0-based显示位置source AU2，PTS均80000us，best_effort从288000变96000（流单位），pkt_dts从288000变240000；同一像素hash。全段351个位置best_effort不同。decode.c的guess_correct_pts使用PTS/DTS，所以触发DTS变更可影响坏时间轴下的修正选择。这是已有元数据兼容边界，不能宣称正常PTS下已发现丢帧/错序，也不能把它仅按pkt_dts豁免通过。H264 FATE extradata异步flush后的错误标志波动也在未改基线复现，非候选特有。

50轮perf-v2与8个FATE样片、覆盖缺口的完整数值保持REPORT.md；没有新测量。两源树保持干净，候选仍只保存为被否决patch和独立二进制。
