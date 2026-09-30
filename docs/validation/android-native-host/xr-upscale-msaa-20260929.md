# Foundation XR 超分、ETFR 与原生 MSAA（2026-09-29）

本轮在 `feature/malos/swan_performance` 的既有 XR 工作上实现，源码尚未提交。FSR1、Qualcomm SGSR1 的新 Vulkan 渲染实现放入 Foundation，shadPS4 只负责传递图像、每眼范围、退休槽和 OpenXR 注视信息。Swan 的真实 GPU 测试通过；Beat Saber 已撤掉 `force_disable_msaa` 并恢复双目安全提示页。真实眼动传感器尚未获得用户权限，不宣称已完成人眼追踪或佩戴体验验收。

## 复用边界和设置

- [Foundation 模块与接入契约](../../../foundation/modules/upscale/README.md)：`spatial::foundation_upscale`，依赖现有 `fsr1` 与 `foveation_vulkan`，不依赖模拟器 Scheduler、VMA、OpenXR、JNI 或窗口。输入为显式 Vulkan binding、命令缓冲、image view、尺寸和 signed UV；输出为单眼线性采样的 sRGB view。
- FSR1 的 EASU、RCAS 使用 Foundation 原有 shader/constants，修正 RCAS 边缘 `texelFetch` 的越界坐标；SGSR1 的原始源码和 BSD-3-Clause 许可保存在模块内，来源为 Qualcomm 官方固定 revision `d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e`，详见 [来源说明](../../../foundation/modules/upscale/third_party/sgsr/README.md)。这里是空间超分 SGSR1，不是时域 SGSR2。
- 每眼先独立截取 guest 图像，再重建到 runtime 推荐的单眼分辨率；负 Y UV、array layer、crop、gamma 都保留。滤波不跨左右眼边界，UI overlay 在重建后以全分辨率合成。当前 SDR；HDR 不冒充支持。
- 新 XR 超分完整实现都在 Foundation。原有非 XR `fsr_pass.cpp` 的历史 compute 路径本轮未迁移；非 XR 行为保持原入口。
- Android Graphics 设置支持全局和每游戏：XR Upscaler（Off/FSR1/SGSR1）、XR Foveation（Off/Fixed/Eye tracked）、Foveation Level、Sharpness。默认 FSR1 / Eye tracked / Low / 50，XR render scale 仍 1.0。
- FDM 作用于宿主重建 pass；FSR 的 RCAS 保持 full rate，SGSR 单 pass 同样接入 FDM。**不改变 guest draw 的着色密度，也不等于整个游戏获得 ETFR 性能收益。**
- 每个 in-flight frame × eye 使用独立资源槽，重用前由宿主等待退休；Foundation 自身不提交 GPU 队列。内置 shader 可用 `tools/rebuild_shaders.py --check` 验证。

## OpenXR 眼动

沿用 Foundation `XrEyeGazeTracker`、filtered gaze 与 Pico tracking-mode hook，跟随 Azahar 的接入方式。额外 eye-gaze action set 与左右控制器 action set 一起 attach/sync。输入来自预测显示时间的 LOCAL space；失焦、未佩戴、无效或超过 100 ms 的数据回退固定中心。

PSVR 使用实际绘制眼姿态和每眼 FOV 把视线转到眼空间，保留设备的外倾角；影院模式使用视线与影院平面的交点。左右手与蓝牙手柄映射沿用前一轮实现。

首次 XR 启动按设备请求 Pico/Oculus 眼动权限。Swan 上首次权限面板阻止 XR session 从 IDLE 进入运行；本轮没有代用户授予权限，重启应用后进入 FOCUSED。实际 `gaze_valid=0`、`tracked_draws=0`，缺数据时固定中心持续工作。需要在系统应用权限中授予眼动权限后，由用户佩戴验证。

## FDM 根因与源码 Turnip 修正

### 静态密度图必须在命令录制前可读

旧 `FragmentDensityImageRing` 对不带 dynamic 标志的密度图仍用 GPU staging copy 更新。静态 FDM 可在 CPU 录制 render pass 时读取，GPU copy 尚未执行，第一帧或注视变化会读到未初始化/旧内容。

Foundation 默认静态路径改为 linear/coherent 的 host-visible image，按查询到的 row pitch 在录 pass 前写入；从 PREINITIALIZED 转 GENERAL，后续保持 GENERAL 并记录 HOST_WRITE→FDM 的依赖。显式启用 dynamic feature 的调用方才走 GPU staging + `FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT` view。普通 PP 的动态渲染附件也跟随 Foundation 的布局常量。若格式/tiling 不支持则回退 full rate。

Vulkan 的 [image-view 标志说明](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewCreateFlagBits.html)明确区分静态 host 读取与 dynamic device 读取时机。

### A840 缩放存储和硬件路径

驱动继续从 `references/mesa-turnip` 源码构建，Azahar XR 基线 `1b588fceef`，保留之前独立 GPU hang 诊断改动。本轮三项修改：

1. `tu_cmd_buffer.cc`：non-subsampled fast store 按 `!bin_is_scaled` 判定；单眼/共享 viewport 会关闭 HW bin scaling，但软件 viewport 仍可能缩放，不能因此选择不扩展像素的 store。
2. `freedreno_devices.py`：仅 A840 将 `has_hw_bin_scaling` 设为 false，使用现有软件 viewport + scaling store 路径，仍保留 FDM。A840 的硬件寄存器路径未完全定位；这是有像素验证的定向兼容修正，不宣称硬件缩放路径已修好。
3. `tu_formats.cc`：将 CPU unpack 已支持的 `R8G8_UNORM` 正确公布为 FDM 格式，消除紧凑 RG8 密度图的 format feature 验证错误。

