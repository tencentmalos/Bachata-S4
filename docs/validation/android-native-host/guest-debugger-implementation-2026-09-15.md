# FEX guest debugger 实际接入与验收（2026-09-15）

本记录接续同日的[只读审计](guest-debugger-zero-fps-audit-2026-09-15.md)。用户随后明确要求优先完善 guest debugger，并授权修改 `~/workspace/spatial_mcp_publish`。当前已实现可通过独立 Native Debugger MCP 使用的 guest 寄存器、单步、软件断点和恢复链路；旧记录中“Step Unsupported / 生产 RSP 未接”已被本次实现取代。**本轮没有证明或修复 TMNT 圈／PLAY 后白屏、guest 0 FPS 的根因。**

## 实际实现

- `src/core/guest_cpu/debug/target.h` 是不依赖 FEX 类型的控制接口；`rsp_server.cpp` 提供 loopback RSP。CPU context 持有监听线程，默认关闭，Session 销毁时关闭连接并 join。
- `fex_context.cpp` 的 owner 在原有 Run/InvokeGuest 内进入调试等待点。等待前退出 JIT、释放执行 lease 和 call-frame pin；寄存器写入通过 mailbox 交回原 owner 执行，必须匹配 stop epoch。不会由网络线程调用 FEX ExecuteThread，也不保存旧 JIT C++ 栈继续执行。
- 单步复用 FEXCore 自带 TF dispatcher 路径和 `CompileSingleStep`，执行单条临时翻译，不改变正常块缓存或全局 MAXINST。已知 TF 入口产生的下一条 RIP 才使用 `GetGuestBlockEntry`；异步位置不能套用这一结论。TF 在 native HLE 前恢复。真实 SYSCALL 完成 native HLE 后停在 guest successor，CALL/RET/自循环按实际后继停下。
- guest INT3 通过 FEX 的 generated-trap 出口返回 owner，分类为新追加的 `StopReason::Breakpoint`；不会把 host SIGTRAP 交给 Android 或 LLDB。软件断点使用 checked VM token 修改实际 guest 字节，走既有 shared/live-thread/decoder 失效路径，保存原字节和 mapping identity。读取断点地址返回原字节；旧映射拒绝盲目恢复。
- Pause 不取消 HLE 等待。线程列表区分 guest-running、guest-stopped、hle-boundary-read-only 和 idle，并标明 guest ID、真实 host TID、线程 generation、snapshot kind、stop epoch、invocation；HLE 快照还提供入口 operation 编号。HLE 寄存器是边界副本，不能写或单步。native HLE 本身仍可能工作或等待，**这不是冻结整个 native 进程**。
- production Linker 发布模块名、实际基址和映射段给 debugger；没有把 host 函数地址当作 guest 地址。RSP 支持 target/threads/libraries XML、GPR/RIP/RFLAGS/XMM/MXCSR/FSBase/GSBase、有限内存读、代码补丁、Z0/z0、pause/continue/锁定单步及 detach。无有效快照的寄存器返回 unknown，不填伪造零值。
- 调试代码目录原先会命中仓库 `[Dd]ebug/` 忽略规则，已添加源码例外，避免交付时遗漏这三个新文件。

## Native Debugger MCP 修复

`spatial_mcp_publish/dev_tools` 的 `GuestDebugSession` 曾硬编码 `pc` 和 PowerPC 默认寄存器列表，真实 x86 `rip` 因此被拒绝。现按 target XML 解析默认 scalar/general 列表，优先 `generic="pc"`，再选择唯一的 `pc/rip/eip`；不能仅依据 code_ptr，因为 LR 也是代码指针。歧义和无效原始值仍拒绝恢复。

安全门未绕开：停在已登记软件断点上时，MCP 仍要求 provider 原子 step-over 证明。当前 FEX provider **不声称这一能力**；可直接执行已验证的“移除该断点 → locked Step → 需要时重插”流程。未来地址上的断点现在能够正常继续并命中。

- dev_tools focused commit：`f6385072`，所在分支 `codex/litep-profiler`。
- publish 引用提交：`4be28e3b`，所在分支 `feature/ida-mcp-csharp`。两者为本地提交，本轮未 push。
- 本机安装：`native_debugger_mcp-0.2.0-local.20260915.fex-osx-arm64`。通过仓库 package manager 安装；current.txt、Codex 六个对应 MCP 路径已核对，保留原参数和环境；Doubao wrappers 已重建。
- 11 个生成 wrapper 均通过 initialize/tools/list，包括六个必验产品。Native Debugger 为 87 tools。已存在的 Codex MCP 进程不会因路径更新自动重载，本轮通过**新版独立 MCP 的 stdio JSON-RPC**完成实机验收，未假称旧连接已热更新。

## 证据与结果

精确文件 SHA256、Build ID、主仓 dirty source 文件哈希与子仓身份见 [artifacts.json](guest-debugger-20260915/artifacts.json)。主仓基于 `4da582b7` 加现有及本次未提交工作；host/JNI 保持 RelWithDebInfo，FEXCore Release。FEX 子仓 `385a0cc4` 未改，既有 Foundation/Oboe/其他未提交工作保留。

