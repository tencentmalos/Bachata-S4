# FEX 混合栈与 guest watchpoint 实现（2026-09-15）

本记录接续[基础 guest debugger 接入](guest-debugger-implementation-2026-09-15.md)。用户要求直接推进混合栈、watchpoint，并明确未挂接 debugger 时不要显著改变行为和性能。**这次交付的是诊断能力，不是 TMNT 圈／PLAY 后 guest 0 FPS 的根因修复或可玩验收。**

## 默认关闭路径与性能约束

- 未挂接时不调用 `_Unwind_Backtrace`，不保存 HLE/native 栈记录，不生成内存观察 IR，不打开全局逐指令执行，也不修改 FEX 全局 `MAXINST` 或普通块缓存策略。
- FEX 每个 owner 有一个很小的编译状态和前置 pass。普通编译只做关闭判断；普通 JIT 没有访存回调。Run/HLE 边界保留少量关闭判断，`HleScope::WaitFor` 增加默认无操作的诊断入口，不能把这描述为严格零开销。
- 只有实际配置软件 watchpoint，才以现有 TF / `CompileSingleStep` 执行不入普通缓存的单指令翻译。该调试模式很慢，会影响调度与时间相关行为，作用于此 context 的 guest owners；不是可持续开启的 profiler。
- 最后一个观察点移除或 detach 后重新走普通执行。真机定点测试验证：同一 owner 恢复循环进度超过 10000 次，观察 IR 计数不再增加；未挂接的两层嵌套 HLE 回调增加的 native capture 和观察 IR 均为 0。这里没有用游戏 FPS 宣称一个精确的百分比性能结论。

## Watchpoint 的实现与语义

FEX 子仓增加了一个可选的 `DebugMemoryAccess` IR 操作及 pass 前插接口，改动集中在六个文件。JIT 观察回调保存动态/静态 GPR、向量寄存器、NZCV、FPCR、FPSR；回调仅在当前 guest owner 上比较范围并记录命中，没有分配、锁、VM 调用、guest 回调或日志。普通代码不会产生该 IR。

主仓 `MemoryWatchPass` 在优化和寄存器分配之前处理实际内存 IR。支持标量读写/TSO、向量加载/存储及元素访问、广播、PUSH/POP、常见原子 RMW/CAS，保留地址偏移、扩展和比例的语义。首次匹配的访问只有在该 guest 指令确实完成时才提升为 watch stop；访存 fault 不得伪装成已退休的观察点命中。

- 复用 RSP `Z2/z2` 写、`Z3/z3` 读、`Z4/z4` 读写观察点；最多八个 1–8 字节范围。
- 命中事件区分被观察地址、实际访问起始地址/宽度、执行访存的 instruction RIP 和指令完成后的 architectural successor RIP。同值写入也会命中，不是比较内存是否变化。
- 观察范围必须落在同一个当前映射中；执行前重新核对映射身份和范围。重映射后不会悄悄把旧观察点移交到新 backing。
- 观察点配置本身不修改 guest 代码，因此 guest 暂停完成后，即使某个 owner 仍等待在 native HLE，也允许配置。代码断点与代码补丁仍保留更严格的所有 owner parked/idle 约束。
- REP/bulk、masked/gather、非临时向量访存和未覆盖的 x87/组合访存路径先明确拒绝，**不能静默漏报后声称覆盖全部指令**。预编译检查会在实际执行前停车。TF 可见/可修改指令仍遵守基础 Step 的拒绝规则。
- 拒绝执行使用 `T05...;shadps4-stop:unsupported;`，保持客户端 stopped epoch 可用；不能以普通 `E16` 替代停止事件，让客户端误认为仍在运行。MCP 会报告 `StopReason=unsupported`，允许读状态、移除观察点和 detach。
- 只观察真实 guest IR 的内存访问。native HLE 的 guest-buffer 写入、GPU 写入、其他 host 线程访问均不在此范围，不声称 Android 硬件 watchpoint 或整进程数据竞争检测。

## 混合栈的事实来源

`Debug::Target::ReadMixedStack` / `qXfer:shadps4-mixed-stack:read:<hex thread id>:offset,length` 提供带 context ID、线程 ID、generation、provider stop epoch、phase、限制说明的结构化结果。每次 transfer 的首片固定快照，后续片使用同一份有界数据；新连接清空旧混合栈缓存。

