# Runtime / Foundation 输入复核与修复（2026-09-13）

被审主仓 `768636aadd6dd136ea2bff042627d642d58e5c3e`，Foundation `96a3eea`。本轮根据用户要求直接处理卡点并接通输入，再交付[下一轮整版实施 spec](../../specs/android-native-host-runtime-after-input.md)。验证发生在主仓 `768636aa + dirty patch`，Foundation 已先提交并推送 `5388ef45313d6c32cb5f4bb5b07f1246ee381370`；修复代码已由主仓 `98eb0534` 交付（对应源码hash已核对，见[delivery-source](2026-09-13-runtime-input/delivery-source.json)），本报告随后归档，不能把后来的 commit ID 倒填到旧 binary 的 SCM 字符串。

**结论：host 的普通 APK 装载和标准输入链路已经有实测基础；TMNT 的生产 guest 运行尚未成立。旧 Stage0“crt 已完成，卡在首个 HLE”的结论撤回。** 本轮没有继续在失真的 harness 上堆成功标记；修复了装载审计、生产 pad 接线、ART DSO 装载和输入退役，剩余工作的依赖方向已经明确。

## 审核发现及本轮处理

| 问题与来源 | 修复 / 当前边界 |
|---|---|
| **P1：旧 Stage0 漏真实 RELRO，执行判据不可信。** harness 只映射 PT_LOAD，把段间空隙清零；恰好遗漏真实 PT_SCE_RELRO 的文件内容。只处理 RELATIVE，忽略失败写入及符号/PLT/TLS，保护和重定位次序也不可靠。native pc=0 被解释成 guest null import | 重写为严格 load audit：PT_LOAD+RELRO、checked segment 读取、表/地址边界、checked relocation 写入、最后权限；明确 `EXECUTION_NOT_RUN`。生产 guest 执行仍待正式 Module/Linker/VM/HLE 组合，未声称修好了 native null-PC 的所有原因 |
| **P1：输入只在旁路对象中可读。** Foundation Android library 没有被应用实际采集路径使用；JNI adapter 状态与生产 `pad.cpp → GameController` 不同 | Foundation source→应用 JNI→Foundation InputHub→主仓 OrbisPadAdapter→**真实 scePadRead/ReadState/SetVibration**；adapter 只编入 host DSO，JNI 导入同一实例。默认实体按键/轴、overlay、端口/用户、历史、句柄和反馈已接 |
| **P1：会话/反馈寿命不完整。** app-local worker、晚到 observer 和设备复用可能使旧输入/震动进入新会话 | 去掉重复的 HapticsPump/VibratorHapticsSink；Foundation Kotlin actuator 无自建 worker，由主线程有限 pump 调度。闭包持有不可变 token，source/设备 epoch 校验，反馈队列每设备保留最新命令，序号拒绝旧命令，generation end 不能清理新会话 |
| **P1：真实 ART dlopen 失败，而 executable-linked host probe 通过。** pinned Tracy 的 embedded rpmalloc 使用 initial-exec `_memory_thread_heap` TLS；host 带 `R_AARCH64_TLS_TPREL64`，ART 后装载拒绝 | Android 关闭当前可选 Tracy，desktop profiling 保留。没有修改 FEX rpmalloc。新增完全不链接 host 的 `host_dlopen_smoke`，先 dlopen/dlclose，再跑契约；普通 APK 实际加载通过，最终 ELF 无该重定位 |
| **P2：Stop 错用窗口 focus，重启无新 focus 事件时输入永久停用。** 本轮收尾自检发现 | Stop 改为 generation-scoped quiesce；不会改变 Activity focus，也不能被随后 focus 事件复活。旧 Stop 不影响新 generation；增加专门负例及每轮 Service 的真实 pad 按下/松开验证 |
| **P2：无录音实现却返回静音成功；端口循环越界。** NullAudioIn 伪造可打开录音，audioin 的 `<= NUM_PORTS` 越界且初始化非线程安全 | backend Open 返回无设备，生产 HLE 返回 NOT_OPENED，不发布端口；修正边界和局部静态初始化，增加重复打开/无槽泄漏负例 |
| **P2：harness 无正式可复现链接闭包。** FEX tests target 的 loader include/link 环境不足 | 根 host 生成 `shadps4-host-loader.cmake`，APK 与可选 audit 使用实际 DSO/headers/defines 和 ABI/API/STL 检查。audit 不依赖 FEX archive，默认 OFF；不再手列 host archive 或依赖临时 include 文件 |

