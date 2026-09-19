# TMNT 屋顶选择性重编译与工具补齐 — 2026-09-16

本轮在 `codex/android-fex-round2` / `4da582b7` 的已有 dirty 工作区上直接实施；不新增 spec，不提交或推送。
FEX/Foundation 子仓未修改，之前的工作保留。具体文件/工具源 SHA 在 [source-manifest](guest-recompilation-20260916/source-manifest.json)。

## 结论和范围

此前 `rooftop.cpp` 的五个函数确实仅计时并调用 original。本轮新增 `rooftop_recompiled.cpp`，把屋顶路径中四个函数的真实逻辑编译成 x86-64 guest C++：
label 命令写入、默认硬件状态输出、draw/compute 提交数组构建、九参数 label+flip 入口。
原 GPU 提交、buffer/label 语义和同步等待继续走现有链路，没有删等待、假成功或把 guest 指针交给 host 原生函数。

[源码/使用说明](../../../guest/games/CUSA50828/01.08/README.md) · [重编译契约](../../../guest/games/CUSA50828/01.08/rooftop-recompile.contract.json) · [通用链路](../../guest-selective-recompilation.md)

这只是经过验证的四函数单元；整个 FrameCoordinator、任务系统、FMOD 和 GNM renderer 未被重新实现。
`submit_packets` 的正常实现覆盖 0–15 个历史缓冲；更大 count 在写入前进入原 trampoline，并不修复原程序对非法输入的行为。
原函数的 stack-canary 属于编译器保护；新 guest payload 延续 SDK 的 freestanding/no-stack-protector 约束，不复制反编译得到的错误 GOT 常量。

## 实际补齐的工具能力

1. **Guest 绑定**：builder/runtime 支持 128 个显式原 guest 函数/数据绑定。函数检查 preimage/RX；数据检查范围、大小、对齐、R/RW，生成 typed const pointer slot 和编译期布局断言。共享原存储，不复制全局变量。安装先检查全部绑定才修改入口；VM 保留范围覆盖绑定页面。直接 guest 调用没有 host SDK 跨界。TMNT 此包使用三个函数绑定，数据绑定由独立 fixture 验证。
2. **重编译证据**：`reverse_study.guest_recompile_bundle` 已安装到 `reverse_study_mcp-0.3.0-local.20260916.recompile2`。按显式小范围导出完整机器码、指令、CFG、外部进入内部地址的引用、推断 ABI/伪代码及 SDK code/data refs。实际发现 SQL spine 不含完整 data refs，已改为读取 SDK xref；最终证据确实包含 `+0x193b4 → GOT +0x1d3b1b8`，不再把调用图当成全依赖集。
3. **构建准入**：`recompile_contract.py` 校验已审阅契约的源/证据 SHA、模块/ABI、prototype、原入口字节和逐项状态。它检查证据一致性，不把自由文本 `reviewed` 当作语义等价证明。契约更新后重建仍得到与真机相同的 package/ELF SHA。
4. **隔离差分**：新 fixture 工具直接读取精确 SHA 的用户本地 analysis ELF；原函数机器码保持原样，仅用 fixture GOT 接到效果记录 mock。真实 FEX 按同一份初始内存分别执行 original/recompiled，比较完整 RAX、64 KiB 内存和有序外部调用。不会在真实 Session 双重提交 GPU 工作。
5. **无 auto probes 的采样**：`guest-auto-tag capture --compiled-only --recipe ...` 检查 recipe analysis SHA/module/base、Session、已启用包和抓取所有权；生成无伪造 profile 的 manifest，直接交给 batch 工具。
6. **共享知识**：guest-auto-tag/reverse-study skills 增加选择性重编译流程；study0 默认不变。TMNT 共享 symbol index revision2 有9个 typed symbols、2个类型，Native MCP dry-run 验证通过；本轮未生成 GDB ELF 或进行 live 符号绑定。

重编译 evidence exporter 当前限制为每次1–16个精确函数入口、每函数最多4096字节，不会自动递归整个程序；这是重编译工作单元的限制，不改变 auto probes 默认无用户数量上限的设置。
其他 CPU 的 ABI/编译/重定位/异常适配不能从 x86 直接套用。AArch64 静态证据已单独通过，实际执行适配器仍为 x86-64 FEX。

## 定向验证

