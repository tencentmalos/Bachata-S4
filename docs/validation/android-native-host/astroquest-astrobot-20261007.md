# AstroQuest 引入：ASTRO BOT 相关项（2026-10-07）

依据：[引入规划](../../specs/astroquest-reference-import-20261006.md) 的 H1、H4、D3。参考 `references/AstroQuest` v0.18 `9ff3e43`（GPL-2.0-or-later）。按用户要求“需要游戏的也先推进，测试后置”：本轮只有单元测试与编译验证，**未上设备、未跑游戏**。本机与设备都没有 ASTRO BOT（CUSA12392），所有游戏内验收由用户补测。

## 1. H1 时间步补丁包与补丁格式 sdk_version 3

**问题**：ASTRO BOT 每画一帧就把世界推进 1/60 秒，不管这一帧实际花了多久。所以模拟器上帧率低于 60 时，整局游戏按比例变慢（30 FPS 时是半速）。

**补丁包**：`guest/games/CUSA12392/time_step.cpp`，配方在 `01.00/`、`01.04/`。
- 挂在 eboot 调用 `sceGnmSubmitDone`（NID `yvZ73uQUqrk`）的 PLT 入口上。每帧用 `shad_sdk_clock_ns` 测量实际帧间隔，算出步长，写回游戏的三个全局量：double 帧率、float 秒数、u64 微秒。
- 步长算法取自 AQ `known_title.cpp` 的 `TimeStep`：
  - 帧间隔的平均值每帧向新值靠拢 15%。
  - 累计的时间差限制在 ±0.1 s，每帧补回其中 10%。
  - 步长限制在 1/60 到 1/20 秒之间。
  - 超过 0.25 s 的帧当作卡顿，不计入。
  - 平均值离 1/60 不到 0.4 ms 且累计差不到 4 ms 时，直接用 1/60（等于游戏原本的值）。
- 计数器 1 是步长（µs），2 是帧间隔（µs）；每 600 帧写一条日志。
- **未做**：AQ 中的固定分辨率（分辨率控制对象的三个字段）。

**格式 sdk_version 3**（`guest_patch_format.*`，[说明](../../guest-function-patches.md)）：
- `module_signatures`：按固定偏移比对的字节签名，取代整文件 SHA256。
  - 原因：AQ 没有给出这两个 eboot 的文件哈希。
  - 规则：每项最多 64 字节、最多 64 项，合计至少 32 字节。只比对，不搜索。
  - 这两个包各有 6 项签名，都取自 AQ `known_title_builds.h`：重新居中代码、60.0、1/60、16666，以及宽度表和高度表。
- `"import": "<NID>"` hook：安装时用 `Module::FindImportPltEntry` 找到入口，再以该入口的 6 字节作为改写前的校验字节。查找要求：
  - 模块里只有一个该 NID 的 `JUMP_SLOT`；
  - 只有一个完整的 PLT 行跳经这个 GOT 槽，即 `ff 25 disp32` 后跟 `cc` 填充（`loader/plt_import.h` 的 `FindPltEntries`）。
- 桌面（`guest_patch_desktop.cpp`）和 Android（`guest_runtime.cpp`）都在包没有 SHA 时改用签名核对；Big Picture 的匹配显示也跳过 SHA。
- `tools/guest-functions/build.py`：有签名或 import hook 时输出 sdk_version 3，并校验签名的证据说明与长度。

**验证**：
- `tests/test_guest_patch_format.cpp`（gtest）6/6：
  - 签名匹配、改一个字节即不匹配、签名越界；
  - 签名少于 32 字节、版本 2 带签名、既无 SHA 也无签名，都被拒绝；
  - import hook 解析出入口与校验字节，模块没有该入口时被拒绝；
  - import hook 不能同时带 offset，也不能用于 site；
  - PLT 行识别：跳经别的槽、填充被改的行都不算；
  - 两个 ASTRO BOT 包能够加载。
- 两个包构建通过（1.00 `cae8ebaa…`，1.04 `d06c2397…`）。桌面 exe 与 Android host 编译通过。
- **未做**：游戏内的速度和过场音画同步、`guest_patch enable/disable` 对比、装到别的版本时被签名拒绝的实测。

## 2. H4 Android libSceFiber

**现状**：fiber 原先只在 x86_64 桌面构建中编译，Android 上 15 个导入都没有实现。