保留既有 IOFile 关闭文件读写保护、SessionCore 统一 Destroy/RAII/late-drain、G2 事务/poison/失效机制和 Android SDL 排除。此次 FEX 实现未改，`fex_context.h` 只纠正陈旧注释；没有引入 guest callback 或新 Run 语义。

## 真实 ELF 审计更正

使用已有设备 eboot，经当前 `Elf` 的 SELF segment 读取：

| 段/结构 | 本轮实测 |
|---|---|
| PT_LOAD text | VA0，filesz/memsz `0x17b5cdc`，flags5 |
| **PT_SCE_RELRO** | VA `0x1800000`，filesz/memsz **`0x1264c0`**，flags4；旧 harness 的 gap-fill 覆盖了这约1.15MiB真实文件数据 |
| PT_LOAD data/BSS | VA `0x1a00000`，filesz `0xec70`，memsz `0x32de20`，flags6 |
| 已审计并写入的 RELATIVE | 92929；不是 guest 指令数 |
| 尚未解析的符号重定位 | `R_X86_64_64` 406、`JUMP_SLOT` 652 |
| TLS | memsz 40144；audit 只报告，未建立线程实例 |
| 终态 | `LOAD_AUDIT_PASS`、`EXECUTION_NOT_RUN`；请求 `--require-execution` 的退出码为 **3** |

11 项自编 ELF 正负例覆盖：RELRO relocation-before-protect、请求执行不能返回成功、零 RELAENT、表偏移溢出、非整项表、未映射目标、filesz>memsz、VA 溢出、重叠、截断、缺少 DT_NULL。原始日志见 [load-audit-final/result.json](2026-09-13-runtime-input/load-audit-final/result.json)。fixture 由脚本生成，不含游戏字节。

`TryLoadSegment` 检查实际文件和 SELF 映射范围、压缩/加密不支持、索引、seek 和精确读取；它**不是整个 Elf::Open/动态解析器的恶意输入安全审计**。生产 loader 的完整范围检查仍属下一版。

旧 tombstone 的 `pc` 属 ARM64 host；异步 JIT 停止时 CPUState.rip 也可能陈旧。现有证据不能推出“crt 已跑完”或“首个 unresolved HLE 是唯一卡点”。本轮尝试的 debugger inspect 因 `device_module_abi_mismatch` 未建立会话，没有可引用的 LLDB 停止现场；报告不把它说成调试成功。旧 [Stage0 文档](pkg-v2/hn2/stage0-eboot-exec-2026-09-13.md)保留原文并加更正，不删失败历史。

## 当前输入实际边界

Foundation `modules/input` 是独立 C++ target 和独立 Android Kotlin library，不依赖应用 JNI、Activity、SDL/OpenXR 或主仓 Orbis ABI。Android source 按设备能力选轴，避免不存在的 BRAKE/GAS 覆盖真实 trigger；HAT/key 合并；所有 listener 操作在约定 Looper，退役 listener 不能重新注册旧设备。InputHub 校验有限值/range、单调 packet/feedback 序号和会话/连接 epoch。

主仓按物理位置映射 PS4 位和轴，实体/overlay 分开合并，保留有限历史以免短按被最终 neutral 吞掉；连接/句柄属于会话，旧 handle 不复用。`scePadSetVibration` 命令流向对应设备 epoch；主仓负责 PS4 持续电平的有限刷新，Foundation 执行器保持有界命令语义，没有手机震动 fallback。

