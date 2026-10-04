# Android：设备休眠时直接启动游戏导致崩溃（2026-10-03）

设备：AYANEO Pocket DS（adb `01108YHE01017563`，Android 13），血源 CUSA03023。启动方式均为
`am start -n com.shadps4.android/.MainActivity -f 0x24000000 --es game_id CUSA03023`。
本地未提交。

## 1. 现象与根因

- **崩溃**（APK `1881c71d`，07:08，crash buffer）：`BackgroundServiceStartNotAllowedException`，抛出点 `SessionViewModel.kt:68` 的 `context.startService`。
  - 当时 uid 状态为 `TPSL`（top-sleeping）且已 idle：进程在后台常驻 35 分钟，启动意图在设备休眠时到达。
  - 活动先恢复、随即因休眠暂停，Compose 链路（导航 → DisplayModeGate → SessionScreen → `viewModel.launch`）仍继续执行，在后台调用了 `startService`。Android 12 起，idle 的后台 uid 不允许启动服务。
- **冷进程时不崩，但请求被浪费**（同一旧包，10:09）：新进程的 uid 还在宽限期内，系统允许启动服务；服务等 Surface 10 秒超时（`No live Surface for installed game`）后失败。唤醒后停在没有游戏的黑色会话页。
- **重放路径**：
  - 崩溃后任务仍留在最近任务里，基础 intent 带着原来的 `game_id`。从最近任务重新打开时，系统用这个 intent 加 `FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY` 重建活动，旧代码会再次启动游戏（实测确认 intent 为 `flg=0x34100000 ... (has extras)`）。
  - 进程在后台被杀后恢复时，导航栈恢复到会话页，`SessionScreen` 会再次自动启动游戏；同一进程内会话结束后活动重建，也会重新启动。
- **更正**：此前记录中“唤醒后会重放启动”的说法未经核实。07:20 的自动启动来自没有退出的后台脚本，不是 App 的重放。

## 2. 修复

- **只在前台启动服务**（`SessionViewModel` + 新 `ForegroundStart.kt`）：
  - 启动请求等到宿主活动处于 RESUMED 才调用 `startService`；休眠或在后台时请求保持挂起，回到前台后启动。
  - 设备进入休眠的瞬间，活动可能仍是 RESUMED 而 uid 已是后台：此时捕获 `BackgroundServiceStartNotAllowedException`，等活动离开并重新回到 RESUMED 再试；若活动一直停在 RESUMED，每秒重试一次。不会崩溃。
  - 活动销毁（返回、配置变化）时挂起的请求随之取消；配置变化后新的会话页会重新发起。
  - 门控用活动本身的生命周期（`LocalView` 的 ViewTreeLifecycleOwner）。最初用的是导航条目的生命周期，它要等进入过渡（Navigation Compose 默认 700 ms 淡入）结束才进入 RESUMED，唤醒后启动比现在晚约 0.5 秒（0.76 s 对 0.22 s），已改掉。
- **每个会话页最多自动启动一次**：成功调用 `startService` 后在 `SavedStateHandle` 记一个标记（随会话页的保存状态跨进程保留）。
  - 带标记的会话页恢复时不再自动启动；若此时会话状态为 Idle（进程已重建），提示 “The game session has ended” 并返回游戏库。
  - 用户主动的重试（停止报告页的 Retry、XR 错误层的重试）不受限制。
- **最近任务重启忽略启动参数**（`MainActivity.consumeLaunchIntent`）：带 `FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY` 的 intent 不再触发 `game_id` / `open_last_game`。
- **未采用前台服务**：Manifest 明确不声明 `FOREGROUND_SERVICE*`（Play 拒绝无合规用途的 specialUse FGS），`FexSessionService` 也没有调用 `startForeground`。

## 3. 验证

- **单测**（`ForegroundStartTest`，4 例）：未恢复时不启动；被拒后在暂停/停止期间不重试、重新恢复后重试；一直恢复时按 1 秒重试；活动销毁时丢弃。
  - 负对照：门控改为 STARTED 时 2 例失败（“暂停中启动了会话”、“暂停中重试”）。
  - feature/session 与 app 模块单测共 22 例全部通过。
- **设备**（APK `29269222` 为导航条目门控，`5d5818f7` 为活动门控）：

| 场景 | 结果 |
|---|---|
| T1 冷进程、熄屏时启动（两版各一次） | 活动恢复后约 25 ms 即暂停、随后停止；链路走到 DisplayMode 后等待，没有启动服务、没有崩溃。唤醒解锁后启动服务：导航条目门控 0.76 s、活动门控 0.22 s（从 onResume 算起），游戏正常运行 |
| T2 进程常驻、熄屏 75 s（uid idle）后启动（两版各一次） | 活动恢复后 5–29 ms 即暂停、随后停止，同样只走到门控，无崩溃。唤醒后 0.76 s / 0.06 s 启动 |
| 亮屏、App 在后台时直接启动（`5d5818f7`） | onResume 后 0.15 s 启动 |
| T4 会话页在后台时进程被杀（`run-as kill -9`），再打开 App（`29269222`） | 活动带保存状态重建，记录 `Not restarting CUSA03023 on a restored session screen`，返回游戏库，未启动服务 |
| T3 `am crash` 后从最近任务界面重新打开（`29269222`） | 系统用基础 intent 重建（`flg=0x34100000 ... (has extras)`），记录 `Ignoring the launch extras of a task restarted from recents`，停在游戏库，未启动服务 |

- T3、T4 涉及的代码在 `5d5818f7` 中没有变化。最终源码构建 `bb00a108` 只比 `5d5818f7` 多一行注释，已装机，未再跑游戏。
- 证据：[设备日志摘录](evidence/android-sleep-launch-20261003/device-log-excerpts.txt)、[单测结果](evidence/android-sleep-launch-20261003/unit-tests.txt)。

## 4. 边界

- “恢复态被拒后重试”的路径在设备上没有复现（几次实测中链路都在活动停止后才走到门控），只有单测覆盖。
- 旧包的 uid idle 热启动崩溃本轮没有重新复现：10:13–10:14 设备被操作（电源键唤醒、解锁、手势进入最近任务并划掉 shadPS4、回到桌面），很像有人在使用设备，但日志不能排除脚本注入。计划中的复现因此作废，测试脚本随后在亮屏时启动了一次游戏，已通过界面停止。崩溃证据为 07:08 的 crash buffer。
- 没有在 XR（OpenXrActivity）上验证；从游戏库“Launch options”面板启动的路径未上机（点 Launch 会新建该游戏的配置文件），它与直接启动进入同一个会话页门控。
- 游戏库导入服务的 `startService` 未改：只由前台点击触发。
- 测试前后设置文件（`files/settings`）未变；设备最后停在桌面，会话已停止，设备上的临时 dump 文件已删除。

## 5. 代码与产物

- `feature/session/.../SessionViewModel.kt`：`launchOnEnter` / `relaunch`、`SavedStateHandle` 标记、前台门控。
- `feature/session/.../ForegroundStart.kt`（新）：`startWhenResumed`。
- `feature/session/.../SessionScreen.kt`：传入活动生命周期；恢复态返回游戏库。
- `app/.../MainActivity.kt`：忽略最近任务重启的启动参数。
- `feature/session/build.gradle.kts`：单测依赖 `kotlinx-coroutines-test`。
- `feature/session/src/test/.../ForegroundStartTest.kt`（新）。
- APK：实测 `29269222`、`5d5818f7`；最终 `bb00a108`（host `f5d22fe4`，与此前装机相同）。