**实现**：`src/core/host_runtime/guest_fiber.h`（只有头文件）。
- **切换原理**：guest 的 HLE 调用以整组寄存器到达宿主，返回时宿主写回的寄存器就是 guest 接着用的（FEX 适配层写回全部通用寄存器、rip、MXCSR：`fex_context.cpp` 的 `ApplyRegistersToCpuState`）。所以切换 fiber 时：
  - 保存离开的上下文的 rbx、rsp、rbp、r12–r15 和 MXCSR，载入要进入的上下文的这些寄存器；
  - veneer 末尾的 `ret` 从新栈弹出返回地址，就回到了目标上下文；
  - 第一次运行的 fiber：在它的栈顶依次放入口地址和一个陷阱返回地址，`ret` 就像 call 一样进入入口，rdi、rsi 是两个参数。
- **与 AQ（`fiber_fex.cpp`）和桌面的差别**：
  - guest 内存只通过 `GuestFiberMemory` 访问，生产实现是 `ReadData`、`WriteData`，以及 pin 后用 `atomic_ref` 做 CAS。fiber 的 state 字段用 CAS 修改：离开的 fiber 由 Run 变为 Idle 也是 CAS，这样另一个线程看到 Idle 时，也一定能看到之前保存的寄存器。
  - 桌面放在 TCB 里的线程上下文、放在 fiber 里的已保存上下文指针，改为放在会话内的两张表里，分别以 guest 线程（`HleScope` 的 id 与 generation）和 fiber 地址为键。锁只保护这两张表，读写 guest 内存都在锁外进行：访问 guest 内存可能要等代码发布，而代码发布要等其他线程的 HLE 调用返回。
  - 跨线程恢复：线程 A 挂起的 fiber 可以在线程 B 上恢复，之后它 ReturnToThread 回到的是 B 的 sceFiberRun。
  - SetFpuRegs（SDK ≥ 3.50 的 fiber）：新 fiber 的 MXCSR 置为 0x9fc0。x87 控制字不在 HLE 寄存器帧里，保持线程原值，这一点与桌面不同。
  - 以下情况会以具名故障结束会话（桌面这里是断言）：栈签名被覆盖（栈溢出）；fiber 入口函数返回，此时落到 `FiberEntryReturned` 陷阱 veneer。
  - `GetInfo` 的剩余栈空间按从栈底起连续未被改写的字数计算。桌面的这个循环遇到改写过的字时不前进，会死循环。
  - fiber 对象的布局与桌面相同（对游戏是不透明的 256 字节存储）。fiber 栈第一次进入前会通知 GPU 回放（`NoteGuestStack`）。
- **接入**：
  - 15 个 NID 只按 `#libSceFiber#1#libSceFiber#Function` 准入；
  - provider 为 `libSceFiber`（0x10000026）；游戏自带 libSceFiber 模块时让给它，不发布；
  - `PublishVeneer` 从 `Bind` 中抽出，新增 `InternalVeneer`，用于没有导入名的内部 HLE 入口。

**验证**：
- `tests/host_runtime/guest_fiber_tests.cpp`：用一段缓冲模拟 guest 内存，桌面 clang-cl 102/0，已加入 CMake（HOST_BUILD_PROBES），NDK 编译通过。覆盖：
  - Initialize 的栈参数、名字截断、各类错误；
  - Run 进入入口；fiber 内的 GetSelf 与线程帧指针；
  - Switch 时挂起与 Idle 交接、恢复时写回参数；ReturnToThread；
  - 另一个线程恢复挂起的 fiber；
  - 借用线程栈的 fiber 不可恢复，也不改写调用者的返回槽；
  - 栈签名被覆盖时报故障；
  - 栈用量检查的填充、剩余空间、AttachContextAndRun；Finalize。
- 负对照：把 Run→Idle 的交接改成空操作后，25 项失败。
- Android host 编译通过。
- **未做（需真机与 ASTRO BOT）**：真实 FEX 下的切换，包括规划中的风险“换栈后 FEX 的 call-ret 栈预测失败应退回 dispatcher”。我们库里的游戏都不导入 libSceFiber。

## 3. D3 相机节拍与 SocialScreen