**已验证的是标准映射和 host-origin 生产 HLE 调用。** 保存的自定义 remap/profile 尚未绑定新 native gameplay 路径；原 Android ImGui settings layer 排除后的完整设置职责、所有扩展 pad/传感器/LED API、真实双实体手柄和人体感知振动未验收。不能把这些叫作完整 DS4/PSVR 支持。AYN 真实枚举设备上的 KeyEvent 是注入的合成事件，不是人按按钮；Foundation actuator 使用注入替身验证取消/epoch，不宣称实际马达已振动。下一版补 FEX guest-origin pad 和游戏交互。

## 验证成绩、来源和局限

证据目录：[2026-09-13-runtime-input](2026-09-13-runtime-input/README.md)。末次 host DSO SHA256 `0ce6762bf9a658e37b9bb03debc12fe620626c942af44fea344b2e9d05e18c2f`，Build ID `3293fedc559fb4675e06ec010f29a5fa7b9739fb`。APK strip 后 SHA 不同，但 host Build ID 相同；[manifest](2026-09-13-runtime-input/artifact-manifest.json)同时记录源库、打包库、APK、JNI DT_NEEDED 和符号提供者。

| 环境 / 测试 | 实际结果 | 解释 |
|---|---|---|
| macOS Foundation input | 49 checks /0 failures | 平台无关输入与反馈合同 |
| macOS OrbisPadAdapter | 45 checks /0 failures | 真实 Orbis 布局/映射/历史/句柄；不是 Android game |
| AYN Foundation library instrumentation | 5 tests PASS | 注入式独立 Android 消费者/actuator |
| AYN host shell | dlopen PASS + **85 checks /0 failures** | dlopen 与 executable-linked 契约分别检查，部署 hash 匹配 |
| AYN loader audit shell | **11 cases PASS**，真实 TMNT load audit PASS | guest execution NOT_RUN / require-execution exit3 |
| AYN普通 APK input instrumentation | **6 tests PASS** | 实际 JNI→host scePad、source/key 注入、反馈归属、旧 observer/Stop 隔离 |
| AYN实际应用 Service instrumentation | **1 test /3 generations PASS** | 同 PID **26026**、UID **10157**；三轮实际 FEX Running→Cancelled，每轮 overlay 按下/松开经真实 scePad 可见，input token最终清零 |
| 最终库/打包 | HOST_LINK_PASS、Gradle APK 构建通过 | host 无 SDL/SDL JNI、无 initial-exec TLS；GlobalPadAdapter 只在 host 定义，JNI 为 UND 导入 |
| TMNT生产运行 / Turnip呈现 / 实际音频 / Swan | **NOT_RUN** | 当前 Service backend 仍是 x86 CPU loop；三轮不是游戏验收 |

最初 APK 的 TLS 装载失败、修复后暴露的 JNI motor 字段顺序错误、最后通过日志分别保存。末次构建 SCM 生成引起 host binary hash 变化后，重新跑了 host85、audit11、APK6和三轮 Service；`host-cli-final` / `load-audit-final` / `apk/*-final*` 对应上述最终 manifest，较早目录保留旧产物身份。

本轮验证没有重新跑全部 CPU/R2/desktop 套件，也没有给历史 NOT_RUN 项补 PASS。native-debugger 没有成功建立会话；没有 FEX JIT fault 的新安全点证据。

## 后续方向

关键路径不是再搬一次输入、升级几个库或换驱动试错，而是把 **生产 Module/Linker + 唯一 VM + typed HLE + TLS/pthread/InvokeGuest + SessionRuntime** 作为一个完整运行时做通。现 `Module::Start` 等仍有 native 调用 guest 地址的桌面路径，64GiB guest policy 与原 Orbis 高 VA 布局未统一，公开 `InvokeGuest/HleScope/WaitingHle` 未实现。

[下一轮 spec](../../specs/android-native-host-runtime-after-input.md)按两个连续工作包组织：先完整 guest 运行时与生产 loader；接着同一个 APK 的 Turnip/Surface/AAudio、内容事务、用户配置与真实游戏验收。内部可修复卡点持续处理，不按单个 HLE 或首帧拆成交接任务。
