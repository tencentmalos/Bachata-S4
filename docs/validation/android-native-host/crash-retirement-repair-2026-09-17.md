# 2026-09-17 崩溃调查与异步资源回收修复

本轮修复了一个可确定复现的 host/GPU 双重完成条件缺口；**尚未证明三个历史崩溃均由此引起，也未关闭 Guest JIT / HWUI 根因**。保留上一轮 viewport/fog 修复，继续使用系统 Qualcomm 驱动与 RelWithDebInfo host/JNI。

## 历史证据分组

设备 `9c2841a4`，AYN Thor / API33 / 4KiB / Adreno740。原始 crash buffer、DropBox、exit-info、精确 vendor/HWUI 二进制保存在 `build/crash-repair-20260917/`。

| 记录 | 真实位置 | 本轮判断 |
| --- | --- | --- |
| PID24787 / 11:23:46 / Present | Qualcomm `vkBeginCommandBuffer` → reset 路径，vendor PC `0x2db0f4` | 在读取内部链表时 x8=3，访问 x8+8 导致 fault0xb。不是 begin-info 空指针；也不能仅凭驱动栈断言驱动自身有错。发生在雾修复前。 |
| PID7061 / 11:45:08 / Guest-1 | FEXMemJIT，fault `0xa655e6f10` | 与9月14–16日多次异常地址模式相似；历史记录没有 guest RIP，不能归因于这次 viewport 改动或按键。已补致命故障记录。 |
| PID8483 / 11:53:07 / RenderThread | HWUI `SkCanvas::init`，PC `0x2df6d8` | 二进制确认 SkDevice 对象地址非零，但 vtable=0，间接调用前读 vtable+0x90 崩溃。尚不知谁破坏/释放对象，未直接改系统 UI。 |

旧 RenderDoc replay 也出现过 vendor 同一 fault0xb，但它是独立进程，并不经过本应用 Scheduler，不能据此声称本次修改同时修复 replay。上一轮单独 depth probe 的 `cmd.begin({})` 空指针重载错误已修复，和上述 Present 崩溃位置不同。

## 确定修复：资源不能只依据 GPU signal 复用

Android 的异步提交线程持有 command buffer 的 host 状态。`MasterSemaphore::Wait` 直接等待 Vulkan timeline 并更新 `gpu_tick`，以前它没有等待该 tick 的 `vkQueueSubmit` 返回。GPU signal 可以先于 host 调用返回被观察到；此时 pool、query 或 deferred callback 使用旧 `gpu_tick`，可能过早复用/释放资源。

现在 `TimelineCompletion` 单独发布单调的 host submitted watermark。可回收 tick 为 `min(gpu_completed, host_submitted)`；显式 Wait 在 GPU 完成后还确认对应 host 提交返回，并支持取消与提交失败。每个 scheduler 保留独立 timeline、单一设备 FIFO 保持原样；没有增加逐帧 queue/device idle、全局录制锁或让 guest 每次提交等待 present。

`ResourcePool` 的搜索闭包同时改为读取刷新后的 tick。旧捕获值即使 Refresh 得到新完成水位也不会更新搜索，容易额外扩容；此项本身不是本轮历史崩溃的已证根因。

