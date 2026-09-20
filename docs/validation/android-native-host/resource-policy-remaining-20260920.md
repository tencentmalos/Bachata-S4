# 资源策略剩余项评估与实施（2026-09-20）

基线：本地未提交资源策略，已安装 APK `9d6504bf` / host `e4414aca`。此前实施与测量保留在 [上一阶段报告](resource-policy-vma-20260920.md)，本报告追加纠正，不改写原始证据。原要求：[spec v2](../../specs/android-internal-scale-resource-policy.md)。

## 评估结论

剩余工作包括实现缺口与验收缺口。配置分离、低档物理 mip-drop、尾链上传、VMA 账本和内存档位已有定向证据，不需要重写。优先处理内容一致性、资源身份和等待生命周期，再做小 RT 准入与成本校准。

| 优先级 | spec 对应 | 现状／缺口 | 完成条件 |
| --- | --- | --- | --- |
| P0 | §7.2 / §4.5 动态 extent | 锁重入及格式复用误判已修复，真实 cache 往返两驱动20/0，血源诊所MOVE/正常Stop通过 | 本轮确认的死锁已闭环；真实guest DRS/hint历史内容仍需独立验收 |
| P0 | §4.5 回读 | 完整线性RGBA cache→guest backing和36MiB回读已过，ring失败已补临时buffer；padding、depth/stencil、异步生命周期仍需审查 | 保留最新内容、guest 布局与地址生命周期；大图、非 coherent、取消及旧 view 退役验证 |
| P0 | §5–6 streaming | 当前以描述符 mip 数决定 drop；没有足够的 resident/initialized 证据或完整部分 dirty 验收 | 保留链可读且已初始化后才准入；无效/未上传 mip 不被新增访问；0→1→3→1 与地址重用检查 |
| P1 | §7.1 小 RT | 短边 ≤64 一律保守 native；缺少后处理链证据传递与合法 1×1 尾端处理 | 以 producer/consumer、附件和 shader 语义证据准入；未知 UI/阴影/历史继续保护；不按游戏名特判 |
| P1 | §10 覆盖率 | 旧 `attachment_passes` 实际在每次 BindResources 累计，是 draw 计数。99.7623%→98.5735% 不能当作 pass 比例 | 分别报告 draw 与实际 rendering scope，保留口径版本；按原因/资源族解释 native 回退，复测原门 |
| P1 | §4.1–4.4 身份/复制 | 有界 plan、首次附件重规划、copy 继承已有；不同 layout、GC 重建、映射复用及多子资源仍缺整链证据 | 确认最新 backing、部分更新、plan/内容代际、不发生 stale view 或重复提升 |
| P1 | §6 / §10 shader | 像素/尺寸/fetch 与 view base 已测；全部采样 LOD/Grad、sampler 范围、gather fallback 未闭环 | 真实 GPU 对照与 SPIR-V 验证，Render=1/Texture≠High 也覆盖 |
| P1 | §8.1 VMA | 内存档位及真实账本已有；较小 ring 的 wrap wait、超大图 fallback、重新上传成本仍需实测 | 同场景峰值/待退役/实际归还、CPU/GPU/P50/P95 分开报告；不把 VMA 当 PSS |
| P2 | WP2 实景 | TMNT及修复版血源均已有限MOVE/Stop；极端 Render 与画质/性能匹配样本缺失 | TMNT+血源可操作、正常 Stop；HUD/材质/后处理；未触发 DRS/streaming 明确未覆盖 |

canonical 跨对象 streaming、host 自适应分辨率/动态预算、新 codec、通用 HUD 拆分均非本版必需项。scratch pooling / fused detile-scale 是测量后选择的优化，不以尚未实施判整个正确性路径失败。

原 spec 的总覆盖率不下降与小 RT 保守保护可能冲突；本轮保留原验收门，不通过删保护或改分母宣布通过。先纠正计数口径、给出同场景资源族和原因证据，再评估是否需要另行调整要求。

## 血源死锁现场

Swan PID14725/gen1，run UUID `d55ddccb8efe28649ca2881d4a2e1567`，Turnip86ca472f，Render0.5/TextureMedium。两个非停止快照相隔24.507秒，flip761、PM4 2387973、draw59044、present761均不变，队列提交/overlay仍前进；首次观察游戏推进已停止443秒。

独立 Native Debugger session `7f6e1a930bed4acc98738ee7bcf00f1a`，exact Build-ID `319b15342906d15954310d4c743bab3b247f0796` 匹配已安装 host。GPU命令线程19939：`FindImage → ResolveOverlap → UpdateImage → std::mutex::lock → futex`。FindImage 的 scoped_lock 生命周期覆盖 ResolveOverlap 调用，同一 cache 的非递归锁发生重入。现场新描述符为25×21、pitch128、B10G11R11、Thin2DThin；同地址旧缓存格式R16G16B16A16Sfloat、CpuDirty|GpuModified，足以证明不能把任意同地址尺寸变化都归为DRS。

提交线程19941在SubmissionWorker空队列条件变量等待；此现场无需归因Turnip/KGSL。调试器已continue/detach并清理，当时目标仍存活、启动身份未变、TracerPid0；未热改寄存器或强行解锁。随后用户明确批准结束卡住会话并验证修复。正常 UI Stop 停在 Stopping 超过9秒，因此对该旧进程执行 force-stop，核对 PID 消失后才跑 GPU 检查及安装。

完整本机证据：`build/validation/resource-policy-remaining-20260920/debug-hang-final`（含协议、栈、变量、清理证明）；两次进度快照同目录。旧 host/APK 已另存，避免后续构建覆盖匹配符号。

