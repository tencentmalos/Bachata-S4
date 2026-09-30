# Beat Saber 左右 Move 手序（2026-09-29）

用户在 Swan 的 Beat Saber CUSA12878 v02.04 反馈左右手反了。本轮增加每游戏的 Move 手序设置，给该游戏开启交换，并安装新包验证了实际 HLE 映射。物理握持效果仍等待用户确认，不能仅凭软件回归宣称验收完成。

## 定位与实现

Foundation 的 OpenXR action subpath 是 `/user/hand/left` 和 `/user/hand/right`，内部 Left=0 / Right=1；runtime 更新和 XR UI 没有交换它们。旧适配器把 `sceMoveOpen` 的 index 0 固定接左手、index 1 固定接右手。这是模拟器自己选的枚举顺序，不能据此假定所有游戏都以相同方式解释 Move 槽位。

Beat Saber ELF 的 `sceMoveOpen` PLT 在 `0x18225e0`，调用点 `0x1380c01` / `0x1380c76` 分别传入 index 0 / 1。离线反汇证明两个槽位的使用，但不单独证明 PSVR API 有通用的左右手约定；本次交换依据是用户在该游戏中的实测反馈。

- 增加 `input.xr_swap_move_hands`，默认 false，可全局或每游戏覆盖。Android System 页显示 **Swap XR Move Controllers**，修改后重启游戏生效。
- 启动服务解析最终配置，经 JNI 调用 `Move::SetXrSwapHands`，在 `sceMoveInit` 捕获本轮映射。正在运行时改变配置不会使已有手柄突然跳边。
- 统一通过 `HandForHandle` / `HandIndexForHandle` 分配物理手。Move 按键、扳机、陀螺仪、VrTracker grip 位姿/速度及震动一起交换，不留下输入和反馈来自不同手的情况。
- 只给 CUSA12878 开启交换：index 0 → 右手，index 1 → 左手。源码没有游戏 ID 特判；OpenXR 硬件左右路径、XR 菜单指针、眼图和蓝牙普通手柄维持原映射。非 OpenXR 的 SBS 适配器也不应用这个设置。

## 验证

| 验证 | 结果 |
|---|---|
| Android host / APK 构建 | 通过 |
| Swan 原生 `openxr_pose_tests` | 83 checks / 0 failures，exit 0 |
| Kotlin runtime 单元测试 | 136 tests / 0 failures / 0 errors |
| 真游戏 HLE | index 0=right，index 1=left，两个实际 handle 对应正确 |
| 原有眼缓冲修复 | 仍为 SBS 5184×2400，每眼 2592×2400 |
| 物理双手、按钮、震动 | 等待用户反馈；软件回归不能替代此项 |

原生回归分别测试正常与交换两种顺序、先打开物理右手、左右不同 grip/aim 与姿态/速度、扳机和 face button、陀螺仪坐标转换、失联/无效句柄以及震动回到正确的物理手。还验证了初始化后配置变化不重映射，以及非 XR 路径不被改变。设置测试覆盖每游戏优先级、显式 false、非法值、导入和 Android 可见分类。

新包 APK SHA `9a5ec50c40b4a405a030768e8a99910e099270173f3e164f4bfbdd0df4224219`；host `f07e7e52…`，JNI `d2a2c75c…`，源码 Turnip `87aeb405…`。完整身份在 [identity.json](evidence/xr-hands-20260929/identity.json)。

设备安装成功后已重启 Beat Saber。首次保存的状态为 PID 9609 / generation 1 / UUID `534fa07148b9dec8cb5513112b980d97`，1116 guest flips，Running；重连后同进程累计 20315 flips，仍 Running。游戏日志记录了两只 Move 的物理手映射及完整眼缓冲尺寸；没有本轮 DeviceLost 修复或歌曲通关声明。

## 设置与断连交接

本轮只写 CUSA12878 的 `input.xr_swap_move_hands=true`，保留原 `gpu.force_disable_msaa=true`。设备短暂从 adb 断开后已重连：安装 APK 全包 SHA 与构建包一致；全局 JSON 与启动前备份逐字节相同；每游戏 JSON 回读确认只新增手序设置；自有测试目录 `/data/local/tmp/shad-xr-hands-20260929` 已清理。本轮 [CleanupPending](evidence/xr-hands-20260929/cleanup-pending.json) 已解除，物理验证仍单独待确认。不要把有意保留的手序配置当临时诊断开关清掉。

没有新调试属性、debugger、forward 或游戏输入注入。原 GPU 属性、缓存和 devcd15 的旧 CleanupPending 保留，未动游戏文件。物理验证若仍反，继续检查游戏手柄角色分配，不据此全局颠倒 OpenXR 设备路径。

[紧凑证据](evidence/xr-hands-20260929/)；完整构建日志和调用点反汇在本地 `build/validation/xr-hands-20260929/`。本轮无 commit / push。
