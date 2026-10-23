# 呈现路径重构：swapchain 预旋转 / 一次写入 swapchain / 不用 ImGui 合成画面 / 空闲重画（2026-10-15）

设备：AYN Odin3（Adreno 830，面板原生竖屏 1080×1920，横屏使用），KH3 CUSA15072。基线 `e6b1e0f27` 加本地未提交改动；最终 APK `9c4b2ec8`，host `7c89aa24`（Turnip 为 `codex/a8xx-cmdstream-audit` 诊断构建，诊断开关默认关）。

## 结论

- **预旋转**：同一 APK，启动后 40–70 s 片头同一段，用 `debug.shadps4.pre_rotate` 交替 4 轮：

  | 轮次 | 预旋转 | SF 中游戏层 | GPU 忙碌（660 MHz） |
  |---|---|---|---|
  | 1 | 开 | DEVICE，transform 0 | 42.5% |
  | 2 | 关 | CLIENT，ROT_90 | 51.5% |
  | 3 | 开 | DEVICE，transform 0 | 42.3% |
  | 4 | 关 | CLIENT，ROT_90 | 50.7% |

  打开后 SurfaceFlinger 不再每帧用 GPU 合成，同频率下 GPU 忙碌少约 8–9 个百分点（相对约 17%）。关闭时 5 个图层都是 CLIENT，打开后都是 DEVICE：游戏 SurfaceView 带 ROT_90 是整屏退回 GPU 合成的原因。这一组是在下面的“一次写入”之前测的。

- **一次写入**：2D 窗口下 guest 图像在呈现 pass 里直接做后处理转换写进 swapchain，不再先写 `frame->image`。同一 APK，用 `debug.shadps4.present_direct` 交替 4 轮（片头同一段，均为预旋转开）：

  | 轮次 | 路径 | GPU 忙碌（660 MHz） |
  |---|---|---|
  | 1 | 直接写入 | 40.7% |
  | 2 | 先合成 `frame->image` | 42.3% |
  | 3 | 直接写入 | 40.6% |
  | 4 | 先合成 `frame->image` | 42.3% |

  少一次全屏写，GPU 忙碌少约 1.6 个百分点。片头两种路径都是 60 FPS，不宣称帧率提升。

- **不再用 ImGui 合成画面**：游戏帧和 splash 都由呈现 pass 直接画，ImGui 只画状态层、对话框、devtools 这些叠加层。

- **空闲重画**：状态层可见但没被操作时，空 vblank 不再重画。奥林匹斯存档点约 21 FPS 时，每秒呈现次数等于游戏帧数（21–22/s）；打开 Controls 面板时为 47–50/s。

## 原来的问题

1. **swapchain 固定 IDENTITY。** `SetSurfaceProperties` 的注释说明这是为了与 azahar/citron 保持一致。Odin3 横屏时 `currentTransform = ROTATE_90`，IDENTITY 的 1920×1080 缓冲要靠合成器旋转。高通 DPU 不能旋转线性 RGBA 缓冲，于是 SF 把全部图层改为 CLIENT，在 GPU 上合成。SF 的合成在 rb0 上每帧抢占游戏：KH3 #11 的 trace 中累计约 247 ms/s 的驻留时间，并且每帧都要跨进程切换页表。
2. **游戏帧至少写两次。** `Compose` 中 pp_pass 先写 `frame->image`。`Present` 再在 DockSpace 里开一个 "Display##game_display" 窗口（黑色背景），用 `ImGui::Image` 把 `frame->image` 当纹理画进 swapchain，游戏尺寸取自这个窗口的内容区。这是上游 #2152（桌面 devtools 停靠）带来的写法。
3. **状态层让每个空 vblank 都重画。** `MustKeepDrawing()` 在状态层开着时恒为真，而 Android 上状态层默认开。帧率低于刷新率的游戏，每个没有新帧的 vblank 都会整帧重画并呈现一次，每次 SF 还要再合成一次。

## 现在的顺序

2D 窗口（不是头显、头显镜像或画布采集），每个新的游戏帧：

1. GpuComm `CaptureFrame` 交出 guest 图像（不改）。
2. present 线程 `PresentGuestImage`：
   1. 从帧池取一个槽（`GetPresentSlot`），只用它的栅栏和 FSR/SGSR 的槽位，不分配也不写 `frame->image`。
   2. acquire swapchain。
   3. `ImGui::Core::NewFrame`。游戏区域取 DockSpace 中央节点（`GetGameArea`，单位为帧缓冲像素）：桌面上是窗口减去菜单栏、状态栏和停靠的工具窗口，Android 上是整屏。中央节点是 PassthruCentralNode，ImGui 不在这里画东西。
   4. 只有需要重建（FSR1/SGSR1，guest 图像小于游戏矩形）时，重建 pass 先写一张自己的图。
   5. 状态层、对话框等照常提交。`PrepareDraw` 在 pass 之外结束 ImGui 帧并录制纹理上传。
   6. 一个 dynamic rendering scope：`loadOp = Clear`（不透明黑，即黑边）→ `PresentPass` 画游戏矩形（viewport 等于该矩形，只着色游戏像素；按预旋转取样；sRGB 解码、gamma 编码与原 pp_pass 相同）→ `RecordDraw` 画 ImGui 叠加层。swapchain 图像只写一次、不回读。
   7. 提交等待 guest 图像的 semaphore，并把 draw scheduler 的退休读者设为这次提交（与原 Compose 相同）。

没有重建时，guest 图像到屏幕只写一次。

**仍走合成路径（`Compose` 写 `frame->image`，呈现时复制）的情况**：

