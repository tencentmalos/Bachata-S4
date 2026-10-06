# AstroQuest 与当前 shadPS4 XR 实现对照

调研日期：2026-10-06（Asia/Shanghai）。

AstroQuest 是围绕 PSVR 游戏《ASTRO BOT Rescue Mission》开发的独立 shadPS4 项目，提供 Quest 3 独立运行和 Windows PCVR 两条路径。它对 Astro 所需的 DS4 空间追踪、麦克风、NGS2 音频和游戏时间步做了专门实现；本仓当前则已有 Swan 上的双 Move、PSVR 投影、普通游戏影院、超分和宿主眼动重建路径。两边的核心 OpenXR 投影思路相近，值得吸收的是具体兼容模块与游戏修复。

建议保留本仓原生 bionic 宿主、typed Session 和现有 Vulkan XR 架构，按模块评估移植。本次为源码与既有记录对照，没有编译或运行 AstroQuest，没有部署设备、执行游戏或建立性能 A/B。

## 1. 对照版本与项目身份

| 对象 | 固定版本 | 用途 |
|---|---|---|
| 本仓 | `feature/malos/swan_performance`，`2c5df26d555f343b9fb60b58539cb74b3da76826` | 本文所称“当前实现”的代码基线 |
| Foundation | `588da6e2a7b66aa8926581c6ce50b3e6389e75e5` | 上述主仓固定的依赖版本 |
| [bigmak94/AstroQuest][aq-repo] | `9ff3e43060b0a25aaa498566caa32649f06966bf`，发布版本 `v0.18` | 主要外部参考 |
| [iced-coffeez/shadVR][shadvr-repo] | `main` 为 `4d28d79a273c70a2a70160c580f22a083bdcd459` | 同名项目辨别，未作为功能实现参考 |

AstroQuest 0.18 于 2026-10-06 12:06:27 UTC（北京时间 20:06:27）发布。早期新闻和 0.13 说明主要描述欧洲版 CUSA12392 v1.00；[0.18 发布说明][aq-release]已增加同地区 v1.04、SteamVR/Index、更多控制器、观众镜像和触摸板动作替代，不能继续按“只支持 1.00”描述该版本。其他游戏、地区和版本的兼容性不能从这两版 Astro 推广。

另一个 `shadVR` 项目的名称和描述声称 OpenXR，但本轮检查其公开 `main` 与官方主干的提交差异，额外提交只有修改 README 的 `4d28d79`。该提交和公开源码树不足以证明已有可用 VR 实现，故不与 AstroQuest 混为一谈。[对应提交][shadvr-commit]

## 2. 功能与架构差异

| 方面 | AstroQuest 0.18 | 本仓基线 |
|---|---|---|
| Android 宿主 | Android OpenXR 前端启动独立 aarch64 Linux/glibc 模拟器进程，FEX 执行 x86-64 guest | 原生 Android/bionic 宿主内集成 FEX，按 typed Session 管理 guest 与宿主资源 |
| Android 图像链 | 模拟器使用 Vulkan/Turnip；通过 AHardwareBuffer/dma-buf 共享图像，OpenGL ES 前端提交 OpenXR | 同进程 Vulkan XR，通过 GPU mailbox 传递图像与元数据 |
| 双目与重投影 | Projection Layer 携带图像对应的绘制姿态，由运行时重投影 | 已有相同基本机制；双眼 pose/FOV 与图像成对传递 |
| 控制器模型 | 重点模拟一个被追踪的 DS4；实体游戏手柄或 VR 控制器均可提供输入 | OpenXR 左右控制器对应两个 Move，含位姿、速度、按键、震动和球心偏移 |
| 实体手柄位置 | 用头显手部追踪估计手柄位置，配合手柄 IMU；缺失时使用可调整的假定位置 | 当前 VrTracker 有效结果路径只接受 HMD 和 Move，尚未提供 DS4 空间追踪结果 |
| 麦克风 | 实际麦克风输入、增益和按键吹气替代 | Android AudioIn 尚无真实麦克风 provider |
| 游戏音频 | 增加 Astro 使用的 NGS2 引擎和耳机虚拟环绕 | 已有 Audio3d、Oboe、混音及节拍修复；NGS2 HLE 仍有大量占位函数 |
| PCVR | Windows OpenXR、VDXR/SteamVR、头显断连恢复及观众镜像 | 当前这套 OpenXR runtime 接入位于 Android 构建路径，尚无对应桌面入口 |
| 画质策略 | Quest 默认关闭 MSAA，使用 FXAA/锐化；提供缩窄 FOV 的选择 | XR 独立 render scale、FSR1/SGSR1、输出三档、MSAA 和眼动 FDM 重建 |
| 游戏节拍 | 按 Astro 版本修改时间步和内部动态分辨率，配合显示刷新节拍 | 通用 XR 设置和 guest patch 基础设施；未发现对应 Astro 专用包 |
| 影院与辅助功能 | 围绕 Astro 的启动、游戏及调试界面 | 普通游戏影院、环境场景、PSV/ImGui 状态层和双目截图录像 |

