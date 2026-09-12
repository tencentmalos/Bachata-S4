# Android native host PKG v2：真实游戏整版交付任务书

> **当前实施入口（1a7da92c后）**：[生产Android Runtime整块spec](android-native-host-production-runtime.md)，[host对象/首链复核与反例](../validation/android-native-host/pkg-v2-host-closure-review-2026-09-12.md)。正式target与388个对象已存在；完整host共享库和游戏APK未完成。沿用本文验收目标，连续补齐生产接线，不以9个符号或first-link为交接终点。

> **后续迁移执行组织**：按[两工作包 bionic 方案](android-native-host-full-link-plan-2026-09-12.md)连续推进，五组依赖前置、FFmpeg7.1.5独立provider、匹配libadrenotools供给及三方库配置核查已直接完成，详细结果见[依赖记录](../validation/android-native-host/bionic-prerequisites-2026-09-12.md)。本文的功能/正确性/设备验收条件保持有效，内部编译顺序不再拆成交接版本。

> **Git/COMMON后续复核**：[2026-09-12记录](../validation/android-native-host/pkg-v2-common-review-2026-09-12.md)。图形修复已提交，遗漏资料已归档并推送；COMMON复核发现的小问题已在本轮直接修复并通过正式回归，无需后续重复处理。直接开始target/link，不另拆扫描里程碑。

> **2026-09-12 执行更新**：以 [Vulkan 复核与修复](../validation/android-native-host/vulkan-review-2026-09-12.md) 叠加当前工作区继续。用户要求本轮直接修补明确缺口，下一 AI 继续整版工作；默认 **Android/bionic Turnip**。图形编译缺口与 acquire 局部行为已有修复，不重复从原来的两份 `/tmp` sweep 起步。仍须完成正式 CMake 全链接、Turnip 的实际 native 加载、生产 guest/VM/HLE/Surface 和真实 PKG 真机验收；不另拆微型 spec。

日期：2026-09-12。状态：**待实施，未完成设备验收**。起点：`codex/android-fex-round2` / `0f4fd74b1ef7b704fe6250805f7a6365c538e1a6`，FEX 固定 `385a0cc4`。先读 [本次复核](../validation/android-native-host/full-pkg-review-2026-09-12.md)和[证据](../validation/android-native-host/2026-09-12-review/README.md)。

## 1. 一个版本，一个终点

用户最新要求是补齐正常执行真实 PKG 所需的完整链路，持续推进到真机可验，**不要再把每个小接口、每个 TU 或每个 HN gate 当成需要另行规划的版本终点**。

本版交付：普通 Android APK 从 UI 正常安装 **TMNT: Splintered Fate，CUSA50828，1.00 本体＋1.08 更新**，使用主仓原生 ARM64/bionic host 服务和现有 `guest_cpu_fex` 执行实际游戏，进入可操作场景，有正确的游戏画面、输入响应和实际音频，至少连续运行十分钟，然后正常 Stop，并在同一进程重新启动同一真实内容。

目标设备仍是 Swan / Android 16 / ARM64 / **4 KiB**。当前 AYN Thor `9c2841a4` 在线，先用它完成同一真实链路的辅助验收，随后在 Swan 上验收；AYN 成功不得改名为 Swan 成功。Swan 缺席不能成为推迟本地构建、内容安装或 AYN 真机调试的理由。

此任务书替代 [host-native v1](android-native-host-v1.md) 中“先到最小自有 ELF/一个 HLE，再等待下一轮规划”的**交付节奏**，以及 [旧完整链路方案](android-native-host-full-load-chain-2026-09-12.md) 中仅能 fsyntax-only / 只差入口的错误前提。原 V0/Round 2/G2/G3/H3 的正确性合同继续有效；这是 host 集成的新版本，不能据此倒改旧测试归属或宣布 V0_ACCEPTED。

