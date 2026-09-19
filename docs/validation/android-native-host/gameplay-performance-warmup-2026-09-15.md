# TMNT 实际关卡 warmup 与性能定位（2026-09-15）

本轮首次确认当前 APK 可以从 PLAY、教程确认、关卡加载进入 **Leonardo 的 3D 屋顶教程**，并响应左摇杆移动，镜头跟随，提示由 MOVE 推进到 ATTACK。此前“仅到主菜单、未验证 PLAY”的状态由此更新；不等于完整游戏、长时间连续游玩或 Swan 验收。

实际关卡约 **12–13 FPS / 79 ms 一帧**。优先排查 GPU 工作完成/同步路径；CPU 上 GuestAddressSpace 查询和高频 FEX/HLE mutex 边界也有显著成本。尚未精确定位最慢 shader/pass，不能把下面的 host 区段耗时直接称为 GPU timestamp。

本轮新增外部 warmup 脚本及定向测试、取证和分析文档；**没有修改生产 C++/Kotlin、FEX、Foundation、Oboe 或 SELF，没有实施/宣称性能优化，没有完整回归、commit/push 或新 spec**。保留原工作区改动。

## 1. 测量身份和实际场景

- 设备仅 AYN Thor `9c2841a4`，Android API33 / ARM64 / 4 KiB，Adreno740。未操作另一台 Swan。
- 主仓 `codex/android-fex-round2`，HEAD `4da582b7b3a36d74403a4985c3c03f37e570bcb7` 加既有 dirty 工作；不能用这个 HEAD 代替 APK 源码身份。
- 沿用 [HTTP2 修复的最终 APK 和源码清单](http2-offline-repair-2026-09-15.md)：APK SHA-256 `37330cd7d17903d8417e4be4f01b2de5cc820bdf1e10eaba6bda0b35af05fd8e`，host Build ID `0f5649b4d3b8f518aa568923e7908879b07c288a`，JNI Build ID `717514731d098a4ff3b5fe782737434933366fad`。host/JNI RelWithDebInfo，FEXCore Release，playstoreDebug。
- 固定私有 Turnip，Mesa `26.0.0-devel / 5ac41be677`，SHA-256 `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`。GPU Reshape 未请求，observation/bounds/replacement 均关闭。
- 两份主要 PROF、simpleperf、GPU 利用率均来自 **PID410 / generation2 / run_uuid `340f0cac9b86fab7e1febe345b102801`**，均在 debugger attach 尝试之前。恢复后的 ring 开关对照是同 PID **generation3**，单独记录。
- 原 SELF SHA-256 仍为 `6122da7190de6b08d921b2c42c3ca9ed11dc4d11524f1139ff67aeceea5b204d`。本轮没有换测试游戏文件或修改存档。

输入结论：反复 Circle 确实到达 guest Pad，菜单 ring 中实际读取到 20 次 `0x2000` 按下及 0 释放；它不是“输入根本没接上”。当前默认 `is_circle_enter=false`，系统报告 Cross 确认、Circle 取消。用 Cross 进入 PLAY，再 Cross 确认教程，等待加载后进入屋顶；没有为了推进而偷偷改 Circle/Cross 映射。曾怀疑的 Kotlin controllerPublisher 捕获旧 sink 问题本轮没有证实，不应写成根因或已修复。

截图与最终记录见 [证据清单](gameplay-performance-20260915/artifacts.json)。generation2 的 `movement-final` 收到显式场景/输入响应审核，终态 **GAMEPLAY_REVIEWED / exit 0**。generation3 恢复运行再次显示 MOVE→ATTACK，但观察者未在脚本超时前提交审核，脚本正确保留 **TIMEOUT_UNVERIFIED / exit 2**；不事后篡改为通过。实际画面恢复与该脚本的终态是两个事实。

## 2. GPU 路径是当前帧率的首要嫌疑

两份 PROF 分别约 45 秒、35 秒，区段结果一致。下表取第二份 35.0156 秒窗口；只有完整配对且不跨丢事件标记的区段参与统计。

| 实测项 | 结果 | 能说明什么 |
| --- | ---: | --- |
| 主线程提交/翻页完成间隔 | 均值 78.94 ms，p95 86.82 ms | 与屏幕约 12.7 FPS 相符；不是菜单 60 FPS |
| GPU 提交线程 `VideoOut.Prepare` | 443 个完整区段，均值 **66.51 ms**，p95 68.60 ms | 每帧大量 wall time 位于 PrepareFrame 及其调用链 |
| `Present.FreeFrameWait` | 均值 0.021 ms | 这批数据不支持“显示帧池用尽”是主要等待 |
| `Present.FrameFenceWait` | 均值 0.024 ms | 不能把 66 ms 全部归到这个已测 fence 等待 |
| 同线程 `Vulkan.Submit` | 888 次，均值 0.033 ms | 已标记的 queue submit 调用很短；不含其前后所有工作 |
| GPU 利用率独立只读采样 | 12 次，86.47–88.68%，均值 **87.40%**；均为 680 MHz | 是设备级 busy，包含显示/overlay 等工作，不能直接归给某个游戏 shader |

