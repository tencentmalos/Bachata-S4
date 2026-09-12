# 原生 host / 完整 PKG 链路复核（2026-09-12）

结论：**会话重构和 Android 平台适配有实质进展，但还不是“只补最后一个入口就能跑游戏”。下一版应统一交付真实 PKG 的安装、host 全链接、FEX/Orbis 完整执行、显示/输入/音频和设备验证，不能以若干 TU 编译或入口冒烟结束。** 执行入口为 [PKG v2 整版任务书](../../specs/android-native-host-pkg-v2.md)，取代此前“完成一个小 gate 再另行规划”的交付节奏。

审阅 HEAD `0f4fd74b1ef7b704fe6250805f7a6365c538e1a6`，分支 `codex/android-fex-round2`；origin 实查 `0e10defc04457e019c07fd2a78d4f58d506133b2`，本地领先 **43** 提交。FEX pin 仍为 `385a0cc4`。本次没有修改生产代码、FEX 子仓、安装包或设备游戏数据；新增独立复现程序/证据和文档。源码/hash/命令见 [证据目录](2026-09-12-review/README.md)。

## 1. 哪些进展成立

| 项目 | 复核结论 |
|---|---|
| HN0 主干 SessionCore 重构 | 已实现 shared runtime、control lease、generation 和 typed outcome；原 JNI 直接取裸 context 的代码已替换，旧 GuestFault→exit0 路径已修。下文是新边界反例，不将旧实现原样列为未修 |
| host 生命周期测试 | 本次重新构建/执行：**767 checks / 0 failures**；现有矩阵未覆盖本次发现的快速取消、异常和 drain 迟到路径 |
| runner | 本次 **23/23** |
| Android JVM tests | 本次 `--rerun-tasks`：runtime **92/0**、session **11/0**、library **6/0**，合计 **109/0** |
| HN1 RasterizerHooks | 内存层直接包含 Vulkan rasterizer 的耦合确已减少；不等于整个 loader/HLE 链接闭包已完成 |
| NDK time/UUID/AAudio/Android Surface | 有实际源码增量；报告主要是 `-fsyntax-only`。`shadps4_host_core` 仍不存在，新 AAudio 文件未加入当前主仓/app CMake target，app 仍只链接 CPU smoke |
| HN4 bounded acquire / surface rebuild | 100 ms acquire 调用和重建代码存在；完整 Stop/Surface 生命周期仍不成立，见 §2.5 |
| HN-U01 历史设备结果 | 有 AYN UI 冒烟叙述，但记录仅摘录 9 个 generation、首轮身份缺失，没有逐轮 stop receipt/terminal 原始日志。支持有限 UI 启动观察，不足以证明完整 10 轮 Start/Cancel、allocator retry 或全部生命周期合同 |
| 当前设备 | 本次 adb 只读枚举确认 AYN Thor `9c2841a4` 在线；没有新跑设备 session，Swan 本次未连接 |

历史 device report 所说“十次 allocator claims 全成功”不符合进程初始化逻辑：allocator 一次成功后，后九次 context 重建不会再执行 SetupHooks。旧 call_once 缺陷也只在**首次 probe 返回失败**后暴露；正常十次重建不能验证这个负例。保留历史记录，新增更完整证据，不倒改旧结果。

## 2. 必须纳入下一版的缺陷

### 2.1 P1：提前取消的销毁分支绕过 control drain

[SessionCore::OwnerBody](../../../src/core/host_runtime/session_core.cpp) 第 129–151 行将 runtime 发布为 Ready，检测到早期取消后直接 `backend_.Destroy`，没有设置 `tearing_down_` 或等待 `in_flight_control_`。另一个 Stop 此时仍可在第 257–264 行取得 lease。

独立探针通过现有 BeforePublish gate 和 backend latch 确定执行：第一次 Stop 留下 CancelPending → owner 开始 Destroy → 第二次 Stop 进入 RequestCancel → owner 完成 Destroy → 第二次控制调用继续访问已销毁资源。输出：

