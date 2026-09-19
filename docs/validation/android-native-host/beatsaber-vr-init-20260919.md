# Beat Saber VR 初始化、Camera 固定帧与 Reprojection 直通（2026-09-19）

## 已核实的初始化卡点

本批先用精确构建绑定的 guest C/C++ wrapper 记录真实返回值，再修 HLE。设备为 AYN Thor
`9c2841a4`，不是 Swan；输出目标是 Android SBS，尚未接 OpenXR。

1. 基线注册了 `PlayStationVR` 和 `None`，实际读取顺序为 `[None, PlayStationVR]`。
   入口观察器命中 RegisterProviders/ReadDeviceNames/SelectDevice，未命中 PSVR Initialize。
   这只说明当前选择结果；游戏 managed 层为什么未继续切换仍未完全恢复。
2. 临时偏好 patch 保留原 selector、返回值与 fallback，只重排栈上副本。真实进入
   `UnityPSVR_Initialize +0xd6f690` 后，先遇到 sysmodule HMD provider 缺失。
   Android 现在按功能族发布已有受检 HMD/Tracker/Move/Camera/SetupDialog provider，真实 LLE 优先。
3. 下一个返回码为 HmdOpen `0x81110009`。设备信息此前 owner 为 -1；SBS adapter 现在使用
   session 已初始化本地用户（本机为 1000），打开返回真实虚拟句柄 `0x0f000000`。
4. 按用户要求接入固定默认 Camera buffer。游戏实际通过 Open、SetConfig、SetVideoSync、Start，
   再执行 Tracker 初始化、HMD 注册和 FOV 查询。
5. guest tagged log 精确记录 `sceHmdReprojectionInitialize(mode=2) = 0x81110016`，之后执行清理。
   该次运行尚未命中 CameraGetFrameData，不能用固定帧单测冒充游戏已经消费相机帧。

上述第 5 步构建：APK `a0ff8d331deab93f30b2c0a44fce65f6ff6fd373d9ba81eead6e6870271a1765`，
host `7adccd41fd6d7a9df4620395d5aabcf5fd3d8968d93df2a0ad23a07332f7bdf0`，
patch `5f1174aa68b6c2a073c3360c69e964b8a6ba4d6e497f844416a49eca96d846de`。
PID13215/gen1/run_uuid `2083f40bb58fbd5a9a0023801aaf74bf`。
完整导入 1164 行：632 runtime_bound、347 guest_export、171 refused、14 not_relocated；
VR 族 55 行中 54 bound、1 refused（StartMultilayer）。绑定不代表该入口语义完整。

## Camera 的实际实现边界

`GuestCamera` 复用桌面 ABI/config table，以 guest VM 动态映射代替桌面固定地址。
两块共享常量像素区域共 4,096,000 字节：RAW/Y8/Y16 为零，YUY2 为 Y=16、U/V=128。
支持两路、四级图像元数据、520/584 字节两种 FrameData，填充有效 guest 指针、帧序号和时间戳。
映射直到 session 结束才释放；Close/Reopen 不使 GPU 读者失去地址。包含配置、同步、开始、读取、
停止、关闭和内部别名共 14 个入口，错误参数、陈旧句柄、SBS 未启用继续返回具名错误。
这不是实际光学追踪或摄像头采集。

专项设备检查：Camera 71/0，sysmodule provider 73/0。没有完整回归。

## Reprojection 直通：正在验证

遵循用户明确选择，不实现 PSVR 光学畸变或姿态重投影。恢复 guest 必需的工作区校验、
显示缓冲区注册、start/end user event、提交与释放标签；双眼纹理由现有呈现 pass 采样。
无独立重投影 shader/pass、CPU 回读或额外眼图复制。正常呈现和必要同步仍有成本。

固件 11.00 `libSceHmd.sprx` SHA256
`c5005afcb8bddeaa6de23cd581eabe080087bb45b36c8a9dc89cf6af227f0e71`：

- 初始化 helper `+0x86d0` 确认参数 +0 为 0x810 onion 工作区，+8 为 1MiB garlic 区；两者不能相同。
- SetUserEventStart/End 保存 equeue 与 event id，可以在 Initialize 前注册。
- multilayer 层步长 0xa8；Unity 提供左右 GNM Image descriptor 的指针，根对象先复制再受检。
- submission +0 是 8 字节对齐的完成标签；`+0x169f0` 保存其地址，`+0x157ec/+0x16c1a` 在释放后清零。
  Android 必须等 GPU 读取与对应提交退休后清零，不能在 HLE 接收时释放。
- 直通目前面向一个颜色层、可选颜色 overlay；其他深度/额外层或不支持纹理格式具名拒绝。

新专项设备检查 29/0，包括延迟完成与 Stop 等待；尚未作为真实游戏图像验收。

## 证据与后续事项

[导入全量清单](beatsaber-vr-init-20260919/imports-before-reprojection.json)、
[guest 返回码事件](beatsaber-vr-init-20260919/guest-init-events.txt)、
[patch 命中](beatsaber-vr-init-20260919/guest-patch-before-reprojection.txt)、
[运行状态](beatsaber-vr-init-20260919/runtime-before-reprojection.txt)。

该次可见画面仍为旧 presenter 缓存复制的 Unity 标志，不能称为 Beat Saber 双眼场景或可玩。
正常 Stop 后长时间停留 Stopping，已保留 `build/guest-beatsaber-20260919/stop-timeout` 的状态和 logcat，
随后 force-stop 才清理进程；没有将这次停止标为通过。

用户追加的 Foundation `error_trace.zip / bug_dump` 在当前 VR 卡点处理后实施：故障后自动收集，
正常 runtime 可经 debugbus 主动抓取，覆盖 guest/主日志、可获取 tombstone、ringbuffer 和系统日志，
基础能力提交 Foundation main。界面 `Emulation Interrupted` 实际是 PS 按钮停止确认菜单，
不是单独的错误类别；应按 Fault/BackendFailure/停止超时等实际原因判断。

## 直通第一轮设备执行

APK `5167ae49aa97160ae76f0eb1f7753a4a713fd63907bed0c5ee346b26708c3f0e` /
host `b6e09931ce3fa3b5d084bef51d678c6827fa656b3a662728f47279bd797aca20`，
PID26343/gen1/UUID `1ec2839d13be4d24da43bc45b5228c7b`。
真实 guest tagged log 证明 ReprojectionInitialize 已返回 0；随后 fault 位于
`ktP9j1fN-zE`（VideoOutConfigureOptionsInitialize_），还未提交眼图。

重新全量核对 15 个 VideoOut 导入：14 bound、仅此项 refused。
静态固件 +0xd690 是 `memset(options,0,size)`；Unity +0xd757ec 请求 16 字节。
同时发现旧 Android ConfigureOutputMode adapter 把第四参数误当 size；正确顺序为
`(handle,reserved,mode,options,size_mode,size_options)`。本轮按族修正，并复用桌面
Mode 初始化和 HDR 配置逻辑，传入 host 栈副本；不对物理 Android 显示器宣称 PSVR 120Hz。
