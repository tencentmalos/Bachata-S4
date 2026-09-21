> 2026-09-20 插件加载续项：[修复与实测](validation/android-native-host/beatsaber-plugin-loader-20260920.md)。两处 DllNotFound 已消失，原始选择顺序也会在 HMD 设置完成后进入 PlayStationVR；无 patch 普通启动已出现双眼局部元素，但画面异常、菜单不可验收，SBS 仍未完成。下方“仍全零/插件未修复”保留为此前证据。

> 2026-09-20 guest log 续项：用户要求 SBS 始终固定正前方，已停止陀螺仪积分并修正 camera 零四元数。位姿回报有效后仍黑屏；真实插件加载因动态模块拒绝返回 ENOENT，尚未修复。见 [实测报告](validation/android-native-host/beatsaber-guest-log-20260920.md)。下文历史陀螺仪规划不覆盖当前用户约定。

# shadPS4 PSVR 适配调整规划：先做 SBS，再落地 Swan OpenXR

> 2026-09-20 AYN 实施更新：[验证报告](validation/android-native-host/beatsaber-sbs-20260920.md)。已接直接眼图和独立/共享视图合成，修复 EventFlag Stop 阻塞，双驱动 GPU 定向检查通过；真实游戏眼图仍全零，尚未达到本规划的双眼场景与菜单验收。新批处理已实跑并保留 partial/frontier。

> 2026-09-20 续项：SBS 尚未完成。先按 [Reverse Study 批量调用链标定与 HLE 重建 spec](specs/reverse-study-batch-hle-reconstruction-20260920.md) 补齐分析工作流，再推进眼图表示、提交/释放和停止闭环。该 spec §2 区分历史 AYN 证据与当前源码；旧缓存启动画面不能作为真实双眼或 Swan 验收。本文件保留原两阶段目标，后文“当前”实现状态按历史记录日期理解。

本文把 PSVR 适配拆成两个有明确边界的阶段：第一阶段在普通 Android 设备上让 Beat Saber 产生可检查的左右眼 SBS 输出；第二阶段在 Swan 上把同一条双眼图像链路接入 OpenXR。第一阶段不要求头显、OpenXR 运行时或 Move 6DoF，因此可以先验证渲染、缓冲、尺寸、呈现和输入框架。

## 目标和边界

目标游戏先固定为 PS4 版 Beat Saber（`CUSA12878`）。第一阶段的成功条件是：游戏经过启动所需的 VR 初始化后，能在普通 Android `ANativeWindow` 上显示左右眼图像拼成的 SBS 画面，并能用普通触控或手柄完成菜单级操作。这里的“VR 初始化通过”只表示已经得到足以产生图像的虚拟/诊断设备状态，不代表已经实现真实头显或 Move。

以下能力不作为第一阶段的完成条件：

- 真实 HMD 姿态、6DoF、光学畸变和时间扭曲；
- PS Move 的完整按键、姿态、振动和追踪；
- OpenXR runtime、Swan 专用显示输出；
- Beat Saber 的完整可玩性、全流程稳定性或性能达标。

如果游戏在缺少 Move 或某个必需的 tracker 状态时直接退出，要把它记录为当前阶段的阻塞证据，不能用无条件返回成功来掩盖。可以提供静态姿态和诊断设备，但状态中必须明确标记为 `diagnostic`。

## 设计原则

