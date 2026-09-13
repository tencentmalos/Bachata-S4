# Android 生产 Runtime 整版实施：从可加载 host 和已接通输入到 TMNT

日期：2026-09-13。执行者：用户指定 Opus4.8。分支 `codex/android-fex-round2`；从包含本文的最新提交继续。本文件以[本轮复核和修复](../validation/android-native-host/runtime-input-review-2026-09-13.md)为事实基础，更新[生产 Runtime spec](android-native-host-production-runtime.md)的施工顺序和完成判据。旧文的 ABI、生命周期、内容事务和整版验收要求继续生效；与本轮事实冲突的旧状态不再作为待办。

**目标是一版普通 Android APK：UI 安装 TMNT 本体与更新，经生产 loader、FEX、Orbis HLE、Turnip 和原生 Surface 进入可操作场景，持续运行十分钟，正常 Stop，在同一应用进程完成三轮游戏启动/停止。** 内部按下面两个工作包连续实施；不在“能链接、首个 HLE、第一次 present”后停下来要求新的微型 spec。允许按风险分提交，并报告真实卡点。不得在缺少证据时承诺游戏兼容性或把崩溃算成进展通过。

## 起点和固定选择

| 项目 | 实际起点 / 约束 |
|---|---|
| 主仓 | 被审基点 `768636aa`，已验证修复 `98eb0534`；正式地址 `https://github.com/tencentmalos/Bachata-S4`。不要 reset 回旧基点丢掉输入/加载修复 |
| Foundation | owned `codex/shadps4-android-fex-v0`，`5388ef45313d6c32cb5f4bb5b07f1246ee381370`；独立 input C++/Kotlin 库已接入，不再新建另一套设备/反馈框架 |
| FEX | 保持 `385a0cc4`。本轮未修改子仓；先读其贡献规则和主仓子仓归属记录，不把预期改动直接写进只读参考分支 |
| native / APK | NDK29.0.14206865、native API33、arm64-v8a、c++_shared；compile/targetSdk 与 native API 分开。主仓 host 使用 C++23 |
| 设备 | Swan / Android16 / 4KiB 是目标；AYN Thor / API33 / 4KiB 已有辅助普通 APK 证据。没有 Swan 就如实记录 `SWAN_NOT_RUN` |
| 图形 | 默认且只启用匹配的 Android/bionic Turnip，不静默退回系统驱动。验证实际选中驱动的 shaderInt64 |
| 内容 | TMNT CUSA50828，gd/01.00 本体 + gp/01.08 更新；身份见 [pkg-set.json](../validation/android-native-host/2026-09-12-review/pkg-set.json)。1.08 不是独立本体 |
| 延后 | 16KiB、系统驱动兼容、PSVR/Move/Beat Saber VR、finite Step、广泛游戏兼容 |

当前两个 native 库有明确分工：`libshadps4_host.so` 唯一持有 host/Orbis/InputHub，`libshadps4_fex_session.so` 持有 JNI/SessionCore/FEX，通过 DT_NEEDED 引用 host。APK 已真实装载两者；**不要求为了“一库”再次合并链接图**。保留单份全局状态，生产 backend 通过正式接口组合它们；如有充分理由调整库边界，须证明没有复制 registry、allocator、input 或 renderer。

本轮已闭合且不得重做的基础：SDL-free host 构建与 ART 装载；Tracy initial-exec TLS 原因隔离；显式 host 数据目录；Foundation Android 采集→JNI→真实 scePad 读写；会话/连接 epoch、短按历史、焦点/Stop/重启和反馈退役；无麦克风的明确失败。当前 Service 跑的仍是 CPU loop，尚未启动生产游戏。

## 工作包一：完整 guest 运行时与生产 loader 一起接通

这是整版的关键路径。先在同一个生产 backend 中用自编 guest fixture 验证真实数据与控制流，再用 TMNT 继续定位；fixture 是内部诊断入口，不另建一套能跑测试却不被游戏使用的 loader/HLE。

### 一个 VM 所有者、一条可审计的装载链

从 `src/core/host_runtime/session_backend_fex.cpp` 的 smoke 职责分离正式 game backend，沿用 `SessionCore → ISessionBackend`。复用 `src/core/module.cpp`、`linker.cpp`、`loader/elf.cpp`、`memory.cpp` 和 `address_space.cpp` 的实际模块/符号/Orbis策略，不把 CLI audit 变成第二套生产 loader。

准备不可变 launch 描述：受控安装根、effective entry、base/update/module hash、argv/env、用户和设置快照、驱动包、Surface generation。Prepare 完成 host 路径、配置、挂载、用户、HLE registry、模块与 VM 准备；任何失败进入当前 generation 的 Failed，并完整回收已经取得的资源，UI 不停留在虚假的 Preparing。

