# ASTRO BOT：位姿自动化、首关采样与 Android SBS 转向（2026-10-11）

## 本轮结果

在 `feature/malos/vr_perf` 上接续 One Piece 修复，新增有时限的 DebugBus HMD/DS4
位姿输入，Thor 的 ASTRO BOT EU（CUSA12392 / 01.04）已自动通过身体/DS4 对齐页，
并通过“看向左侧星球，再按 Cross”进入第一关。记录式试验和场景截图证明这两个
转换；尚未实现本地视觉识别的完整自动进关状态机，也未完成重场景性能修复。

用户随后报告设备平放时朝向错误、贴桌面左右转变成侧翻。本轮一并修改 SBS
传感器姿态与重新校准路径；数学校验和设备接入验证通过，物理转动手感待复核。

## 位姿接口与试验

用法与可复用计划见 [VR automation](../../../tools/vr-automation/README.md)。
`pad pose` 与已有按钮共享 PID/generation/run UUID、owner 和递增 action ID；
每次持有最多 10 秒，超时、失焦、模态界面、会话切换或 release_all 后清理。
Head/DS4/按钮可同时存在。仅覆盖 guest VrTracker，不改宿主 OpenXR 位姿或持久设置。
Android 只有在输出成功写入 guest 后才确认消费，原生计算本身不算回执。
`pad status` 包含最近真正交付的姿态、追踪质量、时间与查询计数；Pad trace v1
不含位姿，另存 actions.jsonl。修正 SBS 的 HMD/眼/Move 缺失 z=+1.5 m 座位偏移。

验证设备：Thor `9c2841a4`，API 33，Adreno 740，SBS，用户原设置保持不变。
候选 1 APK SHA256：`e284eb295243ad4fb066f54f93797fb73c7e19712e4dbe3dbd0bdacdb4e59445`。
输入脚本每步记录理由、会话身份、截图 SHA、实际消费回执，结束执行 release_all。

- 初始对齐：HMD (0,0,1.5)，DS4 (0,-0.17,1)，方向均为单位四元数；两轮进入开场。
- 左侧星球：yaw 79°、pitch +14°（xyzw 为 0.09403738,0.63133699,-0.07751844,0.76587301），
  注视 3 秒，再 Cross 300 ms。单纯注视没有进关；加 Cross 后进入有平台、金币和蓝色提示牌的首关。
- 回到中立姿态采样，3 秒中 90 个完整帧：平均 33.354 ms，p50 33.338 ms，
  p90 33.910 ms，p99 35.833 ms，最大 39.146 ms。约 30 FPS，不代表更深场景。

Litep 的 self elapsed 聚合：Rasterizer.Draw 1867.5 ms/168028 次，
Audio3dObjectSetAttributes 602.2 ms/10484 次，sceGnmDrawIndexOffset 590.4 ms/102108 次，
Vulkan.RecordChunk 488.9 ms/12930 次。这些跨线程耗时可能含等待，不能等同 CPU 利用率。
GPU 缺少校准时钟，未建立 CPU/GPU 关键路径，也没有据此宣称优化收益。
当前 Litep 不识别 `0b467a…+slow-frame-v1`；对照 Foundation 编码器确认该后缀未改 wire
字段后，仅在本地分析进程登记兼容解码，原 PROF/sidecar 和安装的工具均未改写。

## SBS 姿态修复依据

旧实现只在启动收四次重力样本来冻结设备轴，之后对角速度积分。两个问题已从代码确认：

1. `ResetOrientation` 只清积分四元数，不更新 Kotlin 的冻结轴；HMD `Recalibrate`
   甚至只报告 CALIBRATING，不重设姿态。设备移动后重新校准，朝向和轴基准可能不一致。
2. 大于 100 ms 的样本间隔丢弃整段角度变化；主线程加载/失焦造成的漏样无法补回。

用户报告时 PID 28267 没有注入 owner/pose，HMD q 约为
(0.0701,0.5407,0.2755,-0.7917)，53 秒变化很小，与“静止但朝向错误”相符。
21:40:18 的启动重力为 (6.702,-1.202,7.057)，明显不是平放时的初始基准。
这证明当时存在错误静态朝向，不证明具体哪次漏样造成了全部误差。

修改为优先订阅 `TYPE_GAME_ROTATION_VECTOR`，其次普通 Rotation Vector，确实不可用时
才保留旧 gyro fallback。融合四元数自然设备→世界（+Z 向上）；按当前中立姿态的重力和
屏幕右方向构建 PSVR X右/Y上/Z后基准。用 `B^-1 * (Q0^-1 * Q) * B` 输出相对姿态，
不经过欧拉角。HMD 注册、重新校准/重置、输入恢复和屏幕方向改变时重新建立基准。
绝对样本跨漏帧仍更新朝向，只停止推算该段角速度；防止重复、过期、非有限四元数及
gyro/fused 两条路径同时写入。OpenXR 自己管理的姿态不受影响。

Game Rotation Vector 不依赖磁北，但仍允许缓慢航向漂移；没有承诺无限期零漂移。
依据：[Android position sensors](https://developer.android.com/develop/sensors-and-location/sensors/sensors_position)。

## 验证与边界

- Thor 执行：Debug Pad 2626/0、VR sensor 167/0、VR ABI 241/0。
- 新增矩阵：四个显示方向、倾斜启动后放平重置、左右 90° 只产生 yaw、直立设备、
  漏样后恢复绝对角度、q/-q 等价、非法样本与双发布者隔离。
- Kotlin VrGyroAxes 5 项通过；Python 计划验证 2 项通过。
- Android host/JNI 与 fdroidDebug APK 构建通过。
- 最终功能 APK `089e1850fa85ab821d2634c094436aa292409f0df3d95ec3a5d52e06a20fc486`
  已安装，ASTRO PID 32098 正常查询 HMD。日志与 sensorservice 确认实际订阅
  QTI Game Rotation Vector；静止 HMD 接近单位姿态（向量分量约 10^-4），无注入。
  之后仅按项目规则格式化 C++，无语义修改。物理左右转动、Odin3、XR 和 PC 未实机验收。
- 本轮没有修改游戏 profile、Foundation、FEX 或 Mesa。主干合并同时带入此前已提交的
  会话/状态层、KH3 字体修复和 One Piece GC/Pad 修复。

## 保留的故障与证据

首轮开场 PID 12064 出现 DeviceLost：KGSL ctx20 ts31201，前有 CP AHB bus error；
随后 PrepareLastFrame 断言。第二轮越过此点并到首关，不能据此声称 GPU hang 已解决。
拉到的 snapshot SHA256 `8ee84e7aa0eaf126606a32e92cde3cb5dbd6d9b5a01ccff9b5df4a5149ab6fa4`
实际为 10-04 的冻结旧快照（ctx16/PID11632），**不属于本次故障**，未用于归因。

本地未入库证据：`build/validation/astro-thor-20261011/`。包含 candidate-1 的试验 1–12、
录像、scene-neutral.png、host/logcat/dmesg、原始 PROF 及分析结果，和 candidate-gyro 的
APK/身份/传感器状态。录像均已结束，自有 scrcpy 会话已关闭；试验 12 最终 release_all
成功。重场景性能、持续 GPU 稳定性和完整视觉自动化留待下一轮。
