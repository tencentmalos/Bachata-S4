# PSVR Projection Layer、XR 倍率与手柄位姿（2026-09-29）

本轮按用户要求参照 Azahar 改为真正的 Projection Layer，并检查 OpenXR 左右眼与左右手柄。分支 `feature/malos/swan_performance`，基点 `b0778a7f`；包含此前尚未提交的 GuestFault、XR 和 Foundation 工作。本轮没有提交或推送。

## 结果

- Swan 的 XRRuntime 在改前报告两个 `QUAD`（分别 EYE_LEFT / EYE_RIGHT），改后报告 shadPS4 的一个 `STEREO_PROJECTION`，每眼子图 2592×2400。双目透视图不再走有限距离影院平面。
- 新包 `013367fc…` 已安装并核对设备 APK SHA。Beat Saber CUSA12878 v02.04 进入语言选择页；PID 32588 / generation 1 / UUID `3430c7905872416dd5c84a5d5c6fcdb2`，记录终点 17631 guest flips、Running。实际系统合成截图有正常语言菜单。没有执行歌曲或挥砍验收。
- XR 独立默认倍率为 **1.0**；服务实际日志 `Guest internal scale=100.0 percent XR=true`。原全局 2D 倍率 0.5 和游戏设置文件前后逐字节相同。
- 手柄按 OpenXR 左右 action path 保持身份，VrTracker 使用 grip，ImGui 射线使用 aim。修正 Move 陀螺仪的坐标系，补齐 VrTracker 线速度。设备读取到两手 active、独立位置/旋转及不同的 grip/aim。
- **合焦舒适度、实际握持偏移、游戏内挥砍方向仍需佩戴反馈。** 单眼系统截图和 HLE 测试不能证明这些全部正确。此前 CP opcode / GPU hang 不在本轮修复结论内。

紧凑证据：[目录](evidence/xr-projection-20260929/)。本地完整原始记录位于 `build/validation/xr-stereo-msaa-20260929/`。

## 图像与绘制姿态必须一起传递

参照本机 Azahar `foundation/modules/xr/src/XrSceneVulkanLayer.cpp` 的 `rendered_views_`：保存成功绘制图像对应的 pose/FOV，提交层时复制保存的值。不能把新一次 `xrLocateViews` 的显示时刻姿态直接标在之前已经画好的图像上。

shadPS4 的图像来自 guest，绘制姿态来自 HMD reprojection 提交参数。核对固件 11.00 的 `hmd-analysis.elf`（0x9834–0x9865）和 Beat Saber 2.04 caller，恢复 56 字节 tracker record：position 在 +0，XYZW quaternion 在 +0x0c，其余字保持 opaque；加入大小与偏移断言。游戏在 0x103aa83 查询 VrTracker，头部位置/旋转保存到 0x1ee2900 / 0x1ee290c，随渲染帧排队；0x103e5e3 附带该队列记录提交纹理。实机 `StartMultilayer` trace 的 56 字节值与真实头部姿态一致，见 `submitted-pose.json`。

实现链：

1. `GuestReprojection` 读入该帧的 head pose，验证有限值、四元数长度并归一化。
2. 从 runtime 的同一次 head/eyes snapshot 取得头部到左右眼的相对平移和旋转，组合到提交的 head pose。保留真实 IPD 和设备外倾角，不固定为两眼平行。
3. 随帧传递双眼 pose/FOV，经 presenter 到 GPU mailbox；复制图像和元数据在同一把锁下完成。等待 swapchain 后重新判断最新 mailbox 的模式，避免 2D / perspective 切换时套错层类型。
4. 对 perspective 双目提交 `XrCompositionLayerProjection`，每眼使用各自 pose/FOV 与 SBS 半幅。无效绘制姿态时暂不提交该层并记录警告，不降级为立体影院。
5. 普通游戏以及 PSVR 显式 `Start2d` 继续影院 Quad，用户之前保留的 2.5 m / 3.2 m 配置不变。

Swan 实测两眼有约 ±5° 的外倾，眼距约 64 mm。Guest VrTracker 的左右 eye pose 与 Projection 的校准来自同一设备源；Beat Saber caller 会读取和保存左右眼四元数。当前尚未逐 draw 捕获游戏 view matrix，不能把这部分静态调用证据扩大成所有 PSVR 游戏的相机兼容性结论。

Guest 的 `sceHmdGetFieldOfView` 仍提供虚拟 PSVR frustum；提交的 tangent-to-UV 变换已经描述它。输出改用 runtime FOV 时，先按新 FOV 的 tangent 边界计算 UV，再采样 guest 图像，避免直接把不同 frustum 拉伸。保留 atlas 偏移与负 Y。测试覆盖不对称 frustum、不同 head pose、旋转校准和倾斜眼轴。

