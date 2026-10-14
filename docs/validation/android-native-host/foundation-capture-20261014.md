# 截图/录像迁到 Foundation `modules/capture`（2026-10-14）

## 1. 目标

shadPS4 的内嵌截图、录像与 XR 双目合成原来是本仓自己的实现（`capture_recorder.cpp`、
`openxr/xr_capture.cpp` + `host_shaders/xr_capture.comp`）。SteamPSP（xrgame-native）做了同样的事，
两边已经分叉。本轮把这部分下沉到 Foundation `modules/capture`（`spatial::foundation_capture`），
两个宿主共用同一套 DebugBus 协议、编码器、XR 图层合成和 PC 旁观（`liveview.py --follow`）。
接入方式见 Foundation `docs/guides/emulator-capture.md`。

## 2. 版本

| 项 | 值 |
| --- | --- |
| 主仓基线 | `malos/main` `e6b1e0f27`，分支 `feature/malos/foundation-capture` |
| Foundation | `b977fef`（原 pin，`codex/prof-streaming-integrity` 末端）→ `dc8c18a`：在原 pin 上合入 `feature/malos/capture-producers` `b42ab4c`，分支 `feature/malos/shadps4-capture` |
| scrcpy capture SDK | `my_mcp_tools` `master` `83fa1fb` |
| Turnip | 预编译 `turnip-a8xx-reemit2`（`65b63f66`，与 `references/mesa-turnip` pin 一致） |
| APK | `playstoreDebug`，SHA-256 `607b7df37e9cb8caddd6190af1cc11394aa2ad08ed81e16a6bc5621717b94d4a` |
| `libshadps4_host.so` | Build ID `d69b3911a2b90d971aa50c4f819478e661d2fd4b` |
| `libshadps4_fex_session.so` | Build ID `e7307c574c406197d6fae3e20f68cac4bef4a32c` |
| 设备 | Swan（`B3110`），`Pico/swan/swan:16/BQ2A.260122.002-BP2A.250705.008/20260922015006:user/test-keys`，系统 6.2.0，boot id `6d4335a5-667c-426d-815b-1a82edaa0fbb` |

## 3. 改动

- `capture_recorder.{h,cpp}` 改为适配层：`ConfigureEmbeddedCapture()` 设定产品名 `shadps4`、canvas
  生产者名 `final_render_target`、属性前缀 `debug.shadps4.capture`、日志标签 `shadPS4`；
  `HandleEmbeddedCaptureCommand(command, args)` 转给 Foundation（截图目录仍是 app 的 captures）；
  `EmbeddedCaptureBinding()` 给出 presenter 用的 present 队列（带 `DrainSubmissions` 钩子）或 XR 用的
  graphics 队列。`CaptureRecorder` 包装 `FrameCapture`：每帧 `sync`/`acquire`/`record`，
  提交时挂等待/信号信号量，提交后 `present`。
- `openxr/xr_capture.{h,cpp}` 改为包装 `XrLayerCapture`：mailbox 图像经 `copyScreenImage`
  进入 Screen 槽（mono 或双眼并排），环境层、影院 quad / PSVR 投影、PSV 状态层按
  环境 → 屏幕 → 状态的顺序登记，再用宿主已定位的双眼 `compose(eyes)`。
- 删除 `host_shaders/xr_capture.comp`（合成着色器在 Foundation）。
- `vk_presenter.cpp`：截图请求从 `CaptureRecorder::TakeStill()` 取，回读完成后
  `CaptureRecorder::StillDone(...)`；XR 会话中选 canvas 不再强制走窗口输出。
- `diagnostics_commands.cpp`：`capture_screenshot` / `capture_video` / `capture_source` 与新增的
  `capture_config` 都交给 Foundation。

源文件共 9 个，+218 / −1310 行。

## 4. 行为变化

- **生产者按优先级接管**：XR 会话 > 2D 呈现画面。XR 会话进行时 canvas 也由它给出（mailbox
  图像），不再为截图把画面改画到 Android 窗口。2D 模式下只有呈现画面一个来源，选 `xr` 也给
  canvas（`source: shadps4.final_render_target`），不再挂起等待。
- **XR 截图为全尺寸**：每眼为运行时推荐尺寸（Swan 2592×2400，双眼 5184×2400）；录像仍为一半
  （每眼 1296×1200）。原实现截图也是一半。
- **新增 `capture_config`**：`video_bit_rate` / `max_size` / `max_fps` / `i_frame_interval`，与 scrcpy
  参数同义；`reset` 恢复默认。
- **直播**：`capture_video start_live` 由 Foundation 提供，`liveview.py --follow` 可跨越生产者切换
  （本轮未在 shadPS4 上实测）。

## 5. Swan 实测

工具：`capturectl.py --app shadps4`（`source`、`screenshot`、`record --seconds 6`）。进程 PID 31169
（两轮同一进程），每轮 2–2.5 分钟（14:10:55–14:12:51、14:13:28–14:15:57），最后 `STOP_EMULATION` 正常结束。证据在本机仓外
`xrgame-native-evidence/shadps4-capture-20261014/run1-tmnt`、`run2-astro`（游戏画面不入库）。

| 轮 | 游戏 / 模式 | 生产者 | 截图 | 录像（6 s） |
| --- | --- | --- | --- | --- |
| run1 | TMNT（CUSA50828），2D | `shadps4.final_render_target`（`FrameCapture`） | xr / canvas 均 1920×1080 mono，内容为主菜单 | 两段均 1920×1080，60.36 / 60.35 fps，0 跳帧 |
| run2 | ASTRO BOT（CUSA12392），PSVR | `shadps4.xr_composite`（`XrLayerCapture`） | xr 5184×2400 双眼合成；canvas 5184×2400 SBS（mailbox），两张都是标题画面 | xr 左眼 1296×1200 34.56 fps；canvas 5184×2400 33.35 fps；均 0 跳帧 |

日志：run1 `capture: frame producer attached (generation 1)`、`frame recording 1920x1080 at 60 fps`；
run2 `session generation 2, encoder surfaces available`、`still view pitch 10.1 deg, FOV up 43.0
down -43.0 deg`、`recording 1296x1200 (xr, file) from a 2592x1200 source`。两轮日志无 capture 错误。

## 6. 未覆盖

- 普通游戏在 XR 影院模式（环境层 + 影院 quad + PSV 状态层的合成）：Swan 上 TMNT 以 2D 运行，
  本轮没有影院场景的截图；PSVR 的 PSV 状态层本轮未开。
- PSVR 沉浸状态 quad、错误面板仍不在 xr 合成里（与原实现相同）。
- 直播与 `--follow`、录像中途失败的处理、`capture_config` 改参数后的录像。
- HDR / 非 8 位 swapchain 截图（宿主回读路径未改，仍明确报错）。
- 迁移前后的帧率 A/B；桌面构建不受影响（模块只在 Android + SDK 构建中存在），未另行编译。
