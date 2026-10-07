# AstroQuest 引入：不依赖 ASTRO BOT 的 PSVR 通用项（2026-10-07）

依据：[引入规划](../../specs/astroquest-reference-import-20261006.md) 的 D1、D4、C3、C4、C5、WP-G。参考 `references/AstroQuest` v0.18 `9ff3e43`（GPL-2.0-or-later）。本轮只有 host 单测与编译验证，**未上设备、未跑游戏**；ASTRO BOT（CUSA12392）不在本机，相关验证留待用户补测。

## 1. D1 VrTracker 校准状态与 DS4 结果

- `vr_tracker_devices.h`（新）：`PadRegistry` 最多登记 4 个 DS4，按登记顺序，第一个为玩家手柄；颜色按请求或取第一个空闲色；注销保持顺序。`Recalibrations`：`sceVrTrackerRecalibrate` 之后该类设备的结果报 CALIBRATING 200 ms，且无论游戏多晚来查，至少报一次（AQ `vr_tracker.cpp` 的规则：ASTRO BOT 1.01 起在就坐轮廓画面等 CALIBRATING 出现再消失）。
- `sceVrTrackerGetResult` 接受 DS4 handle：返回 OK、`connected=1`、灯色；目前没有 DS4 位姿来源（D2 未做），状态为 NOT_TRACKING，校准窗口内为 CALIBRATING。HMD 与 Move 只在本来 TRACKING 时才改报 CALIBRATING，不调用 Recalibrate 时行为不变。
- `Recalibrate` 对 HMD 要求已登记 HMD（原来 SBS 下未登记也返回 OK）。
- Android 准入 `ufexf4aNiwg`（RegisterDeviceInternal）、`tNJrfYsY3wY`、`24kDA+A0Ox0`（RegisterDevice2，固件从 `libSceVrTrackerFourDeviceAllowed` 导出，按该库名匹配）。`tNJrfYsY3wY` 的三参数 ABI（第三个为灯色）取自 AQ 对 ASTRO BOT 调用的解读，未与固件核对。

## 2. D4 时间域与 Move 加速度

- `vr_time.h`（新）：宿主 VR 样本统一为 steady clock（CLOCK_MONOTONIC）纳秒，换算到进程时间微秒时保留样本相对现在的年龄（预测位姿则为提前量）。VrTracker 的 `device_timestamp`、Move 的 `timestamp`（OpenXR 与非 OpenXR 两支）都改用进程时间，与 `sceVrTrackerGetTime` 同域。
- Android 陀螺仪事件的时间戳是 CLOCK_BOOTTIME，JNI 入口按当前 BOOTTIME−MONOTONIC 差值换成 MONOTONIC（设备休眠后两者不同）。
- Move 的 OpenXR 分支补加速度：静止时读数（重力反作用，LOCAL +Y）转到 grip 的机体坐标，量值沿用非 OpenXR 分支的 9.81。单位（m/s² 还是 G）未与固件核对，两支保持一致；不含线加速度。

## 3. C3 视角重置与头部状态

- OpenXR LOCAL 空间变更（`XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`）到达生效时刻时，在发布该帧位姿之前调用 `GuestVrSensor::NotifyRecenter`；Android 会话在 `sceSystemServiceGetStatus` / `ReceiveEvent` 时把新的 recenter 计数转成一个 `ResetVrPosition` 系统事件（会话开始前的 recenter 不算）。
- 头部线速度：runtime 未给 `XR_SPACE_VELOCITY_LINEAR_VALID_BIT` 时，由相邻帧位置差分得出（每次向新差分靠拢一半，间隔超过 100 ms 清零，recenter 时清零；AQ `vr_runtime.cpp` UpdateHead 的做法）。runtime 首次给出或改变 `velocityFlags` 时写日志，供 Swan 上确认。
- flip 到显示延迟由 1 µs 改为 13000/10000 µs（90/120 Hz，取自 AQ，非 PSVR 实测）。

## 4. C4 `VR_VIEW` 能力位与社交屏端口

