# PSVR HLE 结构体与 ABI 复核（2026-09-18）

这次复核针对 Beat Saber CUSA12878 1.00 的解密 eboot，以及本地 PS4 11.00 固件中的 `libSceHmd.sprx` 和 `libSceVrTracker.sprx`。固件文件 SHA-256 为：

```text
libSceHmd.sprx       c5005afcb8bddeaa6de23cd581eabe080087bb45b36c8a9dc89cf6af227f0e71
libSceVrTracker.sprx 3a8718d1fa72768d173bfc6d6211934b573c1cc94d3543124cc2e0bc8b050163
Beat Saber eboot     71278f9c85ef93a9b2e8703299d6875020eb4d8f7b821bf98eb36ec73340592b
```

固件的反编译数据库被另一个本地 IDA 会话占用，本轮没有关闭它，而是使用 ELF 符号大小、导出函数机器码和游戏调用点交叉校验。下面的偏移均是 `libSceHmd.sprx` 或 eboot 的模块相对地址；它们是证据索引，不是运行时命中计数。

## 已标定的结构

`sceHmdReprojectionInitialize` 不是无参数函数。11.00 导出在 `0x14b30` 处读取三个 ABI 参数：

```text
rdi -> 参数对象
esi -> mode，允许 0..2
rdx -> 保留参数，必须为 NULL
```

固件读取参数对象的 `0x00、0x08、0x20、0x24、0x28、0x2c、0x30`，并且要求 `0x00/0x08` 非空、`0x20<=6、0x24<=7、0x28..0x30` 为零。Beat Saber 在 eboot `0xd70b78` 准备一个 `0x38` 字节对象（其中 `0x10=0x100、0x18=0x38、0x20..0x24` 来自 `0x0000000500000005`），以 `mode=2、rdx=NULL` 调用该 NID。由于 `0x00/0x08/0x10/0x18/0x34` 的语义还没有被固件导出名证明，代码中使用显式 `opaque/field` 字段，避免错误地把它们当作 host 指针。

`sceVrTracker` 的公开参数和结果记录在当前 HLE 中已经有完整字段，但过去没有 ABI 尺寸护栏。本轮补充了固定布局检查：

| 记录 | 固件/调用证据 | 尺寸 |
| --- | --- | ---: |
| `OrbisVrTrackerQueryMemoryParam` | 查询函数清零并读 0x40 字节 | `0x40` |
| `OrbisVrTrackerQueryMemoryResult` | 输出初始化为 `size=0x40` | `0x40` |
| `OrbisVrTrackerInitParam` | eboot 的初始化记录 | `0x80` |
| `OrbisVrTrackerGetResultParam` | `sceVrTrackerGetResult` 的本地输入副本 | `0x38` |
| `OrbisVrTrackerResultData` | 现有 HLE 的结果族最大记录和 LED 数组；固件分支还需动态输出快照复核 | `0x5f0` |

`PoseData` 为 `0x40`，`HmdInfo` 从结果记录 `0x80` 开始，`user_frame_number` 位于 `0x1c0`。这些值已经写成 `static_assert`，防止 ARM64 编译器或后续字段整理改变 guest ABI。

## 尚未足够证实的结构

`sceHmdReprojectionSetDisplayBuffers` 的真实 ABI 是四个参数：`(video_handle,
buffer_index0, buffer_index1, reserved)`。固件把第四个参数作为保留参数，非空时拒绝；两个 Beat Saber 调用点（eboot `0xd70333`、`0xd756f7`）传入 `2,3,NULL`，这与两个 VideoOut buffer slot 一致。固件随后通过 VideoOut 的 buffer 属性/地址查询得到左右显示地址，并不是一个“第五个左右眼地址结构体”。句柄、slot 的所有权和 Android presenter 的映射仍未实现，所以入口继续保持具名未完成状态。

`sceHmdReprojectionStartMultilayer`（上述 SHA 的 `libSceHmd.sprx+0x17a50`，导出长度 `0x17f`）使用六个整数类参数。当前命名修正为 `(layers, layer_count, submission, shared_layer_data, opaque_arg4, reserved)`，层数 1..3，第六参数必须为 NULL。第四参数不能继续称为已证实的 `pose`，第五参数也不能直接称为已证实的 `frame_id`。

复制 helper `libSceHmd.sprx+0x17950` 以 `0xa8` 为步长；外层导出只检查**第 0 层**的 `0x7c..0x9c`，然后 helper 重写内部各层的这一区域。此前“检查每层保留字”的描述过度，现予纠正。`RCX` 被写到 `kind<2` 的内部层 `+0x80`；`kind>=2` 时写零。尾部 `+0xa0` 独立复制，并由内层验证为零。

已加入 `OrbisHmdReprojectionLayer`（`0xa8`）与 `OrbisHmdReprojectionSubmission`（`0x50`）的部分 wire 布局和 `sizeof/offsetof` 断言。地址字段全部用 `u64` 保留，未知区域用 `opaque` 命名。这只证明固定布局，不表示嵌套 GPU 对象可安全传给 host。

