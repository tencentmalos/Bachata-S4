# Beat Saber 设备启动证据（2026-09-18）

这份记录只描述本批 Android 设备启动边界，不把启动存活写成 VR 可玩结论。

## 环境

- 设备：`9c2841a4`，型号 `AYN Thor`，Android API 33；这不是 Swan。
- 游戏：Beat Saber PS4 CUSA12878，已导入完整本体树。
- APK：`app-fdroid-debug.apk`，SHA-256：
  `1e936348ff97e792a45567dfd4eee76e73dada874e071c8cffe31ca68a48e31f`。
- 图形路径：设备当前使用的 Turnip Android Vulkan 驱动；本记录没有切换 OpenXR，也没有连接 HMD/Move provider。

## 模块准备与导入审计

模块准备按以下顺序完成：

```text
eboot.bin
  → libSceFios2.prx
  → Il2CppUserAssemblies.prx
  → PS4Util.prx
  → libc.prx
```

实际审计共 1164 条：

| binding | 数量 |
| --- | ---: |
| `runtime_bound` | 617 |
| `guest_export` | 347 |
| `refused` | 186 |
| `not_relocated` | 14 |

`refused` 按库的重点分布为：`libSceHmd=1`、`libSceVrTracker=1`、
`libSceGnmDriver=38`、
`libScePosix=34`、`libkernel=33`。原始逐条审计见
[beat-saber-device-import-audit-20260918.json](beat-saber-device-import-audit-20260918.json)。
本次副本 SHA-256 为
`2fcc1f34ce9f4d0c29bffb54f91cfcb90baadd8f6a906a615ca3e884f9ad4fe2`。

## 实际运行结果

本批先修复了 Android session 文件描述符与桌面 HandleTable 不同的问题：Beat Saber
传入的第一个 guest 文件 `mmap` 现在能通过 session fd、host fd 和文件生命周期 pin
完成映射。随后设备启动实际走过取消状态、event flag、equeue user event、基础
VideoOut、SaveData memory 数据描述符和 Android 时钟回退。本批另外加入
HMD/VrTracker/Camera/Move 的 guest 记录校验桥接；由于 provider 尚未加载，设备启动日志
还没有把这些入口当作真实 HMD/Tracker/Camera/Move 调用来验收。

重新安装上述 APK 后，进程 PID 15740 在有界观察窗口内持续存活；日志持续出现
`sceGnmSubmitDone` 和 Oboe 音频回调，未出现新的 `SIGSEGV`、`GuestFault` 或
`ProductionRuntime terminal outcome`。这证明启动后的图形/音频循环已经存活。

本批 HMD 的浮点 `sceHmdReprojectionSetOutputMinColor`、Camera 8 个入口和 Move 7 个入口已在实际导入审计中变为 `runtime_bound`。
Camera adapter 只复制并校验 `Open/Config/Start/VideoSync/FrameData` 记录，明确拒绝
固定 PS4 camera pool 的 Android 映射；Move adapter 保留桌面初始化和句柄语义，查询、
状态、震动和灯球在没有控制器时返回桌面已有的 no-controller 结果。`runtime_bound`
在这里表示 guest adapter 已安装，不表示设备已经有 camera、Move 或 tracking provider。

仍然没有以下证据：可操作菜单、左右眼 SBS 图像、HMD 姿态、VrTracker 结果、
Move 输入、OpenXR session 或 Beat Saber 可玩性。`libSceIme`、`libSceMove`、
`libSceMouse` 的 `SysmoduleLoad=0x805a10ff` 是 provider 缺失的具名结果，应保留
为能力边界，不改成伪造成功。

## Android SBS 虚拟 HMD 复验

随后修正了 HmdSetupDialog 的 Android 导入准入条件：此前 NID 集合已定义，但准入分支遗漏
了该功能族，导致 7 个入口仍被错误记为 `unsupported_import`。最终复验 APK SHA-256 为
`b4c4829c5cb76cf23232025dd1eda429111515dbf30d743f40361e404a70c9bf`。修正后的最终设备审计（PID
`23765`）为：

| binding | 数量 |
| --- | ---: |
| `runtime_bound` | 624 |
| `guest_export` | 347 |
| `refused` | 179 |
| `not_relocated` | 14 |

Android logcat 记录 `vrSbs enabled=1`，说明 APK session 在 guest platform ready 前已经启用
陀螺仪 provider；`guest_vr_sensor_tests` 在设备上输出 `z=0.479060 w=0.877782`，四元数
积分误差 `0.000365`。但是该 30 秒有界窗口仍没有 `Lib.Hmd`/`Lib.VrTracker` 实际调用、第二次
guest flip 或 Draw/dispatch，屏幕仍是黑屏叠加层。因此这轮证明的是准入、生命周期和传感器
输入链路，尚未证明 Beat Saber 已经进入 HMD 生命周期，更没有 SBS 图像或可玩性验收。

原始审计和运行日志保存在 `build/psvr-sbs-20260918/virtual-gyro-20260918/`；桌面 session
仍保持 no-provider 语义，真实 PSVR/OpenXR provider、手柄和 reprojection 输出继续列为未完成。

## 后续推进

下一批按 PSVR 功能族闭环：恢复 HMD/Tracker/Camera 的结构体字段和 guest 地址
转换，核对句柄、回调、取消与 stop 顺序，再把 `SetDisplayBuffers`/multilayer
接入 presenter 的 SBS 输出。SBS 可观测输出成立后，才进入 Swan OpenXR provider
和设备生命周期适配。

## 2026-09-19 SBS 可见性复验

上述 2026-09-18 启动记录保留其当时的黑屏边界；后续 presenter 复验已越过该边界。连接的
AYN Thor 在 Android SBS provider 开启时，约 8 秒和 18 秒截图显示左右并排的 Beat Games
源帧；重新安装最新 APK 后，8 秒、18 秒和 48 秒截图继续显示左右并排的 Unity 启动画面，
状态层均为 `FPS 60.0`、`Guest flip 60.0`。证据截图位于
`build/psvr-sbs-20260918/visible-sbs-20260919/`。

为适配当前 Turnip 的 VideoOut alias 行为，presenter 从第一张有颜色的源帧建立一次受检 CPU
snapshot，经 utility buffer 缩放并复制到左右眼；后续黑色复用 buffer 不会覆盖该缓存，并会
周期性尝试采纳新的非黑源帧。该路径只证明 SBS 画面可见和提交稳定，尚未证明 Beat Saber
菜单/关卡可操作性、动态 reprojection、真实 PSVR/OpenXR provider 或手柄输入。