- `GuestVrSensor::HeadsetReady()`：虚拟 SBS 头显，或正在运行的 OpenXR 会话。桌面与 Android 的 `sceVideoOutGetDeviceCapabilityInfo` 在此时置 `VR_VIEW`（0x20）。
- `VideoOutDriver` 新增社交屏端口（handle 2，`SCE_VIDEO_OUT_BUS_TYPE_AUX_SOCIAL_SCREEN`）：只在 HeadsetReady 时可打开；flip 提交即完成（更新 flip 状态、触发 flip 事件、释放上一缓冲 label），不交给 presenter；vblank 由主 vblank 折算为 60 Hz；flip label 放在驱动对象页主端口 16 个之后，GPU 的 EOP 写 label 照常。缓冲接受 `YCbCr420Bt709`（0x08322200，两平面 4:2:0），只在社交屏端口，不建图像。Close 改为按 handle 关闭对应端口；flip 完成与 vblank 发信号抽成 `CompleteFlip` / `SignalVblank`，主端口行为不变。
- 桌面在没有头显时打开社交屏返回 `INVALID_VALUE`（原来所有非主输出都断言）。GPU 回放的初始状态只保存主端口。

## 5. C5 外倾眼的平行包络

- 游戏按头部朝向渲染平行的双眼画面；把它当作外倾 θ 的眼的视图提交时，每个方向偏 θ。`ParallelEnvelopeFov`：以头部朝向提交该眼的投影视图，FOV 取在头部坐标系中包住该眼视锥的范围（四角投影的外接框），由合成器重投影到真实的外倾眼（AQ `openxr_view.h`）。
- 外倾超过 0.5° 的眼自动使用；每只眼的外倾角首次读到及变化超过 0.1° 时写日志（`XR eye N cant ... deg`），供 Swan 读出 θ。
- CPU 单测（`guest_vr_abi_tests`）：θ = 0、±5°、±10° 绕 Y 的外倾，眼视图方式在视锥 3×3 采样点上的最大角误差等于 |θ|，平行包络为 0（< 1e-4 rad），所有采样点都落在包络内；θ = 0 时包络等于原 FOV；4° 俯仰也被包住；45° 偏转使视锥角落到头部后方，返回空。

## 6. WP-G 触摸板模拟

- `src/input/moved_finger.h`、`stick_finger.h`、`pad_gestures.h` 原样取自 AQ（GPL-2.0-or-later）。
- `OrbisPadAdapter` 端口 0：每游戏设置 `input.touchpad_emulation`（Android System 页，默认关，重启生效）。右摇杆移动手指（StickFinger）；R2 按住即按下触摸板、R1 向前滑一次、L2 向后拉并在松开时放手（PadGestures），按住 PS 键时不触发。摇杆和按键照常送给游戏。屏幕 overlay 或 DebugBus 的真实触摸优先，并重置模拟手指。模拟手指随时间移动，游戏每次读手柄时刷新。
- 触摸 id：每次新落下的触摸分配新 id（1..127 循环，同桌面），overlay 触摸也改为如此（原来恒为 1）。
- 桌面 `GameController` 尚未接入。

## 7. 验证

- `guest_vr_abi_tests`（桌面 clang-cl 单独编译）241/0：DS4 登记、校准窗口序列、时间换算、静止加速度、外倾包络。
- `guest_vr_sensor_tests` 102/0：新增头部速度差分（90 Hz、1 m/s 收敛到 1 m/s）、runtime 给出速度时保留、recenter 不产生速度、间隔超过 100 ms 清零。
- Kotlin `TouchpadEmulationTest`、桌面构建与 Android host 构建见提交时的验证（下节）。
- 未做：设备与游戏验证（Beat Saber 回归、Swan 上 `velocityFlags` 与外倾角读数、社交屏端口与 `VR_VIEW` 对三款 PSVR 游戏的影响、触摸板模拟的实际手感）。

## 8. 剩余（规划中仍未做）

D2（DS4 位姿来源）、D3（相机节拍与 SocialScreen HLE，需先看 Astro 导入表）、C2（XR 驱动 vblank）、C7（会话丢失重建）、WP-I（卡死自动转储）、E3（NGS2）、E4 Android 麦克风、WP-H（ASTRO BOT 专用：时间步补丁包、governor、Android fiber 等）、WP-G 桌面接入。