装载必须覆盖 PT_LOAD、PT_SCE_RELRO、BSS、SCE dynamic/dynlibdata、procparam、符号/依赖和 TLS。检查长度、加法溢出、文件/内存包含关系、对齐、重叠和重定位表项大小；所有读取、映射、写入、保护操作检查结果。先写入和重定位，再发布最终权限/可执行状态。不能用 RW 零页填补所有段间空隙，也不能把加了 `TryLoadSegment` 误称为整个恶意 ELF parser 已加固。

本轮对真实 base eboot 的准确审计是：3 个映射段，其中 1 个 RELRO；92929 项 RELATIVE；406 项 `R_X86_64_64`；652 项 `JUMP_SLOT`；TLS memsz 40144。后面三类尚未由 audit 完成。**这些数字是该文件的装载结构，不是运行到何处的证据。** 实施应解析实际内容，不能按数字硬编码。

先写出并通过可执行检查的 VA 布局：模块、direct/flexible memory、栈、TLS、veneer、FEX gate、GPU alias。现 guest_cpu 64GiB 策略与旧 Orbis 高 VA 布局并未统一；明确 guest 地址与 host backing 的关系、FEX 实际 lookup 范围及 tag/decoder 失效路径。保留 reservation 所有权；不扫描 maps 后用 MAP_FIXED 覆盖空洞，不重复预占后再调用自占式 SetupHooks，不仅修改一个地址上限常量。

Orbis VM HLE、FEX pin/执行权限与 renderer/GPU 观察必须使用同一 backing/alias 账本。映射发布保持 G2 的 pre-mutation 退休、poison、sink/pin 排他、超时 retry epoch；在 HLE 边界释放/转换自己的执行资格，不能 drain 仍由自己持有的 Run lease。SetupHooks、rpmalloc 与 ART 地址空间/信号/TLS 的所有权需要实际普通 APK 证据；本轮关闭 Tracy 不等于 FEX allocator 风险已全部关闭。

### guest/host ABI、线程和回调形成一个完整合同

`Module::Start`、Linker 的主入口、`_malloc_init`、module init/fini、guest pthread/once/destructor、信号及媒体回调不得通过 native 函数指针执行 x86 地址。宿主 ARM64 HLE 地址不能写进 guest GOT。resolver 明确区分 guest export、host HLE、guest ABI 数据，按 NID/library/module/version 解析并生成可识别、限长的 guest veneer。

沿用既有 `guest_cpu/api`、typed registry 与 [E1/E2/E3](android-fex-round2-n4-to-apk.md)、[H3 修复要求](android-fex-round2-g3-repair-h3.md)。公开 API 目前没有完成 `HleScope / InvokeGuest / WaitingHle`，本包应把它们正式实现并用于生产回调。不要另起一个 `CallGuest` 包装 native cast，也不能把 FEX 内部 `HandleCallback` 当作已满足生命周期合同的可直接替代。

必须一起完成：

- 签名、参数/返回值位宽、SysV ABI 与 host ABI、guest 指针方向/长度/溢出检查和有界 pin；部分 pin 失败回收、host 抛异常、errno/fenv/FP 恢复均有负例。禁止把 arbitrary guest pointer 当 host 可用指针直接传递。
- 长期 owner、独立 guest 栈/FS/GS/TLS、每线程错误归属；两 owner 一方出错不污染另一方。TLS 模板和每线程实例从实际模块装载，不把 host bionic TCB 暴露给 guest。
- guest→HLE→guest 至少两层嵌套；返回位置、栈和寄存器恢复；可取消 WaitingHle。Stop 不跨活动 JIT/C++ 帧任意 longjmp，也不使用 settle sleep 或恢复 host SIGILL 来补控制流。
- 真实退出 gate、非法 veneer/未解析 import 的结构化失败；未实现函数提供准确 unsupported 语义，不能把所有 NID/返回值打成成功零值。固定生产代码需要的 syscall stop wrapper，production `V0_BUILD_TESTS=OFF` 排除测试 gate/trace。

故障必须分清 host PC、guest RIP、snapshot 来源及有效性。原 native `pc=0` 没有证明 guest RIP=0，也没有证明 crt 完成。异步 JIT stop 的 CPUState 可能陈旧；使用固定 FEX/layout 的 spill 安全点或已验证的 host-PC→guest 元数据。记录 context/thread/generation/invocation 和最后一个确证的 HLE/guest 副作用；未知 host fault 不能包装成正常 guest 返回。保持 ART/FEX/GPU handler 链及异步信号安全，不用通用 SIGSEGV catcher 掩盖所有错误。

### 本包的内部通过条件

自编小 ELF/SELF fixture 经**生产** module/linker/backend 执行，确证 guest 写入、RELRO/GOT 正确、typed HLE 输出、TLS 隔离、嵌套回调和 Stop；坏地址/坏表/未实现 import 在安全边界返回可归属的失败。然后直接推进真实 TMNT 的模块初始化和入口；逐次修复最早确定的缺口，保存原始失败，不能从“host 崩在更远的位置”推导“guest 阶段已通过”。

