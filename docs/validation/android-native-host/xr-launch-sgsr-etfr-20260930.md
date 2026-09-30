# Launch 前置选项、SGSR、真实 ETFR 与高分辨率输出

本轮沿用 `feature/malos/swan_performance`，未提交。用户要求把普通游戏影院入口、render scale、超分等放到游戏详情的 Launch 前置面板，全部横向平铺，不增加独立 Display / XR 页。另按用户明确选择，提升的是 **XR 输出缓冲**，不是 guest 游戏源图分辨率。

## 前置面板

每个设置标题下方直接平铺互斥选项，当前项使用高对比色高亮，无下拉菜单。点击选项只修改草稿；按 Launch 原子合并到当前游戏 profile 后再启动，取消不保存。并发更新的其他 profile 字段不被旧草稿覆盖。手柄上下选择设置、左右切换，Cross 在选项上切换，在 Launch 上启动；选中屏幕外的行会自动滚动。

| 项目 | 选项 |
|---|---|
| 普通游戏显示方式 | 2D Screen / XR Cinema |
| 对应模式 Render Scale | 0.25 / 0.375 / 0.5 / 0.75 / 1.0 |
| XR Upscaler | Off (Bilinear) / AMD FSR 1 / Qualcomm SGSR 1 |
| XR Output Resolution | Recommended / High / Ultra |
| XR Foveated Rendering | Off / Fixed / Eye Tracked |

PSVR 标题显示固定的 `PSVR · Immersive XR`，不能从这里切回 2D。普通游戏切换模式后显示对应模式的独立 render scale，2D 与 XR 值互不覆盖。XR scale 默认仍为 1.0，非 XR 设置沿用原配置。SGSR 是当前 Beat Saber 每游戏选择，全局默认未改。

Swan 实机：从 TMNT 前置面板点 XR Cinema + SGSR，再点 Launch，启动到主菜单。运行状态 `upscaler=2`、`fdm_enabled=true`；原始游戏截图 2592×1458，系统合成截图可见左右眼的影院平面。该轮 PID29978 / gen1 / UUID95533ad2ba13f90accbd9f37c212903a 正常 `user_stop`。测试新建的 TMNT profile 已存证后删除，恢复原来没有每游戏覆盖的状态。

## XR 输出尺寸

启动时对两眼的 maxImageRect、OpenXR system swapchain 上限、Vulkan maxImageDimension2D 和 maxFramebuffer 尺寸取交集；SBS 宽度上限先除以二。推荐档采用建议尺寸（能力缩小时钳制），高档对宽高分别取推荐与上限的中间值，极高档取有效上限。投影由 OpenXR FOV 定义，不由输出像素宽高比决定。只在重启时重建 swapchain，不在运行中修改尺寸。

| Swan 档位 | 每眼像素 | SBS 画布 |
|---|---|---|
| 推荐（默认） | 2592×2400 | 5184×2400 |
| 高 | 3376×2976 | 6752×2976 |
| 极高 | 4160×3552 | 8320×3552 |

`eye_width/eye_height` 的 HMD 硬件报告仍保留 runtime 推荐值，未通过虚报面板尺寸改变 guest 投影；guest 源图仍记录 960×1080、颜色和深度 S:8。PSVR 仍用 Projection Layer，普通游戏仍用影院平面。

Launch 页使用相同尺寸算法查询实际 OpenXR/Turnip 能力。只建立短生命周期的 XR/Vulkan instance，不创建 XR session、VkDevice、swapchain 或输入动作；沿用 JniHelper，不建立第二套 JNI 环境。和游戏 XR 初始化串行化，避免并发 instance。Swan 拒绝非 VR Activity 的 `xrCreateInstance`（`current activity is not marked as an xr app`），所以在实际 XR 启动中缓存能力，按系统 fingerprint 和完整 driver identity 匹配，Launch 页回读后标注 `Per eye · last verified`。无缓存且查询失败时只显示档位名称，不显示猜测尺寸。每次游戏启动重新核查 runtime/设备上限。

中间验证包曾采用 90% 限制边、保留比例，实际为 3452×3196/眼。用户随后明确指定三档，旧 Near Maximum 已被替换。该中间包首次原图请求被旧 16,777,216 像素上限拒绝（SBS 为 22,065,184 像素）。修正为双目允许两个原有单眼预算（总计最多 33,554,432 像素、RGBA8 readback 128 MiB），单目上限不变；不降采样，仅显式截图分配。修后得到完整 6904×3196 PNG，但随后该会话 DeviceLost，见下方边界。

