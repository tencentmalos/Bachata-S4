# Android guest shading rate 推进记录（2026-09-19）

本记录保留 shading-rate 单独验证时的状态；后续按用户要求恢复默认 1×1，并进入 [internal scale 实现与提交快照](internal-scale-20260919.md)。文中的“没有 commit/push”描述原验证批次，当前统一交付不改变其历史结果。

## 范围与当前结论

按照本轮要求，**FDM 不启用，后续仅留给 VR 路径另行设计**。普通游戏使用 `VK_KHR_fragment_shading_rate` 的 pipeline rate：低画质 2×2（理想着色密度 1/4）、中画质 2×1（1/2）、高画质 1×1。不缩小 render target 或贴图，不改变输出分辨率。这与后续 internal scale / ASTC 物理缩图是独立工作。

AYN Thor / Adreno 740 的 Turnip 和系统 Qualcomm 驱动均完成实际 GPU 输出回读测试，各 **35,630 项检查 / 0 失败**。这是着色粒度与状态切换的验证，不等于游戏帧率提升四倍或所有 GPU 工作减少四分之三。

## 修正之前接入的三个问题

1. 原实现同时启用 `fragmentDensityMap` 和 `pipelineFragmentShadingRate`，违反 Vulkan 的设备 feature 互斥约束。现在 FDM 固定关闭，相关 feature 从设备创建链移除，不启用 FDM 扩展；既有 FDM map / upload / PP 专用 pipeline 分支均不会运行。
2. 原 combiner 为 `REPLACE, KEEP`。实际只启用 pipeline feature，primitive/attachment feature 都关闭，因此必须为 `KEEP, KEEP`。REPLACE 还会丢掉 pipeline 请求的粒度。
3. 原来在 `Scheduler::BeginRendering` 设一次状态，同一 pass 的后续绘制不会重设；host helper 绑定静态 pipeline 后状态也可能失效。现在直接绘制、indexed 绘制、间接绘制及 indirect-count 路径，统一在 guest 图形 pipeline 绑定后设置。Compute、传输和 Presenter 不进入此路径；没有增加全屏三角形、blit、copy、附件或 GPU 等待。

依据：[VkDeviceCreateInfo，VUID 04481](https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceCreateInfo.html)、[vkCmdSetFragmentShadingRateKHR，VUID 04510/04511](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdSetFragmentShadingRateKHR.html)。

## 兼容性边界

- 按设备返回的具体 fragment size **和 sample-count mask** 选择。两套 AYN 驱动的 2×1 / 2×2 均支持 sample mask `0x7`（1/2/4 samples）；不把它误认成支持 8 samples。
- 不支持请求的粒度/sample 组合时使用 1×1；没有扩展时不创建 dynamic-rate PSO，也不调用 rate 命令。
- 要求逐采样着色、没有 guest FS、片元着色器写 buffer/image 或 image atomic 的 PSO 保持 1×1，避免减少 guest 可见的存储副作用。资源检查在 PSO 创建时做，缓存预加载使用还原后的 multisampling/资源信息。
- shader depth/stencil 输出、sample mask 等其他组合可能被实现进一步限制到 1×1；当前诊断日志反映请求选出的 rate，不能作为每个 shader 的实际 invocation 计数。
- 透明边缘、小图元、UI 和细纹理允许因主动降低画质而损失细节；未声称所有游戏的图像均已验收。
- hot path 使用已持有的 settings 对象读取 atomic override；无每次 draw 的额外 singleton mutex。每次 guest bind 后一条 dynamic state 命令，避免 PSO 数量按三档膨胀。

相关限制：[VkPhysicalDeviceFragmentShadingRatePropertiesKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFragmentShadingRatePropertiesKHR.html)。

## Android 设置和诊断

之前手工写到 `files/host/config.json` 的 `GPU.fdm_quality` 不会自动被当前 Android native session 读取，因此旧低画质配置不构成生效证据。本轮接通真正的 Android `RuntimeProfileStore`：

- 设置 → GPU → **Shading Quality**，三档；全局/单游戏均可保存，单游戏优先，下次启动读取，默认高画质。
- Service 在启动 native session 前，将枚举转换为 0/1/2，通过 JNI 写入渲染器使用的 atomic 值；损坏的 profile 拒绝并记录后回到高画质。
- `gpu.shading_quality` 是 Android profile id。历史 native JSON/TOML key `GPU.fdm_quality` / `fdmQuality` 保留兼容，含义是 guest shading quality，**不控制 FDM**。
- 下列 debugbus 命令只临时调档，不改持久设置；下次启动恢复 profile：

```sh
adb -s 9c2841a4 shell dumpsys activity service \
  com.shadps4.android/.service.FexSessionService shading_quality low
# low / medium / high / status
```