保留 `eboot_prologue_harness` 的 load-audit-only 行为：`LOAD_AUDIT_PASS / EXECUTION_NOT_RUN`，`--require-execution` exit3。生产执行的证据由正式 backend 产生；不要恢复旧 `STAGE0_PASS` 判据。

## 工作包二：同一个 APK 的游戏、平台和生命周期验收

工作包一完成后不再交回规划，继续绑定应用的完整资源。构建沿用根 host targets、`scripts/android/build-host-android` 和生成的 `shadps4-host-loader.cmake`；Gradle 使用同一份产物，不手列 archive，不引入 SDLActivity/SDL JNI。把“先外部构建再 Gradle”的当前两命令流程整合为可从新输出目录重现的 canonical 脚本/任务，并检查源码、gitlinks、NDK/API/STL、Build ID 和 APK 中实际库的身份。

- **Turnip / Surface**：锁定 bionic 驱动包和 hash，保留 adrenotools 四 hook；将实际 loader handle 的 `vkGetInstanceProcAddr` 传给 Vulkan dispatch。库、namespace、driver、instance/device、ANativeWindow 的寿命由 session 所有。记录实际 Vulkan driver/device/扩展和 shaderInt64；环境变量或系统驱动 vkjson 不算证据。绑定 AndroidWindow/Presenter generation，Surface 更换先 Stop/drain 旧使用者；有限 acquire、已取得 semaphore 的消费、fence/waitIdle 退出和错误处理都要可验证。
- **输入 / 用户 / 设置**：保留已接通的 Foundation→host 单份状态。生产 game Prepare 安装真实用户/端口，使用同一 `scePad*`；补齐保存的 remap/profile 到新 native 路径的应用，配置切换中立化旧状态，保持 overlay/实体合并。当前按物理位置的默认映射已验证，自定义映射尚未接入。补真实体按键/轴、同型号双设备、实际震感及游戏消费；host-origin JNI 测试不能替代 FEX guest-origin 调用。保留 Kotlin 菜单/设置控制，处理原 Android 排除 ImGui settings layer 后的真实 UI 职责。
- **音频**：AAudio 实际输出，处理短写、音量、open 失败、Stop 和 stream 生命周期；Audio3D 的 OpenAL 正常保留。麦克风无实现时继续返回 NOT_OPENED，不恢复静音假成功。首帧不能替代音频/输入验收。
- **内容与恢复**：UI 导入 base/update，校验 title/category/version/hash；effective overlay 事务支持失败恢复，启动期间锁定版本。使用正常安装路径、保存与用户目录，不从测试硬编码 `/data/local/tmp` 启动游戏，不提交游戏字节。
- **退出与重启**：guest exit、用户 Stop、Prepare/Run 异常、Surface 退出进入同一 generation 退役流程。先关 ingress/取消执行与平台工作，再 drain/join/reclaim；旧观察者/Surface/input 不清理新 generation。保留 late-drain 和真实完成回执。实际观测线程/FD/VM/GPU/音频资源的三轮前后变化，进程缓存必须有界且解释清楚。

## 验收记录与交付

| 验收对象 | 要交付的直接证据 |
|---|---|
| 既有修复不回退 | host dlopen + 85 项契约、Foundation 49 项/Android 5 tests、pad portable 45 项、APK input 6 tests、Service 3 轮输入/CPU-smoke 基线；按实际改动选择回归，源码改变后说明新增矩阵 |
| 完整 guest backend | 生产 loader fixture 的 guest 侧副作用、typed HLE、TLS/callback/取消和负例；确切 case ID /失败出口，不把历史 R2 NOT_RUN 填成 PASS |
| 整版内容 | UI 导入本体和更新、可控的真实 TMNT 场景、有效 Turnip、画面/音频/按键实际响应、连续十分钟日志 |
| 寿命 | **游戏**同 PID 三轮启动/停止，generation/input/Surface/driver/owner 退役与资源变化；当前 CPU smoke 三轮不计入游戏成绩 |
| 平台 | AYN 和 Swan 分开记录 API/page size/UID/设备/driver；普通 APK 身份，Swan 缺席保留 NOT_RUN |
| 可检出交付 | 子仓先 commit/push，再主仓 gitlink；远端 commit 可取得。完整 parent/child pins、source dirty patch（如有）、构建配置、APK/hash、打包库 Build ID、符号/原始日志位置和复现命令 |

提交证据只含源码、自编 fixture 生成器、受限日志和 hash，不含游戏、APK/DSO、驱动包、凭据或无关子仓工作。每项明确 PASS/FAIL/NOT_RUN；崩溃、重复 case ID、缺失终态、超时均使 runner 非零。最终报告先讲真实可运行程度及最早未闭合的链路，不用任务数量或搬迁文件数量替代结果。