## 真实眼动验证

用户已授予 `com.picovr.permission.EYE_TRACKING`，并确认佩戴、左右移动视线。此项在原运行会话 PID20848 上完成，SGSR 已通过 DebugBus 切换生效；不是模拟 gaze。16 个样本覆盖约 15.79 秒：

| 计数 | 首次 | 最后 | 增量 |
|---|---:|---:|---:|
| 重建 draw | 2,798,281 | 2,799,471 | 1,190 |
| 使用真实 gaze 的 draw | 5,944 | 7,134 | 1,190 |
| 变更后密度图上传 | 6,067 | 7,257 | 1,190 |
| 有效 gaze | 5,331 | 6,419 | 1,088 |
| 无效 gaze | 1,272,830 | 1,272,830 | 0 |

每次样本均 `gaze_fresh=true`、`upscaler=2`、`foveation=2`、`fdm_enabled=true`。未佩戴阶段的负对照则 tracked 不增长、gaze 无效，回退固定中心。完整原始样本见证据目录。结合[上一轮 GPU 密度粒度验证](xr-upscale-msaa-20260929.md)，真实眼动输入和地图更新链路已得到实测；**作用范围仍是宿主 presentation/upscale pass，并非所有 guest draw 的 ETFR，也未测得帧率收益。**

## 验证和交接

- 尺寸算法：Mac C++20 + UBSan，42 项边界、单调性与能力限制检查通过。
- Android runtime：139 项；library：10 项，0 失败，覆盖 PSVR 强制 XR、模式独立 scale、草稿合并和手柄导航。
- Android host / APK 构建通过。最终身份、设备全包 SHA、运行状态和截图见[证据目录](evidence/xr-launch-sgsr-etfr-20260930/)。完整构建日志、大图和旧会话日志保存在 `build/validation/xr-sgsr-etfr-20260930/`。
- scrcpy 一次性截图本轮得到黑图（会话均已自动清理）；采用系统 `screencap` 核查合成结果，以及宿主原始 PNG 核查纹理。不能把 scrcpy 黑图当作游戏黑屏，系统合成和宿主图均实际可见。
- 旧 PID20848 的停止请求曾触发 `scePthreadSemInit` mapping preparation cancelled / WrongState，终态 GuestFault；发生于升级前的旧包停止阶段，证据已留存。没有把它归因到新的输出尺寸，也没有修改 HLE/VM 把错误隐藏。之后 PID28443、TMNT PID29978 的 stop 均正常。该停止取消错误仍待单独处理。
- 不宣称佩戴清晰度、合焦、物理左右手或歌曲完整体验验收；旧血源 CP opcode/GPU hang 不在本轮修复范围。原 GPU 属性与历史 CleanupPending 不动。

最终包 APK `c55c4793` / host `830759c5` / JNI `d8104cad` / Turnip `48adf9b6` 已安装并核对完整 APK SHA。Beat Saber PID2930 / gen1 / UUIDfc120955ca5d97f0b8b476680395abbf：SGSR、Extreme (`output_resolution=2`)，实际每眼4160×3552；原图请求 producer1216 得到8320×3552 PNG，后续1721flips时仍Running，最终正常 `user_stop`。该轮未佩戴，gaze走固定中心回退，不将它当作新的真实眼动测试。

停止后返回普通 MainActivity，Launch 页通过匹配缓存显示三档尺寸，实际 UI 层级与合成截图均核实。最终停在 Beat Saber Launch 面板，SGSR / Ultra / Eye Tracked 被选中，用户可直接更改或启动。全局 profile 逐字节不变，原 Move swap 保留；设备端本轮原图和 UI dump 临时文件已清理，能力缓存为功能数据保留。中途设备端新增的血源 profile 保留，没有覆写用户的选择。没有 commit/push。

### 本轮额外故障（未归因或修复）

中间高分辨率包 PID32441 在原图已写出后 `vkQueueSubmit ErrorDeviceLost`，内核同 TGID 有 GPU read translation fault（ctx52/tid32639），不能把图片生成成功等同于稳定性通过。源码和驱动未变的另一轮，伴有设备端手势输入的血源 CUSA03023/PID11567（推荐档2592×2400、SGSR、真实 gaze）也在等待 frame fence 时 DeviceLost/assert。该轮启动来源未向用户确认，本轮未发送血源启动指令。已保存匹配 native log、crash buffer 和首轮 dmesg，不把这两轮混为同一游戏或认定都是提高分辨率导致。三档 UI/能力读数可验证，驱动稳定性仍有独立未完成问题。