1. **输出先行，设备后接入。** 先把“两个眼睛的图像如何产生、合成、呈现和检查”做成独立的 `StereoFrame`/`EyeAtlas` 链路，再接 OpenXR 的交换链和预测时间。
2. **普通 Android 可运行。** SBS 阶段的构建和运行路径不依赖 OpenXR 库或头显服务；OpenXR 只作为 Swan 阶段的可选 presenter。
3. **沿用桌面 HLE 语义。** 先完成主 ELF 和实际加载 SPRX 的功能族盘点，复用桌面已有的离线和不可用语义。缺失能力保留具名的未完成状态，不用统一返回 0。
4. **每个功能族闭环。** VR 初始化、VideoOut/立体缓冲、tracker、Move、输入、回调、取消和销毁要分别盘点和验证，不能按一次运行暴露一个 `UNSUPPORTED_IMPORT` 再逐个补。
5. **不改游戏投影作为第一步。** 第一阶段先验证左右眼输出和呈现；相机、FOV、投影矩阵和畸变修正放到有实际帧证据之后。这样能区分 HLE/输出问题与游戏本身的渲染问题。

## 目标架构

```text
Guest VR HLE（sceHmd/sceVrTracker/sceMove 等）
                │
        GuestVrSession（按 session/generation 管理）
        ├─ VirtualHmdState / tracker 状态
        ├─ StereoFrame / EyeAtlas
        ├─ VrInputProvider
        └─ OutputMode { Flat, SBS, OpenXR }
             │                    │
   SbsPresenter                  OpenXrPresenter
   ANativeWindow                 Swan + OpenXR swapchain
```

`GuestVrSession` 是两阶段共用的边界。HLE 只向它查询和提交 VR 状态，不直接操作 Android Surface 或 OpenXR 句柄；呈现器负责把已经生成的左右眼结果送到具体后端。所有句柄、回调、取消和销毁都绑定到 session/generation，避免重启或切换输出模式后使用旧地址。

## 第一阶段：Beat Saber 普通 Android SBS

### P0：完整盘点和基线

以 Beat Saber 主 ELF 及实际加载的 SPRX 为准，生成可归档的导入清单。至少覆盖以下功能族及别名/库后缀：

- HMD 初始化、能力查询、配置、姿态和时间；
- VR tracker 的初始化、状态、结果、时间戳、取消和销毁；
- VideoOut 的普通缓冲、立体缓冲、flip/present、回调和输出尺寸；
- Move/MoveTracker 的设备枚举、按钮、姿态、振动和连接状态；
- `libSceSystemService`、输入、线程/同步和游戏依赖的 sysmodule/provider；
- 与启动准备、存档、奖杯或离线流程相邻的导入，确认它们不是被 VR 初始化误归类。

清单每行记录完整 NID、库名、可读名、实际绑定结果（guest LLE、桌面 HLE 适配器、Android bridge、未接入）、首次调用位置和当前阶段是否阻塞。`docs/android-hle-migration-status.md` 中的 `desktop_hle_scalar_incomplete`、`desktop_hle_declared_incomplete`、`android_bridge` 和 `unsupported_import` 状态继续沿用。

基线需要先确认：关闭 SBS 时桌面/Android 的普通 VideoOut 和输入行为没有被改变；启用 SBS 后，游戏至少能进入产生 VR 帧的路径。普通 DS4 或触控不是 Move 6DoF 的替代品，只能作为第一阶段的菜单和诊断输入。

### P0.1：Beat Saber PSVR 生命周期和驱帧深挖

针对 `CUSA12878` 1.00，先按实际 eboot 的调用顺序追踪生命周期，而不是把 HMD、tracker 和 reprojection 当成互不相关的空函数。当前静态盘点得到的主链是：

```text
HmdInitialize315 → HmdOpen → VrTrackerInit/RegisterDevice
 → CameraOpen/Start/GetFrameData
 → VrTrackerGpuSubmit → VrTrackerGpuWaitAndCpuProcess
 → VrTrackerGetTime/GetResult
 → HmdReprojectionInitialize/SetDisplayBuffers/StartMultilayer
 → GnmSubmitAndFlip → VideoOutSubmitEopFlip
 → Reprojection UserEventStart/End
 → Stop/UnsetDisplayBuffers/Finalize
 → VrTrackerUnregisterDevice/Term → HmdClose/Terminate
```

第一阶段的帧时序边界固定为：