## 本轮已落地

1. `ResolveOverlap` 在 `FindImage` 已持有 cache mutex 的范围内直接执行 Track/Touch/Refresh，不再调用会再次取同一锁的 `UpdateImage`。extent 变化分支增加格式、类型、位宽、sample、mip/layer、tiling 和 bank swizzle 兼容检查；格式池复用继续走原 alias 路径。
2. compatible extent 退休时仍保留最新 GPU 内容并同步写回 guest backing。内部转移在退休 image 的独立 plan 副本上进行，避免其临时 native readback 污染持久 plan；原有 storage/exact/readback 等安全决定没有被清除。这样 `1920×1080→1600×900→1280×720→1601×901→1920×1080` 回到旧尺寸时仍按其原安全计划分配。真正的 guest/GC 回读仍永久 native，并标注 upscaled 的有损边界。
3. image 回读超过32MiB ring时使用按需临时 buffer，异步任务按值捕获其所有权；GPU完成后 invalidate VMA allocation 再写 guest backing，不把 ring Map 失败当有效指针。没有扩大常驻 ring。
4. `Scheduler::BeginRendering` 返回是否实际新建 Vulkan rendering scope。直接/间接 draw 分别累计 draw 和实际 begin-rendering 数，诊断 `attachment_counter_version=2`。新增首个 native 转换次数和最多128组 fragment/尺寸/原因/附件统计，溢出显式计数；总 draw/pass 不受分组容量影响。分组 pass 归属为启动该 scope 的 shader，不代表该 scope 只有一个 shader。

## 定向验证与保留的失败

| 检查 | Qualcomm | Turnip86ca472f |
| --- | ---: | ---: |
| 原有 Image/TileManager/scale/VMA GPU probe | 13,327,401 / 0 failures | 13,327,401 / 0 failures |
| 新真实 TextureCache + Rasterizer + guest direct backing | 20 / 0 failures | 20 / 0 failures |

新 fixture 覆盖上面的尺寸往返、最新 GPU clear 内容回写、显式 readback promotion/upscaled 标记、R16G16B16A16→B10G11R11且尺寸25×21的地址复用，以及36MiB回读首尾像素。实际使用非递归 cache 锁、Vulkan队列、映射/保护与scheduler退役，不以纯 plan mock 替代。

首轮夹具使用空 Rasterizer 导致 PageManager::UpdatePageWatchers 空指针，崩溃日志保留；改为完整生产 Rasterizer 后，第二轮20项中1项失败，揭示回到旧 extent 时继承内部 readback 的 native 标记。补退休 plan 隔离后，两驱动各20/0。不能把夹具崩溃写成生产修复包崩溃，也没有删除第二轮反例。此处尺寸 fixture 是线性 RGBA；不代表 tiled padding、depth/stencil 全回读组合通过。

CPU policy34/0，host/APK构建成功、`git diff --check`通过。未改shader lowering，本轮不重复宣称新的 Grad/LOD 验收。

## Swan 血源实际复测

安装与设备回读 SHA 完全一致：

- APK `6d89758bab35a9dc40b257893636e8f8a7ff33fc7e0280defd33100b191ea24f`
- host `c07bdea3bce54413e8dbbb30bcfbb6cc0ad1083a99b6427296bda7bf4902dbe8`
- JNI `105c3d53be9ac3b30a594db9b2c5beb2b7b4b5806f94b5194e75b7221e9a5907`

普通 Library Launch CUSA03023；PID16849/gen1，UUID `d1006e9e63a2a963711b3ae2da7c3e08`，Turnip86ca472f。用户当前 Render0.5/TextureMedium 原样保留，global JSON 前后逐字节一致，没有新 per-game override。

越过旧卡点，实际经过开场人物/字幕进入诊所，角色与HUD可见。短时真实触屏摇杆移动/相机操作后，前后截图可见人物位置与游戏视角变化；host presents 从3448到3476，之后持续到4286。此为有限 MOVE/相机验收，不包括战斗、存档循环或完整游戏稳定性，也不是原卡点精确 shader 的逐指令重放。

最终正常 UI Stop，在1.771秒内得到 `Stopped / user_stop / guest return=0`；修复会话无需 force-stop。TracerPid0、输入token0/buttons0、RenderDoc未加载、profiler capture idle，未装新探针/自动输入循环。保留用户的中档与Turnip设置。

`bloodborne-fix-memory5.txt` 在诊所前后累计：draw100281/1912701=5.2429%，实际scope45354/459207=9.8766%；含加载/动画，非固定场景增量，不能与前轮TMNT/legacy作A/B。128组溢出1,021,653 draws，因此只能用已有分组作候选线索，不能宣称完整覆盖原因归因；主场景存在 readback/mixed-pass native 连锁，覆盖率任务仍未完成。

该快照 VMA reserved3027.14MiB、live2597.82MiB、free429.32MiB、retiring53.16MiB；PSS1008.96MiB、SwapPss903.46MiB独立报告，不能相加或据此声称净RAM/FPS改善。系统内存压力仍高。下一步优先追实际主附件为何发生readback并传播mixed-pass，保留保护规则；再补有效mip、部分dirty、small-RT语义准入和采样组合验收。

本轮代码仍本地未提交，未改FEX/Foundation/Turnip子仓，未做完整游戏回归。表中其余项继续保留，不能把已确认的死锁闭环等同整份spec完成。证据索引见[manifest](evidence/resource-policy-remaining-20260920/manifest.json)；大二进制、现场调试证据与含真实环境的头显截图留在忽略的 `build/validation/resource-policy-remaining-20260920/`。