`VideoOut.Prepare` 在 `VideoOutDriver::SubmitFlipInternal` 包围纹理查找/更新、`Presenter::PrepareFrame`、FSR/后处理、scheduler Flush 和请求入队。PrepareFrame 内尚未完整细分纹理更新、命令缓冲结束、ImGui 上传、timeline Refresh、命令池取用、延迟回调等部分。66.51 ms **不是**已经定位到的一次 `vkWait*`，更不是 GPU pass timestamp。

原关卡观察到 GPU 提交线程处于 `adreno_drawctxt_wait`；恢复后的 generation3 又以同 UID 只读采样，8 次中 5 次观察到同一个等待点（TID14653），证据保存在 `restored-wchan.json`。这加强了 GPU 工作完成/同步路径的判断，不能据此推导具体 user-space 调用栈。独立 GPU 利用率采样与 PROF 并非逐事件同步，不能相加计算“GPU 占每帧多少百分比”。

需注意 StatusLayer 中的 **all presents / overlay redraw 约 60 Hz**，而新游戏帧只有约 12.7 Hz。后续还要区分 guest draw/dispatch、host 后处理、重复呈现/UI 各自的 GPU 成本；不能看到高 GPU busy 就直接归因 guest shader 编译器。

## 3. CPU 热点：地址检查和高频 host/FEX 边界

NDK29 simpleperf，普通 APK 的同 UID `run-as` 采样，`cpu-clock:u / 100 Hz / frame-pointer / 45 s`：**6393 samples，lost 0**。最初 `cpu-clock` 请求失败，切换用户态事件后成功；保留两次日志。不包含内核 CPU 样本，JIT 栈存在未解析部分。

以下为调用栈按优先级排他分类后的用户态样本占比，**不是线程 wall time，也不是可获得的 FPS 提升百分比**：

| 类别 | 样本 | 比例 |
| --- | ---: | ---: |
| VM / GuestAddressSpace | 1379 | **21.57%** |
| FEX host adapter | 1106 | **17.30%** |
| 其余 mutex/cond 路径 | 332 | 5.19% |
| Graphics HLE | 217 | 3.39% |
| 其他 HLE | 502 | 7.85% |
| GPU host 代码 | 671 | 10.50% |
| 已识别 FEXCore 代码 | 63 | 0.99% |
| 其他 / JIT / 未解析 | 2123 | 33.21% |

最确定的单函数热点：`GuestAddressSpace::ValidateRangeLocked` **叶节点本身 789 / 6393 = 12.34%**。现有实现持 address-space 锁线性扫描 mappings，Read/Write/Pin 等反复经过它。可以优化查找和重复验证，但必须保留 mapping generation、权限、单一 mapping 覆盖、split/remap、VM publication 和 pin 语义；不能直接重新启用之前伴随纯色画面的未经验证排序实现。

第一份 PROF 的四个 mutex lock/unlock 名称合计 **2,584,769 次 / 44.95 秒，约 5.75 万次/秒**。四个 worker TID8308–8311 各自在 pthread_mutex_lock 区段停留约 34 秒/45 秒，里面包含等待，不能把四份耗时相加称为 CPU 消耗。Guest-16/TID8349 是样本最多的线程（1768），但本次 host 采样不足以确认它的具体 guest/FMOD 顶层职责。

FEX adapter 的已见成本还包括计时、原子操作、RunInternal、DispatchNative、frame→weak_ptr registry 的 map 增删、寄存器/flags 重建及快照。下一步应落实此前讨论的 **编译 guest mutex 非竞争快路径 + host/FEX 慢路径**，并评估每次 HLE 入口的必要工作；不能为提速绕过 owner/epoch/cancel、重入/TLS 或调试状态正确性。本轮只测量，没有改生产 mutex。

## 4. 音频和 profiler 不应误判

音频生产者 TID8350 的 45 秒窗口：`sceAudioOutOutputs` 累计 44.64 秒，其中 `Audio.QueueWait` 44.47 秒，wrapper 排除内部等待后约 **0.169 秒**。这反映专用生产者等有界输出队列的背压，不能说“主线程在向 Android 音频设备写 44 秒”。现有数据不把 Oboe 输出队列活跃处理排为首要瓶颈；FMOD 的 guest mixing/同步、加载时 flush 等仍是另一层问题，不能据此宣称整个音频系统无成本。

恢复后的 generation3 保持同一教程位置、无自动输入，以每段约 15 秒的实际 guest_flip 增量做 ring **ON→OFF→ON**：

