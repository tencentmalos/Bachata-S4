# PSVR 文字模糊：镜像窗口误控制眼缓冲（2026-09-29）

用户反馈 Projection 版依然看不清文字。本轮在 Swan 复现并修复了额外降采样：**游戏每眼 960×1080 → 每眼 540×607 的中间图 → 最近邻放大到 XR 每眼 2592×2400**。上一轮仅核对最终 XR swapchain 尺寸，漏掉了这个中间环节。

## 原因与修改

`PrepareVrFrame` 调用 `GetRenderFrame(true)`；后者原来仍使用 `SetExpectedGameSize` 的窗口尺寸。Swan Android 镜像 Surface 的缓冲为竖向 1080×1920，ImGui 显示区再按 16:9 适配成 1080×607。这个尺寸原本只适合镜像显示，却被用作双目合成目标，每眼只有 540×607。

`Runtime::Publish` 随后把这张小图用 nearest 放大到 5184×2400 的 XR SBS swapchain。最终 runtime 报告的高分辨率无法恢复之前丢掉的文字细节。XR render scale 已经是 100%，并非配置未生效。

本轮修改：

- Runtime 提供实际已分配的 `FrameExtent()`。PSVR 双目帧按此尺寸分配和直接采样 guest 眼图，不再受 Android/ImGui 镜像尺寸限制。
- 发布时尺寸、格式相同就直接 `vkCmdCopyImage`。需要格式转换或影院 resize 的情况使用驱动支持的线性过滤，避免最近邻放大的方块边缘。UNORM mailbox→sRGB XR 的字节复制保留，避免二次 gamma。
- 增加有界源图/合成尺寸日志，记录 host extent、mip、array layer 和 UV。设备两眼实际读 layer 0 / 1、mip 0。
- 原有显式 PNG 截图只接普通 VideoOut，PSVR 请求会永远 pending。抽为共享 `RecordEmbeddedScreenshot`，双目路径也调用；同时修正原有读回屏障的布局为 PP 实际输出的 `GENERAL`。默认编码截图路径不变，PNG 仍是显式原图诊断路径。

## 同设备旧新验证

先装只增加日志/原图采集、保留旧分辨率的基准包，再装修复包。两个包均保持源驱动、游戏、render scale 和 texture quality 相同，未修改用户设置。

| 项目 | 基准 | 修复 |
|---|---|---|
| APK SHA 前缀 | c9020cd0 | a86181c4 |
| host SHA 前缀 | 3a9eb568 | c2e55e3f |
| PID / generation | 4437 / 1 | 7406 / 1 |
| Guest / host 原始眼图 | 960×1080 | 960×1080 |
| 合成前采样 | mip 0，左右 array layer 0/1 | 相同 |
| 实际双目 PNG | 1080×607 | 5184×2400 |
| 每眼合成目标 | 540×607 | 2592×2400 |
| 镜像预期尺寸 | 1080×607 | 1080×607，已不影响眼图 |
| render scale / texture | 100% / medium | 相同 |

原始眼图按 runtime FOV 取样的范围约为 877×797 texels（UV scale 0.91365×0.73838）；这是正确的视场采样范围，不应通过拉伸整个 PSVR frustum 来掩盖分辨率问题。

两个完整原图均已取回，尺寸由 PNG IHDR 和采集状态独立确认，manifest 保存 SHA：

- `build/validation/xr-resolution-20260929/before.png`
- `build/validation/xr-resolution-20260929/after.png`

修复后的语言菜单文字轮廓更完整。两次抓图的头部/手柄姿态不同，不作为相同像素的图像质量评分。随后系统合成截图已显示双手剑与教程的 “Put your sabers into these rings.” 提示；本轮没有注入游戏按键或手柄输入。不据此宣称歌曲完成或全部游戏渲染正确。

原有 1.0 表示保持游戏原始渲染尺寸；本次没有把 guest 的 960×1080 渲染目标强制提高到 Swan 原生分辨率。修复移除了额外降采样，源图分辨率仍构成细节上限。也没有调整合焦参数、双眼姿态、MSAA 开关或用户的 medium 纹理设置。

## 回归、证据与终态

- Android host、APK 和生产 PP probe 编译通过。
- Swan 使用同一份源码 Turnip `87aeb405…` 运行生产 PP 像素测试：**17456 checks / 0 failures**。包含新增的同格式直接复制，保留 UNORM→sRGB 误 blit 的负对照。
- 设备安装 APK SHA 与构建包一致；包内 host/JNI 的 SHA 见 manifest。
- 修复包记录到 PID 7406 / UUID `61745afe035d3ec7d6b765cbf72ce962`、7444 guest flips、Running；之后系统截图进入教程。没有设备丢失或 GuestFault 修复声明，旧 CP opcode 问题仍独立存在。
- 全局和 CUSA12878 设置与上一轮备份逐字节相同。每游戏 Force Disable MSAA 仍开启。
- 未增加持久调试属性；原 `kgsl_preempt_rb`、`etfr.subsample=0` 及旧 GPU CleanupPending 保留。自有 probe 目录和两张设备原图已清理，本地证据保留。无 debugger、forward 或注入按键，游戏继续运行供用户体验。

负结果也保留：旧包 PNG 请求缺失 PSVR 接入而 pending，已取消；旧分辨率下编码尝试报 MediaCodec configure/input Surface/start failed，已停止；首次像素 probe 缺少 staging 目录的驱动文件，启动即拒绝，补齐并核 SHA 后上述像素测试通过。这些不计作 GPU 执行失败。

[紧凑证据](evidence/xr-resolution-20260929/)；完整构建日志、原图及 SurfaceFlinger 原始输出在本地 `build/validation/xr-resolution-20260929/`。未 commit / push。