**相机**（`src/core/host_runtime/guest_camera.h`）：
- **节拍**（AQ `camera.cpp:57-79`）：
  - 读取要求下一帧（readMode 位 0），或者是开始后的第一帧时，等到下一帧时间（每 1/60 s 一帧）。
  - 还要等游戏再呈现一次（`guest_presents`），最多等 200 ms：游戏一直不呈现时仍然出帧。
  - 等待时不持锁、不持 pin，并可被 Stop 取消；同一时刻相机被 Stop 或 Close 时立即返回。
  - 不要求下一帧的读取直接返回最新一帧。原先每次读取都会加帧号。
  - 原因：游戏在一个线程上把相机读入三格环形缓冲，主线程用最新的完整一格。相机若跑在比真机慢的游戏前面，就会把正在用的那一格清掉。
- **新增 4 个函数**：
  - `SetExposureGain`、`GetExposureGain`：按传感器保存，值回显到每帧元数据的 `exposureGain`。游戏靠它判断一帧是按什么曝光拍的。默认值取自桌面（20/100）。桌面 `sceCameraSetExposureGain` 在参数非空时返回 PARAM，看起来是条件写反了，本仓按非空才有效处理。
  - `GetCalibrationData`：虚拟相机返回空的校正网格。
  - `SetAutoWhiteBalance`：接受，画面本来就是黑的。
- 写回 guest 内存前先释放相机锁。

**SocialScreen**（`src/core/host_runtime/guest_social_screen.h`，7 个函数，provider 0x10000027）：
- 记录模式和独立模式状态，配合 C4 的社交屏 VideoOut 端口：独立模式下，游戏把电视画面 flip 到该端口，flip 完成，但没有地方显示。
- AQ 这 7 个函数全部返回成功。本机找不到这个库的固件错误码，所以调用顺序不对时（未初始化、重复初始化、重复打开、未打开就关、空参数）做具名拒绝并结束会话，不编错误码。
- 参数结构（s32 mode + 28 字节保留）是 AQ 对 ASTRO BOT 调用的解读，未与固件核对。
- 每次调用写一条 `Lib_VideoOut` 日志，记录当前状态。

**验证**：
- `tests/host_runtime/guest_camera_tests.cpp` 更新：
  - 注入可控的呈现计数与时钟；
  - 等待帧时间；游戏不呈现时 200 ms 后出帧；游戏呈现后立即出帧；
  - 取消立即返回；另一线程 Stop 时等待结束；
  - 曝光增益回显、各类参数错误、校准数据、白平衡。
- 这个测试原先没有 CMake 目标，本轮已补上，NDK 编译通过；设备上未运行。
- Android host 编译通过。
- **未做**：ASTRO BOT 的相机读取与社交屏流程；这两个族是否被它导入，以 AQ 的实现为依据，本仓没有它的导入表。

## 4. E3 NGS2：走固件模块（LLE）

NGS2 是 PS4 的通用音频引擎，不限于 PSVR；我们库里目前只有 Tetris Effect（CUSA13427）导入它，共 13 个函数。用户提供了固件 `libSceNgs2.sprx`（解密后的 ELF，372,112 字节，SHA256 `fbe55d7da893743a6267798acff1a2093ab7ac5f1606b48f8f3d3c5a8d7e74f6`）。经对比，用户决定按 LLE 推进。

**选型依据**

| | LLE（固件模块） | HLE（移植 AQ 引擎） |
|---|---|---|
| 正确性 | 固件原样实现，函数和效果器齐全 | AQ 中 Tetris 用到的 ParseWaveformData、CalcWaveformBlock、PanInit、PanGetVolumeMatrix 4 个仍是占位；EQ、几何、语音回调、UserFx2、Stream 等也是空的 |
| Android 接入 | 58 个导入只差 3 个 libc 函数，本轮已补 | AQ 直接解引用 guest 指针；在宿主锁内调用 guest 回调；`SystemLock` 持宿主锁返回 guest；`VoiceGetState`/`SystemRender` 的写入长度由 guest 决定、不设上限。约 3500 行需要按 Android 的内存和回调规则重写 |
| 性能 | 混音 DSP 是 x86 AVX 代码，Android 上经 FEX 翻译执行（未测）；ATRAC9 解码走原生的 AJM HLE | 原生 ARM |
| 依赖 | 需要用户提供固件文件 | 无 |

