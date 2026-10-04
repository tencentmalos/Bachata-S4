# Status Overlay、运行时提示与 IME

当前只有 `foundation/third_party/imgui` 一份 ImGui 源码（含字体嵌入工具）。
StatusLayer 使用 Foundation OverlayShell 的 Simple/Summary/Detail/Controls；
`RuntimeTooltips` 是额外的 `ImGui::Layer`，用于短暂提示，二者共享 context 和 SDF atlas。

## XR Status Layer

Launcher 的 Launch options 提供 **Status Layer** 横向平铺选项：Horizontal / Vertical / None，默认 Horizontal，
启动时保存到每游戏配置 `gpu.xr_status_layer`；取消面板不保存。普通 2D 模式不显示此选项。

- PSVR：独立、只显示的 ImGui 状态层，固定在视野左上角；VIEW 空间双眼共享 quad。
  横向 1152×192（3 列 × 2 行，1.2×0.2 m），纵向 384×576（1 列 × 6 行，0.4×0.6 m）；
  均距眼 2.5 m，共用左上角 (-1.7, 1.1, -2.5) m。PSVR 标题的 2D 开场也不创建 PSV 模型/GI。
- 普通游戏影院：保留固定在 LOCAL 空间的 PSV 模型与原有主题/灯光/GI；
  Horizontal / Vertical 均保留机模，None 隐藏。旧布尔配置 true → Horizontal、false → None。
- 状态数据共用 Foundation PerfHud 的快照，游戏 FPS 不计状态层刷新。
  ImGui 在 PresentThread 最多每秒更新 4 次，合成器复用已释放图像；GPU 忙或
  swapchain 零超时等待未就绪时跳过更新，不等待状态层 fence。
- `xr_status visible on|off` 只作本会话开关，`xr_status status` 查看实际路径与更新次数。
  PSVR 状态条不接管手柄、鼠标或键盘。原有错误 ImGui 面板不受 Status Layer 选项影响。

初次创建字体/管线以及会话销毁仍可同步；同一 GPU 上的绘制与合成本身也有成本，
不能称零开销。原有 XR guest mailbox 同步未改变。
实现与实测见[PSVR ImGui 状态层](../validation/android-native-host/xr-status-imgui-20260930.md)。

## 操作

- Only FPS 条与 Summary 标题行是同一组控制：柱状图图标开关 Detail，滑杆图标开关 Controls；
  点 Summary 标题行其余部分切回 Only FPS。面板底部不再有额外按钮，也没有单独的“-”折叠。
- Detail 标题行与 Controls 相同：左侧标题，右侧 “-” 按钮隐藏 Detail（与 FPS 条的 Detail 图标等价）；
  Detail 宽度有上限，桌面小窗口下也不会占满整屏。
- FPS 位置可在 Controls → `FPS position` 选左上/右上/左下/右下；桌面默认左下，Android 默认左上
  （避开底部触控按键）。Detail 停靠在 FPS 的另一侧。
- 状态层只接受鼠标/触屏点击，不抢键盘和手柄焦点（Status/Detail 窗口 `NoNav`）；只有 Controls、
  IME 等模态面板接收导航输入。桌面鼠标点在面板上时不转发给游戏，移动不受影响。
- Android 暂停菜单的 `Status overlay controls` 可在状态面板隐藏时重新打开 Controls。
- 桌面按 F10 打开/关闭 Controls；PS4 IME 正在输入时不接管此快捷键。
- DebugBus：`overlay simple|summary|show|hide|detail|controls`、`overlay text small|medium|large`、
  `overlay status`。修改先排入渲染线程，响应含 `request: queued`，`overlay:` 是当前已应用状态。
  桌面经 TCP 发送时一条命令一个参数：`python scripts/debug/debugbus.py "overlay detail"`。
- Controls 可选 None / Only FPS / Summary、字号、FPS 位置与面板透明度（只作用于 Detail 与 Controls）；
  保存到 UserDir 下的 `status-overlay.json`（`status_anchor` 0–3 对应左上/右上/左下/右下，缺省按平台默认）。
- Only FPS 与 Summary 不画背景和边框，只有描边文字与曲线；Summary 末尾的 Detail、Controls 按钮常驻
  半透明底，悬停/打开时更亮（2026-10-04，见[记录](../validation/android-native-host/window-chrome-status-layer-20261004.md)）。
  不保存 live modal、IME 输入内容或指针捕获。
- Detail → Renderer 显示实际选用的 GPU：`名称 (类型, 第 n 块/共 m 块)`，用于区分集显/独显；
  机器上有多块 GPU 时同一行也出现在 Summary。

FPS 仍指实际新游戏画面，overlay 重画单独计数。CPU/GPU、倍率覆盖率、重传、tile/pass
及 GPU timing 保留原始含义和颜色；GPU 无样本时显示 unavailable，不补零。

