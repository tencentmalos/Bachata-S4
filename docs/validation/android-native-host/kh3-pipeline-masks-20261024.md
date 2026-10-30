# KH3 慢帧：动态写掩码管线复用（2026-10-24）

**最新结论：去重候选已撤回，未安装。** Odin3 重新连接后的实际缓存审计为
2569 条 graphics / 2569 个归一化唯一键，重复为 0；当局 87 条新管线中 86 条
含新着色器排列。88.86 ms 管线的两个 stage 排列都是本局新增，不能由掩码归一化
复用。下方为候选实现及其撤回依据，不能把合成用例结果当成游戏优化收益。

## 本轮范围

在用户要求“看一下慢帧的情况做一轮优化”后，读取 Odin3 `6edb0593` 的现有驻留 ring。
应用仍是 APK `ac5b5a96`，KH3 CUSA15072 update 01.10、PID 24451 / generation 1，
run UUID `b820015f8830eda4a7447bdf16e05d36`。采集时状态为 Paused/background，
末次呈现距查询约 623 秒；没有发输入、恢复、停止、覆盖安装或修改配置。

本地原始证据：`build/validation/kh3-slowframes-opt-20261024/`。
PROF 文件 `ring/6edb0593-24451-ring-1792808535402309-4.prof`，67,025,633 bytes，
SHA256 `04bbe3d11cff1d14cf6fe0f951b0f7d310413bd7e1ba9e4283bef8269ca08c6e`；
LiteP index `7dbc30ea802f488ab1c40a9c343417e1`。原始文件及分析 JSON 留本地，不入仓。

## 当前慢帧

排除启动线程 24615 的两个约 1 s 周期、线程切换、缺标记和末尾未闭合周期后，
Guest-31 / TID 24616 有 2503 个完整 GNM 提交周期：

| 指标 | 结果 |
|---|---:|
| 完整周期时长总和 | 44.378824 s |
| 首尾跨度 | 46.497716 s |
| 中间缺标记窗口（排除） | 2.118892 s |
| 平均 / P50 | 17.730 / 16.703 ms |
| P95 / P99 | 26.005 / 37.082 ms |
| >33.33 / >50 ms | 56 / 5 次 |
| 最大 | 81.659 ms（SDK frame 67557） |

这些是提交标记间隔，不是上屏 FPS。与此前不同场景不能作性能 A/B。
256 chunks、0 skipped、0 scope pairing errors，但 ring 按线程独立覆盖：GpuComm
仅保留末尾约 0.375 s，Guest-1 约 0.649 s。不能从它们在早期慢帧中缺席推断无工作。

frame 67557（81.659 ms）和 69691（62.344 ms）的 owner 分别在条件变量等待
81.607 / 61.398 ms。没有足够的生产者数据把它们归到游戏 GC、GPU 或宿主锁。
87 次 Pipeline.CreateGraphics 的最后一次早于这两个周期；这两帧不是同窗新管线编译。
新分段保留的 PoolDecommit 最大 11.629 ms，GPU invalidation 最大 0.603 ms，
未复现昨日 79 ms 的回收尖峰。无 sched/KGSL/GPU query sidecar，不推断 on-CPU 或 GPU busy。

