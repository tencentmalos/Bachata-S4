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

## 5. 剩余

E4/H6 Android 麦克风、H2 governor（需要扩展补丁 SDK）、H3、H5（即 D2 的 B 档）、C2、C7、WP-I、WP-G 桌面接入。
