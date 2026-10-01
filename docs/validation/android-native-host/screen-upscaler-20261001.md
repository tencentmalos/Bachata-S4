# 2D 屏幕超分：FSR1 / SGSR1（2026-10-01）

普通（非影院）2D 游戏此前只有双线性放大：Android 上旧桌面 FSR 被强制关闭，Foundation 的 FSR1/SGSR1 只在 XR 路径使用。本轮让 2D 屏幕复用同一套 Foundation 超分。

## 实现

- **设置**：新增 `gpu.screen_upscaler`（Off / AMD FSR 1 / Qualcomm SGSR 1，默认 Off，全局/每游戏，重启生效）。
  - 游戏详情 Launch 面板在 2D 模式显示 “Screen Upscaler”，XR 模式仍显示 “XR Upscaler”，两个键互不影响。
  - `XrRendering.resolve(..., xr=false)` 读取该键，锐度复用 `gpu.xr_upscale_sharpness`；foveation 固定为 0。
  - 经原有 `nativeSetXrRendering` 下发，JNI 接口不变。
- **宿主**：`Presenter::PrepareFrame` 在 2D（无 XR runtime）、非 HDR 且 guest 图像小于输出帧时，调用 `SpatialUpscalePass::Render` 重建到帧尺寸，然后进入原 post-process pass。
  - 帧尺寸已按游戏宽高比适配窗口，不会拉伸。
  - foveation 强制关闭，`FoveatedEye{}` 为空。
  - guest 图已大于或等于输出时跳过，不额外耗 GPU。
- **不与 guest 串行**：
  - 超分录入 presenter 本帧已有的命令缓冲，在 post-process 之前执行。不新增 submit、fence wait 或 queue 往返，不阻塞 guest 提交线程。
  - 每帧 slot 为 `frame->id*2`，只在 `GetRenderFrame` 已等过该帧 fence 后复用；Foundation `VulkanUpscaler` 内无 CPU 等待。
  - GPU 上仍是 guest 帧之后的一次全屏 pass，这是同队列的固有顺序。
- 运行时可用 DebugBus `xr_render filter off|fsr1|sgsr1` 切换。命令名沿用 XR，2D 同样生效。

## 验证

- **Kotlin**：XrRendering 5、RuntimeSettingCatalog 3、GameLaunchOptions 4，全部通过。新增 2D 键、XR 键隔离、默认 Off、非法值拒绝等断言。
  - `:core:runtime` 全量仅 3 项失败，即既知的 Windows 宿主可执行位/路径断言（DiagnosticExporter、TurnipPackageInstaller、RuntimeInstaller），干净树同样失败。
- **构建**：host、APK `59bf2b50` 构建通过，驱动 `10377f4e`。
- **Swan 血源 2D**：Launch 面板选 2D Screen + AMD FSR 1。
  - 服务日志 `upscaler=1 foveation=0 statusLayer=2`。
  - `xr_render status` 显示每个呈现帧一次 draw（3234 次 draw / 3229 次 present），`fdm_draws=0`。
- **同会话运行时 A/B**（诊所，30 FPS 封顶，各 30 s）：

  | 轮次 | Off | FSR1 | SGSR1 |
  |---|---|---|---|
  | 第一轮 | 28.41 | 28.64 | 28.77 |
  | 第二轮 | 28.63 | 28.77 | 28.56 |

  在帧率封顶下看不出开销。未做不封顶场景的 GPU 时间测量，不作性能收益声明。
- **画面**：输出 1920×1080，三种滤波的亮度、颜色一致，无二次 gamma。FSR1/SGSR1 的门框、壁灯、拱券雕花边缘明显比双线性锐利。截图仅保存在会话 scratchpad，未提交（含游戏画面）。
- **终态**：血源每游戏显示模式恢复为 XR Cinema（XR Upscaler 仍为原来的 SGSR），会话已停止；`screen_upscaler` 每游戏值留为 FSR1，只在 2D 模式生效。

## 未覆盖

- HDR（A2R10G10B10Bt2020Pq）仍为双线性。
- AYN 等手机横屏未实测。
- 其他游戏未测。