1. `GpuSubmit` 创建当前 guest frame，记录 generation、sequence 和预测时间；
2. `GpuWaitAndCpuProcess` 将 tracker 阶段推进到可读取结果；
3. `GetTime/GetResult` 只读取当前 frame 的时间和诊断姿态；
4. GNM EOP flip 是 guest submit 边界，不能在命令刚生成时释放 buffer；
5. VideoOut presenter 真正完成 host present 后，才完成当前 frame；
6. Stop、Finalize、HmdClose 或新 generation 会取消活动 frame，所有旧 token 都拒绝提交。

已经新增 `GuestVrSession` 状态机和 Android ARM64 单测，代码状态会在 `Hmd`、`VrTracker`、`VideoOut` 和 `GuestGraphics` 之间传递。该层目前提供静态诊断姿态和时序证据，不伪装真实 Camera、HMD 追踪或 Move。完整盘点和实际限制见 [Beat Saber PSVR 生命周期记录](validation/psvr-sbs-20260918.md)。

### P1：SBS 输出骨架

在现有 `GuestGraphics`/VideoOut presenter 之上增加 per-session 的输出状态，而不是增加全局 HMD 标志：

- `OutputMode::Sbs` 和配置项（每眼宽高、左右顺序、像素格式、是否镜像）；
- `StereoFrame`：记录左眼、右眼、预测/提交序号、尺寸和有效性；
- `EyeAtlas`：默认宽度为单眼宽度的两倍，高度为单眼高度，左眼在 `[0, eye_width)`，右眼在 `[eye_width, 2 * eye_width)`；
- 立体缓冲注册、获得、提交、flip/present 的 HLE 桥接，沿用普通 VideoOut 的生命周期；
- Android `ANativeWindow` 的单次呈现路径，确保 SBS 不会绕过已有的队列、同步和释放逻辑；
- 输出诊断：每帧记录尺寸、左右眼有效位、present/flip 序号和丢帧原因，可选生成左右眼校验色块。

当前 `src/core/libraries/videoout` 主要是普通缓冲接口，没有假定一个现成的立体注册 API。因此先以实际导入清单为准补齐完整立体功能族；如果 Beat Saber 只需要普通注册加左右眼 atlas，则保留普通接口语义，在 session 层合成 SBS，不伪造未被调用的扩展。

对 Beat Saber 1.00 的静态 eboot 盘点没有发现 `sceVideoOutRegisterStereoBuffers` 导入。SBS 接入点应先追 `sceHmdReprojectionSetDisplayBuffers` 与 GNM/VideoOut EOP flip 的真实参数和缓冲所有权，再决定是对已有双眼 guest buffer 做 atlas 合成，还是在 presenter 侧分屏。当前 HMD reprojection 声明仍有未恢复的结构参数，不能把这些无参 stub 直接当作已经完成的立体输出。

### P2：虚拟 HMD 和诊断姿态

实现能支持输出链路的最小虚拟设备状态：

- 由配置提供单眼分辨率、FOV、IPD 和默认姿态；
- 初始化、状态查询、结果、时间和销毁使用明确的 session 状态机；
- 默认姿态可以是静态零位或可控的测试姿态，状态标记为 `diagnostic_static_pose`；
- 未启用 SBS 或初始化失败时，保持桌面已有的不可用/离线语义；
- 回调、等待、取消和重启时清理旧 session，不把 guest 指针直接传给 host native 函数。

这一层只负责让游戏获得可解释的设备信息和帧状态，不先修改游戏的相机或投影常量。若 Beat Saber 进一步要求真实 tracker 才能继续，则把缺口归档到 Move/Tracker 功能族。

### P3：普通 Android SBS 交互

参考 Azahar 的 `azahar-sbs-interaction.md`，把 SBS 交互做成独立输入层：