负对照证明：固定静态图更新后，旧驱动在 256×256/4×4 密度时仍只填充 64×64 区域，矩阵 737,280 项失败；2×2 时同样只覆盖部分目标。关闭 UBWC 或改 3D load/unaligned store 未修复；只修 store predicate 仍失败；加 A840 软件缩放后同矩阵通过。`nofdm` 虽能绕开，但不是交付方案。试验性的 bin-register reset 无效，已撤回。

FDM 的实际着色粒度受驱动 tile 布局约束，调试色块不是逐像素圆形边界。没有基于这些计数声称帧率提高。

## MSAA

之前的 8× 故障是宿主将 framebuffer 的共同 sample mask 当作所有逐格式图像的上限，错误把管线钳成 4×，随后与实际 8× attachment 不一致。当前保留 guest 管线 key 的真实 sample count（含缓存预载），按每个实际 color/depth attachment 的 Vulkan image usage/format 能力校验；无附件管线另用相应能力。

Swan 原生 probe 验证 2×/4×/8× color+depth、sample mask、guest texture fetch/dimensions 与 resolve。Beat Saber 实际日志出现 `960x1080 R8G8B8A8Srgb L:2 S:8`，渲染继续，已删除该游戏的 `gpu.force_disable_msaa=true`。之前[投影/MSAA 记录](xr-projection-msaa-20260929.md)中的“生产 MSAA 未修”仅为旧时点结论。

## 验证与证据

[证据目录](evidence/xr-upscale-msaa-20260929/)保存精确身份、通过/负对照日志、设置及截图清单。大体积构建中间产物留在 `build/validation/xr-upscale-20260929/`。

| 验证 | 结果 | 范围 |
|---|---:|---|
| Foundation 超分实机 GPU | 1,572,894 / 0 | Off/FSR/SGSR × fullrate/FDM × linear/encoded，crop、翻转、左右隔离、gamma、RCAS 边缘、缩放差异 |
| 2592×2400 实际密度 shader | 4 个 pass 通过 | FSR/SGSR 注视中心 x=.25→.75；`gl_FragSizeEXT` 着色改变 6,220,800 像素，无非法色值 |
| Khronos validation 1.4.363 | 无 VUID/Error | probe-only layer chain 覆盖源码 Turnip；console 有一项 deprecated-setting warning |
| MSAA 游戏语义 | 39,230 / 0 | 2×/4×/8× |
| MSAA Off | 39,230 / 0 | 原关闭选项仍有效 |
| 双目 PP/颜色回归 | 17,456 / 0 | 原生产 shader、array slice 与 GPU 读回 |
| Foundation foveation CPU | 通过 | 数学和密度图 |
| Foundation FSR CTest | 2/2 | constants/source 与 builtin SPIR-V 一致性 |
| Android runtime Kotlin | 138/0 | 包括设置解析及错误值回退 |
| Android host/APK | 构建通过 | 最终包安装后核对全 SHA |

GPU 日志末行的 `draws=27 FDM=12` 取自大尺寸测试之前的统计快照；四个大尺寸测试另计，不把旧统计当作最终总量。synthetic gaze 验证的是 GPU 地图更新和着色响应，不能代替真实眼球输入验收。

### 游戏运行

Swan `PB3110PGL6240001G`，Beat Saber `CUSA12878`。首次权限面板阶段是 mono 黑图；重启后 PID19467/gen1/UUID45335cdcb990d1f8057a4496d25801b0，真实 FOCUSED 会话、左右控制器活跃，原生双目安全提示页 5184×2400。切换 FSR→SGSR→密度诊断→FSR，6734 flips 时仍 Running，随后由本轮 STOP 指令正常停止，未发现该轮 DeviceLost/GuestFault。

`game-fsr.png`、`game-sgsr.png`、`game-sgsr-density.png` 是同一设备姿态的独立帧，分别 producer 1073/2531/3279；不是逐像素相同场景或性能 A/B。密度诊断为真实 shader 粒度染色，已关闭。此轮首次游戏证据对应中间验证包 APK79e8bb14/hostb521d8dc/driver e118fd41；最终包身份与复测另见 `delivery-identity.json` / `delivery-status.txt`，不混用两套二进制身份。

最终 APK `cb153e15` / host `293af748` / JNI `064c3243` / 源码 Turnip `48adf9b6` 已安装并校验设备 APK 全 SHA。最终 GPU/validation/MSAA/双目 PP 测试再次通过。PID20848/gen1/UUID25e1c92391ea28883eb9b550295ff6ca 在 3550 flips 时 Running、FOCUSED，最终原图 producer1448 为相同的双目安全提示页；颜色与深度日志均记录 S:8。游戏保留运行，等待用户佩戴操作。

### 交接边界

- 最终设置：FSR1、Eye tracked、Low、Sharpness50；无有效眼动时固定中心。Beat Saber 保留左右 Move swap，只撤掉 MSAA 禁用。
- 原全局 2D scale 0.5 与 texture medium 不变，XR scale 1.0。原 `kgsl_preempt_rb`、`debug.spruntime.etfr.subsample=0` 及旧缓存/GPU snapshot CleanupPending 未动。
- 自有 probe 目录和 5 个截图设备副本已清理，密度诊断已关闭；本地证据保留。
- 未验证真实眼动、佩戴清晰度/合焦、左右握持与歌曲挥砍、长时间 GPU 稳定性。本轮没有声称修复旧血源 CP opcode/GPU hang，也没有完整游戏可玩或性能收益声明。
- 无 commit/push。Foundation 和 Turnip 的源码改动以及主仓既有 dirty 工作均保留。