外部依据：[Quest 架构说明][aq-quest]、[PCVR 架构说明][aq-pc]、[Quest OpenXR 前端][aq-xr-host]、[共享缓冲实现][aq-gl-frames]。本仓依据：[XR runtime](../src/video_core/renderer_vulkan/openxr/runtime.cpp)、[GuestVrSensor](../src/core/host_runtime/guest_vr_sensor.h)、[构建接入](../CMakeLists.txt)。

### 2.1 图像传输和重投影

AstroQuest 的 Quest 前端以 OpenGL ES 接入 OpenXR。前端分配 AHardwareBuffer，将 dma-buf 描述符交给 glibc 模拟器；模拟器通过 Vulkan 导入并写入共享图像。它还保留导入失败后的 GPU readback/CPU 拷贝路径，不能只凭架构名称认定每种环境都没有 CPU 图像拷贝。位姿和控制信息通过 Unix socket 传递。[协议][aq-protocol]、[图像导出器][aq-exporter]

其 PCVR 则是单进程：OpenXR 线程使用模拟器的 Vulkan device，接收最新完成的双目图像，并在游戏两帧之间重复提交旧图像及其原绘制姿态。设备扩展、物理 GPU 选择、共享 queue 锁、sRGB 和会话重建都是实际接入内容。[PCVR 源码][aq-openxr]

本仓同样保留 guest 提交的 head pose，并结合设备眼位与 FOV 构造投影视图；无效姿态不作为有效透视帧发布。原理上没有“必须采用 AstroQuest 才能正确重投影”的缺口。[GuestReprojection](../src/core/host_runtime/guest_reprojection.h)、[既有 Swan 验证](validation/android-native-host/xr-projection-msaa-20260929.md)

两边对外倾眼轴的处理有所不同：AstroQuest PCVR 计算能够覆盖实际眼视图的平行投影视锥，再交给 runtime 合成；本仓保留 runtime 的每眼外参并随 guest 绘制姿态组合。前者是兼容 PSVR 平行相机的选择，不能仅凭实现不同判定优劣，需要具体游戏相机和实机画面验证。[平行视锥计算][aq-view]

本仓 mailbox 也存在 GPU 完成等待，因此同进程架构不等于“零等待”或自动获得性能收益。两边都没有本轮同设备、同游戏、同画质的时延或帧率比较。

### 2.2 DS4 空间追踪与双 Move

Astro 的重要输入对象是游戏里的 DS4。AstroQuest 从左右手掌位置求中心与横轴，结合手指方向或实体手柄 IMU 确定姿态；追踪不可用时，提供可调整的假定位置。其 VrTracker 将该状态写入 DS4 的 `pad_info.device_pose`。0.18 还增加了用 VR 控制器按键模拟触摸板按下、划动和拉回释放的操作。[手柄几何][aq-held-pad]、[VrTracker][aq-tracker]

本仓当前 `sceVrTrackerGetResult` 的虚拟设备路径只接受 HMD 和 Move；即使注册了 DS4 handle，也没有相应有效结果路径。普通蓝牙手柄的按键输入和 DS4 的六自由度位置追踪是两个不同能力。[本仓 VrTracker](../src/core/libraries/vr_tracker/vr_tracker.cpp)

另一方面，AstroQuest 的 Move 读取仍返回 `ORBIS_MOVE_ERROR_NO_CONTROLLER_CONNECTED`。它的 Astro 交互不能证明 Beat Saber 等双 Move 游戏可用。本仓已有双手身份映射、速度、震动和从 grip 到虚拟球心的 75 mm 偏移；该偏移仍是待物理标定的候选值。[AstroQuest Move][aq-move]、[本仓 Move](../src/core/libraries/move/move.cpp)、[球心与眼位验证边界](validation/android-native-host/xr-controller-alignment-20260930.md)

### 2.3 麦克风和 NGS2

AstroQuest 已接入游戏麦克风，包括增益、输入强度诊断和按键替代吹气。Android 侧需要把实际采样传给 glibc 核心；PC 使用相应录音后端。[AudioIn 实现][aq-audioin]

本仓 Android 的共享 AudioIn 后端为 `NullAudioIn`，guest bridge 明确保留无设备错误契约，尚未开放真实数据桥。后续接入需处理采样格式、guest 输出范围、权限、阻塞读取取消和 Stop 生命周期，不能仅将接口改成返回成功。[AudioIn](../src/core/libraries/audio/audioin.cpp)、[guest bridge](../src/core/host_runtime/guest_audio_input.h)