可保留内部依赖顺序、小提交、定期进度和定位 fixture；一次失败后应定位并补齐，而不是把“到达下一个不支持点”当成整版完成。**真实 PKG 主线使用同一套生产 loader/HLE/runtime，禁止游戏路径失败后回退固定循环，也禁止 fixture 专用 loader/HLE 成为交付内容的执行路径。**

16 KiB 设备适配、PSVR/Move、Beat Saber、广泛游戏兼容、有限 Step 和另做 UI 外观不在本版范围；现有 ELF alignment、PS4 自身 ABI 粒度和 CPU 安全合同保留。

## 2. 执行规则：连续推进，准确识别外部阻塞

- 默认在当前 owned 主仓分支工作，先记录 HEAD/dirty/submodules，保留已有研究和未提交文档。按下述三个工作包的依赖连续推进，不为常规 CMake 拆分、平台适配或补一个真实 HLE 再索要许可。
- 不新引入整个旧 FEX backend、glibc runtime、X server 或 Vortek。Android reference 取导入/UI 契约；ARM64 reference 取 guest entry、veneer、TLS/callback 的结构，适配到当前 CpuContext/GuestAddressSpace。
- 动依赖前读 [子仓归属](../subrepository-ownership.md)。FEX child 的贡献约束仍适用；先在主仓完成可行的固定版本适配。若最终确实需要受限制的 child 接口更改，提交精确接口提案和失败证据，明确该外部阻塞并继续不依赖它的工作，不能擅改 child 或宣布该功能完成。
- Mac desktop 的 Vulkan-Hpp/libc++ 错误不等于 Mac 不能 Android NDK link。本次已有 [Darwin NDK 真实 shared-link 证据](../validation/android-native-host/2026-09-12-review/ndk-darwin-link-probe.txt)。首选在现机建立 Android host 全链接；Linux/CI 补 desktop/Linux 回归。只在给出实际失败命令/缺少能力后使用“环境阻塞”。
- 普通设备运行应优先使用现有已授权连接。不要结束其他应用任务、刷机或全局更改系统配置来迁就本任务。没有目标内容/设备时，完成独立代码和打包，状态标明“设备验证未完成”，不以模拟 PASS 填表。

## 3. 固定真实输入，先验证本体和更新

本次直接读取 PKG 头/param.sfo 并计算全文件 SHA，确认输入如下。绝不提交这些包、解包后的游戏代码/资源或密钥。

| 用途 | 本地定位 | 身份 |
|---|---|---|
| 本体 | `/Users/bytedance/game/ps4/TMNT/CUSA50828/` 下 `TMNT.Splintered.Fate_CUSA50828_v1.00_… .pkg`；执行脚本从显式路径或下述 manifest 获取，不靠模糊匹配选包 | CATEGORY `gd` / APP_VER `01.00` / SHA `4ebea14adbabdd48c68875839f9e65800954a8a916247b1f353e8d76b414f2b6` |
| 更新 | `/Users/bytedance/game/ps4/TMNT.Splintered.Fate_CUSA50828_v1.08.pkg` | CATEGORY `gp` / APP_VER `01.08` / SHA `c3ad762919ca1e2bbf781d856b842379b08d0a5e6ef71fff122f0d8a859e0af1` |
| 重复更新 | 本体目录下另一个 1.08 文件 | 与顶层更新全文件 hash 相同，不重复安装 |

完整准确路径、大小和 SFO 字段见 [pkg-set.json](../validation/android-native-host/2026-09-12-review/pkg-set.json)。共同 Content ID `UB0511-CUSA50828_00-0673996215904686`。文件存在和身份匹配不证明解包树完整或游戏兼容；本版要继续验证。

使用已有 PKG native extractor、ContentImporter、overlay UI 和 Library repository：

