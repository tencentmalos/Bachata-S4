# 状态层去背景、窗口模式菜单栏与状态栏（2026-10-04，本地未提交）

用户要求：status layer 的 FPS 与 Summary 不要背景，两个按钮半透明；参考 citron 为窗口模式提供菜单栏和状态栏。

## 1. 状态层（桌面与 Android）

- Only FPS 与 Summary 不再画面板背景和边框，只有文字（带描边）与曲线；Summary 末尾的 Detail、Controls 两个按钮常驻半透明底（hover/打开时更亮）。Detail 与 Controls 面板保留背景，透明度仍由 Controls → “Panel opacity” 控制。
- 去掉了已无意义的 “FPS opacity” 与 PerfHud 页的背景透明度滑杆；旧的 `status-overlay.json` 中 `fps_opacity` 被忽略。
- Foundation（`modules/imgui_overlay`）：
  - `StatusAppearance::button_opacity`：设置后 Summary 两个按钮空闲时也填充该透明度；未设置保持原行为（其他宿主不变）；
  - 状态层背景透明度为 0 时不再画边框（原来至少留 20% 的边框，等于在空处画一个框）；
  - Only FPS 文字跟随 `text_outline` 描边。
- shadPS4：`status_overlay.cpp` 设 `background_opacity = 0`、`button_opacity = 0.4`，FPS 条 `simple_background.a = 0`。
- 触屏上点一下切换模式后，Summary 后面仍有一层浅色块（用户在 Thor 上发现）：那是可点区域的悬停高亮。触摸抬起后 ImGui 指针仍停在抬起处，该区域一直算悬停。现在 `StatusOverlay::ReleasePointer` 在触摸结束（抬起、取消、IME 切换、画面尺寸变化）时先发松开、再把指针移出屏幕；松开排在移动之前，点击仍落在原处。桌面鼠标不走这条路径，悬停高亮照旧只在光标停留时出现。

## 2. 窗口模式菜单栏与状态栏（桌面）

参考 citron（`src/citron/main.ui`、`main.cpp`）：菜单 File / Emulation / View / Tools / Help；状态栏左侧为可点开关、右侧为读数，读数每 500 ms 刷新；全屏时两者隐藏；渲染区在两栏之间，游戏不画在栏下。

实现：`src/imgui/window_chrome.{h,cpp}`（只编进桌面，Android 列表中移除）。

- 在 `ImGui::Core::NewFrame` 中、游戏画面所在的 DockSpace 之前绘制：主菜单栏与底部 `BeginViewportSideBar` 缩小视口工作区，游戏画面窗口随 DockSpace 落在两栏之间，`SetExpectedGameSize` 按新区域取帧比例。
- 菜单：
  - File：Open User Folder / Open Log Folder / Open Screenshots Folder / Exit（弹出退出确认）。
  - Emulation：Pause ↔ Continue、Stop（退出确认）、Configure...（游戏内设置）。
  - View：Fullscreen、Show Status Bar（保存在 `user/window-chrome.json`）、Status Overlay ▸ Off / Only FPS / Summary（当前模式打勾）/ Detail / Controls... F10、Reset Window Size ▸ 1280x720 / 1600x900 / 1920x1080（指游戏区，窗口高度自动加上两栏）。
  - Tools：Capture Screenshot（无/含叠加层）、RenderDoc Capture（已加载时）、Guest Patches ▸ 已装包逐个开关、Mouse ▸ 摇杆/陀螺仪/触摸板、Reload Input Config、Developer Tools（Ctrl+F10，原调试菜单栏，菜单项接在本菜单之后）。
  - Help：About shadPS4（版本、分支、提交）。
  - 菜单动作与热键共用同一组 SDL 事件（新增 `SDL_EVENT_RESIZE_WINDOW`），热键不依赖菜单是否显示。