Only FPS 复用 Cemu 最新接入所用的 Foundation Simple 组件，现为无背景的 FPS 数字。
Summary 是独立模式。几何使用宿主实际 DPI，
桌面最小目标 32 dp，Android 48 dp；不会根据游戏渲染倍率改变触屏目标。

Android 点击边沿经带 owner 生命周期的有界 mailbox 送到渲染线程，再由 Foundation
路由命中面板。按下后即使移出面板，抬起也被捕获；失焦/销毁/溢出会取消。
PS4 IME 打开时独占触屏，直接进入现有 ImGui 输入框/键盘/关闭按钮，隐藏触控手柄后
仍可操作。同一帧真实 pointer 输入优先于同时到来的手柄导航；原有 PS4 映射不变。

## 桌面窗口菜单栏与状态栏

窗口模式下（全屏时隐藏）游戏窗口有菜单栏与状态栏，参考 citron：

- 菜单：File（打开用户/日志/截图目录、Exit）、Emulation（Pause/Continue、Stop、Configure...）、
  View（Fullscreen、Show Status Bar、Status Overlay 子菜单、Reset Window Size）、Tools（截图、
  RenderDoc、Guest Patches 逐包开关、Mouse 模式、Reload Input Config、Developer Tools）、Help（About）。
- 状态栏：左侧 Status 模式、FSR/BILINEAR、VOLUME（点开滑杆）、PAUSED；右侧 Building N shaders、
  Scale、Game FPS、Frame ms，每 500 ms 刷新。Show Status Bar 保存在 `user/window-chrome.json`。
- 两栏在 DockSpace 之前绘制，游戏画面落在两栏之间；状态层的安全区避开两栏。点在栏上的点击不进游戏，
  菜单关闭后键盘还给游戏。实现 `src/imgui/window_chrome.{h,cpp}`。

## Summary 设备指标

CPU/GPU 频率、负载、温度及电池数据由 Foundation 工作线程每秒采样；渲染线程只读取快照。
Android 电池通过 Foundation C++ 调用公共 BatteryManager/Intent API，缺项回退 sysfs，
统一复用原有 JniHelper 的初始化与线程环境，不需要宿主 Kotlin 电池轮询。
GPU 主频直接读取 KGSL/Mali/devfreq 的可用节点；内核权限
禁止读取时不显示频率，不填假值。充电、数据缺失或状态未知时不沿用旧续航估计。
这次下沉已通过离线测试与 Android 构建，设备显示待验收，见[验证记录](../validation/android-native-host/summary-native-metrics-20260929.md)。

## SDF 与颜色

RGBA/Alpha8 font atlas 转为 Foundation SDF，字体 draw 绑定 SDF fragment pipeline；
图标/图片使用普通 RGBA pipeline。首次或新增字符时上传，静态帧不重复上传。
当前脏 atlas 整张替换并等待已提交 GPU 工作完成，不宣称部分区域更新或性能提升。
`Detail > Overlay renderer > Font mode` 显示请求/实际模式、atlas 上传数和 SDF draw 数。
字号使用解析后的像素尺寸；Property label/value 均支持可选 RGBA 颜色。

## 诊断记录

提示按 tag + 内容变化去重，最多同时显示 3 条，普通 4 秒、警告 7 秒；隐藏状态面板不会
禁用运行时提示。每条 `[OVERLAY_EVENT]` 同时进入普通 ImGui 类别日志和
`LogDir/guest-patch.log`（后者不受类别过滤影响）。

| tag | 含义 |
| --- | --- |
| `session.stage` | 被状态采样观察到的运行阶段 |
| `session.stop` | 被采样观察到的停止原因 |
| `session.fault` | 被采样观察到的终止详情 |
| `session.terminal` | 生命周期直接写入的 Stopped/Failed，不依赖渲染仍可用 |
| `present.stall` | 至少 1 秒无新游戏画面 / 恢复 |
| `texture.reuploads` | 跨过 4 次/帧重传阈值 / 恢复 |

采样提示带 `pid`、`generation`、`run_uuid`、`mono_ns` 和 severity；终止记录带 phase、
reason、detail。停帧可能是正常加载，不单独据此判定故障。日志不记录 IME 输入文本。
先按 PID/generation/run UUID 对齐，再与 [Guest GPU / PM4 trace](../debugbus-gpu-command-trace.md)
的身份和时间关联；tag 是定位入口，不是 GPU 根因证明。

## IME 所有权

IME 保持在 shadPS4：`src/core/libraries/ime` 与 `src/core/host_runtime/guest_ime*` 负责
PS4 ABI、UTF-16 长度、过滤回调、结果/原文恢复、语言和确认/取消键。没有移到 Foundation，
也没有新增共用 `emulation type`。Foundation 仅记录
[跨模拟器边界](../../foundation/docs/guides/emulator-ime-ownership.md)。

验证范围与实机剩余项见 [本轮验证](../validation/android-native-host/status-overlay-20260925.md)。