1. 识别 CATEGORY/TITLE_ID/CONTENT_ID/APP_VER，base/update 顺序明确，更新不能作为独立本体直接登记。ParamSfoReader 需返回这些字段，旧版本比较、跨 title 更新、缺本体在写入前拒绝。
2. 先在构建机/测试工作目录完成一次可重复内容检查，输出 eboot/SELF、sce_sys、sce_module 和资源树清单，以及每次装载实际依赖的模块/动态 imports/系统模块来源。最终验收必须另走 app UI 安装，不能只 adb 推入树或修改 Room DB。
3. 复用现有 staging/安装记录，修复 `overlayStaging` 的原地逐文件覆盖风险。可选择“生成新 effective tree 后原子切换”或“版本化 base/update＋有明确优先级的 VFS”，但取消、空间不足、进程终止后必须可恢复到一个完整版本；launch 和安装互斥，不能在运行中更新其 backing。
4. 记录 source package hashes、应用后的有效版本、实际 eboot/module hashes、安装事务状态；不能继续使用 `sha256="pkg-extract"` 作为内容身份。更新 metadata 与有效文件树一致，保留未被 patch 替换的本体文件。
5. zlib 已接入，不为完成基本导入先 vendor libdeflate。验证 PFSC 块长度/解压完成、读写短操作、取消/回滚、路径边界；按格式处理 padding，不靠补零或丢失数据继续解包。
6. LaunchDescriptor 包含真正的安装根/guest entry 路径、内容版本、参数和能力/设置快照。service/JNI/backend 消费该描述，不只读 gameId。缺入口、损坏模块报实际错误，不回退 smoke。

如果正常链路需要额外系统模块，从现有经用户授权的配置/文件中验证；不得自动下载来源不明的游戏、固件或运行时。缺少模块时给出具体名字及实际解析证据，不笼统说“游戏不兼容”。

## 4. 工作包 A：可靠的 APK、构建和运行所有权

### 4.1 修复本次复现的 Session 边界

在当前 SessionCore 上补齐，不回退旧 JNI 全局裸 context：

- 将 Prepare 后的提前取消、正常返回、GuestFault、异常统一进入“禁止新控制 lease → drain → owner Destroy → 真 terminal”。快速取消不能在 Ready 状态直接 Destroy。
- control lease 必须 RAII 归还；Prepare/Run/Destroy 和线程入口均有定义好的异常恢复。不能依赖 JNI 线程 catch 捕获 owner 异常，错误分配/格式化失败也不能越过线程边界。
- Stop/drain timeout 是“仍被拥有、尚未退出”的可观察状态，不是可让 owner 离开的 immutable terminal。保留有资格执行 Destroy 的 owner/协调者，迟到控制调用退出后完成回收；严格区分退役失败/poison 和可以安全重试的状态。
- Start 在解锁 join 后复查 generation/phase；Finalize/WaitTerminal 绑定具体 generation，不得让旧 waiter join 新 owner，多个 waiter 不互相改变所有权。析构不能留下 joinable thread 或让运行中 owner 引用已析构 SessionCore。
- 保留 G2 execution lease、publication、shared/local/decoder retirement、poison、sink/pin 排他。任何“先让游戏运行”的改动不得绕开它们。

把 [本次独立 probe](../validation/android-native-host/2026-09-12-review/session_edge_probe.cpp) 的提前取消重叠、Prepare/Run throw、控制 throw、迟到 drain 转成正式回归；测试现有代码真实路径，不只返回假状态。反例在修前有明确失败、修后有正确清理和同进程重启。

### 4.2 长寿命游戏 Service

移除 Running 后十秒未结束就 Failed/stopSelf 的逻辑。游戏运行没有这种固定寿命；只有启动阶段、取消请求、停止/销毁等待使用明确预算。观察可采用事件流或有界等待循环，等待超时仅表示仍运行，不代表会话失败。

onDestroy/Surface 回调不在主线程做秒级 JNI 等待。定义前台/后台策略及异步清理 owner，保留 generation 和终态归属，stop 失败必须向 UI/诊断返回，不能被 handleStop 忽略。由外部服务生命周期触发销毁时，也不能误杀较新 generation。