OpenXR 允许提交与最新 located views 不同的 pose/FOV，runtime 按其合成；它们必须准确描述被提交图像。[ProjectionView 规范](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerProjectionView.html)

## 左右手柄核对

`xrLocateViews`、VIEW→LOCAL 的 head locate、左右 grip/aim locate 使用同一 predicted time 和 LOCAL 参考空间。位置使用米，四元数按 XYZW 传递。左右 Move handle 由 index/path 决定，不依赖 open 或 register 顺序；生产 HLE 测试故意先打开右手验证。

修复两项：

- OpenXR `XrSpaceVelocity` 的角速度以 LOCAL 轴表示，而 Move gyro 使用手柄自身轴。现在用 `inverse(grip.orientation)` 旋转角速度；无有效方向时不输出伪有效的 gyro。
- `sceVrTrackerGetResult` 原来仅写角速度，线速度一直为零；现同时复制真实 linear velocity。追踪结果本身的速度保持参考空间表达。

加入 `DebugBus xr_tracking status`，分别显示 head、眼、grip、aim、有效位、active 和预测时刻。现有蓝牙 PS/Xbox 按键合并不受影响；不为它们伪造 6DoF。

尚未声称完成 PSMove 球心到各款 OpenXR 手柄握持点的物理标定。原始加速度采样也不是 OpenXR core pose API 直接提供的数据；本轮没有合成加速度来冒充硬件读数。

## 8× MSAA：限制在当前模拟器选样逻辑，不能简单说 Turnip 不支持

源码 Turnip `87aeb405…`（基线 `1b588fceef`）的 `tu_device.cc` 给全格式共同保证的 framebuffer color/depth/stencil mask = 7，即 1/2/4。`tu_formats.cc` 对 A7xx 及以上、符合条件且位宽 ≤64 的附件额外支持 8×。

Swan 使用相同驱动的只读能力探针结果：

| 格式 | image sample mask | 支持计数 |
|---|---:|---|
| RGBA8 UNORM / sRGB | 15 | 1/2/4/8 |
| RGBA16F | 15 | 1/2/4/8 |
| RGBA32F | 7 | 1/2/4 |
| D24S8 / D32F | 15 | 1/2/4/8 |

当前 `vk_graphics_pipeline.cpp` 使用全局 color/depth mask 交集，先将 guest 的 8× 钳成 4×，随后要求二者相等而触发断言；纹理路径则已经按格式查询。这不是 XR swapchain 的 1× 限制。

后续正确修法应根据实际附件的格式/usage 校验能力，并保持 rasterization、颜色/深度图像与 shader 样本数一致。不能直接把全局 mask 加上 8，也不能只改 pipeline 的钳制。本轮仅定位和增加 caps 探针，**未修生产 MSAA 选择**，Beat Saber 每游戏 `Force Disable MSAA=true` 保留。没有执行 8× 渲染正确性验收。

[Vulkan limits](https://docs.vulkan.org/spec/latest/chapters/limits.html) 说明全局 mask 是相关格式共同保证的集合；单个 image format 可以支持更多样本数。

## 验证和状态

| 检查 | 结果 |
|---|---|
| Mac `guest_vr_sensor_tests` + UBSan | 73/0 |
| Swan `guest_vr_sensor_tests` | 73/0 |
| Swan `guest_reprojection_tests` | 96/0 |
| Swan Foundation/OpenXR input mock | 34/0 |
| Swan 生产 Move/VrTracker HLE pose 测试 | 33/0 |
| Android runtime Kotlin 设置测试 | 134 tests，0 failures/errors/skipped |
| Android host、probe、APK | 构建通过 |
| 实包 | APK 013367fc / host 6dc9b500 / JNI 683a08ed / Turnip 87aeb405 |

完整 SHA 和 probe host 区别见 `identity.json`。HLE probe 使用最终 mailbox 模式复核前的 host（HLE 与几何代码相同）；最终 mailbox 版本另重新编译并通过当前游戏运行验证。

保留负结果：首次 caps probe 因 user path 为相对路径而在启动前拒绝；初版新 Kotlin 测试误读不包含 Android 专属选项的 legacy catalog，改读 Android catalog 后全部通过。两项均不作为 GPU 或游戏失败计数。

结束时恢复本轮临时 `debug.shadps4.hmd_log` 为空，清理自有 `/data/local/tmp/shad-xr-stereo-20260929`；无 debugger、forward 或注入按键。原 `debug.mesa.tu.debug=kgsl_preempt_rb`、`debug.spruntime.etfr.subsample=0` 和之前 GPU CleanupPending 不变。当前游戏留在 Running 给用户体验；仅保存/缓存可由游戏正常更新，未修改 ROM、更新包或外部编辑存档。
