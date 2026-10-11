# Odin3 会话退出、状态层与启动修复（2026-10-10）

基线：`feature/malos/swan_performance`，`6730e0075b6d93a0494a01d3e5bf80e13d297cf7`。本轮提交包含 Foundation 的配套修改。设备为 Odin3 `6edb0593` / Adreno 830，Android 15。

## 最终界面约定

- 删除 Compose 的 System RAM；状态层只显示 `RAM 已用/总量 GiB`，无 RAM-APP / RAM-SYS。
- Android 读取 `ActivityManager.MemoryInfo` 的 `totalMem - availMem`，每 2 秒在后台采样；不把 `/proc/meminfo` 的另一种可用内存估算混进来。PC 继续用系统物理内存提供者。
- 旧状态层的 Linux 内存提供者按 `MemTotal - MemAvailable` 估算；旧 Compose 显示使用 Android 框架 API。两个计数的“可用”定义不同，因而可以同时出现约 10 GiB 和约 4 GiB。Android 的 `availMem` 不是进程占用，也不是承诺随时可无代价回收的内存。本次按用户选定的系统口径统一显示，不再用两组名字相近的数值并列。
- Only FPS 模式移除；旧 Simple 持久化设置迁为横向 Summary。点击读数切换横向 Summary / 与它等宽且紧贴其下的 Detail。独立 Detail 按钮移除。
- Controls 选择各项数值，包括是否显示 FPS；CPU/GPU 曲线及其勾选项移除，只留 FPS 历史曲线。Detail 的 GPU 计时保留数值，移除其曲线。
- Controls 入口只有图标，没有常驻或触摸悬停背景。
- Detail 使用四个一级折叠组：Performance、Rendering、Textures & scaling、Diagnostics；具体统计在二级折叠组，默认收起。
- **最终要求是无背景**：曾试用的 40% 背景已撤销。Detail 标题、属性名/值、说明和图表文字复用 FPS 的四方向黑色描边。与 Summary 共用 Controls 中的 Text outline 设置。面板及折叠标题背景均透明；设置面板自身的背景设置不受影响。
- 上述状态层实现为 PC/Android 共用代码；Windows 主机 SSH 不通，本次没有声称完成 PC 实机验证。
- **位置与边框（2026-10-11）**：状态层原先离安全区左上角 12 dp（Foundation `flat_margin_dp`），再加面板内边距 8×6 dp。Foundation 新增 `OverlayMetrics::status_margin_dp`（只作用于 Status 和贴在其下的 Detail，未设置时仍用 `flat_margin_dp`）与 `StatusAppearance::transparent_frame`（背景透明时保留主题边框，线宽随几何缩放取 1 dp）；shadPS4 设为 2 dp 并开启边框。Foundation overlay 单测 292 例 / 75,299 断言通过（新增 2 例，还原绘制代码后新例 2 项断言失败作负对照）。Odin3 APK `cd997564…` 实机截图：边框距屏幕左/上约 5 px，文字左缘约 22 px，但条高由 Controls 图标的 48 dp 触控尺寸撑开（约 110 px），边框离文字较远。随后按用户要求紧凑化：Foundation 新增 `summary_touch_target_dp`、`summary_padding_x_dp/y_dp`（Summary 行高/按钮尺寸与内边距，测量与绘制经 frame 取同一值）和 `transparent_frame_width_dp`；shadPS4 设为 20 dp、4×2 dp、边框 2 dp。Foundation `6cfa969`（分支 `codex/shadps4-online-content`），overlay 单测 293 例 / 75,309 断言通过。APK `f55bd9a7…` 实机：条高约 50 px，读数离屏幕左缘约 14 px，图标紧贴右边框。整条仍可点击，但 20 dp 低于 Android 推荐的 48 dp 触控尺寸。

## 会话退出和手柄