```text
first=CancelPending terminal=1 exited=1
destroy_with_active_control=1 control_after_destroy=1
```

shared_ptr 只保证 runtime 容器存活，不能保证里面的 FEX context/thread 尚未被 Destroy。所有成功准备后的退出分支必须进入同一关闭准入和 drain 流程；不能单独给常规 Run 返回分支加保护。

### 2.2 P1：owner 异常越过线程入口，控制异常泄漏 lease

`session_core.cpp:112/162/224` 的 Prepare/Run/Destroy 没有 owner 边界 catch。JNI try/catch 位于调用线程，不能捕获新 `std::thread` 上的异常。本次分别令 fake Prepare/Run 抛 `std::runtime_error`，**两个子进程都 SIGABRT（Python returncode -6，shell 常表示 134）**。

`RequestStop` 第 264 行增加 lease，而第 272/277 行 backend 调用可能抛异常；减计数只在第 288 行正常路径执行。本次 RequestCancel throw 被调用方捕获后，owner 仍因未归还 lease 进入 drain timeout，`destroys=0 / next_start=0`。真实 backend 含 make_shared/vector/string/map 等分配，不是一个已声明 noexcept 的接口。

需要 owner 入口/清理的异常合同及 RAII control lease；JNI 最外层 catch 仍保留，但不能承担内部资源恢复。Destroy 失败必须保持正确 owner 和资源所有权，不伪造已退出。

### 2.3 P1：drain 超时后 owner 离开，迟到的控制调用无法完成回收

`session_core.cpp:206–218` 记录 `exited=false` 后退出 owner；后续控制调用返回时只减计数，没有继续 Destroy 的 owner。`SetTerminalLocked` 的 terminal-once 又将该结果固定下来。本次将控制调用保持到 drain 超时，之后释放，仍得到：

```text
terminal=1 exited=0 phase=Stopping
after_control_return_terminal=1 exited=0 destroys=0 next_start=0
```

超时期间封锁新执行是正确的；但不能将“待回收”状态永久写成已终结记录，并让拥有 owner-only Destroy 权限的线程退出。应区分 stop timeout notification 与真正 terminal，延后回收仍由合适 owner 完成。另需覆盖 Start 解锁 join 后重新检查 generation/phase、旧 WaitTerminal 不得 join 新 owner；这两点本次只做静态检查，未声称设备复现。

### 2.4 P1：Service 的十秒终态等待会截断真实游戏

[FexSessionService](../../../android/shadps4-app/app/src/main/kotlin/com/shadps4/android/service/FexSessionService.kt) 第 73–76 行在 Running 后立即 `nativeWaitTerminal(..., 10000)`；游戏超过十秒没有结束，会发布 Failed 并 `stopSelf(startId)`。第 138–143 行 onDestroy 又同步请求停止/等待终态。这是 smoke 的运行寿命限制，不是游戏 Stop 的超时预算。

下一版必须支持正常运行超过十分钟；长时间未结束不能自动记故障。onDestroy 当前还在 Android 主线程执行最长 1 秒 + 10 秒等待，应改为有明确 owner 的异步清理。不能只把十秒调成更大的游戏时长上限。

### 2.5 P1：WSI 的单次 100 ms timeout 没有形成有界退出

[AcquireNextImage](../../../src/video_core/renderer_vulkan/vk_swapchain.cpp) 第 146–158 行在 timeout/not-ready 时无限 retry；外层 [VideoOut PresentThread](../../../src/core/libraries/videoout/driver.cpp) 的 stop_token 不能在函数未返回时被检查。唯一实际 `RequestStop` 调用位于 [Presenter 析构](../../../src/video_core/renderer_vulkan/vk_presenter.cpp) 第 510 行，尚未接 session Stop、Surface detach 或 present-thread join 前的控制协议。

即使 flag 已设置，`Presenter::Present:860–862` 仍把 Acquire 的 false 当成必须 Recreate；重建里的 `device.waitIdle()` 无 timeout，且没有窗口 generation/ANativeWindow lease，不能证明退出有界或当前窗口有效。析构发 flag 也不能替代“先停止任务、join 渲染线程、再销毁资源”的顺序。