- Android 触摸事件只更新目标指针/按钮状态；
- 在渲染帧回调中把触点映射到左半或右半，再映射到游戏坐标；
- 同一交互控件在左右眼各绘制一次，避免用户只能点击一半画面；
- 菜单、返回和模式切换由一个中心状态机处理，不能在两个眼睛的 UI 上各维护一份状态；
- 普通手柄先映射为菜单导航和诊断按键，不宣称为 Move 兼容。

### 第一阶段验收门槛

必须同时满足：

1. 在普通 Android 设备上不安装 OpenXR runtime 也能启动 SBS 路径；
2. Beat Saber 实际运行到可产生 VR 图像的场景或菜单，屏幕出现稳定的左右眼 SBS 画面；
3. 左右半区尺寸、顺序和内容可检查，不能只是重复同一张空白图；
4. 记录至少一段带有输出尺寸、present 序号和 HLE 状态的日志/JSON，能区分“输出完成”和“Move/真实追踪未完成”；
5. 触控或普通手柄能够完成菜单级操作，退出、重启和切回 Flat 模式不使用旧 session。

阶段报告要明确写出仍未完成的 HMD、Move、tracker、畸变、性能和完整游戏流程，不能把“能显示 SBS”写成“Beat Saber VR 已可玩”。

## 第二阶段：Swan OpenXR 真正落地

第二阶段复用第一阶段的 `EyeAtlas` 和 `GuestVrSession`，只替换 presenter、时间源和输入提供者。OpenXR 的代码组织优先参考 Foundation/Azahar 已有链路：

| Azahar/Foundation 组件 | shadPS4 侧的复用方式 |
| --- | --- |
| `OpenXrSessionManager` | `OpenXrPresenter` 的 session 状态、READY/STOPPING/EXITING 事件、frame-wait 线程和 `predictedDisplayTime` 发布 |
| `XrGameVulkanLayer` | OpenXR swapchain 的创建、`Acquire/Wait/Release`、图像 view/framebuffer 和资源生命周期 |
| `XrInputSystem` | action set、手柄/扳机/摇杆、手部 pose、有效姿态和 haptic；再映射到 `VrInputProvider`/Move HLE |
| `XrMath` 及视图定位代码 | FOV、pose、时间戳、坐标系和左右眼 view 的转换 |
| `XrGameSurfaceLayer` | 眼睛布局和辅助层的组织；SBS 阶段只使用其 atlas 思路，不强行复用完整 UI 层 |

### O1：Swan 环境和 Vulkan 绑定

先确认 Swan 镜像中的 OpenXR runtime、Vulkan 扩展、权限和 Android 生命周期，再实现 presenter。OpenXR 必须使用 shadPS4 已有的 `VkInstance`、`VkPhysicalDevice`、`VkDevice`、queue 和分配器；通过 `XR_KHR_vulkan_enable2` 或等价路径核对物理设备，不创建第二个 Vulkan device。

runtime 不存在、session 创建失败或设备丢失时，回退到第一阶段的 SBS/Flat presenter，并报告具名原因。不能把 OpenXR 不可用伪装成 HMD 已连接。

### O2：OpenXR 帧和交换链

按 `OpenXrSessionManager` 的状态机处理：

1. 监听 `READY` 后开始 session，`STOPPING` 时停止提交，`EXITING/LOSS_PENDING` 时释放资源并通知 guest；
2. 用独立的 frame-wait 逻辑取得预测显示时间；
3. 每帧执行 `wait frame → begin frame → locate views → acquire/wait swapchain image → 复制/分屏 EyeAtlas → release → end frame`；
4. 在 swapchain 重建、分辨率变化和 session 重启时使旧的 `StereoFrame`/资源失效。

第一版 OpenXR presenter 先采用简单、可验证的 atlas 分屏：将 `EyeAtlas` 的左半和右半分别写入 OpenXR 左右眼图像。可以使用 Vulkan blit/compute 或渲染 pass，但不能依赖隐式线性过滤；缩放、格式和 sRGB 规则要记录在 presenter 配置中。