已确认的编译等待来自[此前同会话报告](kh3-slowframes-20261024.md#管线构建停顿仍存在)：
SDK frame 17270 的 85.85 ms 周期有 88.86 ms 管线创建 → recorder 等待 → 提交排队。
本轮查询的累计统计为新 graphics 87、bind wait 51 次 / 1373.2 ms，峰值 87.41 ms。
预载 2517 条、驱动缓存启动 61,829,062 bytes，缓存确实在用。缺失绘制内容进入
CPU readback 后关闭 skip 的正确性保护继续保留。

## 候选实现（随后撤回）

Odin3 日志明确启用了 `extendedDynamicState3ColorWriteMask: 1`。现有
`GraphicsPipeline::CreateNative` 在此路径中已把颜色写掩码声明成动态状态，并使用固定
RGBA 静态占位值；真实掩码由 `DynamicState::Commit` 每次绘制发出。但是缓存原先仍
把 `GraphicsPipelineKey::write_masks` 纳入哈希和相等比较，只有通道掩码不同也会
创建新的布局、管线及等待任务。

本轮修改：

- `GraphicsPipelineKey::ForLookup` 仅在支持动态掩码的设备上归一化 write_masks。
  其余着色器、附件格式、swizzle、blend、samples、深度、拓扑等字段继续区分管线。
- 在线查找与上一条管线 memo 使用同一查找键。绘制使用当前准备好的原始掩码，
  不再从第一次创建的管线读取旧掩码；直接与间接绘制均经过该路径。
- 预载旧配方使用相同规则，在布局创建/编译调度前去重；选择使用记录优先级最高的
  原配方作代表，避免冷变体挤掉热变体的预载优先级。原始配方及其哈希保持不变，
  usage 保存仍按原始配方身份，未改磁盘格式版本、未清缓存。
- shader replacement 仍按实际 map key 移除对象；不支持该动态状态的驱动保留原键。
- 新包同时包含上一轮 Foundation `bbe7709b` 的慢帧来源线程迁移修复，便于后续及时
  留下高频线程记录。该修复的独立验证见上一报告。

这项改动减少可合并状态造成的管线数量；它不缩短一条真正新着色器的驱动编译过程。
尚未取得 Odin3 的配方文件，**不能证明此前 88.86 ms 那一条属于可合并项，也未测得
本游戏实际合并数或帧率收益**。

## 验证与边界

- macOS C++ 回归 `tests/video_core/pipeline_key_tests.cpp`：785 checks / 0 failures。
  覆盖 8 附件 × 16 通道掩码、静态驱动回退、原配方/哈希保留、其它状态不能合并、
  动态状态脏位、新命令缓冲重新发送及替换管线后的键失效。
- 合成输入共有 128 次请求、121 个原始唯一键；动态查找合并为 1 个，静态路径仍为
  121 个。这是缓存键回归，**不是实机 Vulkan 编译次数或游戏收益测量**。
- 负对照只移除归一化操作：同 785 checks 中 241 failures，动态唯一键恢复 121。
- Android NDK host、`pipeline_key_tests`、`command_recorder_tests` 编译通过；未在
  Android 执行这两个测试。APK 构建与身份见下方交付记录。
- 没有新的游戏画面或运行 A/B。采集后 Odin3 从 ADB 消失，仅 Thor `9c2841a4` 在线；
  未把另一台设备当成 Odin3 测试，也没有安装/结束原会话。

重新连接后先只读审核实际配方，量化可合并排列；用户保存并安装新包后，验证冷/热
启动预载、通道掩码切换画面、自动慢帧窗口及相同场景的构建等待。不能以单纯分布
下降代替同场景对照。

## 候选包（已撤回、未安装）

- APK SHA256：`ecb1244128c6fb5ac2cefaab9021ccf8d5ba4b6f213955467af1fbbb8b71d2a3`。
- APK 内 host SHA256：`578c523959c24d22297280959712f933917ea7c8be06ad7aeae3cd7545d030b4`，与最终 NDK 构建产物一致。
- APK 本地路径：`build/validation/kh3-slowframes-opt-20261024/shadps4-kh3-dynamic-masks.apk`。
- 最终 Gradle assemblePlaystoreDebug 通过；尚未安装到 Odin3。

## 重新连接后的实际缓存审核

用户回复重新连接后，同 PID 24451 / generation 1 / 同 run UUID 仍为 Paused/background，
没有恢复、输入、覆盖安装或退出游戏。通过 run-as 只读导出 `*.key`、`usage.use`、
`profile.bin`，不读取 ROM、存档或账号数据。归档 `odin3-recipes.tar` 为 6,687,744 B，
SHA256 `4153f47a65a059f0725fbfb64d9c99e412179537f62cfa939b5e2c7d24b99238`。

全部 2606 个 envelope 的 magic/version/length/XXH3 校验通过，2604 条配方版本为 7。
对 graphics 还验证了完整 key 的 XXH3 与文件名一致；布局从生产头文件编译取得，并用
Android NDK 静态断言核验：sizeof(key)=344、write_masks offset=280/size=32。
脚本仅在本地副本中清这 32 bytes，以完整归一化字节分组，不依赖哈希相等推断键相等。

| 实际缓存 | 结果 |
|---|---:|
| graphics / compute | 2569 / 35 |
| 去除动态掩码后的 graphics 唯一键 | 2569 |
| 可合并旧配方 / 本局新配方 | 0 / 0 |
| 本局新 graphics（mtime 与累计统计吻合） | 87 |
| 本局新增 stage 排列（与 translated 统计吻合） | 146 |
| 包含新 stage 的新 graphics | 86 / 87 |

目标 `g_0x53dc2e18266aa87a.key` 单独一组，mask `[15,15,15,15,15,15,15,0]`，
usage first rank 657 / first_ms 317049 / compile_us 88857；日志 88.86 ms。它的两个
非零 stage key 均未在启动前配方出现。这支持“真实新着色器编译”，而非已有配方
因掩码不同再次创建。并不据此解释其它 owner 条件等待慢帧。

据此撤回全部动态掩码候选代码和测试 target，保留之前的音频、会话、mutex、诊断与
Foundation 慢帧线程迁移修复。候选 diff、测试源码和正/负对照结果留在本地证据目录，
候选 APK `ecb12441` 不作为后续待安装包。新生产源码不承担这项无当前收益的查找副本
和去重开销。后续应安装仅包含慢帧采集修复的候选，再获取缺失的生产者窗口；安装
会结束当前局，须等用户保存后明确同意。

## 撤回后的采集修复包（已安装）

已重新构建 Android host、host_library_smoke、command_recorder_tests 和 Playstore Debug APK，
不含动态掩码候选，保留此前慢帧采集修复。随后按用户“已保存，可以安装”的授权覆盖安装，详见下节；未执行性能 A/B。

- APK SHA256：`35a79908b99ab653eaa959c448699e17b9e01751538cc7cdb7adb01fe1f79828`。
- host SHA256：`732d6391d35fad94cdbdbf4b3fc227621d65766f41d656fcd11c7d3b80d2633e`，已核对 APK 内库与构建产物一致。
- 本地 APK：`build/validation/kh3-slowframes-opt-20261024/shadps4-kh3-slowframe-capture.apk`。
- 此前纯只读检查结束时仍是 PID24451 / gen1 / Paused/background；用户保存授权后才结束旧会话。

## Odin3 覆盖安装与自动采集验收（11:03–11:10）

2026-10-24 11:03，`adb -s 6edb0593 install -r` 成功。再次读取设备实际 `base.apk`
的 SHA256，与上列 `35a79908…` 完全一致；包内 host 已与构建产物核对。
不清数据、配置、存档或着色器缓存。通过明确的 `game_id=CUSA15072` 启动 KH3，
新 PID30164 / generation1 / run UUID `dbe6e7f341f82d05e6c9a09da171acca`。
选择 Continue，随后确认加载页的“开始游戏”，进入黄昏镇餐馆门口；没有移动角色或保存进度。

在启动 FrameMark 约第 2 帧开启自动采集，参数为 factor=2、min_ms=33、pre=5、post=2、
count=8、mib=8，默认 2 s 历史 / 30 个基线样本 / 1 s 冷却。未打开 fine scopes、GPU
detail 或 continuous streaming。首批保存 8 个窗口，0 failed；另有 1 个待完成窗口取消，
跨来源窗口未当作正常慢帧。首批全部导出后重新布置场景采集，冷却改 5 s，仍最多 8 份、
每份 8 MiB，避免持续快照。

原始 PROF、sidecar、截图、各状态和索引在本地
`build/validation/kh3-slowframes-opt-20261024/installed/`，不入仓。

| 阶段 / SDK frame | 提交周期 | 导出文件尾缀 | SHA256 |
|---|---:|---|---|
| 片头 1315 | 45.206 ms | `slow-1792811046794304-1.prof` | `745f53eeb7e4280abe136d323f4ac8407664051a656672242cbfe3c466d0e74d` |
| Continue 加载 8651 | 101.868 ms | `slow-1792811166393479-2.prof` | `8829b9641dc9e2206171800560c6d34c2e39a926b0f7c89a1bb71dbd4a4c57f1` |
| 场景切入 10598 | 39.154 ms | `slow-1792811223983339-3.prof` | `06ec8bb059a19279f380b7b4993fa1ba33d9eb272d06b052adedede52e2ecc0b` |
| 餐馆门口 19426 | 33.361 ms | `slow-1792811362546704-9.prof` | `68b1f6b82ede831f108d119fc4694f96f2f468c16ed8d6628e0c81758f16466f` |

4 份索引各有 8 个完整周期（5 个 pre + trigger + 2 个 post），结束标记单列为
missing_end_marker，不进入完整周期统计；无丢块、截断或 scope pairing errors。
PROF header 的初始 main_thread_id 为 30332 / Guest-30，实际触发及完整窗口的 marker
owner 是 30333 / Guest-31，说明旧版的“只接受初始线程”漏采路径已被实机覆盖。
没有注入假 FrameMark、强行停线程或下调倍率制造触发。

frame10598 的窗口覆盖约 171.82 ms，保留 Guest-1 10812 条、GpuComm 28519 条、
VkRecorder 6353 条事件，三者记录均覆盖 trigger 的起止时间。与此前几十秒后手工 dump
相比，关键高频线程没有全部落到慢帧之后。窗口边缘仍有未闭合 scope，且没有 sched/
KGSL/GPU query sidecar，不能据此宣称每条线程全程留存或推断 on-CPU/GPU busy。

此次只验收采集。frame8651 的 owner 条件等待为 101.720 ms，frame10598 为
36.880 ms；没有显式 selected-notify 链，不能据此把原因指认到某个生产者。
frame10598 窗口没有 Pipeline.CreateGraphics，PoolDecommit.GpuInvalidate 最大
0.193 ms，也未重现此前约 79 ms 的内存回收尖峰。

启动加载驱动缓存 62,848,874 B，预载 2604 条配方耗时 541.4 ms。11:09 读取累计统计时
新 graphics / compute / shader translation 均为 0，pipeline bind build wait 为 0；
这是本次暖缓存场景，不能当作对前一次游玩的优化 A/B。音频 Oboe stalls/errors/
reconnects 均为 0；未做长时间听感或过夜验证。

交还时同 PID 正常 Running，DebugBus pad 已释放（owner 为空、debug_active=false）。
ring 与上述有上限的自动慢帧采集保留开启，continuous capture 关闭；没有调试器、输入
循环或后台抓取脚本。采集器满 8 份后不再增加快照，后续需导出后重新布置。

## 11:46 PSO 缓存现场核查

用户询问缓存是否生效时，同 PID30164/gen1/UUID 会话已从后台恢复。只读状态与日志
保存在 `build/validation/kh3-slowframes-opt-20261024/pso-check-114656/`。
启动成功载入 62,848,874 B 驱动缓存，预载 2569 graphics + 35 compute 共 2604 条。
本局首次使用 734 种管线，新增 graphics 3、compute 0、shader translation 4，
构建共 63.8 ms、最大 48.22 ms；bind 仅等待 1 次 / 11.00 ms。因此缓存正在复用，
不能把剩余卡顿归为缓存整体失效。Turnip 的 `reported_cache_hit` 仍有既知误报，
不拿该字段计算命中率。

导出自动窗口 id15 / SDK35021，PROF SHA256
`9f21a281973ddc3541417bd48172f547ee281dbdb1da0d14cd789cadf4173dec`，
LiteP index `cb43de5ac888417ab2be44743acadccf`：8 个完整周期，14 chunks，
0 skipped / truncated / scope pairing errors。87.733 ms trigger 的 owner 在
sceKernelWaitEqueue 中 73.090 ms；窗口内没有 Pipeline.* 或 WaitReady scope，
PoolDecommit.GpuInvalidate 最大 0.480 ms。该窗口没有新 PSO 编译证据，但缺
selected-notify / sched / GPU sidecar，不能进一步把等待指认为 GPU 瓶颈。

持久化仍有缺口：`DriverPipelineCache::NotePipelineCreated` 同时要求累计新增
16 条且距上次请求至少 30 s，worker 没有超时刷新；正常析构会最终 Write。
因此少量新增可能长期只在内存，异常退出后下次仍要编译。当前累计实际文件写入
仅 1 次，11:03 的 63,175,602 B；配方/usage 已到 2607。这是落盘时机问题，
不影响本局内存缓存复用，本次核查未修改代码或运行设置。

另发现慢帧 id13 把后台暂停约 1575.8 s 计成提交周期，必须从性能分布排除；
暂停/恢复边界重置采集基线仍待补齐。其余窗口不能据此直接删除。