NGS2 是更独立的一块差异。AstroQuest 实现了 Astro 使用的 sampler voices、ATRAC9/HE-VAG/PCM 解码、子混音、混响、其他效果 rack、mastering 和游戏效果回调。本仓的 NGS2 HLE 仍有大量占位函数，已有 Audio3d/AudioOut 修复不能替代它。这里描述的是 HLE 实现差异，不推导为所有游戏或固件模块路径都没有声音。[NGS2 引擎][aq-ngs2]、[本仓 NGS2](../src/core/libraries/ngs2/ngs2.cpp)

移植时应保留本仓已有音频端口节拍和 Oboe/Foundation 输出，重点审计 NGS2 的 guest 回调 ABI、内存分配回调、音频工作线程和会话退出。对方 glibc 核心中的直接指针访问不能原样代替本仓 guest 地址检查。

### 2.4 Astro 专用时间步与分辨率控制

AstroQuest 的 `known_title.cpp` 按已识别游戏版本调整时间步、内部渲染尺寸和节拍。其版本表包含 CUSA12392 的 1.00、1.04 两组独立地址和识别条件。游戏低于 60 FPS 时，正常运行速度有赖于这些专用修正，不能把效果全部归因于模拟器执行效率。[控制逻辑][aq-title]、[版本识别与地址表][aq-builds]

作者设计的节拍将游戏帧分配到整数个头显刷新周期，例如 90 Hz 下的 45/30 FPS，并在 GPU 负载和帧时间允许时调整游戏内部渲染尺寸。这是值得参考的策略，但应区分通用显示节拍与游戏内存修正。

本仓已有按 title、模块身份和 preimage 安装的 guest patch 包，适合承载 Astro 专用部分；不宜把固定地址持续写入逻辑直接散落到通用 renderer 或会话循环。[guest patch 格式](guest-function-patches.md)、[包多选实现](validation/android-native-host/guest-patch-selection-20261004.md)

### 2.5 超分与眼动范围

AstroQuest Quest 路径以低源分辨率、关闭 MSAA、FXAA/锐化和缩窄 FOV 为主要取舍。其说明明确没有实现 Application SpaceWarp 所需的深度和运动向量链路；高刷新率头部重投影不能等同于生成了同刷新率的新游戏帧。[Quest 画质与性能说明][aq-quest]

本仓已有 FSR1/SGSR1、每眼输出尺寸选择和真实眼动输入，但 FDM 作用范围是宿主 presentation/upscale pass，不能写成所有 guest draw 已实现 ETFR。Swan 输出像素尺寸也不能当作 guest 源图的真实细节。[输出与眼动验证](validation/android-native-host/xr-launch-sgsr-etfr-20260930.md)、[眼缓冲分辨率修复](validation/android-native-host/xr-eye-resolution-20260929.md)

## 3. 可以单独评估的渲染修复

| 项目 | 源码对照结果 | 建议 |
|---|---|---|
| Cube array 光照层号 | AstroQuest 将 `face + 8 × cube` 转为六面排列；本仓已有等价的 `face - 2 × floor(face / 8)` | 已有对应语义，不重复移植 |
| GS 输入图元数量 | AstroQuest 补齐 TriangleFan、Polygon、LineLoop、AdjTriangleStrip、AdjLineStrip；本仓当前 `NumVertices` 缺这些分支 | 独立评估转换路径和边界用例，再进行 Astro 场景验证 |
| GS 输入寄存器和多眼 viewport | 0.18 发布说明记录了这两类修复 | 本轮未逐项证明本仓存在相同问题，先对照具体 lowering 与 shader 数据流 |

源码：[本仓 Cube 坐标](../src/shader_recompiler/backend/spirv/emit_spirv_image.cpp)、[AstroQuest Cube 坐标][aq-cube]、[本仓 GS 输入图元](../src/shader_recompiler/backend/spirv/spirv_emit_context.cpp)、[AstroQuest GS 输入图元][aq-gs]。

作者将 GS TriangleFan 支持与 Astro 1-4 关卡的崩溃修复关联；这属于作者的运行记录。本轮确认了源码差异，尚未在本仓复现该关卡故障。

## 4. 实测记录与可比性

- **AstroQuest 作者记录**：Quest 3 关卡约 30 FPS，轻场景约 45 FPS；PCVR 常用目标为 60 FPS。数字随场景、源分辨率、刷新率和版本变化，不能与 Swan 上的 Beat Saber 或血源横向比较。
- **AstroQuest 0.18 的明确验证边界**：作者佩戴验证了 PC 游戏 1.04 及触摸板替代动作，以及 Quest 版 Touch 控制器。其他项目主要使用模拟头显和未佩戴 Quest 3；SteamVR 本身及非 DualSense 游戏手柄不在作者手边。Index、Bigscreen Beyond、Pimax 等还包含社区报告，应保留来源区别。[发布说明][aq-release]
- **本仓既有证据**：Swan 已验证 Projection Layer、实际双眼输出、Move HLE 位姿与手序、真实眼动输入、影院及截图录像。已有报告仍保留物理握持标定、部分佩戴体验和 GPU hang 等未完成项，不能将运行或截图成功扩大为所有 PSVR 游戏完整可玩。
- **本轮新增验证**：仅公开源码、发布说明和本仓源码的静态对照；没有新增构建、设备操作、游戏兼容性或性能结论。