参考 Vulkan 对 [command buffer 生命周期](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html)和 [host 外部同步](https://docs.vulkan.org/spec/latest/chapters/fundamentals.html)的规定。这里保护的是 host 提交调用与资源复用的所有权边界，不是把 Vulkan barrier 变成 CPU 等待。

## 验证

- `gpu_async_tests`：设备 **38/0**，覆盖 GPU 先完成、host 仍未发布、延后确认、旧确认不回退、取消、提交失败。
- `android_submission_probe`：系统 ICD **348/0**，3个独立 VkDevice generation。真实空 command buffer 已 signal timeline，特意延后 host acknowledgement，Wait 和 IsFree 均不得提前放行；其余 fill/copy/readback、query、queue backpressure 和 poison 检查通过。
- **旧 host 对照：348 checks / 6 failures**。使用雾修复最终 host `096afb0e…`，只在测试可执行文件补回旧版内联 `KnownGpuTick` 定义以满足当前 probe 链接；没有改旧库的 Wait 或提交代码。每代稳定失败的两项正是“Wait 提前返回”和“IsFree 提前为真”。脚本/对象/日志保留在 evidence 目录。这个反例证明机制缺口，不是历史游戏 crash 的确定性复现。
- `fex_fatal_log_tests`：设备 **11/0**。独立子进程执行真实 x86 `xor eax,eax; mov eax,[rax]`，原有 SIGSEGV 仍正确到达终止路径，文件包含 block、RIP metadata、host PC/instruction/GPR；正常子进程文件为空，创建 owner 后不允许更换记录 fd。该测试产生的 SIGSEGV 不属于 APK 崩溃。

测试原始失败也保留：最初把 CPU probe 挂在 host DSO target 上缺少 FEX adapter 符号；现已放在 `cmake/fex` 的真实 `guest_cpu_fex` test target。旧库对照第一次遗漏 spdlog 静态依赖的链接失败同样未计为有效反例。

## FEX 致命异常记录

Android 丢弃 stderr，过去已有的 block 提示不会落到普通 APK 可取回的位置。现在生产 Session 在创建 guest owner 前打开 `files/host/log/fex-fault-<pid>-<generation>.txt`，fd 随 CPU context 关闭。

仅当地址保护、interrupt、GPU access fault 等既有处理全部拒绝，且异常 PC 位于该 owner 的 FEX JIT 范围时，才向预先打开的 fd 写入不超过2048字节的记录。不分配内存、不获取 VM/context 锁、不进行 host unwind、不改变异常转发、普通执行不采样。`mapped_rip` 是 FEX 最近的 RIP 元数据结果，FEX 仍可能回退到 State.rip；**不是任意 JIT 中途完整 guest 寄存器快照**。host GPR 标为 x0–x30，绝不冒充 x86 架构寄存器。

## 普通 APK 身份与边界

本轮安装包 SHA256 `493e38ad0c3c3b396e56c190bc71e14ef014df241cef8554ce6a79c369738a0b`。

- APK host entry：`73e7ca4c4c392adafeeed1813d6214fab72a994025fb89724eb5c07668c16313`。
- JNI `libshadps4_fex_session.so`：`a8ce07b090453cd715019c7cbb8d5ed2d5dbc8fceb8aece6dcf43e21dc2aaa6c`。
- Host/JNI 为 RelWithDebInfo，FEXCore 为 Release，APK variant 为 playstoreDebug；系统 Qualcomm512.676.53/build69e13475cb。
- 新包 PID19740，generation1、2、3实际进入屋顶 MOVE 并持续出帧，startup从一开始每3秒按 Cross。短暂 loading 的零计数增长随后恢复；不是崩溃/永久死锁证据。前两轮 UI Stop 到 Stopped/user_stop；第三轮约154秒观察到guest_flip1451，返回菜单再Resume正常。三个 fatal 文件均为空。游戏留在第三轮屋顶运行，自动输入已停止，TracerPid0。
- warmup 的 STOPPED_UNVERIFIED / TIMEOUT_UNVERIFIED 没有改成 PASS；本轮是启动/崩溃检查，未做完整输入主流程评审。一次在 StopRequested 时过早尝试重开游戏被拒绝，失败目录保留，不能计为新 generation。

## 调试器与保留限制

native-debugger inspect 找到准确 host 符号。Xcode 默认 LLDB major 无法匹配，CodeLLDB自带22与server21不匹配；显式使用NDK21 liblldb后，host adapter 在 SymbolLocatorDebugSymbols 内 SIGBUS。没有将工具故障当作目标 crash。会话 `9bc180801b6b4164a6b6b776da56d2a6` 的证据已导出至 `debugger-failed/`，stop返回cleaned、目标存活且TracerPid0。

本轮没有 FEX 子仓/Foundation/驱动源码改动、RenderDoc 抓帧、完整回归、新 spec 或 commit/push。所有此前 dirty work 保留。当前修复不构成十分钟或完整游戏稳定性验收；历史 Guest JIT 和 HWUI 异常仍需用新记录进一步定因，不能以暂未复现宣布修好。