- OpenXR 头显及其窗口镜像（runtime 要一张图）；
- 画布录像和内嵌截图（读 `frame->image`）；
- PSVR SBS 帧（GPU 命令线程合成的双眼图）；
- 下文“有交互或动画”的时段；
- `debug.shadps4.present_direct=0`（桌面 `SHADPS4_PRESENT_DIRECT=0`），用作对照。

**splash** 由 presenter 自己解码成一张 RGBA8 图（`EnsureSplash`），用同一个 pass 原样画在游戏位置，隐藏后随调度器释放。NullGPU 提示画在游戏区域上的一个透明 ImGui 窗口里，它是 UI，不是画面。

## 预旋转

- `vk_swapchain.cpp`：`currentTransform` 受支持时 `preTransform = currentTransform`，90°/270° 时交换存储尺寸。`GetExtent()` 是存储尺寸，`GetPresentExtent()` 是逻辑尺寸，`GetRotation()` 是顺时针四分之一圈数。`debug.shadps4.pre_rotate=0` 回到 IDENTITY（对照用）。
- 游戏：`present_frame.frag` 按旋转取样，游戏矩形换算到存储坐标。
- ImGui：`RecordDraw` 在 CPU 上把顶点和裁剪矩形转到存储方向（四分之一圈把轴对齐矩形映射为轴对齐矩形），draw call 数不变。SBS 右眼的偏移按同一旋转换算。
- 触摸输入仍是逻辑坐标，不受影响。
- 带叠加层的截图读回存储方向的 swapchain，保存前在 CPU 上转回逻辑方向。

## 空闲重画

- **只是状态层可见**：不重画。状态层随下一个游戏帧更新；距上次呈现超过 250 ms 才刷新一次，游戏停住时数字仍会更新。
- **有交互或动画**（`MustKeepDrawing()`：对话框 `Layer::ShouldKeepDrawing`、状态层 mailbox 有新触摸或命令、Controls 面板打开或指针按住，即新的 `status_overlay_interactive`）：这段时间的新游戏帧走合成路径并保留，空 vblank 用保留的帧重画，不经过 GPU 命令线程。多出的那次写只在这段时间发生。
- **直接呈现后需要重画**（游戏停住时的 250 ms 刷新、桌面暂停）：窗口没有保留画面，由 GpuComm 重新捕获正在显示的 guest 缓冲（`RequestRedrawCapture` → `CaptureFrame(redraw=true)`，不消耗截图请求和回放帧）。PS4 上游戏在下一次 flip 完成前不会写正在显示的缓冲。捕获结果在下一个 vblank 合成并保留，之后的重画复用它。若期间完成了新的 flip（`displayed_serial` 变化）或有 flip 排队，就丢弃该次捕获并释放它对退休的占用。
- **触摸边沿**：交互之外帧率只等于游戏帧率，一次短按的按下和抬起可能在同一帧被取出。路由器在同一帧里看到按下再抬起，就不算点击：修复前 21 FPS 时 Controls 图标点不开。现在状态层每帧最多处理一个按下或抬起，其余留到下一帧，并在积压期间把自己算作交互中，以触发下一次重画。修复后连续三次短按依次打开、关闭、打开面板。

## 验证

- 两组对照的 dumpsys、GPU 采样和日志在本地 `build/validation/present-prerot-1015/`、`present-direct-ab-1015/`（不提交）。启动日志写明所选路径：`2D window frames drawn directly|composed`。
- 截图：
  - `screencap`：方向正确，颜色与合成路径一致，状态层在左上角，文字不镜像。
  - DebugBus `screenshot overlays`：1920×1080 横向。
  - 点状态栏可展开 Detail，Controls 面板可开关。
- 呈现次数：用 SurfaceFlinger `--latency` 统计游戏层最近 1.5 s 的实际呈现次数。约 21 FPS 时，闲置为 21–22/s，打开 Controls 为 47–50/s，关闭后回到 21/s。
- 构建：Android host 编译无新增警告，APK 构建通过。

## 未覆盖

- **PC**：桌面与 Android 走同一条呈现路径（没有平台分支）。本机没有 clang-cl/LLVM，也没有桌面构建目录，桌面未编译、未运行。devtools 停靠、HiDPI（帧缓冲缩放）、HDR、FSR（桌面 `fsr_pass`）、窗口菜单栏/状态栏的游戏区域均未实测。
- 有重建（FSR1/SGSR1）时仍要多写一张重建图。
- PSVR SBS 和 XR 镜像仍走合成路径。
- 重新捕获只在交互测试中间接经过，没有单独构造“游戏停住”的场景。
- 其他设备和方向：面板原生横屏的设备（`currentTransform` 本来就是 IDENTITY）不受预旋转影响；180°/270° 未实测。
- XR 输出（`PresentToXr`）未改。

## Azahar / Citron 需要同样修正

两者在 Android 上都固定 IDENTITY，竖屏原生面板横屏运行时同样会让 SF 每帧用 GPU 合成：

- Azahar（本机 `b10cd2d24`）`src/video_core/renderer_vulkan/vk_swapchain.cpp` 第 264–268 行：优先 IDENTITY，不支持时才用 `currentTransform`。
- Citron（本机 `607661351`）`src/video_core/renderer_vulkan/vk_swapchain.cpp` 第 259–263 行：`#ifdef ANDROID` 下 `preTransform` 固定为 IDENTITY（注释 "do not allow surface rotation to deviate from the frontend"）。

修法：

1. `preTransform = currentTransform`，90°/270° 时交换 `imageExtent`。
2. 最后一个 pass 按旋转取样。
3. UI 叠加层按同一旋转绘制，最好与游戏在同一个 pass 里。

确认方法：`dumpsys SurfaceFlinger` 的 HWC layers 表中，游戏层从 `CLIENT / ROT_90` 变为 `DEVICE / 0`。本次未改这两个仓库，也未在它们上面实测。
