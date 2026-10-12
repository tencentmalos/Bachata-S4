# XREAL One Pro + Eye：Thor 原生位姿诊断准备（2026-10-11）

用户要求先做好诊断再接眼镜（Thor 只有一个 USB-C），重点是型号识别与位姿，并详细留日志。
本次完成的是无眼镜准备与验证；**没有测到真实 One Pro / Eye 位姿，不代表 6DoF 已经可用**。

## 实现与来源

独立工具 [tools/xreal-probe](../../../tools/xreal-probe/README.md)，包名
`org.shadps4.xrealprobe`。没有修改 shadPS4 游戏相机、陀螺仪或渲染路径。

- 非 Unity 生命周期桥参考
  [Clancey/xreal-sdk 固定提交 7cd64886](https://github.com/Clancey/xreal-sdk/tree/7cd64886d4099228e2d1101b7621ab37e92c5da3)，保留 MIT 归属。
- 官方 [XREAL SDK 3.1.0](https://docs.xreal.com/Release%20Note/XREAL%20SDK%203.1.0)，下载档案
  SHA-256 `fd7d0fce103712e7acc85bfcbb8b127007e1d84f64596efb6c11580bea058d90`。
  3 个核心 `.so` 与 9 个 AAR 均经社区固定校验表逐文件核对，复制到 ignored build。
- 官方 Control Glasses `3.1.0.20251118115716` 已安装，APK SHA-256
  `49c9484ebd34e495984b219bb73988a366c03339f7251a9fe9a4702f58f5c76e`。
- SDK 内 `GetPluginVersion()` 返回 **`3.0.20251121`**；这是 3.1.0 发布包内该库的实际返回值，
  不据此把来源改记为 SDK 3.0。应用日志同时保存发布包清单与该返回值。

USB/屏幕只判定候选；SDK `GetDeviceType=11/12` 才确认为 One Pro M/L。
Thor 自带 `Screen-2` 不作为眼镜证据。使用 XR-plugin 的 16 float 完整矩阵接口，
避免社区报告会稳定横滚的 compact pose 路径；不在同一次查询混用两条接口。

本地核对 SDK `InputManager::GetHeadPoseAtTime` / `GetHMDTimeNanos` 的导出实现：
成功写结果时返回 1，未初始化返回 0。有效性还要求状态、原因、矩阵合理性和查询时钟检查。
HMD 查询时钟不是传感器样本时间戳，不能证明硬件样本更新；代码和日志明确区分。
实际矩阵轴向、横滚、平移尺度需接眼镜后验证。

## 设备与产物验证

Thor（Android 13 / API 33）安装诊断 APK，最终 SHA-256：
`4347a73e59566d60e61b3ec31063a5515c35cb9e941d7482dd383d6cab6070f5`。
应用自身计算 installed base APK 哈希并写日志，与本地文件一致。

已观察：

- Gradle Java/NDK 编译及 APK 打包通过；所有 bootstrap、回调、位姿与状态所需符号检查通过。
- 发现 Control Glasses 的 `ai.nreal.framework.net.binder.Acceptor` 服务。
  仅证明可解析到服务，尚未在有眼镜时建立会话。
- 两块内置屏幕均 `candidate=false`，状态 `WAITING_FOR_GLASSES`，`sdk_loaded=true`，
  `attempted=false`，位姿查询与有效查询都为 0；没有误报已连接或虚构 identity pose。
- 设备端 JSONL、SDK 自身 logcat 与 `latest.json` 持续写入；现场 ZIP 导出到 Downloads，
  拉回后 ZIP 完整性检查通过，6 个证据文件齐全。
- Stop 按钮后诊断进程与其 logcat 子进程均结束。
- 无线 adb 通过 shell 核对到同一台 Thor。现有 macOS adb daemon 直连 LAN 报路由错误，
  而 ping/独立 TCP 连接成功；使用 localhost-only TCP relay 连接成功，没有重启共享 adb daemon。
- `collect.py` 的 10 项有界采集均成功；tar/ZIP 摘要解析通过。用已知静止/平移矩阵、
  无效查询与截断末行检查了摘要计数、位置范围与错误行处理。

证据在本地 ignored `build/research/xreal/validation-no-glasses`、`validation-final`、
`export-smoke.zip`；当前连接、运行目录及 APK 身份记在 `active-device.json`。
SDK 与官方 APK 原件仅保留在本地研究目录，未纳入版本控制。

## 记录内容与接入后的验证顺序

日志包括 USB VID/PID/接口/授权、完整显示设备清单、网络接口、运行时服务、SDK 文件哈希、
安装 APK 哈希、每个初始化阶段、原生错误与眼镜事件、模式/质量/失踪原因、约 20 Hz 的原始矩阵，
以及前一次 Android 进程退出原因。JSONL 刷新与轮转、1 Hz 最新摘要、原生日志独立保留。
主机采集额外保存系统日志尾部、USB/Display/Activity/服务/退出信息。

接入后先确认 USB → SDK One Pro 型号 → Eye 插拔事件/能力 → Tracking，再分别测静止、
左右转头、抬低头、横滚、仅平移，以及遮挡 Eye 后丢失/恢复。选择 6DoF 或支持 position
都不构成实际六自由度测量成功的证据。

拔出立即清除可用位姿（仅凭候选消失时有 2.5 秒防抖）；新会话需 Stop 后重开，
不声称 SDK 同进程热重连已完成。参照社区已知的异步 DestroySession 竞争，诊断工具
通过结束自己的独立进程回收资源，不修改 SDK 二进制，也未加入信号拦截。
眼镜尚未接入，真实初始化、Eye 跟踪、拔出与恢复均待下一轮实测。


## 眼镜接入与运动验证（22:39–22:42）

用户在 Thor 上接入 One Pro + Eye，并完成静止、左右转头、抬低头、侧倾及约 20–30 cm
平移。USB VID/PID 为 `3318:0436`、产品名 One Pro；外接 `DP Screen` 1920×1080@90 Hz。
SDK device_type=11（One Pro M），tracking_type=0、state=2、reason=0。
SDK 日志出现 LeopardGrayscaleCamera、NRSLAM single camera high performance config
及约 15 Hz 的相机帧。RGB 插入回调未收到，不能据此判断启动前已接好的 Eye 缺失。

116 秒记录中共 2214 次查询；约 22:40:59.829–22:41:01.754 出现 22.25 cm 平移，
同期最大朝向差 4.30°，支持位移并非仅从旋转生成。启动前 17 次（约 0.87 s）为精确
单位矩阵，同时 SDK 报 pose_buffer empty，虽然仍声称 Tracking。两次位移突跳分别约
0.86 m/51 ms 与 0.65 m/50 ms；根因未确认，不能把 Tracking 状态视为质量保证。

原始记录仅本地保留在 `build/research/xreal/validation-motion-2242`、`motion-poses.jsonl`、
`motion-sdk-logcat.txt`、`before-foundation-refactor.tar`；此阶段安装 APK 仍是独立 probe
`4347a73e59566d60e61b3ec31063a5515c35cb9e941d7482dd383d6cab6070f5`。

## Foundation 提取与模拟器接入

父仓与 Foundation 均切到 `feature/xrealonesdk`。可复用的发现、Java/JNI SDK 生命周期、
原始矩阵、有效性检查、坐标转换与 native 日志下沉到
[`foundation/modules/xreal`](../../../foundation/modules/xreal/README.md)。Probe 只保留示例
宿主、权限/日志界面、采集脚本。官方二进制仍在 ignored 目录，不纳入仓库。

主 App 使用一个进程级 `XrealHeadTracking`，仅在打包了固定 SDK 且检测到候选设备后启动；
CAMERA 权限由宿主处理。轮询目标约 60 Hz（主线程 Handler，并非传感器频率保证），
原始/转换后 Pose 约 20 Hz 写 `files/xreal/<时间戳-PID>/poses.jsonl`，状态变化与 5 秒心跳
写 events，SDK native 阶段/错误独立写 native，日志按 8 MiB 轮转。SDK 生命周期跨游戏复用；
退后台暂停，重新进入恢复。拔插/SDK 创建失败后需结束 App 进程重启；不调用已知有异步竞态
的 DestroySession，也不修改 SDK 二进制。

GuestVrSensor 在 SBS 模式单独选用 external head，不冒充 OpenXR 渲染会话。眼镜 Pose
按社区 display-pose 的行矩阵转置和 Y 轴基变换转为右手 Y-up/-Z-forward。首次有效样本
建立位置原点和 yaw 中性方向；HMD Recalibrate 只重置头部，Android 传感器重启只重置
手柄。SDK 初始 identity、非法姿态、500 ms 未更新不作为有效头追。50 ms 内约 0.8 m
这类突跳（>0.35 m 且 >8 m/s）重新衔接位置原点，计数在 `xr_tracking status` 暴露，
原始数据仍保留。这是连续性保护，不是修复 SLAM 或硬件时间戳缺失。

Thor 的融合陀螺仪默认作为右手柄，左手未追踪；右手位置为虚拟座位坐标
(0.08,-0.17,-0.48) m。DS4 使用右手姿态与已有 grip-to-pad 偏移，位置质量标为 Partial；
Move 的第一个逻辑设备默认对应右手。眼镜转动/平移不会移动这个虚拟手柄锚点。
游戏按键仍来自 Thor 自身按键。仅模拟控制器朝向，未宣称 Thor 具有 6DoF 控制器位置追踪。

构建（JDK 17/NDK 29）：

```sh
python3 foundation/modules/xreal/tools/prepare-sdk.py build/research/xreal/sdk/package foundation/modules/xreal/build/sdk
cmake --build build/android-host-api33/native --target shadps4_host guest_vr_sensor_tests -j 8
./android/shadps4-app/gradlew -p android/shadps4-app :app:assembleFdroidDebug --offline \
  -Pshadps4.xrealSdk=/absolute/path/to/foundation/modules/xreal/build/sdk
```

未带该参数的普通构建不包含官方 SDK。验证：Foundation 数学测试 70/0；Mac 上
GuestVrSensor 测试 192/0（包括两路隔离、两种重置、位移突跳、超时、OpenXR 优先级）；
Android native host 与主 App APK 编译通过。设备与游戏结果见后续记录。


## 主 App 设备验证（23:10–23:13）

安装 APK SHA-256 `dcfd88037525ae8534d788de3173ae9bc93ea90ebd2f31da491984e68a544c36`。
Thor 原生 `guest_vr_sensor_tests` 也是 192/0。已停止独立 probe，主 App 是 SDK 的唯一宿主。
主 App PID 20829，日志目录 `files/xreal/1791731440981-20829`；启动 SDK 约 10.35 s，
主线程同步启动暂有等待，后续轮询约 60 Hz。约 88 s 内记录 5176 次查询的 1543 个
降采样日志：1540 可用，最初 3 条 identity 被拒；model=11，6DoF Tracking。
本轮已记录样本中的最大相邻位移为 3.90 cm，未出现上一轮那种大跳。

通过真实 `pad status` 的已写回 guest 输出确认（没有注入诊断 Pose）：

- HMD 查询 4146 次，connected=true、Tracking、旋转/位置质量 Full；
- DS4 查询 2139 次，connected=true、Tracking、旋转 Full、模拟位置 Partial；
- 两路四元数不同；`xr_tracking status` 为 external_head=1、openxr=0、右手 active=1、左手 active=0；
- ASTRO 已进入人物/DS4 对齐页，截图约 29–30 FPS；外接 DP Screen 截图确认接收同一 SBS 内容。

这验证了 SDK → Foundation → Android JNI → GuestVrSensor → HMD/DS4 VrTracker 的真实链路。
未代替佩戴者验收轴向、舒适度或完整关卡；眼镜内部 SBS/3D 显示模式与光学校准仍需佩戴确认。
本轮保留游戏运行，交给用户试。头部/手柄中性参考在进入本局时建立；若在加载期间把眼镜
搬离佩戴位置，佩戴后重新进入本局可建立新参考。模拟右手没有位置追踪。

本地证据：`build/research/xreal/app-xreal-run.tar`、`app-pad-status-final.txt`、
`app-tracking-status.txt`、`app-sdk-logcat.txt`、`app-astro-ready.png`、
`app-glasses-display.png`。运行日志继续在设备上按界限记录。

## ANR 根因与异步生命周期修正（2026-10-12）

以上第一版主 App 的主线程设计已废弃。旧版 23:10:46 / 23:15:17 / 23:17:53
三次主 App ANR 都是输入/焦点等待超过 5 秒。其中 PID 23953 明确对应：主线程
23:17:48.459 进入 CreateSession，23:17:58.615 才返回，约 10.16 秒。
它在游戏库的 Activity.onResume 就创建 SDK，而且主线程轮询/写日志。
另一次 23:18:50 的 SIGSEGV 发生在 Guest-37 的 FEXMemJIT（tombstone_24），不是
上述输入超时的同一故障；暂不据此推断 SDK 导致了 Guest 崩溃。

修正：

- Foundation `XrealPoseTracker` 自有 `XrealPose` HandlerThread；SDK load/start/poll/
  pause/resume、硬件 inventory 和日志全部在该线程。低层 `XrealSession` 拒绝主线程创建
  和跨线程操作。主 App 与独立 probe 共用此实现。
- 主 App 只在前台、PSVR SBS、会话 Preparing/Ready/Running 三个条件同时满足时启用。
  游戏库、普通 2D、OpenXR、Stopping/结束、退后台都没有周期查询/扫描/心跳。
- 关闭时立即取消源/样本发布，SDK pause 异步执行。不等待 SDK、不 join；生命周期
  token 改变后丢弃旧请求的返回值。发布短锁不包围 SDK/Binder/文件调用。
- 只有一个在途查询，每次结束后再延迟 16 ms，不堆积查询任务。超过 500 ms 的查询结果
  不作为新鲜数据发布。guest 只读自己的快照，没取得位姿/数据超时则用中性头部 Pose，
  不等眼镜；默认位置质量 Partial，实时有效位置才是 Full。Thor IMU 仍独立模拟右手柄。
- SDK 保留进程级单实例、暂停复用。没有强行杀 SDK 线程，也没有调用已知有竞态的
  DestroySession；若 SDK 自身永久阻塞，只影响该专用线程，需重启 App 才能重建 SDK。
  SDK 自己派发的 Java 回调依然由 SDK 管理，不能仅凭这次改动承诺任何场景绝无 ANR。
- `adb ... shell dumpsys activity com.shadps4.android/.MainActivity debugbus xreal_status`
  直接读 requested/source/operation/scans/queries，既不唤醒 worker，也不调用 SDK。

同批修正中性姿态：首次有效 HMD 样本将初始 pitch/roll/yaw 全部归零，但位置轴保留
重力方向；绕世界竖轴转头仍是 yaw。VrTracker 登记头显时同步重置头部和 Thor 参考，
`xr_tracking recenter` 可在外部 SBS 下重新校准两路。普通手机传感器重注册只重置手柄。

基础验证：Foundation 70/0，GuestVrSensor 在 Mac 与 Thor NDK 均 208/0，覆盖无样本、
无效样本、过期样本的中性回退和有效样本恢复，以及两路隔离、倾斜初始姿态和重校准。
主 App/独立 probe 编译通过。首次安装后无眼镜 ASTRO 到标题页 30 FPS，HMD 原点
(0,0,1.5)、DS4 约 (0,-0.17,1.0)，退后台 external_head=0。该次未验证 DS4 对齐通过。
实际眼镜的新版 worker 启动/暂停/恢复验证另记，不能用无眼镜的测试代替。

本地证据在 `build/research/xreal/worker-validation/`。历史 baseline 录像
`calibration-baseline.mp4` 已完成并由 ffprobe 确认 H.264 / 1920×1080 / 174.168 s；
期间用户有操作，它只有视频，没有全源输入轨迹，不标为合格自动化试验。

### 新版无眼镜实测（09:14–09:19，Thor）

最终 APK SHA-256 `bc3ac4800b1a332a3666dc71fe688afca37c895fd628257d496f3aab15132b8b`。
PID 12983，CUSA12392 01.04 / generation 1 / run UUID
`e6d03de90a6b70ddad39e8b8e7979492`。冷启动 Library 481 ms，`xreal_status` 为
`worker=not_created scans=0 queries=0`。进入 SBS 后只有设备探测（当时 USB 接 Mac，
无眼镜），没有尝试 SDK 初始化。按 Home 前 scans=136；后台两次读数仍为 136，
requested/source=false、operation=idle；恢复后 scans=137。

使用同一版默认 HMD 和真实 Thor IMU，没有注入 head/ds4 Pose，仅在标题“继续”按一次
Cross 300 ms。已看到 DS4 对齐提示、橙色对齐进度、对齐页退出和机器人开场动画，
因此默认 Pose 的这个对齐步骤已通过。HMD/DS4 的 guest 位置约 (0,0,1.5) /
(0,-0.17,1.0)，两者位置质量 Partial 也能通过，未为绕过游戏而伪报 Full。
视频 `default-calibration.mp4`：H.264 1920×1080，128.415 s，已完成并独立解码；
`default-trial/` 记录按键意图、发布/guest 消费/释放、身份和前后截图。它是有记录的
人工校准试验，不是完整关卡自动化验收；只覆盖 runner 发出的控制，非全源输入轨迹。
录像与操作按 host 时间作近似关联。`alignment-video.png` / `intro-video.png` 为
录像 68 s / 88 s 的原始帧。

本轮系统 events 没有 PID 9078/12983 的 ANR 或 native crash；仍不能代替眼镜接入后
对 SDK 初始化/暂停/恢复的验证。新版独立 probe 已编译，未替换安装到设备。

本轮最后点击退出菜单 Stop，SBS/source/requested 全部变 false，operation=idle；
停止前后的只读采样 scans 均为 255、queries 均为 0，没有在结束会话后继续扫描。
SDK 未接设备，故这里没有把零查询误当作真实硬件 pause 成功的证据。

Stop 的 native phase 已到 6、input_token=0，但紧接着的系统截图仍留在最后一帧，
没有把它当作返回 Library 的视觉证据。保存后重启主 App 回到游戏库，留待眼镜接入复测。