因此不接受 hn4-wsi 文档“即使没有 RequestStop，100 ms 也限制 teardown”的结论。需要带原因的 acquire 结果（acquired/recreate/timeout/cancelled/device-lost）、有代次 Surface handoff、真正接到控制器的 stop 和受控 fence 等待。Vulkan 对 swapchain 外部同步及 acquire 语义见 [官方接口](https://docs.vulkan.org/refpages/latest/refpages/source/vkAcquireNextImageKHR.html)。

### 2.6 P1：allocator 审计选错了当前链接分支，可靠性仍未收口

[allocator-provider.md](allocator-provider.md) 说 bionic 的 InitializeAllocator 是 no-op、不使用 small allocator。但构建记录包含 `ENABLE_FEX_ALLOCATOR=1` 的 JemallocLibs 和无该宏的 Dummy 两种编译；实际当前 JNI so 的 `InitializeAllocator` **调用 `rpmalloc_initialize_config`，SetupAllocatorHooks 也非空**。本次对现有产物的 [反汇编证据](2026-09-12-review/allocator-linked-implementation.txt) 已确认这一点；不能仅凭 `__BIONIC__` 判断选中了 Dummy。

retry guard 的修复是真实的，但只让 probe 超时可重试；进入 SetupHooks 后的 fatal trap 不会因此变为 recoverable。现有占位/sleep/释放也没有传递区域所有权。十次同进程正常启动既没触发 guard 的失败重试，也没验证 ART churn 下的冷启动。

provider 仍需按实际最终符号/宏重新评估，检查 main-owned 固定版本适配是否可行。Linux 缺少 public regions-taking SetupHooks 是事实，但不能把“没有这个公开重载”直接等同于“所有主仓适配方案都必须改 FEX 子仓”。保留 FEX 贡献约束；若最终确需 child 接口变化，交付具体缺口，不绕过约束。

### 2.7 P2：AAudio 当前会忽略短写和音量，并吞掉打开失败

[aaudio_audio_out.cpp](../../../src/core/libraries/audio/aaudio_audio_out.cpp) 第 68–85 行只处理负返回值；AAudio 可以成功写入少于 buffer_frames 的帧，剩余数据被丢弃。第 88–97 行 SetVolume 是 no-op；“上游已应用音量”的注释不成立，当前 AudioOut 上游复制 PCM 并把 volume 交给 backend。构造时 OpenStream 失败仍返回非空 PortBackend，使 `sceAudioOutOpen` 的失败检查失效，后续 Output 静默丢数据。

阻塞写模型可以保留，并不强制改成 callback；但需有界补齐 partial write、真实 gain/channel 转换、失败传播、断连重建和 stop。实际 `PortBackend::Output` 由 host port worker 消费 guest 拷贝的缓冲，不应按“guest 线程直接写 AAudio”的注释推导线程安全。返回帧数合同见 [AAudioStream_write](https://developer.android.com/ndk/reference/group/audio#aaudiostream_write)。这些源码还没有进入正式 native target，不能记 device audio PASS。

## 3. 完整加载方案中的四个误判

1. **不是只有最后一步 UNREACHABLE。** Execute 在主入口前就可能执行 guest `_malloc_init`/模块初始化；`Module::Start` 的 native pointer cast 没有 ARM 保护。guest argv/EntryParams/TLS 仍含 host 地址；LIB_FUNCTION/LIB_OBJ、pthread 和 callbacks 也未接正式 guest 边界。HleBoundary 枚举存在不证明 HLE-return/callback 已具备完整生产语义。
2. **没有直接 video include 不等于 headless 全链接成立。** `libs.cpp::InitHLELibs` 直接引用所有 RegisterLib；linker 又调用 SysModule、Kernel、VFS 和配置/进程对象。把全部 RegisterLib 加进 renderer-free target 与实际依赖矛盾。必须输出真实 link closure，不能用空注册函数或忽略 undefined symbols“解决”。
3. **Mac desktop 失败不等于 Mac 不能 NDK link。** 本次在这台 Mac 用 Darwin NDK 对当前 SessionCore/backend/status 完成 `aarch64-linux-android33` shared library **真实链接，带 `--no-undefined`**，得到 AArch64 ELF。该小 probe 不证明完整 host 已链接，但直接否定“本机最多只能 fsyntax-only”。完整 Android host link 的阻碍应按实际失败逐项定位；Linux CI 用于 Linux/desktop 回归，不能被当作停止 Android 开发的泛化理由。[NDK CMake 文档](https://developer.android.com/ndk/guides/cmake)。
4. **不能照搬 reference 的 maps gap-fill/MAP_FIXED 或全局移除 Execute。** 当前 hardened GuestAddressSpace 负责 reservation/pin/失效；需要 Orbis policy 和实际 backing 的统一所有权。可以分离 guest 逻辑可执行权限与 host 页保护，但不能去掉 guest Execute 后让 Query/发布/缓存失效失真。64 GiB 是当前后端策略/lookup 配置，改变它要验证高位地址完整性，不能只修改 USER_MIN 或简单将整个 PS4 地址空间平移。

## 4. 实际 PKG 的新发现：有本体，也有匹配更新

本次只读 PKG 头和 entry table 中原始 param.sfo，并流式计算 SHA-256，没有解包或执行。完整路径/hash 见 [pkg-set.json](2026-09-12-review/pkg-set.json)。

| 本地材料 | SFO | SHA-256 |
|---|---|---|
| 用户指定的顶层 `TMNT.Splintered.Fate_CUSA50828_v1.08.pkg` | CUSA50828 / APP_VER 01.08 / CATEGORY **gp** | `c3ad762919ca1e2bbf781d856b842379b08d0a5e6ef71fff122f0d8a859e0af1` |
| `TMNT/CUSA50828/` 下 1.00 本体 | CUSA50828 / APP_VER 01.00 / CATEGORY **gd** | `4ebea14adbabdd48c68875839f9e65800954a8a916247b1f353e8d76b414f2b6` |
| 同目录另一个 1.08 更新 | 与顶层文件相同 content ID、SFO，**全文件 SHA 也相同** | 同第一行 |

共同 Content ID 为 `UB0511-CUSA50828_00-0673996215904686`。顶层 1.08 是更新类别，不能默认它包含完整本体资源。本地已有匹配本体，无需把“等待用户补文件”设成当前阻塞。解压树/eboot/动态 imports/引擎和运行兼容性仍未核实，文件名中的 backport 标签也不是实际兼容证据。

当前 ContentImporter 已有 update/DLC overlay UI 和 `overlayStaging`，可复用；但它直接逐文件覆盖目标树，取消/失败可能留下半更新，未使用 contentId/sourceUri 做身份和安装记录校验。ParamSfoReader 也未输出 CATEGORY/APP_VER。下一版要完成**本体识别、更新匹配、可恢复安装、effective tree/hash 和 launch 传参**；不能重造一套导入 UI，更不能继续假定只有一个完整 PKG。

## 5. 下一版的统一交付标准

下一版以 **TMNT 本体＋1.08 更新在普通 ARM64 APK 内正常运行**为单一目标：UI 导入/更新 → 主仓 loader/Orbis runtime → FEX guest → 原生 Vulkan 画面 → 可操作游戏场景和实际音频 → 至少十分钟运行 → Stop → 同进程重新启动。首帧、guest entry、一个 HLE 或 `.so` link 是内部检查点，均不足以结束整版任务。

允许按依赖顺序分批提交、报告中间事实和使用定位 fixture；不需要每个内部点另请用户确认或另写小 spec。执行 AI 应在同一任务中补齐真实游戏所需的代码和依赖，再推到真机验证。仅在确切外部材料/设备/贡献边界阻塞时记录不能继续的部分，并继续独立工作。代码“可验”和设备“已验”明确区分，不能保证或虚报未知游戏兼容性。
