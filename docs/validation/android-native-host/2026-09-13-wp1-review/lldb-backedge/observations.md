# 热循环中断问题：LLDB 观察与退出证明

本节为当次工具返回的人工摘录，不是重新运行后的地址。完整本地导出位于 `build/wp1-review/lldb-apk-cleaned`，原始文件摘要见 evidence-manifest.json；提交只保留摘要与清理证明，未提交整份进程模块/内存转储。

- 调试会话 e8d63055c965472090ee72c032aeec19，AYN 普通 APK PID13905，stop epoch1。
- root owner TID13955 在失败后的 `pthread_join` 等待；child owner TID13956 仍在 JIT 循环。
- child host PC `0x7445ed9870`，x28=`0x7430004140`。x28 仅在此次匹配 FEX 的 ARM64 JIT 上解释为 STATE；这里没有声称 CPUState 是异步停止的完整 guest 快照。
- entry `0x7445ed9864` 保存 JIT header；`0x7445ed986c: str xzr,[x28,#0x3ec0]` 是中断探针；循环尾 `0x7445ed9884: b.ne 0x7445ed9870` 回到探针之后。guest counter 热循环因此不再触碰 InterruptFaultPage。
- 对照 pinned FEX frontend CondJUMPOp / JumpTargets 与 JIT EmitEntryPoint，MULTIBLOCK=0 仍允许当前 guest entry 的局部回边。主仓新增 EntryBackedgePass 将有 guest EntryPoint 的边改成 ExitFunction；指令内部 REP/atomic loop 不改。没有改 FEX child，没有把 MAXINST 调成1。
- G47 先观察 guest counter≥10000，再 Cancel/WaitStopped；修后 guest242/0。生产 APK 的 child 热循环+父线程VM操作也通过。

调试用 CodeLLDB1.12.0 / LLDB21.1.7 与 NDK29 server21 匹配；较早的 Xcode/CodeLLDB22 配对失败不构成 stop 证据。一次只读 image lookup 因 DAP response mismatch 失败，一次表达式被只读策略拒绝；未执行 inferior helper。

stop_session 已清理 controller/server/transport；5秒证明 target 仍存活、startTime相同、TracerPid=0、EmergencyRecoveryUsed=false。没有遗留调试暂停或活动控制器。修后测试在未附加 debugger 的普通APK里完成。