HLE 只作为没有固件文件时的退路，本轮不做。AQ 引擎的完整分析（对象模型、guest 内存访问、回调、锁、渲染流程、占位清单、4 个占位函数的 ABI）已整理，需要时再展开。

**模块的导入**（`tools/ps4-guest-code imports`）：
- libc 约 40 个：数学、字符串、`fopen/fread/fseek/fclose`、`_Stoul`、`_Stod`、`vsprintf_s`、`printf_s`、对象 `Need_sceLibcInternal`；
- libkernel：pthread mutex 7 个、`usleep`、`pthread_self/equal`、`sceKernelGetTscFrequency`、`sceKernelGetCompiledSdkVersion`；
- AJM 10 个：Initialize/Finalize、ModuleRegister/Unregister、InstanceCreate/Destroy、两个 batch 构建函数、BatchStartBuffer、BatchWait。

Android 上原来只缺 `_Stod`、`vsprintf_s`、`printf_s`。其余都已准入：AJM 15 个 NID 齐全；`Need_sceLibcInternal` 已映射到 libc 的 `Need_sceLibc`。这 3 个只在报错和报告路径上调用：
- `printf_s` 只在 “Failed to AJM Decode” 与 “Failed to initialize AJM Decoder” 两处用到；
- `vsprintf_s` 用于格式化报告消息，随后调用已注册的处理函数；
- `_Stod` 的包装函数没有直接调用者。

**改动**
- `guest_libc_policy.h`：准入 `vsprintf_s`（`+qitMEbkSWk`）、`printf_s`（`w1NxRBQqfmQ`）、`_Stod`（`c41UEHVtiEA`）。准入后由游戏自带的 libc 提供实现，不给宿主函数。Tetris 的 `sce_module/libc.prx` 里，这 3 个 NID 都带 `#E#A` 后缀，与策略表里已有的 `_Stoul` 相同，是 libc 自身的导出；libc 从 libkernel 导入的符号是 `#A#B`。
- `guest_runtime.cpp` 本地库表加 `libSceNgs2`：游戏确实导入 libSceNgs2、`sys_modules` 中有该文件、且游戏自带模块不提供它时才加载，并核对它的导出库和模块名。
- 桌面原本就会从 `sys_modules` 加载这个文件（`sysmodule_internal.cpp` 的 `ModulesToLoad`）。所以文件放好后，桌面上导入 NGS2 的游戏都改走固件实现，不再是静音的占位 HLE。

**文件放置**（固件文件不进仓库；本仓 `firmware/` 未被 git 忽略，所以未放在仓库内）：
- `D:\workspace\shadps4-win-bb\user\sys_modules`
- `%APPDATA%\shadPS4\sys_modules`
- 仓库外备份 `D:\workspace\ps4-firmware\sys_modules`，附 README 记录 SHA 与来源
- Android 需要推到 `files/host/sys_modules`，本轮未推送。

**验证**：Android host 编译通过（HOST_LINK_PASS）。**未运行游戏**：桌面与 Thor 上 Tetris 是否出声、FEX 下 NGS2 渲染的耗时，都留待用户补测。桌面上的 Tetris 是 `D:\game\ps4\zar\CUSA13427.zar`。

## 5. D2 A 档：头显控制器当 DS4 位姿

PSVR 游戏用 PS Camera 追踪 DualShock 4 灯条，得到手柄的空间位姿；ASTRO BOT 在就坐画面等手柄进入追踪状态，游戏中也画出手柄、从手柄位置发射道具。设备上没有 PS Camera，改用头显控制器的位姿代替。