JNI 使用稳定的结构化 session/stop/snapshot 协议；保留线程局部 JNIEnv 和引用生命周期。Loaded/Ready/Running/GuestFault/Stopped/NotExited 语义不可混淆，不能以 ordinal 数值巧合代替协议版本。

### 4.3 Allocator 和 native 构建闭包

重新以**实际链接的** `InitializeAllocator`/hooks 为准审计：当前 so 使用 rpmalloc 路径，不是 bionic no-op。修订 provider 设计，涵盖 small allocator、mmap/munmap providers、JIT/backing 和进程寿命；不能只分析未被链接的 Dummy 分支。

retry guard 仅修 probe 失败后的重试，不能恢复 SetupHooks fatal trap。对 ART 下最终区域所有权给出正确方案：持续持有/移交，或经过完整论证的固定版本 provider 适配；禁止把延长 sleep/多跑几次当成并发安全证明。使用明确故障注入验证首次失败后恢复，并在普通 app UID 下做冷进程与同进程重建/Java churn 证据。若接口边界确实阻塞，按 §2 处理，不能宣称 HN-A01 完成。

在根 CMake 的实际源列表上建立可复用 `shadps4_host_core` 及 platform/renderer/audio 组合，不复制两份巨大 CORE 列表，也不直接调用带 quick_exit 的 Emulator::Run。**完整游戏 app 必须链接其真实所需的 HLE/视频/音频/输入及依赖**。小 headless target 可以帮助定位，但不能用空 RegisterLib 或 `--unresolved-symbols=ignore-all` 交付。

逐项补齐 NDK/bionic 闭包，包括 AAudio 源及 `libaaudio`、Android Vulkan WSI、date/tz source和目标、FFmpeg/媒体、配置/文件系统、必要的 SDL/ImGui adapter 或对它们的真实解耦。HLE body 仅因没 include 视频头不能自动归为 headless-clean；用 link map/undefined symbols 判断。Desktop adapter 保留其有效构建。

为辅助设备建立一致 native API33 profile；当前 JNI33/FEX prebuilt35 不能继续未加身份检查混用。Swan compile/target SDK36 与实际 NDK API 分开锁定，native API35 可以使用但须与 minSdk/依赖合同一致。记录 NDK/JDK/CMake、实际 metadata、ABI/STL/pins；每个 profile 有独立构建目录和依赖身份。

尽早完成 `--no-undefined` 的全 host `.so` link、打 APK 和普通 app 加载，再增量推进游戏链路；不要完成几千行后才第一次 link。复用 Foundation 的真实 DebugBus 入口，网络/反射/packing 只有闭包和生命周期验证后才启用，不新造通用诊断框架。

## 5. 工作包 B：完整生产 loader → guest → Orbis → renderer

### 5.1 VM 和真实模块

统一 Orbis MemoryManager 的 policy 与 GuestAddressSpace 的 reservation/backing/pin/权限/失效所有权。列出 guest VA layout 与 host VA bits，明确固定地址、可重定位区和 FEX/ART 保留区。旧 USER_MIN=64GiB/桌面大范围与当前后端上限不直接兼容。

根据**本体＋更新真实模块**的 PT_LOAD、TLS、relocations、direct/flexible memory 等需求实现固定/动态映射、file backing、真正 alias 和受控 guard。不能简单把全部 guest 地址平移，也不能去除 backend 上限而不验证高地址 lookup/tag/decoder/cache 的完整性。MAP_FIXED 只能替换已拥有范围，不能照搬扫描 maps 后覆盖空洞的竞争代码。

区分 guest 逻辑 Execute 与 host protection；如调整 x86 guest 数据页的 host PROT_EXEC，必须让 QueryGuestExecutableRange、veneer、发布和失效仍按 guest 权限工作。PS4 页粒度、host 4KiB、GPU tracking 各自独立。