| 验证 | 结果与范围 |
| --- | --- |
| AYN Thor / API33 / ARM64 / 4 KiB，真实 FEX 定向测试 | **54/0**；指令后继、flags、SSE、栈 CALL/RET、INT3、HLE syscall、外部 owner mailbox、陈旧 epoch、不可执行 RIP、data-write 拒绝、取消、双 owner HLE/park 与 VM 排空 |
| Native MCP 相关 xUnit | **32/0**，含 RIP 默认读取、未来断点恢复和仍拒绝不安全的跨断点单步 |
| `dotnet build SpatialDebugTool.sln --no-restore -m:1 -p:UseSharedCompilation=false -v minimal` | **通过，0 warning / 0 error** |
| 新版 MCP → owned adb forward → 实机 FEX | **REAL_FEX_MCP_PASS**：暂停、默认读寄存器、一步、未来 INT3 命中、PC 正确、不安全恢复负例、移除后一步、cleanup ownershipReleased=true |
| 普通 APK instrumentation → production Linker/VM/FEX/RSP | **1 JUnit PASS，3 个同 PID generation**；最终 PID11544 / UID10190，PC0x400000，寄存器写入、断点命中、去断点单步、坏包/坏线程及暂停中 Stop，两轮不依赖主动 detach |
| 默认关闭的普通 APK 路径 | **1 JUnit PASS**；调试属性清空后 production wait ELF 正常进入 Running，24680 无 listener，Stop 正常 CANCELLED |
| Host 库闭包 | **HOST_LINK_PASS**，APK 内 host Build ID 与实际构建一致 |

最终 APK SHA256 `b1e0c784ee40fd59bd8bc9dc826724bc0ae0c2f1144fa7283e5f1f1cd3cf5105`；host Build ID `2961e78f5b8e97d0ddebf1a7a055592413f78710`；JNI Build ID `0ebf9865240bd6d2838316c648029483f49630f9`。APK 测试使用合法合成 ELF，没有渲染、完整游戏内容或可玩验收。

保留失败记录：初次 APK XML 分片失败由 qXfer 前缀长度错误引起，已修正；双 owner 测试初次使用非生产 `resume_internal_drains=false`，排空本来就会返回 Pause/取消 HLE，改为生产配置 true 后通过，不将该失败误报为新的生产死锁。旧 MCP 的 PC-name 错误由客户端修复解决。

## 使用与边界

实际定位圈／PLAY 现场时，在**新 Session 创建前**开启端口，不要默认等待：

```sh
adb -s 9c2841a4 shell setprop debug.shadps4.guest_debug_port 24680
adb -s 9c2841a4 shell setprop debug.shadps4.guest_debug_wait 0
```

通过新版 `guest_debug.start_session` 指定 serial、port=24681、remotePort=24680、expectedArchitecture=i386:x86-64、byteOrder=little、stopOnAttach=true、dryRun=false。先 `control pause` 获取真实 stopped epoch，再 `list_threads`、`read_registers`、`list_modules`/memory 工具按实际工具 schema 查看；只对 guest-stopped/Runnable owner 单步。HLE 线程先按 hle-op、guest/host TID 和 invocation 查 native provider；不要将 boundary RIP 当成 HLE 内部实时 PC，更不能据此断言等待对象的 producer。

结束调用 `guest_debug.stop_session`，确认 ownershipReleased 和 owned forward 清理。测试已清空以下属性；如需再次关闭：

```sh
adb -s 9c2841a4 shell 'setprop debug.shadps4.guest_debug_port ""'
adb -s 9c2841a4 shell 'setprop debug.shadps4.guest_debug_wait ""'
```

当前限制必须保留：

1. PUSHF/POPF/IRET 在 Step 前显式 Unsupported，避免观察或覆盖调试 TF；x87/YMM 状态、硬件 watchpoint、源代码级 step-over/out、混合 unwind 未实现。没有通用任意 JIT 异步全寄存器重建，也没有新增一般 host SIGSEGV 恢复。
2. 写寄存器必须停在 debugger gate；写 RIP 要求 executable mapping。代码字节写入/断点操作要求全部 guest owner park/idle；存在 HLE 活动时返回 Busy。普通 data memory 写入暂拒绝，尚未接 GPU/memory observer 一致性。
3. 单步使用一个 owner；多 action/unlocked vCont 不支持。长期或失败的断点恢复保持停止，不会在无法恢复原字节时静默放行；必要时用户 Stop 整个 Session。RSP 尚无 provider UUID/跨进程重连证明，不将 logical endpoint/thread generation 当作完整进程身份。
4. Debugger 暂停会改变实际超时（包括 Oboe watchdog）；暂停期间不得用于 FPS/音频性能结论。没有完成 wait-object→producer 归属、停帧自动快照和 TMNT 真实 PLAY 等待链验证。已有 ringbuffer 和 host LLDB 可用于 guest debugger 无法解释的 native HLE 区间，不能反向捏造 guest PC。

本轮只做上述定向测试，没有完整回归、Swan 或 16 KiB 验收。