## 5. 建议吸收顺序

以下为候选工作拆分，不表示已开始实施或已具备验收结果。

1. **Astro 的基础交互与音频**：并列评估 NGS2、DS4 空间追踪和麦克风。NGS2 按库整理；DS4 与 Move 保持不同设备语义；麦克风复用宿主音频设施并补 guest 数据桥。
2. **游戏专用修正**：把 1.00/1.04 的时间步、分辨率和必要的座位/重置处理整理成可识别、可禁用的 guest patch。逐项确认模块身份、写入前值和错误版本拒绝行为。
3. **小范围 renderer 修复**：先评估 GS 输入图元缺项。Cube array 对应修复已存在，其余差异须按本仓当前 GCN 编译链验证，不能整批覆盖。
4. **PCVR 接入**：复用现有渲染姿态、图像传输和输入模型，参考对方的 Windows OpenXR 设备选择、queue 互斥、音频设备热插拔、断连恢复和观众镜像。该项可以独立于 Astro 游戏兼容推进。
5. **帧节拍与动态画质**：在同设备、同游戏、同画质下建立基线后，再比较其 governor、整数刷新周期节拍和本仓现有调度。保留实际游戏 FPS、runtime 刷新率、重复帧与输入到显示时延的区别。

本仓已有的 bionic/FEX 集成、typed Session、双 Move 和 Foundation XR 功能继续作为基础。AstroQuest 的 glibc 子进程方式可以作为参考，但没有本轮证据支持为了获取上述功能而整体替换宿主架构。

## 6. 资料入口

外部代码链接固定到本次对照的完整提交，避免 `main` 后续变化影响结论。本仓相对链接对应第 1 节的基线。

- [AstroQuest 固定版本说明][aq-readme]、[Quest 详细说明][aq-quest]、[PCVR 详细说明][aq-pc]、[0.18 发布说明][aq-release]。
- [本仓 PSVR Projection 验证](validation/android-native-host/xr-projection-msaa-20260929.md)、[Move 与默认眼位](validation/android-native-host/xr-controller-alignment-20260930.md)。
- [本仓 XR 输出与眼动](validation/android-native-host/xr-launch-sgsr-etfr-20260930.md)、[影院及截图录像](validation/android-native-host/xr-cinema-editor-20261001.md)。
- [本仓音频节拍与振动](validation/android-native-host/bloodborne-android-audio-vibration-20261004.md)、[guest patch 包选择](validation/android-native-host/guest-patch-selection-20261004.md)。

[aq-repo]: https://github.com/bigmak94/AstroQuest
[aq-release]: https://github.com/bigmak94/AstroQuest/releases/tag/v0.18
[aq-readme]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/README.md
[aq-quest]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/README-QUEST-VR.md
[aq-pc]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/README-PC-VR.md
[aq-xr-host]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/quest-host/cpp/xr_host.cpp
[aq-gl-frames]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/quest-host/cpp/gl_frames.cpp
[aq-held-pad]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/quest-host/cpp/held_pad.h
[aq-protocol]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/core/vr/vr_protocol.h
[aq-exporter]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/video_core/renderer_vulkan/vk_vr_exporter.cpp
[aq-openxr]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/core/vr/openxr_host.cpp
[aq-view]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/core/vr/openxr_view.h
[aq-tracker]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/core/libraries/vr_tracker/vr_tracker.cpp
[aq-move]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/core/libraries/move/move.cpp
[aq-audioin]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/core/libraries/audio/audioin.cpp
[aq-ngs2]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/core/libraries/ngs2/ngs2_engine.cpp
[aq-title]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/core/known_title.cpp
[aq-builds]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/core/known_title_builds.h
[aq-cube]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/shader_recompiler/ir/passes/resource_tracking_pass.cpp
[aq-gs]: https://github.com/bigmak94/AstroQuest/blob/9ff3e43060b0a25aaa498566caa32649f06966bf/shadps4-arm64-main/src/shader_recompiler/backend/spirv/spirv_emit_context.cpp
[shadvr-repo]: https://github.com/iced-coffeez/shadVR
[shadvr-commit]: https://github.com/iced-coffeez/shadVR/commit/4d28d79a273c70a2a70160c580f22a083bdcd459