### O3：姿态、输入和 Move 映射

将 OpenXR 的 `predictedDisplayTime` 作为 tracker 结果的时间基准；`locateViews` 和 controller action 转换成 guest 能理解的坐标系、有效位和连接状态。对于丢失追踪、session 未 focus 或 action 无效的情况，返回明确的无效姿态/不可用状态，让游戏按桌面语义处理。

先验证头显中的左右眼图像和稳定 frame loop，再逐步接入 Move 的按钮、姿态和 haptic。Move 仍然是独立的功能族，不能因为 OpenXR 的 controller action 已经有数据就宣称 PSVR Move HLE 完成。

### 第二阶段验收门槛

- Swan 上能创建并稳定运行 OpenXR instance/session，事件状态和退出清理完整；
- OpenXR swapchain 使用 shadPS4 的 Vulkan device，左右眼图像与 SBS 阶段的 EyeAtlas 内容一致；
- frame wait、预测时间、view 定位和 acquire/release 没有持续丢帧、撕裂或旧资源访问；
- runtime 不可用时能回退到 SBS/Flat，并留下可诊断日志；
- 最后再单独验收头显姿态、Move 输入、畸变/重投影和 Beat Saber 的实际可玩性。

## 推进顺序和产物

| 里程碑 | 主要产物 | 退出条件 |
| --- | --- | --- |
| M0 | Beat Saber 主 ELF/SPRX 导入和功能族清单；桌面/Android/HLE 状态 | 清单完整，未接入项有阻塞依据 |
| M1 | `GuestVrSession`、`OutputMode::Sbs`、`StereoFrame/EyeAtlas` 和诊断输出 | Flat 行为未回归，SBS 能生成可检查的 atlas |
| M2 | 虚拟 HMD、VideoOut 立体功能族、普通 Android 触控/手柄 | Beat Saber 达到第一阶段 SBS 验收门槛 |
| M3 | Swan OpenXR runtime/Vulkan 绑定和 session 状态机 | 能稳定创建、停止、重建 OpenXR session |
| M4 | OpenXR presenter、双眼分屏、预测时间和基础 controller action | Swan 达到第二阶段图像/帧循环门槛 |
| M5 | Move/Tracker、姿态、haptic、畸变和性能优化 | 每个功能族有独立证据，再评估完整 VR 可玩性 |

每个里程碑只做针对性构建和验证：Android host 编译、HLE 单元/适配器检查、导入清单审计、输出尺寸/帧序号检查，以及实际设备上的短时 smoke。当前用户没有要求完整回归，因此不把全量游戏回归作为本计划的前置条件。

## 近期建议

下一步先完成 M0 和 M1：用已准备的 firmware 做只读 SPRX/主 ELF 盘点，同时在 Android 侧把普通 VideoOut presenter 抽象为可输出 `EyeAtlas` 的接口。然后只接 Beat Saber 启动所需的 HMD/VideoOut 功能族，产出第一段普通 Android SBS 证据。确认左右眼输出、生命周期和输入状态机稳定后，再在 Swan 上验证 OpenXR runtime 和 Vulkan 设备绑定，避免把两类问题同时引入。

相关参考实现：

- `foundation/modules/xr/src/OpenXrSessionManager.cpp`：OpenXR session、事件和 frame-wait 状态机；
- `foundation/modules/xr/src/XrGameVulkanLayer.cpp`：Vulkan swapchain 和图像资源生命周期；
- `foundation/modules/xr/src/XrInputSystem.cpp`：action、pose、输入有效性和 haptic；
- `foundation/docs/notes/azahar-sbs-interaction.md`：SBS 触控坐标、双眼 UI 和模式状态同步；
- `foundation/docs/guides/openxr-game-surfaces.md`：左右眼 atlas、尺寸和重建策略；
- `docs/android-hle-migration-status.md`：桌面 HLE 向 Android 迁移时的状态标注规则。
