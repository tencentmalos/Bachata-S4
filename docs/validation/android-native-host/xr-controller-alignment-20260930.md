# OpenXR 默认瞳距与 PSVR Move 原点候选（2026-09-30）

按用户要求撤掉 65 mm 试用覆盖，直接保留 `xrLocateViews` 返回的双眼位置/方向/FOV。HMD 启动尚无设备眼位时恢复原有 ±31.5 mm fallback；有效硬件眼位仍优先，不改头显物理 IPD 或系统设置。

手柄候选修正了原先直接把 OpenXR grip 写入 Move device_pose 的做法。HMD、眼睛和手柄仍在同一 LOCAL 空间、同一 predicted display time 采样；按键/震动及 Beat Saber index0右/index1左映射保持。新增 `MoveSpherePose`，在 grip 局部 -Z 前移 75 mm，并用 `v + angular_velocity × rotated_offset` 把线速度转换到相同点。orientation 与 body gyro 保持 grip；aim 仍只负责 XR UI。没有有效 orientation 时不宣称偏移点的位置有效。

## 定位证据与标定边界

- 当前 Beat Saber 2.04 eboot：`main+0x103af91` 查询 Move tracker；`main+0x103afd9..0x103b00c` 把结果 position/orientation/velocity 复制到每手缓存，`main+0x103b390` 尾部复制到公开缓存。这些路径没有给 OpenXR palm 自动补球心距离。地址来自该构建，不套用 1.00。
- PS4 11.00 VrTracker：`tracker+0x3e10` 按类型2分派 `tracker+0x3bb0`；后者将内部跟踪 position +0x28 直接放到 Move device_pose +0x80。`tracker+0xe02c0` 读 MoveDeviceInfo 的三轴 accelerometer offset 做内部跟踪校准。本轮未把未恢复的 IMU 参数伪造成实测。
- OpenXR 的 grip/aim 原点语义见 [Khronos specification](https://registry.khronos.org/OpenXR/specs/1.0-khr/html/xrspec.html)：grip 定义握持点，aim 是另一条定位射线，不能用 aim 覆盖 Move 来试凑。
- [PSMoveSteamVRBridge 源码](https://github.com/HipsterSloth/PSMoveSteamVRBridge/blob/master/src/main/cpp/driver/ps_move_controller.cpp) 的默认 sphere→holding displacement 为局部后移7.5 cm；这里采用其逆变换作为**虚拟 Move 的初始几何候选**。这不是 Swan 手柄实物测量，也未完成用户佩戴后的绝对标定。球心原点判断由上述跟踪路径与上游适配共同支持，但本轮尚未恢复所有内部光学 solver 的原点转换。

因此，本轮证明“直接 grip”缺少原点适配，完成可体验候选；不宣称用户反馈的偏后已经通过物理验收。若残余偏移随手柄角度变化，应进一步标定局部几何；若只在挥动时落后，还需单独测 guest 图像/控制器时序。

## dynamic 数据库

用户指出 PS4 数据库要用 dynamic。之前两次直接打开带 SCE 自定义 e_type 的插件（0xfe18）返回 `database_in_use` / `open_database=4`，不足以证明数据库被其他会话占用。用标准 ET_DYN 分析副本后，插件、当前 eboot 和固件都成功打开，imagebase=0，分别43/37048/4458函数。本轮未关闭或替换其他会话的数据库，所有自有 workspace 已释放。

`tools/ps4-guest-code/ps4_guest_code.py elf --dynamic` 已支持直接生成这种副本，CLI 输出与实际成功打开的插件逐字节一致；原始 ELF 不变，模块偏移不变。`addressing=relocatable` 仅描述地址来源，不是 loader 的 dynamic 开关。该要求已记录 AGENTS.md。

## 验证与设备状态

| 项目 | 结果 |
| --- | --- |
| Android host / APK | 编译通过 |
| Swan 生产 HLE 位姿回归 | 95 checks / 0 failures，exit0 |
| 同一测试、旧65mm包宿主负对照 | 95 checks / 12 failures，exit1（位置/速度/方向无效时的位置质量） |
| 旋转几何 | identity、pitch90、pitch180；双手yaw90、速度与左右映射已覆盖 |
| PS4 CLI selftest / dynamic CLI | 通过；成功数据库输入逐字节匹配 |
| 安装全包 / host SHA | 与构建相符 |
| 双眼坐标距离 | 63.519919 mm，Projection日志63.520 mm |
| 双手 grip→Move 距离 | 74.999848 / 74.999879 mm，截断误差 |
| Global / CUSA12878 JSON | 安装前后 SHA 完全不变 |

- APK `4ec9b9a6b0c5f985a393254074ad61773b8f77c87658b64eaeaeccfebdf6848e`
- host `d595d3947233ce795ef020fdc9e5bc34f5b533eda4195616e08bb5f6981e785e`
- 原 Turnip `a95b15df82ed9e49e7d2166a5e33f959b06a6655ec626f735cd6a2c98159dec3`
- 体验包 `build/apk-release/shadps4-xr-move-origin-4ec9b9a6-playstoreDebug.apk`

Swan `PB3110PGL6240001G` 新会话 PID12056 / generation1 / UUID `22f3e44e32e8dafba36268e11606177b`，Beat Saber Running。旧 PID10974 请求 STOP，观察到 Stopping/user_stop，随后 Activity 消失；没有取到最终 Stopped DebugBus，不声称旧会话 return0。之后 force-stop、安装、冷启动。没有注入游戏输入或外部改存档。横向左上状态层、FSR、推荐档、手序均保留。

自有测试目录 `/data/local/tmp/shad-xr-align-20260930` 已清理；未开调试属性、调试器或 forward，原 GPU CleanupPending 未动。实物偏移/挥动延迟由用户体验确认；无 GPU hang 或性能收益结论，无 commit/push。

身份、原图坐标、正负测试与日志见 [evidence](evidence/xr-controller-alignment-20260930)。设备日志时钟与 Mac 不同，按 PID/UUID 区分。