返回是请求档位，不冒充硬件测量值。首次遇到的 requested/selected pair 会进入主日志（有界记录），用于核对 guest draw 到达和回退。

## 定向验证

`android_fragment_shading_rate_probe` 使用生产 `Instance`、生产 sample-mask 选择与 `KEEP,KEEP` 设置函数，真实 Vulkan draw + staging readback：

- 96×32 输出分成三个 32×32 区域，同一个 rendering pass 内 guest → 静态 host → guest。
- guest FS 输出 `FragCoord`，每个区域 1024 像素。1×1、2×1、2×2 分别检查 1024、512、256 个不同结果，且每组恰好覆盖 1、2、4 个像素。
- 首尾 guest 档位不同，尾部用 indirect draw；中间 host 无 dynamic shading-rate 状态，始终输出 1024 个不同结果。覆盖高/中/低/非法档位、多次往返以及 full-rate 保护。
- 实际 GPU rasterization 使用 1 sample；MSAA mask 回退有选择器检查，不宣称已完成 2/4/8 samples 的 GPU resolve 测试。
- Turnip 与系统 Qualcomm 各 35,630/0，原始日志在 [Turnip](evidence/shading-rate-20260919/turnip-gpu-probe.txt)、[Qualcomm](evidence/shading-rate-20260919/system-gpu-probe.txt)。
- Android profile 优先级、默认值、三档 native ordinal、非法值与 catalog 校验：7 tests / 0 failures。

没有新增 RenderDoc 捕获，没有完整回归，没有改 Foundation/FEX 子仓库，没有 commit/push；保留本轮之前的 dirty work。


## TMNT 实机结果与交付状态

最终 APK SHA-256 `9560e2cb…`，host `af1ebce9…`（完整值见 [artifacts.json](evidence/shading-rate-20260919/artifacts.json)）。AYN Thor，Turnip Mesa `26.0.0-devel / 5ac41be677`，实际驱动 SHA `fdd37852…`。PID 6302 / generation 1 / UUID `f0bc76e2c1d03382eb9c7b1435dc3ccc`。

- 新 APK 启动前在真正的全局 profile 选择低画质，Service 日志 `Guest shading quality=0`；debugbus 请求值 0，实际 guest draw 选择 `2x2`，`FDM=off`。之后中/高档实际 draw 分别记录 `2x1` / `1x1`，见 [主日志片段](evidence/shading-rate-20260919/guest-selected-rates.txt)。
- 正常游戏流程进入屋顶，真实触屏左摇杆向右输入后角色移动、镜头跟随，教程 MOVE → ATTACK；本轮 warmup 明确视觉检查 frame 7/12，状态 `GAMEPLAY_REVIEWED`。只代表观察到的场景和操作，不代表通关/全游戏回归。
- 低档可见预期的像素细节损失。低档与恢复高档的图像均为正向单画面，未观察到新增翻转或多目复制。
- 同一进程、同一屋顶停止自动输入，先等 4 秒再采集约 20 秒，依次高 A → 低 → 中 → 高 B。GPU timing 全程同配置：

| 档位 | guest flip FPS | GPU.GuestFrame 平均 ms | Host PostProcess 平均 ms |
|---|---:|---:|---:|
| 高 A / 1×1 | 14.21 | 66.95 | 0.379 |
| 低 / 2×2 | 14.80 | 64.02 | 0.379 |
| 中 / 2×1 | 14.01 | 68.66 | 0.378 |
| 高 B / 1×1 | 13.45 | 70.60 | 0.384 |

原始计数和计算见 [measurements.json](evidence/shading-rate-20260919/measurements.json) 及同目录的 before/after 快照。GPU elapsed 包括设备等待，不是纯 shader on-core 时间。高 A 的 draw/flip 为 401.6，后三段约 447；高档重复基线也有漂移。因此本轮只能报告这些观测值，**不能归因出稳定百分比收益，尤其不能用 1/4 密度宣称 4× FPS**。主要开销仍需结合 guest compute、detile/拷贝、带宽与 internal scale 分别继续处理。

截图及完整 warmup 在本地 `build/validation/shading-rate-20260919/`，[截图路径/校验值](evidence/shading-rate-20260919/screenshots.json) 已归档。

结束时保留新 APK、Turnip、屋顶游戏进程；运行档位恢复高画质，原全局 profile 已逐字恢复（下次启动默认高档），自动输入关闭，GPU timing 恢复 OFF，ring 保持 ON，无 profiler 文件捕获/RenderDoc/native debugger。旧 APK 在安装前收到 UI Stop 后未及时结束，随后 force-stop 安装；该旧会话边界不归因于本次 shading rate，也不宣称本轮修复了停止流程。