- **设置**：每游戏 `input.xr_ds4_pose`（Off / Right Controller / Both Controllers，默认 Off，重启生效），JNI `nativeSetXrDs4Pose` → `Core::HostRuntime::SetDs4PoseSource`（定义在 host 库的 `vr_tracker.cpp`，JNI 与 host 是两个 DSO，不用头文件内联变量）。
- **`src/core/host_runtime/ds4_placement.h`**（纯计算，无平台依赖）：
  - Right：右手 grip 加偏移到手柄中心（左 8 cm、前 2 cm，按 DS4 宽度推断），速度换算到该点（v + ω×r）。
  - Both：两只控制器并排当手柄握，取中点；左→右连线为手柄横轴（含横滚），两手前向的平均值去掉横轴分量为前向；中点在两个 grip 的前上方（前 3.5 cm、上 1.5 cm，同 AQ 的掌心规则）。两手间距不在 5–32 cm 时不算握着手柄。
  - 朝向：游戏 `sceVrTrackerRecalibrate(DUALSHOCK4)` 或系统视角重置时，把当前控制器朝向记为“水平、朝头部当前航向”；修正量右乘在控制器自身坐标系，之后绕任何轴的转动都原样传给手柄。未校准前补 40° 俯仰（握持时 grip 前向朝前下，推断值）。
  - 看不见时（400 ms 内无有效样本）：锚点跟随头部位置（不跟随转动，0.25 s 平滑）；位置按“玩家自设偏移 > 最后看到时相对锚点的偏移 > 标准位置 (0, −0.17, −0.50)”放置（AQ `vr_runtime.cpp:878-946`）；显示位置平滑过渡（看到时 35 ms、看不到时 0.3 s）。玩家自设偏移的接口已留（`SetOwnOffset`），调整与持久化属于 H5。
- **VrTracker**：OpenXR 会话中、开关打开、且是第一个登记的 DS4（玩家的）时，`GetResult` 写 `pad_info.device_pose` 与速度，状态 TRACKING（校准窗口内 CALIBRATING），看到时质量 FULL、看不到时 PARTIAL；其余 DS4 仍为 NOT_TRACKING。`sceVrTrackerTerm` 清空放置状态。
- **未做**：`scePadRead` 的姿态与角速度仍为单位四元数和 0（B 档随 IMU 透传一起做）；SBS 模式（无 OpenXR）不放置 DS4。

验证：`tests/host_runtime/ds4_placement_tests.cpp` 26/0（Windows clang 与 Thor ARM64 各一次）；负对照把校准改回左乘、把 grip 到中心的偏移方向取反，2 项失败。`guest_vr_abi_tests` 241/0、`guest_vr_sensor_tests` 102/0；Kotlin `XrDs4PoseTest` 2/0 等设置测试共 12/0。Thor APK `ee7ebe65`（host `d542157e`）已安装；没有头显与 ASTRO BOT，游戏内未验收。

## 6. WP-I 无帧时自动转储

游戏 20 秒没有新的呈现帧时，把每个 guest 线程在做什么写进日志，供卡死问题事后定位；不需要先 attach 调试器、不暂停任何线程。

- **记录**（`src/core/host_runtime/guest_watchdog.h`）：每个运行 guest 代码的 host 线程第一次调用 HLE 时从 1024 个槽位中认领一个（线程退出时归还）。`FunctionAdapter::Invoke` 进入时记下操作号、guest RSP/RBP/RIP、guest 线程号并递增调用计数，返回时恢复外层的值（回调嵌套时显示最内层调用）。单写者 relaxed 存储，无锁、无分配。
- **会话线程** `shadPS4:Watchdog`（`GuestRuntime::Run` 启动，`~Impl` 最先停止并 join）：每 250 ms 读 `guest_presents`；20 秒不变时转储一次，有新帧后才再次触发；会话取消时退出，不在停止过程中误报。
- **转储**：两次采样相隔 500 ms，按调用计数是否变化分为 `progressing` / `blocked_in_hle` / `in_guest_code`；对卡在 HLE 里的线程，经带检查的读取打印调用者和 RBP 链上至多 15 层返回地址，并换算为“模块+偏移”。日志行前缀 `GUEST_WATCHDOG`。
- **DebugBus** `guest_watchdog status | dump | on | off`（默认开；`dump` 立即转储一次）。
- **Thor 实测**（Tetris SBS，APK `090408e8`，host `d860600a`；名称修正后的最终包 APK `abaae7f5`，host `6ba106f0` 复测输出“函数名 + NID”）：手动 `guest_watchdog dump` 输出 66 个线程；主线程 Guest-1 为 progressing（500 ms 内约 10 万次 HLE 调用），3 个线程阻塞在同一 HLE 调用内、返回链落在 `libSceFios2.prx+0x3a8d4/0x38019/0x3b35d`（Fios2 I/O 线程），多个 eboot worker 阻塞在 `eboot.bin+0x103bbfc` 起的同一条链上。随后把操作号显示改为“函数名 + NID”。20 秒无帧的自动触发与手动转储共用同一转储路径，未在设备上构造真实卡死验证。
- **不做**：不像 AQ 那样用实时信号采样原生栈（Android 上与 FEX、ART 的信号处理冲突）；在 guest 代码中忙等的线程只报告状态，不给 PC（异步读取 JIT 中线程的 RIP 不可靠）。