| 固件读取的字段 | 已确认的校验或使用 | 尚未确认 |
| --- | --- | --- |
| layer `+0x00/+0x08/+0x20` | 非空；前两个根交给格式查询 helper | texture/viewport 的完整类型与资源生命周期 |
| layer `+0x10/+0x18` | kind 为 1/3 时非空，为 0/2 时必须为空 | 具体左右眼/深度语义 |
| layer `+0x70` | kind 为 2/3 时为空，否则可选嵌套记录 | 嵌套对象完整语义 |
| layer `+0x78` | kind 0..3；第一层只允许 0/1 | 模式枚举命名 |
| submission `+0x00` | 非空且 8 字节对齐 | 指向的完整对象 |
| submission `+0x08/+0x18` | 分别为 2000..7000、0..1 | selector 的正式名称 |
| submission `+0x20` | 位掩码限制；`+0x28..+0x48` 必须为零 | flags 的枚举含义 |

Beat Saber eboot SHA 如上；调用点 `+0xd6b48b` 明确传 `RDI=rsp+0x280`、`RDX=rsp+0x180`、`RCX=rsp+0x20`、`R9=0`。该分支层数为 2，`R8` 来自栈上的保存值，调用后将其低 32 位加一写回，所以“序号”仍是候选语义。此证据来自静态机器码，不是运行时命中。multilayer 的 Android 入口继续 `refused`，直到嵌套对象和 SBS buffer 所有权恢复。

### PlayAreaWarning 的完整外层记录

上述 SHA 的 `libSceVrTracker.sprx+0xb080`（导出长度 `0x227`）先检查初始化，再检查非空、`size=0x40` 及保留区 `[0x04,0x0f]`、`[0x11,0x1f]`、`[0x21,0x23]`、`[0x2c,0x3f]` 全零。`+0xb310` 随后把缓存 `+0x188e64` 的 **0x40 字节**复制到输出。两个状态字节 `+0x10/+0x20` 与距离字段 `+0x24/+0x28` 的位置符合现有 HLE；距离的单位和缓存更新来源还未动态验证。

代码将状态字节定义为 `u8`，避免复制未初始化 guest 输出时制造无效 C++ `bool` 对象。桌面与 Android 共用 `PlayAreaWarningInfoNoProvider`：未初始化返回 `NOT_INIT`，非法外层记录返回 `ARGUMENT_INVALID`；有效记录返回 `NOT_SUPPORTED` 且不改任何输出字节。后者是明确的模拟器无 provider 策略，**不是固件里的“相机未连接”分支**。固件在有效初始化后直接返回缓存；当前没有缓存来源，因此不保留原桌面成功但不写输出的空实现。

Android adapter 保持上述生命周期优先级，复制完整 guest 记录后调用共用实现，并校验完整输出范围可写。新准入仅覆盖 `#libSceVrTracker#1#libSceVrTracker#Function`；同时排除了 HMD/Tracker/Camera/Move NID 落入通用 libkernel/Posix 准入的可能。

机器可读证据：[psvr-remaining-abi-evidence-20260918.json](validation/psvr-sbs-20260918/psvr-remaining-abi-evidence-20260918.json)。新 `guest_vr_abi_tests` 在 Thor 上得到 `133 checks / 0 failures`，覆盖初始化优先级、NULL/size、每个保留字节的独立扰动、输出字段任意旧值、所有失败路径不写回；这些是 ABI 策略测试，不是 HMD 设备或游戏运行验收。

同样，`sceHmdReprojectionStart`（固件 `0x161e0`）读取一个至少 `0x80` 字节的启动记录：`0x00/0x08/0x10/0x38` 是非空根指针，`0x40` 是受限数值，`0x50` 只能为 0 或 1，`0x58..0x78` 是保留字段。现有无参数成功 stub 已改为接收原始 guest 记录并返回具名参数错误，避免在结构体尚未恢复时错误推进生命周期。

### Camera / Move 的 Android 边界

本批把 Beat Saber 实际导入的 Camera 8 个 NID 和 Move 7 个 NID 从 refused 迁到
受检 guest adapter，但这不是 provider 实现。Camera 的 `OpenParameter`、
`StartParameter`、`VideoSyncParameter` 都按 0x10 字节记录复制；`Config` 含两个
带 `pBaseOption` 的扩展槽，扩展指针非空时拒绝，避免 guest 地址进入桌面实现。
`FrameData` 同时包含左右设备的四级 frame 指针表、size/status 数组、Meta 和另一组
garlic 指针表，不能把它当成普通标量输出。Android adapter 先复制完整记录，写回两个
`status=-1`，然后返回 `ORBIS_CAMERA_ERROR_NOT_CONNECTED`；它不会调用桌面的
`sceCameraOpen`，因为桌面路径会把 `SceCameraGpuGarlicPool` 映射到固定 PS4 地址
`0xfd0000000`。

Move 的 `OrbisMoveDeviceInfo` 是半径加三轴加速度偏移的 0x10 字节记录；
`OrbisMoveData` 是加速度/陀螺仪、按键/扳机、扩展口、时间戳、样本数和温度的连续
输出记录。adapter 只在 guest 输出范围通过检查且桌面函数返回成功时复制这些记录。
当前桌面实现明确返回 `ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED`，所以 Android
启动不会伪造 Move 手柄、灯球或 6DoF 输入；`sceMoveOpen` 保留桌面的句柄生命周期，
该句柄不代表已连接控制器。