- 状态栏：左侧 “Status: Off/FPS/Summary”（点击切换）、“FSR/BILINEAR”（本次运行切换，不改保存的启动选项）、“VOLUME: N%”（点击弹出 0–500% 滑杆）、暂停时 “PAUSED”（点击继续）；右侧 “Building: N shaders”（有待编译管线时，橙色）、“Scale: 0.5x”、“Game: 60 FPS”、“Frame: 16.67 ms”。管线计数为新增的 `PipelineStats::PendingBuilds()`（延迟构建提交时加一、驱动对象建成时减一，预载不计）。
- 输入：点在两栏上的鼠标按下与滚轮不转发给游戏；菜单关闭后把键盘焦点还给游戏（ImGui 导航停在菜单栏时会截走按键）。状态栏窗口 NoNav，点击不抢键盘。
- 状态层的安全区加上两栏高度，FPS 条与 Summary 不压在栏上；“Emulation Paused” 提示移到菜单栏下方。
- 另修：新文件改变 unity 分组后暴露 `input_mouse.cpp` 缺 `<algorithm>`。

## 3. 验证（桌面 `D:\workspace\shadps4-win-test`，血源 ZAR，exe `a1f6cddf`）

- 窗口截图：菜单栏与状态栏位置正确，游戏画面在两栏之间；Summary 无背景、两个按钮半透明；Only FPS 为无背景描边文字。
- 真实鼠标操作（电脑空闲约 13 分钟时，短暂前置测试窗口，结束后恢复原前台窗口与光标）：
  - View 菜单与 Status Overlay 子菜单（当前 Summary 打勾），选 Only FPS 后状态层与状态栏 “Status: FPS” 同步；
  - Tools → Guest Patches 显示 60 FPS、Sound bank reload fix 均已开启；点 60 FPS 关闭后立即 30 FPS / 33.44 ms，DebugBus 7 个 site `enabled=0`，再点恢复 60 FPS；
  - 菜单操作后马上按 F11 能切全屏，说明键盘已还给游戏；全屏时两栏隐藏、游戏占满 1920×1080，再按 F11 恢复；
  - Reset Window Size → 1600x900：客户区 1600×960，游戏区正好 900 行（上栏 30、下栏 30）。
- 发送给 SDL 窗口的 PostMessage 鼠标/按键在窗口无焦点时被忽略（SDL 用原始输入读鼠标按钮，键盘事件没有焦点窗口 ID），故界面验证用真实输入。
- 未覆盖：Configure...、RenderDoc、Mouse 模式切换、Open Folder 未逐项点；多显示器/高 DPI 缩放下的 Reset Window Size 未测。

## 4. 部署

- 桌面：`D:\workspace\shadps4-win-bb`（用户 Big Picture 快捷方式所用）换为 exe `a1f6cddf`；原 exe `4310a56c`、配置、合并版 60 FPS 包与 `status-overlay.json` 备份在 `backup-20261004-patchsel`。血源包改为 `bloodborne_60fps_v1`（`8248715c`，仅 60 FPS）与 `bloodborne_sound_fix_v1`（`08be1eb0`），每游戏选择两者，效果与原合并包相同。
- Android（AYN Thor）：
  - 13:50 安装前 Thor 断开 adb；13:54 有人在设备上上划进入最近任务、划掉 shadPS4（`forceStopPackage ... from pid` 启动器，非崩溃），并开合机盖（`WAKE_REASON_LID`），当时未安装。
  - 14:00 用户确认设备空闲后安装 APK `889d0be1`（host `13a94892`），备份存档后进入血源世界：Only FPS、Summary 无背景，按钮半透明。
  - 用户随后在设备上点切到 Summary，发现后面还有一层：即上面的悬停高亮。修复后 APK `8bb75583`（host `6d36b0b6`）14:13 安装（安装前再次备份存档），进入世界后用 `input tap` 依次切换纵向 Summary → Only FPS → 横向 Summary → 纵向 Summary，四张截图均无该层。模式保持为用户最后选择的纵向 Summary，游戏留在世界中。
  - 另：会话页左下角的 “System RAM …” 是 Compose 指示条（带 65% 黑色圆角底，会话菜单里 Show/Hide 切换，默认显示），不属于状态层，本轮未改。
