# TMNT 可见函数到 C/C++ 拦截的闭环

本轮目标是“可见 → 可编辑的 C/C++ 拦截 → 构建/部署 → 实际命中”。
并未将入口观察计入原函数体重编译完成度。主仓和 FEX 已有 dirty 工作均保留，未 commit/push。

## 当前交付

- [TMNT 源码入口](../../../guest/games/CUSA50828/01.08/intercepts/README.md)：50 个已解析函数
  （FrameCoordinator 加 49 个直接目标）均有可构建入口。已有 6 个入口和 4 个提交 helper 保留，
  另外新增 44 个语义命名 `.cpp` 回调，合并包共 54 个 hook。
- `intercept-workset.py` 消费同一真实帧 workset、完整 recompile evidence 和审核过的 base recipe。
  检查模块/分析 SHA、架构、捕获身份、命名、完整入口指令和 stolen prefix 的外部入口；
  不覆盖已编辑目录；`--select` 可生成局部调试单元。超过容量显式失败。
- `entry-observer-x86_64-avx` 使用公共 guest SDK 的 `shad_entry.h` / `entry_observer.S`。
  保存 15 个 GPR、RFLAGS、原 RSP、FEX 支持的 x87/MXCSR 和 YMM0–15；回调正常返回后恢复，
  尾跳 original trampoline。原 caller 返回地址、栈参数、AL、hidden sret 不靠猜 C 原型转发。
  额外保存区位于原 red zone 下，约 1.4 KiB；callback 不得异常/longjmp、改 FS/GS 或改变调试 TF/RF。
- 按函数 `guest-patch enable|disable --hook NAME` 复用现有 context 与 VM token，修改常驻分派槽；
  不卸载在途代码。全局开关保留。容量由 32 增至 64 hook；SDK counter 仍最多 64。
- 默认回调只有 guest 原子计数，首个和每 4096 次才上报 host。每次启用入口仍需完整保存状态，
  **未测无探针/全量拦截的性能 A/B/A，不声称低开销或性能提升**。正常不安装补丁时无新增调用。
- `intercept-status.py` 将实际状态与精确 package/build/source SHA 绑定，明确 hit/installed-not-hit。
  编辑 C++ 后必须重建、部署，再回写新证据；符合实际新构建的源码身份自动更新，无需人工维护 SHA。
  旧部署/未重建改动会拒绝。`guest-patch` 也修正了无 Service 输出错误返回 0 的问题。
- `reverse_study.guest_frame_workset(interceptionIndexPath=...)` 链接对应的 C++/VS Code；
  源码或配方已改时显示 rebuild-required。历史帧时长与本轮 hook 命中是不同 Session 的证据，未合并时间线。
  新本机入口为 `build/frame-intercepts-20260916/installed-mcp-frames/frames.html`。

## 找到并修复的 FEX 缺陷

真实 FEX 反例中整数、SSE、sret、AL、YMM 与 MXCSR 均通过，x87 值恢复失败。
无拦截的同一机器码探针通过，原失败日志保留。

`SaveX87State` 按逻辑 ST(0)..ST(7)（含 TOP 旋转）保存，旧 `RestoreX87State` 直接写物理槽，
导致 TOP 非零时恢复错误。现在按恢复的 TOP 循环写回物理寄存器；reduced-precision 表示也转换回相应格式。
未修改普通 dispatcher 或未挂接 debugger 的热路径。此次实际验证使用默认完整 x87 精度；
未将 reduced-precision 分支或未运行的子仓 NASM fixture 当成已验证。

FEX 子仓仍在已有 `feature/malos/host-page-size` 分支；新增 Vector.cpp 修复和
`unittests/ASM/FEX_bugs/fxrstor_top_rotation.asm`，其他 7 个既有 dirty 文件保留。
主仓的 Android FEX 定向 fixture 已执行 8 个 TOP 位置的双值栈恢复，验证本次实际修复。

## 验证结果

- 真实 Android/FEX：**27/27**，55-hook 容量与末槽、8 参数、快照读取、SSE/sret、AL、
  destructive observer + 实际 HLE 后的 GPR/flags/MXCSR/x87/YMM 高位、8 种 TOP、
  RIP/CALL/Jcc relocation、按函数开关、未知名字、双 owner、全局停用、入口恢复。
  初次 17/18 与拆分后的失败日志保留：[定向证据](frame-intercepts-20260916/entry-device-final.log)。
- 生成器 8、构建器 11、状态回写 6、共享 reader/索引 30 项通过；受影响的定向检查，未完整回归。
- 普通 APK：AYN Thor `9c2841a4` / API33 / 4KiB / Turnip，RelWithDebInfo host，Release FEXCore。
  PID8776 / generation1 / context1 / UUID `25ce3891a5c3a1ec27a4eed0b91c7f1f`。
  package `7cafaa228271edca08425dac00c8035c7154cfd2d49c33266867bea0ab27aa7e`，54 hook 全部安装且 failed=0。
  **50 个可见函数中 44 个有真实 C++ counter 命中，6 个 installed-not-hit。**
  未命中项是 FMOD sample-loading/playback-state/get-volume/event-stop/event-release，
  以及 `TMNT_RefreshExpiredStateAndNotifyCallbacks`，没有通过伪造调用来凑全覆盖。
- warmup 实际观察到 guest flips，但第二张图在白色加载阶段短暂停帧；随后前台切到 Android WLAN
  热点设置，Service 不存在，warmup 因缺 guest_flip 终止。进程仍存在，不据此称崩溃修复或屋顶通过。
  此次 **没有新屋顶验收**，失败 manifest 保留。先前 v2 的屋顶验收不移植为本轮结论。
- 共享 MCP `0.3.0-local.20260916.intercept1` 已经 package manager 安装；Codex 配置更新，
  六项其他 Spatial 配置和 args/env 保留且路径存在，wrapper 11/11 initialize/tools/list 通过。
  实际调用已安装的 FrameWorkset 工具：49 个子函数源码链接加帧根，身份/命中索引成功接入。
  既有旧 MCP 连接未强制退出，新连接使用新版。技能已同步。

所有身份：[产物 SHA](frame-intercepts-20260916/artifacts.json)、
[源文件 SHA](frame-intercepts-20260916/source-identities.json)、
[实际状态](frame-intercepts-20260916/status-start.txt)。

## 限制和最终状态

入口观察回调与新函数体替换不同。现有实质重编译仍是帧分发器加四个提交 helper；
FrameCoordinator 等原函数体继续执行。通用退出拦截/改返回值需先确认 ABI，升级 typed hook。
23 个未解析间接目标仍需后续动态识别；没有凭调用点捏造入口或函数签名。

x86-64 SysV/AVX 是当前已实现的执行适配器；帧图/索引读取仍可多架构，其他 CPU 不能直接套用此汇编。
没有 full regression、十分钟、Swan、全函数新命中、完整帧主函数重编译或性能提升声明。

自动输入与捕获已停止，未附加 debugger。设备保持 Android 设置前台；下一次启动恢复原来的
`tmnt_frame_recompiled_v2` 包，避免全量拦截默认带入日常运行。全量实验包仍保留在本机构建目录，
需要继续时按 README 部署；当前没有活动游戏 Session 可供热启停。