运行中 guest mmap/protect/remap/unmap 通过 HLE 协调边界完成，不能 drain 自己仍持有的 Run lease；pin/CPU/GPU/alias 对同一 backing 的寿命一致。新增代码动态发布仍先退休旧翻译，失败仍封锁执行。

将现有 loader/linker/module 组合进 HostRuntime，保留真实 SELF/ELF、procparam、静态/动态 relocations、模块依赖、libc 初始化、BSS/RELRO 等逻辑。所有 host-visible 读取使用合法 guest 内存/ABI转换，不能用未检查的 host 指针替代 guest object。

### 5.2 guest entry、HLE 和线程必须同批补齐

不要只修 RunMainEntry：检查 module init/fini、`_malloc_init`、heap callbacks、pthread start、TLS destructors、atexit、guest exception/fiber/media callback。guest 地址全部经过 CpuContext 的正式 Run/InvokeGuest 边界，禁止 cast 成 ARM64 函数直接调用。

Context 是 session 执行域，ThreadHandle 对应长期 native owner；模块 init、callback、主入口不能每次新建 context/thread 并丢掉原 guest TLS、寄存器和缓存身份。构造 guest 栈/EntryParams/argv/env、return gate 和 FS/GS，所有 guest 可见地址来自受控 VM；不要传 host `&EntryParams`/`c_str()`/native TCB。

将 resolver 的 LIB_FUNCTION 从 host 函数地址改为 guest veneer＋typed descriptor；LIB_OBJ 使用 guest ABI 对象/代理。module export 与 HLE export 分开；NID/library/module/version 匹配、非法/未知 gate 和直接 syscall 按既有归属政策处理。Native linker 自己调用 `sceKernelAllocateDirectMemory` 不算 guest→HLE 往返证据。

复用 `call_adapter.h`、registry 和正式 FexSyscallDispatch，补齐真实游戏需要的签名/结构转换、buffer方向和长度、返回 mask、guest/host FP/flags/errno。raw pointer/varargs/aggregate/sret/callback 不能被自动模板“猜成可用”。不得广泛给未知导入返回成功以绕过初始化。

完整实现原 G3/H3 的 HleScope、两层 InvokeGuest、可取消 WaitingHle、独立 TLS/栈，以及两 owner 同时 HLE 的错误/中断归属矩阵。它们是本版执行真实 libc/线程/回调的组成部分，不另留一个“下一版再实现 callbacks”。guest pthread 同步对象按 Orbis ABI/handle 建模，阻塞等待可取消，不将 bionic pthread/TPIDR 当成 guest TLS。

按实际静态 imports＋运行时访问记录维护能力清单。优先补齐该游戏启动/可交互所需的 kernel、sysmodule/libc、filesystem、userservice/systemservice、pad、VideoOut/GNM、audio、保存/异步任务和实际媒体需求；不要求无差别移植全部 5,277 处注册。未被游戏使用的功能可明确 Unsupported；被该场景实际需要的缺口不能借此宣称游戏已通过。

Android Run driver 应复用主仓启动步骤（配置/metadata/挂载/HLE/LoadModule/Execute），移除进程退出和 desktop event-loop 假设。形成 session-owned cleanup：停止 guest/等待者/callback → GPU/audio 安全退休 → handles/modules/mounts/VM 清理。系统库与配置来源可追溯，第二场不能沿用上一场静态函数地址或 TCB。

### 5.3 真实呈现、输入和音频

保留新增 Android Surface 分支，完成 platform surface provider：ANativeWindow 引用、surface generation、尺寸/旋转、detach/recreate 全程归 render controller。不要依赖旧 WindowSDL 指针“刚好是最新窗口”。