| 验证 | 结果及边界 |
|---|---|
| 原机器码 vs guest C++ | **40 cases / 41 checks / 0 FAIL**。容量63/64/255/256、grow成功/失败及高RAX、DWORD溢出、历史缓冲0/15、9/10参数、单调用顺序与内存副作用 |
| 原 guest 函数/数据绑定 | **18/18**。原VM存储共享、host SDK调用为0、禁用后在途引用仍有效，错误preimage/range/alignment/permission/hook alias在修改入口前拒绝 |
| 原 patch loader 定向回归 | **41/41**，含FP/sret/varargs、开关和publication poison，不是全套runtime回归 |
| Python builder / contract / capture parser | **11/11、8/8、3/3**，包含只读数据、错误布局/别名，以及不可信契约/源码/字节拒绝 |
| study0 真正 TMNT bundle | **6 checks**，7个函数证据；错误backend、非入口、重复目标、错误identity拒绝；完整SDK data ref另外检查 |
| AArch64 evidence | **3 checks**，2函数、BL/BLR及AAPCS64元信息；无执行适配器声明 |
| 工具安装 | package manager安装、配置6个Spatial入口args/env保持不变、Codex列表验证、全部**11 wrappers** initialize/tools/list通过 |

早期失败仍保留：第一轮replay的最后八例超过InvokeGuest八参数API容量，改用真实guest九参数caller后完成40例；普通Protect不支持部分mapping，fixture改用已有token VM API；数据导入发现GOTPCRELX，改为明确的hidden guest pointer slot；一次证据导出因复用非空目录被拒绝。这些早期结果不记PASS。

## 普通 APK 和实际屋顶

设备 **AYN Thor `9c2841a4` / API33 / ARM64 /4KiB / Turnip**，未操作Swan。
APK `2412969b00148325867eec18e2e70634230a6666a6c8099a5eda50f13d922d2f`；host
`189a1b29b2cf6548774ba37badbe625d14ead92f195ea831ae3c56179a845b18`。
已再次读取设备安装APK SHA，与本地一致。完整 [artifact identities](guest-recompilation-20260916/installed-artifacts.json)。

PID9049 / generation1 / UUID `03e1f411c395a97ec45807149847628f` / context1。
包 `tmnt_rooftop_recompiled_v1`，SHA `562938ee08dc04889944550320325ea4d53e79cb2fe3745c4415e81288b1119a`，ELF `1342cea8d464ff6de82533d17bbea59520cc57294410bb19e0370977a767d701`。
四个新helper均实际命中；采样计数分别达到1792次并继续增长。不是只安装入口或只跑fixture。

自动warmup进入Leonardo屋顶，实际stick-right输入改变人物位置和镜头，并由MOVE推进到ATTACK。
[前后图与manifest](guest-recompilation-20260916/gameplay.json) 对应同Session，运行中的显式视觉审核通过，最终 **GAMEPLAY_REVIEWED**，自动输入随后停止。

分时开关15秒×3：[原始记录](guest-recompilation-20260916/game-ab-a.json)。

| 模式 | Guest FPS | 平均进程CPU核数 |
|---|---:|---:|
| 重编译 A | 12.986 | 1.809 |
| 原版 B | 13.002 | 1.824 |
| 重编译 A2 | 12.939 | 1.844 |

B的SDK调用计数229488→229488，A2恢复增长，确认开关确实生效。这里是整包开关，B也关闭了五个旧计时包装，不能从中单独推算四个新helper的净开销。短窗口不能证明零开销或性能提升。
两种模式原有shader/驱动等待仍在，本轮没有声称提升FPS。

最终无auto probes、C++包启用的15秒PROF：[summary](guest-recompilation-20260916/profile-final-summary.json)：
199个完整guest frame，平均76.637ms、p95 150.960ms；152个长帧。
长帧重叠中Submit为9.243s，HLE SubmitAndFlip9.224s、GNM.SubmissionGate9.181s，仍是主要等待路径。
GPU覆盖为部分区间，不与guest/host相加。329chunks/0skipped，但5个边界orphan host ends、19个未闭合host spans；保留truncated标识，不称全局无损。

## 最终状态和限制

旧Session先正常Stopped/user_stop，当前新Session保持运行；重编译包启用，auto probes未装，ring和coarse GPU timestamps启用，文件capture关闭、自动输入停止，TracerPid0。
静态study0工作区均关闭。未做全量回归、全游戏/十分钟正式验收、Swan验收或root驱动等待归因。

下一次需要深入时，可以修改这个明确的小函数单元、更新契约和差分，或根据长帧证据转向host队列/驱动等待；没有必要继续给整棵guest调用树加包装。