`sceHmdReprojectionSetOutputMinColor` 没有 guest 指针，三个 `float` 按 SysV
`xmm0..xmm2` 传参；adapter 直接读取每个 XMM 的低 32 位，再调用桌面已有的颜色
下限 stub。它可以进入 `runtime_bound`，但不会使 reprojection、HMD 或 SBS provider
变为可用。

## Android SBS 虚拟 HMD 边界

为了让 Beat Saber 在普通 Android 设备上先走过“没有 HMD 就取消”的分支，增加了一个与
真实 PSVR provider 分离的虚拟层：`GuestVrSensor` 接收 Android `TYPE_GYROSCOPE` 样本，
按事件时间戳积分并归一化四元数；`NativePadBridge` session 建立/失焦/结束负责启停，因而
不依赖是否真的连接了手柄。`sceHmdSetupDialogGetResult` 只有在 SBS session 启用时返回
成功；desktop session 仍返回 `USER_CANCELED`，不会把普通桌面运行伪装成有 HMD。

虚拟层使用已有 wire 记录，不改变固件 ABI：HMD 设备信息返回 1920×1080 和 90/120 Hz
能力，FOV 使用固定 PSVR 兼容值，VrTracker HMD 结果把四元数、角速度、时间戳和左右眼
IPD 写入 `OrbisVrTrackerResultData`。有效 play-area 记录只返回有限的虚拟边界值；没有
camera、Move、手柄或 6DoF 数据来源。`sceHmdReprojectionStartMultilayer` 仍因嵌套对象
和输出所有权未恢复而拒绝，`SetDisplayBuffers` 仍不把 guest 地址直接交给 host。

这是一层 Android SBS 开发期的 HMD 伪装和头动输入，不是对固件 provider 的结构体推断，也
不是 OpenXR 实现。设备单测 `GUEST_VR_SENSOR z=0.479060 w=0.877782 error=0.000365`
已通过；Beat Saber 当前窗口尚未命中 HMD/VrTracker 调用，说明首帧等待链还需要单独定位。

`sceVrTrackerGetResult` 的固件函数（`0x9040`）还会根据设备类型、结果类型和相机结果写入多个内部分支；HLE 目前的结果联合体尺寸正确，但姿态、时间戳、质量和 LED 数组仍没有真实设备来源。诊断零姿态只能在明确的诊断开关下使用，不能作为“已实现 HMD”或 Beat Saber 可玩的证明。

## 对现有实现的调整

- `sceHmdReprojectionInitialize` 改为显式的 `(param, mode, reserved)` 原型，并先做固件已证实的 NULL、范围和保留字段检查。
- HMD 与 VrTracker 记录增加 `sizeof/offsetof` ABI 断言。
- 未恢复语义的 reprojection 入口继续具名保留，不再通过“无参数返回 0”的桌面标量适配器进入 Android。
- 生命周期单测仍只验证状态机；它不覆盖 guest 指针转换、固件对象内容、SBS 图像或 OpenXR。

## 设备运行边界（2026-09-18，更新）

已将本体 PKG 的完整树（`eboot.bin`、`sce_sys`、`sce_module`、`Media`，111 个 PFS 文件）作为独立的 `CUSA12878` 条目导入 AYN Thor。重新连接设备后，模块准备已经完整走过 `eboot.bin → libSceFios2.prx → Il2CppUserAssemblies.prx → PS4Util.prx → libc.prx`，并生成 1164 条实际导入审计。当前 binding 统计为 `runtime_bound=618`、`guest_export=347`、`refused=185`、`not_relocated=14`；HMD 仅剩 1 条、VrTracker 已无拒绝，Camera 8 和 Move 7 已进入 no-provider guest adapter。静态结构体分析不能替代这些运行时桥接。

启动阶段已发现并修复一条 Android 特有的 guest 文件 `mmap` 边界：游戏传入的 fd 属于 session `GuestStorage`，不能直接当作桌面全局 HandleTable 句柄；现在通过 host fd 和文件生命周期 pin 映射。其后又补齐了取消状态、event flag、equeue user event、基础 VideoOut、SaveData memory 数据描述符、Android 时钟回退，以及 HMD/VrTracker/Camera/Move 受检 guest 记录桥接，最新进程已进入 GNM submit 和 Oboe 音频回调循环。这里的证据只到启动后图形/音频循环存活，尚未观察到 SBS、HMD 姿态、tracker 结果或 OpenXR。

下一步仍需从导入审计中按 PSVR 功能族恢复结构体字段、guest 地址转换、句柄所有权、回调和停止/取消语义；在这些证据具备之前，不提交 `SetDisplayBuffers` 或 multilayer 的成功 HLE，也不宣称 SBS 已可玩。`libSceIme`、`libSceMove`、`libSceMouse` 的 `0x805a10ff` 是 provider 缺失的具名结果，应保留为能力边界。