2026-09-12 本地修复已将 acquire 改为每次一次有限超时调用，区分 Acquired/NotReady/Cancelled/Recreate/Error，超时/取消不主动 Recreate，Suboptimal 先消费图像和 semaphore；见 [Vulkan 复核与修复](../validation/android-native-host/vulkan-review-2026-09-12.md)。继续完成 session Stop/Surface detach/join 前调用 Presenter::RequestStop 的生产接线；成功 acquire 期间收到 Stop 仍须完成 semaphore/image 退役，不直接丢弃。VideoOut/presenter/queue/fence/scheduler 退出顺序明确，清除 `waitIdle` 等无界操作形成的停止障碍。DeviceLost/不支持的 feature 要报告实际故障，不靠 assert 整进程退出。

将现有 GNM/PM4/rasterizer/shader/VideoOut 真实路径链接并接入 VM 的 GPU observer/backing。先定位 WSI、再真实 guest 提交，但版本终点必须是游戏帧。记录实际 GPU/driver/Vulkan feature bits/formats；设置里选了 driver 只有在最终库已加载时才算生效。**用户于 2026-09-12 指定默认使用 Turnip**。本版直接接入 Android/bionic Turnip，系统驱动适配不作为首轮任务；不能因方便引回 Vortek。复用现有 driver registry/ABI 校验和已审计的 adrenotools 加载方式，固定所选驱动包 SHA、ELF Build ID/DT_NEEDED 和来源。APK 里的旧 `*-EMULATOR.zip` 可能是 glibc，不能按名字当作 bionic 驱动。当前 `RuntimeProfileResolver` 默认回退 system、`VulkanDriverConfiguration` 只产生环境字典、`CreateInstance` 仍直接使用 DynamicLoader，这三处必须在实际 native driver 接线时一起改：未选驱动时解析受控的默认 bionic Turnip，缺失/不兼容时报明确错误，不静默回退系统驱动；将该驱动的实际 `vkGetInstanceProcAddr` 交给 Vulkan-Hpp dispatcher，并保持库和 namespace 存活到最后一个 instance/device/worker 退出。首次创建 device 后记录实际 driverName/driverInfo、shaderInt64、所需扩展和 feature bits；不能用 `cmd gpu vkjson` 的系统驱动结果证明 Turnip 支持。

连接现有 Android controller snapshot 到原生 controller/pad HLE，处理 generation、按键释放、断连和焦点；以游戏场景实际响应证明，不只 UI overlay 显示按键。

AAudio 可保持阻塞写，按现有 AudioOut host worker 模型实现：短写有界补齐、失败向上返回、初始化失败不得留下“已成功打开”的假端口、正确 per-channel/app gain、格式/通道转换、断连恢复和停止。不要声称无 callback 就天然正确。必须有真实 guest PCM 输出及进入游戏后的音量/静音/重启验证。

资源导入、真实 HLE、图形、输入、音频都在同一主线 APK 中组合。host clear/triangle 或自有 guest tone 仅作定位手段，不能代替实际 TMNT 的显示/音频/交互。

## 6. 工作包 C：直接推进设备验证和可交付 APK

### 6.1 首次上机就记录实际链路

final manifest 保持 service exported=false，普通 app UID；从 UI/instrumentation 走真实导入、更新和 Launch，禁止改 Room DB或临时导出 service 作为最终证据。安装包身份、安装 splits、各 so Build ID/DT_NEEDED/ABI/alignment 与源码/dirty patch/pins 对应。

在每个 session/generation 中记录连续事件：

```text
base/update verified → install committed → effective eboot opened
→ module mapped/relocated → libc/modules initialized
→ actual guest entry → guest-origin Orbis calls → guest threads/TLS
→ GNM/VideoOut submit → Vulkan present → controller consumed/audio frames
→ stop requested/receipt → all session resources retired → second launch
```

输出 source ELF/SELF 与 effective module hash、guest RIP→module offset、解析 NID/veneer、实际 renderer/driver 和错误归属。日志采样需有上限，关键阶段/失败事件不能丢失。不得以“进程还活着”替代 guest 前进、Stop 完成或资源清理。