- MainActivity 真正结束时请求停止；显示模式交接不误停。
- 最近任务移除由 `onTaskRemoved` 触发停止，按 generation 幂等处理，Service 销毁不重复延长等待期限。
- Resume / Stop 面板支持十字键、HAT、左摇杆、Cross 确认、Circle/Options/PS 关闭；按边沿导航，持续按住不逐帧跳选。面板打开时不向游戏漏发对应输入。
- Stop 只有在当前 generation teardown 完成并报告 `user_stop` 后返回 Library；不把启动、重试和后端故障混为主动停止。

## 游戏启动修复与边界

| 游戏 | 查明并处理的启动问题 | 验证边界 |
| --- | --- | --- |
| ASTRO BOT / CUSA12392 | 缺失 `libSceNgs2` 导致 Sound.cpp 断言；恢复完整固件包 | Odin3 到标题、约 30 FPS，累计 3769 帧；未进入关卡/PSVR 校准 |
| One Piece Grand Cruise / CUSA11396 | 缺少内核模块枚举/信息查询；PKG 内已准备的 PRX 被按宿主普通路径解析而报 ENOENT，使 Unity 初始化回调为空 | 查询族设备测试通过；修正 PKG LoadStartModule 分支已编译安装，未完成可靠的进游戏复测 |
| Kingdom Hearts III / CUSA15072 | 缺失 `libSceJson2`；补齐后到标题和 loading | 后续 GPU hang 尚未修复，见独立 snapshot 记录 |

一次本地测试 APK 使用 `bundleFirmware=false`，空固件索引会让旧安装逻辑删除此前归 APK 管理的模块，造成额外回归。已修正：空索引直接保留原文件和所有权记录；正式测试 APK 恢复完整 12 个模块。不删除用户自行添加的固件。固件子仓保持父仓跟踪的提交 `802771d93cff218621c0746548e0b3c1315496e9`。

模块查询实现包含 GetModuleList/List2、GetModuleInfo/Info2、GetModuleInfoInternal；从准备好的 Linker 图返回 guest handle/结构体，通过 guest 地址空间校验读写，沿用桌面的系统模块过滤、容量不足和错误码行为，不返回伪造成功。

## 验证

证据目录（仓库忽略）：

- `build/validation/android-exit-panel-memory-20261010/`
- `build/validation/odin3-three-games-20261010/`

已完成：

- Android host 与 PlaystoreDebug APK 构建。
- Foundation overlay：最终描边版本 **290 个测试，75273 个断言通过**；覆盖透明 Detail、描边存在、展开/收缩仍可点击、悬停不额外着色/弹提示。
- 模块查询设备测试：**1832 项、0 失败**。
- Resume/Stop 与退出设备 instrumentation：**3/3**。
- 空固件索引保留测试：**1/1**。
- 先前 Android 单元测试：183 项通过。
- ASTRO 实际点 Stop 返回 Library、watchdog 无 guest 线程，已截图并保存停止日志。

最后一轮透明描边/仅 FPS 曲线按用户要求构建安装后停止；未再次启动游戏验收该版视觉效果。23:31 的 recorder-off 对照被另一个 Turnip CTS 任务强制结束，不能作为“关闭 recorder 消除 GPU hang”的证据。之后按用户要求停止设备复测。

最终 APK：`android/shadps4-app/app/build/outputs/apk/playstore/debug/app-playstore-debug.apk`，SHA-256 `62618071029bc5b7d2e5fb0b152133a3bf5d1ed720d2502bfb5603b83ea64597`；打包成功，APK 索引含完整 12 个固件模块。

本地构建目录曾锁定旧 Turnip 预构建，现已清空 `SHADPS4_TURNIP_PREBUILT_LOCK`，按当前子仓源码构建 `2b8ed3fc20`；运行日志确认加载 SHA `ba53ae858c03dc7a0760d4cf6a81d6f32cca519514b8fb0900a5151d734c19b2`。更新驱动没有消除 KH3 故障，不视作根因修复。