| ring 状态 | FPS |
| --- | ---: |
| ON | 12.625 |
| OFF | 12.649 |
| ON | 12.743 |

本次未观察到明显帧率差异，最终恢复 ON。它只能排除这次低帧率主要由 ring 开关造成；GPU 限制可能掩盖 CPU 记录成本，也没有验证 streaming file capture 与所有诊断编译开关的零开销。

两份 PROF 原 decoder 报 17/11 个 `invalid zero-sized chunk`。源码核实 `ChunkWriter::emit_skipped_chunk_marker` 写 `(compressed_size=0, uncompressed_size=1)`；WriterThread 在发现 encoder 放弃事件时发出该标记。分析脚本保留 per-thread wire order，在标记处丢弃该线程尚未闭合的 spans，再配对后续事件。**本次所有标记都出现在对应线程首条时间事件之前，没有内部缺口或跨标记已开启 span；热点数值未改变。** 这些是累计损失/切换边界信息，不能断言其全发生在测量窗口，也不能说文件 chunks_skipped=0。边界未闭合 span 不参与耗时总计。独立 simpleperf 的 lost=0 不等于 PROF 无损。

## 5. 调试尝试的扰动与恢复

测量完成后，为取得 `adreno_drawctxt_wait` 上层调用栈，使用 Spatial native debugger 精确匹配 host 符号尝试 attach。默认 LLDB 版本未通过检查；CodeLLDB1.12.3 与 server 主版本不匹配；匹配 CodeLLDB1.12.0 dry-run 通过，实际 attach 在 DAP 15 秒请求超时后失败，**没有取得可用 stopped stack**。

失败 session `aaff7717b97a458eb7350493b05734af`。暂停扰动后，原 generation2 在 18:28:50 出现 `BackendFailed: Timeout in Run continuation: publication admission remained busy`，并伴随音频 underflow。这个故障发生在上面全部性能数据之后，不能当成原先关卡慢/白屏的根因。

已导出 journal/protocol/cleanup，stop 返回 cleaned，target alive、TracerPid0，调试端口/wait 为0，保留既有 RenderDoc forwards。随后以 open_last_game 进入 generation3，重新走 PLAY→教程→屋顶并响应移动。最终无自动按键，游戏保持运行，ring ON、stream capture inactive、GPU Reshape OFF。调试证据入口列在 artifact manifest；没有隐去失败或声称获取了 host 精确等待栈。

## 6. Warmup 交付与接下来的顺序

新增 [warmup-game](../../../scripts/android/warmup-game) 与 [使用说明](../../../scripts/android/README.md)。脚本显式选择 serial/display，默认每两秒按 Circle，可切 Cross、左右方向/摇杆或 none；有界运行、每五秒留图，固定 PID/generation/run_uuid，ADB 失败/换代非零退出。不向 APK 添加轮询，不挂 debugger，不改 SELF/存档。**FPS 前进、按了多少次、出现菜单均不自动通过**；需审核两张实际游戏/输入响应图、同代帧推进、期间实际触屏、文件哈希、新鲜度，才返回 GAMEPLAY_REVIEWED。

定向脚本测试 **7/7**，包括高 FPS 菜单不得自动通过、无输入/无新帧/跨代/非法 frame selector、ADB 失败记账。一次旧 freshness 门槛过严导致 ERROR_UNVERIFIED，改为 60 秒显式图像审核窗口后独立新 run 通过；保留失败记录。恢复 run 因没有及时审核超时，不升级为 pass。

后续直接围绕同一实际教程场景推进，无须再发微型 spec：

1. **先定位 66 ms 的具体来源。** 细分 PrepareFrame 的纹理更新/后处理/Flush，以及 scheduler 的 end、ImGui upload、Refresh、command-pool、pending callbacks；每帧低频打点即可，关闭诊断时不做昂贵采样。结合真正的 Vulkan GPU timestamps/RenderDoc 将 guest 工作、host 后处理、重复呈现分开，找出具体串行等待或最慢 pass 后再优化。现有 free-frame/fence/queue-submit 短区段不是首要修改对象。
2. **然后整块处理 VM 查询和 compiled guest mutex/HLE 入口。** 保留现有正确性与 cancellation/调试不挂接行为，按 real gameplay 的相同画面、同频率/温度条件做前后对照；验证改变的 mapping、锁语义与 guest 输入/像素，不拿纯色 60 FPS 或菜单表现验收。
3. 启动/教程 loading 仍有长等待，需用启动窗口单独追 FMOD synchronous load/flush 的生产者，不能用已进入关卡后的 12.7 FPS 采样解释全部冷启动时间，也不要因此重新改写 Oboe 输出桥。

本次完成的是实际场景闭环、可复用 warmup 和瓶颈定位到子系统/热点函数；没有游戏性能改善数值。
