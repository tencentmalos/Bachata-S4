# Status Overlay、运行时提示与 IME

当前只有 `foundation/third_party/imgui` 一份 ImGui 源码（含字体嵌入工具）。
StatusLayer 使用 Foundation OverlayShell 的 Simple/Summary/Detail/Controls；
`RuntimeTooltips` 是额外的 `ImGui::Layer`，用于短暂提示，二者共享 context 和 SDF atlas。

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
- Controls 可选 None / Only FPS / Summary、字号、FPS 位置、面板透明度与独立的 FPS 透明度；保存到
  UserDir 下的 `status-overlay.json`（`status_anchor` 0–3 对应左上/右上/左下/右下，缺省按平台默认）。
  不保存 live modal、IME 输入内容或指针捕获。
- Detail → Renderer 显示实际选用的 GPU：`名称 (类型, 第 n 块/共 m 块)`，用于区分集显/独显；
  机器上有多块 GPU 时同一行也出现在 Summary。

FPS 仍指实际新游戏画面，overlay 重画单独计数。CPU/GPU、倍率覆盖率、重传、tile/pass
及 GPU timing 保留原始含义和颜色；GPU 无样本时显示 unavailable，不补零。

Only FPS 复用 Cemu 最新接入所用的 Foundation Simple 组件：半透明紧凑条、FPS 数字、
Detail 柱状图图标与 Controls 滑杆图标。Summary 是独立模式。几何使用宿主实际 DPI，
桌面最小目标 32 dp，Android 48 dp；不会根据游戏渲染倍率改变触屏目标。

Android 点击边沿经带 owner 生命周期的有界 mailbox 送到渲染线程，再由 Foundation
路由命中面板。按下后即使移出面板，抬起也被捕获；失焦/销毁/溢出会取消。
PS4 IME 打开时独占触屏，直接进入现有 ImGui 输入框/键盘/关闭按钮，隐藏触控手柄后
仍可操作。同一帧真实 pointer 输入优先于同时到来的手柄导航；原有 PS4 映射不变。

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