## 7. H5 手部追踪推算 DS4 位姿（D2 B 档的位置部分）

参考 azahar 的接入方式（启用扩展、会话后建 Foundation `XrHandJointTracker`、Manifest 与运行时权限）。

- **runtime**：OpenXR 支持时启用 `XR_EXT_hand_tracking`（Swan 扩展清单中有），会话建好后创建 `XrHandJointTracker`；每帧在焦点内定位左右掌心关节（`XR_HAND_JOINT_PALM_EXT`，位置与朝向都被追踪才算），发布到 `HardwareFrame.palms`。CMake 补编 `XrHandJointTracker.cpp`。
- **权限**：Manifest 声明 `com.picovr.permission.HAND_TRACKING` / `com.oculus.permission.HAND_TRACKING`；`OpenXrActivity` 与眼动权限一起申请，每种只问一次，拒绝时 DS4 停在最后看到的位置。
- **`Ds4Placement` 新来源 Hands**（设置项 “Hand Tracking”，即 `input.xr_ds4_pose=hands`）：位置取双掌中点，往前 3.5 cm、往上 1.5 cm（同 AQ）；两手间距须在 5–32 cm 内，且水平分量不少于间距的 60%（同 AQ），否则不算握着手柄。朝向：双掌连线为横轴（航向与横滚），双手手背（掌心关节 +Y）平均为上方（俯仰），不加默认俯仰；校准同 A 档。掌心关节不带速度，用相邻位置差分并以 0.4 平滑（同 AQ）。AQ 的朝向来自 DS4 IMU、手部只校正航向；我们尚未透传手柄 IMU，朝向暂由手部给出。
- **PS 组合键调整自设位置**（`OrbisPadAdapter::Ds4PlaceChordLocked`，仅在 DS4 位姿来源开启时）：按住 PS，十字键上下左右、L1/R1 前后各移 2 cm（自设位置不存在时从当前假定位置起算），△ 在自设位置与看到/标准位置间切换；范围同 AQ（左右 ±0.4 m、上下 −0.7～+0.3 m、前方 0.15～0.9 m）。这些键在按住 PS 期间及之后直到松开都不传给游戏；PS 本身照常传（与 AQ 不同，AQ 推迟到松开）。自设位置存 `<user>/vr_controller.json`，会话 Reset 保留。

验证：`ds4_placement_tests` 42/0（含 Hands 位置/俯仰/不水平/不混用来源/速度，以及自设位置移动、限幅、切换、跨 Reset 保留）。Swan 上的实际手部追踪、权限弹窗与 PS 组合键未验证。

## 8. SBS 模式叠加层两眼各一份

PSVR 游戏在 SBS 窗口下，原来 ImGui 叠加层与 Compose 提示按整屏画一次：状态栏横跨两眼、System RAM 提示只在左眼。现在与 XR 一样两眼都能看到：

- **ImGui**（`ImGui::Core::SetEyeSplit`，present 线程按帧的 `xr_stereo` 设定）：dockspace（游戏画面）仍占整屏；之后主视口工作区宽度减半，状态层按半屏宽度排版，对话框、提示居中到左眼。`Render` 在同一渲染范围内把除游戏窗口与 dockspace 外的绘制列表再画一次，显示原点左移半屏，叠加层落到右眼同一位置（`VulkanRenderer::RenderDrawData` 允许每帧多次调用）。触摸只命中左眼那份。
- **Compose**（`SessionScreen` 新参数 `stereo`，由 `BachataNavHost` 按“PSVR 且 2D”决定，运行中重建 Activity 时重新查询）：System RAM 提示与顶部通知胶囊在 SBS 时左右各放一份。

Thor 实测（Tetris SBS，APK `75a30887`，host `f79e477a`）：Summary 状态层、System RAM 提示、展开的 Detail 面板均在两眼同位置显示，宽度在单眼内。
## 9. 剩余

E4/H6 Android 麦克风、H2 governor（需要扩展补丁 SDK）、H3、DS4 IMU 透传（H5 的朝向部分）、C2、C7、WP-G 桌面接入。