遇到卡死/崩溃：保存 exact ELF/符号身份、logcat、可得 tombstone/host LLDB 安全点和 guest 归属，定位缺口后继续修；不用无效 settle sleep/host SIGILL 修复。真正需要 native 调试时使用仓库规定的 debugger 工作流。不能把 first fault 记作正常已启动完成。

### 6.2 整版完成的八项条件

以下是同一版本的完成清单，不是八个需要各停一次的项目。每项必须有真实结果；所有未满足项在最终报告列清。

| 完成条件 | 最低证据 |
|---|---|
| 构建和安装 | 干净 Android full-host link，`--no-undefined`，可安装 APK＋符号＋全 native 身份；相关 desktop 回归结果独立记录 |
| 真内容 | UI 安装匹配本体/1.08更新，effective树正确；缺本体/错误title/取消更新不产生半安装可运行状态 |
| 真执行 | 实际 eboot 经主仓 loader/relocation/libc/sysmodule/FEX 执行；真正 guest-origin HLE、线程/TLS/callback；无 smoke fallback |
| 真图形 | 游戏正常呈现，进入一个可操作场景；首帧之外持续呈现且 guest frame/flip 计数推进，无只画 UI/host triangle 冒充 |
| 输入和音频 | 控制器可操作场景；游戏有实际音频，音量/静音有效；记录功能异常和帧/缓冲统计 |
| 长运行与退出 | 同一游戏场景连续至少十分钟；主动 Stop 使用既有有界合同，明确完成清理；同进程连续启动/停止三次并实际执行新 generation |
| 失效/故障/生命周期 | 本次五个反例关闭；原 G2/G3 实際触及矩阵回归；Surface变化/后台策略/guest fault/early stop 不破坏 app 或下一场；allocator 冷启动/retry/churn 证据完整 |
| 设备与交付 | AYN 和 Swan 分别保存环境/结果。必须明确哪台已验；APK路径、源码提交、构建脚本、安装/复现命令、符号及已知限制可供另一机器使用 |

“first entry reached”“first HLE returned”“first frame”只能作为中途进度。无法正常进入可操作场景时，版本仍未完成，继续按真实错误补齐；游戏本身是否在此组合可达目标仍需验证，不预先许诺兼容性。

本次只有 AYN 在线：应先完成 AYN 的全部可执行条件。Swan 无法连接时可交付 `AYN_VERIFIED / SWAN_NOT_RUN` 的清晰产物，但不能写“目标 Swan 验收完成”。若代码尚未完整编译/链接或没有实际 APK，就不能写“只差设备”。

## 7. 交付物与执行 AI 的最终报告

在 `docs/validation/android-native-host/pkg-v2/` 归档：source/pin/toolchain/API锁、依赖与HLE能力清单、base/update/effective module manifest、实际 APK/库哈希、唯一 case/原始日志/退出码、设备视频或截图对应的 guest帧、运行/停止时长、内存/VA/线程/fd/GPU/audio 回收基线。构建产物和游戏留在忽略的输出目录，仅提交 manifest/脚本/必要符号定位信息，不提交大型包。

runner 对重复ID、缺尾部、timeout、crash、未退出等返回非零；test-count、case-count、应用功能验收分开统计。保留历史证据，不将旧 smoke 或语法通过填进整版 PASS。

可分批提交代码，但最终按整个版本报告：

1. 实际运行了哪个 base/update/eboot，在哪台设备、哪份 APK，达到何种游戏场景。
2. 八项完成条件的通过/未通过结果；本次复核缺陷如何关闭，原合同的未完成项。
3. 可下载安装的 APK/对应符号和准确复现命令，以及源码/子仓/远端可检出的身份。
4. 如未完成，写最后一个真实失败的 stage/RIP/module/NID/错误、已完成的定位和确切外部阻塞。不要只交新的 gap map 或建议再开一轮 spec。

执行 AI 应从这份任务书继续实施到上述产物和设备结果，而不是再次只做源码巡检或重新拆分规划。