1. 当前 guest leaf 来自 SafePoint / HleBoundary / fault snapshot，按实际来源标注。
2. guest callers 使用最多 12 层的 RBP 链，检查对齐、范围、可读链接、递增关系和可执行 return address；不扫栈猜返回地址。损坏链、没有帧指针或越界时截断。
3. 仅在挂接后保存 HLE 边界；进入 `InvokeGuest` 时捕获真正仍在 native 调用链上的 callback-entry 栈；`HleScope::WaitFor` 提供 wait-entry/return 检查点。最多保留八层边界、每次最多 24 个 native PC，总输出最多 96 帧。
4. 每段保留 invocation、HLE operation、guest/host 类型、模块以及 `hle-entry-capture`、`callback-entry-capture`、`wait-entry-capture` 等 provenance。真实嵌套验证得到三个不同 invocation 的边界（两层 callback + 一层 wait），MCP 实机读取包含 58 个 native 帧。
5. Native PC 通过 `dladdr` 给出可得模块/动态符号；这些是**历史边界采集**，不是把整个 native 进程暂停后获取的当前 LLDB 栈。HLE 可能继续工作，attach 之前已进入的等待没有历史 host 栈，结果会明确说明。

未实现 guest DWARF CFI unwind、可靠的无帧指针 caller 恢复、任意 JIT 指令位置的全状态重建。没有把 `CPUState.rip` / 任意 `GetGuestBlockEntry` 当作异步精确 PC。自定义 native 等待若没有经过 `HleScope::WaitFor`，只能看到最后被记录的边界；需要具体 host 实时等待位置时仍结合 native LLDB。

## Native Debugger MCP

新增 `guest_debug.read_stack(sessionId, stopEpoch, threadId)`，需要 `allowNativeMemoryRead`，限制 transfer 为 64 KiB / 96 帧，并核验 provider identity、phase、帧来源。provider stop epoch 与 MCP session stop epoch 是不同命名空间，不做相等假设。未提供此协议的目标明确 Unsupported。

现有 `guest_debug.manage_breakpoints` 的 write/read/access 类型可直接驱动上述观察点。停止快照新增 `WatchHit`，保存访问地址、宽度、instruction PC 和观察范围；不要用 successor PC 回溯猜测变长 x86 指令。

代码位于 `~/workspace/spatial_mcp_publish/dev_tools`，独立 Native Debugger 产品；未把 live control 接回 SpatialDebugTool。通过 package manager 安装、更新 current.txt / Codex 六项路径、重建本机 Doubao wrappers 并检查全部 11 个 wrapper。Native Debugger 当前目录为 88 tools，包含 `guest_debug.read_stack`。已启动的 Codex MCP 连接不会因配置改动自动热更新，验收通过新版独立 MCP 的 stdio JSON-RPC 完成。

## 验证与可复现证据

精确源文件、FEX patch、artifact SHA256、Build ID、阶段区分与本地 MCP 提交见 [artifacts.json](guest-debugger-mixed-watch-20260915/artifacts.json)。主仓以 `4da582b7` 加既有工作和本轮修改；FEX 在 owned fork 的 `feature/malos/host-page-size` 上有本轮本地修改，已在修改前用 `gh` 核验远端分支；不是已发布的新 gitlink。Foundation / Oboe / 既有音频与图形修改保留。

- 最终 AYN Thor / API33 / ARM64 / 4 KiB 真实 FEX：77/0，覆盖基础 debugger、读写/同值/原子/向量/栈、移除/detach 恢复、无挂接采集关闭、REP/非临时访存拒绝、stale mapping、HLE 等待中配置、两层回调/等待、RBP 有效/损坏链和取消。
- MCP 定点测试 **38/0**；工具文档目录检查 **15/0**；solution build **0 warning / 0 error**。普通 APK **三轮同 PID generation PASS**（包含 Unsupported 停止与恢复），关闭属性路径 **1 JUnit PASS**。实际新版 MCP 的读/写观察点、Unsupported epoch 恢复与三层混合栈均通过，两个 session 均 `ownershipReleased=true`。完整日志见 artifacts.json。
- host 与 JNI 保持 RelWithDebInfo，FEXCore Release，playstoreDebug APK；只做改动相关验证，没有完整回归、游戏性能/画面或 Swan 验收。
- 保留最初失败：向量 store 的地址偏移未处理；初次混合栈测试重复占用了旧 veneer slab；stale 测试试图在 token 仍持有时普通 Write；APK 测试混淆 RSP 与 x86 编码的 RBX 索引。分别修正实现或测试，原始日志保留；没有把这些测试问题归因于游戏故障。

关闭调试属性、清理 owned MCP forward 和结束测试 session 是验收的一部分。既有 RenderDoc forwards 保留。没有重新运行 TMNT 或宣称 guest 0 FPS 已修好。

最终本机 MCP：`native_debugger_mcp-0.2.0-local.20260915.fex-mixed2-osx-arm64`，dev_tools 本地提交 `39185a5d` / `26251f86`，publish 本地引用 `b411cfd5`，未 push。最终 APK SHA256 `37bbf8921ab65c4c1a1aed093045be4fe867df982f6c6c91e688847e719ea294`，host Build ID `7edf2aecf2baaffda23f6a4a905abcf4945bdacf`，JNI Build ID `f1cdd44611304248e259fea4786f15e2aca197ec`；已验证设备安装的 APK 与本地 SHA 一致。
