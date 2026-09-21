# AYN：Tetris / Beat Saber HMD 参数对照与公开资料

2026-09-21，`feature/malos/beat_saber_fix`，基线 `63b6d560`，本地修改未提交。
本轮完成可复用 HMD 日志、两款同 APK 对照、kind 0 固件字段核对和公开资料归档。
**Beat Saber 重影仍在，没有完成显示修复或可玩性验收。**

后续状态：[viewport/MSAA 修复](beatsaber-viewport-msaa-20260921.md) 已在 AYN 消除安全提示页重影，根因是颜色附件实际采样数；本页保留当时 HMD 标定与修复前证据。

## 复用入口

- [公开资料索引](../../../references/psvr-public-api/README.md)：Unity 官方文档、Orbital 类型表、OpenOrbis 头文件、许可证及下载 SHA。未取得 Sony 原版 SDK 参数帮助页。
- [HMD 采集说明](../../hmd-diagnostics.md)：默认关闭，每入口每会话最多 28 组；原始 wire bytes、真实返回值及已准入提交快照。
- [证据 manifest](beatsaber-hmd-log-20260921/manifest.json)、[结构化对照](beatsaber-hmd-log-20260921/comparison.json)、[固件标定](beatsaber-hmd-log-20260921/firmware-calibration.json)。

## 同 APK 实测

设备 AYN Thor `9c2841a4`；Turnip Adreno 740 / Mesa 26.0.0-devel git-5ac41be677，驱动 SHA `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`。
保持 Render 0.5、Texture High、SBS 和初始前向陀螺仪位姿。
实测诊断 APK `acda2879e90446a28f6fd77ef11b66071041f3a2026341c0e043821ef2dd0a64`；host `84946ea3b5fc95f644553221954b86b84eda2cd868d4a717cde616ca23fff7b1`；JNI `1aee3572e2eb62ff3cd830f963ce7c455f85cd7f4497aaeeaec454bd81ba75d8`，安装后整包 SHA 匹配。

| 项目 | Tetris | Beat Saber |
|---|---|---|
| PID / generation | 17493 / 1 | 18239 / 1 |
| run UUID | `541c710c892ca732e8b1a907834a60bf` | `4304fbf2b6e437abbfc5cb0ed1a80dfe` |
| 实际入口 | StartWithOverlay | StartMultilayer，count 1 / kind 0 |
| 采样调用 / 最大采样序号 | 21 / 512 | 25 / 8192 |
| 提交入口采样返回值 | 全部 0 | 全部 0 |
| flags | `0x5` | `0x80000005` |
| 输入颜色图 | 共享 2688×1512，pitch 2688 | 共享 2688×1512，pitch 2688 |
| 独立 overlay | 每眼 1344×1512，pitch 1408 | 无 |
| 输出槽位轮换 | 0 / 1 | 2 / 3 |
| 画面 | 正向、左右分离的警告页 | 正向安全提示和光剑，仍重影 |

采样序号不是总调用次数，提交成功也不代表 GPU 内容正确。
完整记录：[Tetris](beatsaber-hmd-log-20260921/tetris2-complete-hmd.json)、[Beat](beatsaber-hmd-log-20260921/beat-complete-hmd.json)；
现场截图：[Tetris](beatsaber-hmd-log-20260921/tetris2-scene.png)、[Beat](beatsaber-hmd-log-20260921/beat-scene.png)。

Beat 的两个纹理描述符地址不同，但其 32 字节内容完全相同。这符合共享眼图，
不能据此判断双眼被错误复用。实际提交的 UV `(scaleX, scaleY, biasX, biasY)` 为：

```text
left  = (0.5, -1, ~0, 1)
right = (0.5, -1, 0.5, 1)
```

25 个采样均保持此布局；负 scaleY 来自 guest 参数，不能额外再翻一次。
[renderer 日志](beatsaber-hmd-log-20260921/beat-renderer-source.txt) 也确认正确半区到达对应 guest 图像地址。
这只核实提交到采样链的一部分，尚不能证明源纹理、shader 或 MSAA resolve 正确。

Tetris base 为 `(3/7, 6/7, ~0, ~0)` / `(3/7, 6/7, 3/7, ~0)`。
overlay 左眼约 `(1,1,0,0)`、右眼约 `(1,1,0.0109194,0)`；这项小偏移仍须核对
非对称 FOV 和 overlay 坐标合同，未归因为当前游戏问题。
两款 GetDeviceInformation / GetFieldOfView 原始输出一致；采样四元数范数接近 1，
位置为原点、朝向接近正前方，未发现非法位姿。不能由离散采样推断全部帧。
Beat 第二次 Initialize315 返回 already-initialized `0x81110001`，首次为 0；未证明它导致重影。

## 本轮标定和实现

新增 `guest_hmd_diagnostics.h`，包装实际已绑定的 `sceHmd*` ABI adapter，记录入参、返回寄存器、
有效输出、嵌套 texture/sampler 及 renderer 提交。只通过 GuestAddressSpace 读取；不可读具名标记，
失败不伪造输出，日志异常不改变 HLE 合同。原始参数快照不是所有嵌套对象的原子快照。
新增 `summarize_hmd_log.py` 按 PID 提取采样和返回值分布；当前 APK 应读 host 日志，不能依赖 logcat 转发。

固件 11.00 的 `StartMultilayer +0x17a50` 独立证实六参数位置。
单层 kind 0 分支在 `+0x1820f..+0x182a4` 复制 layer `+0x28/+0x38`
至 Start `+0x18/+0x28` 并调用真实 legacy Start `+0x161e0`。
后续 `+0x94c0` 读取纹理和 sampler，并把两组 float4 放入 GPU 参数块。
证据：[精简反汇编](beatsaber-hmd-log-20260921/firmware-kind0-lowering.asm)。

因此把对应 `opaque28` 的前 32 字节正式标为 `color_tan_to_uv[2][4]`，其余 32 字节仍保留 `opaque48`。
没有改变数据布局或采样值，没有凭推测交换矩阵。加入真实 Beat 浮点 bit pattern、
按独立固件偏移转换 legacy Start 的等价测试，以及修改 opaque48 不影响 kind 0 颜色采样的反例检查。
**AYN 最终 90 checks / 0 failures**，见 [测试输出](beatsaber-hmd-log-20260921/calibration-tests.txt)。

公开资料提供额外线索：Orbital 的 VrTraceHook 原型给出 layers/count/settings/trackerState/flipArg/option，
但没有字段布局或 flags 定义。Unity 2018.4 官方文档明确 PS4 的 double-wide 路径，
并说明错误的后处理整图采样会把双眼重复画入每一眼；它是后续检查方向，不是本游戏根因证明。

## 反例和未完成项

- 首次 Tetris PID16271 在进入 Reprojection Start 之前 Guest-50 / VA0 崩溃，
  [反例日志](beatsaber-hmd-log-20260921/tetris1-crash-excerpt.txt) 保留。同 APK 重试 PID17493 才出现正确双目警告。
  此前已存在 PC0 问题，不能把重试成功称为修复或稳定性验收。
- 本轮 Tetris 只检查警告页与 HMD 参数，没有验证 X 后进入游戏；之前的 X 后黑屏未修复。
- 旧 Turnip RDC `build/validation/beatsaber-gyro-display-20260920/baseline.rdc`
  SHA `5bfe8050da75d0dedef655aa832b32df68d89a898b8e4f074951893a6ab71c85` 的
  E1021 / R1825 在 HMD 之前已有异常，E1135 resolve 后 R1851 仍异常，最终 R1535 也异常。
  这是旧 PID14211 的独立线索，不是本轮 APK 同帧证据；本轮查看结果留在
  `build/validation/beatsaber-eyes-20260921/replay/`。
- 下一步应对同一 Beat 运行关联提交眼图与最早出错 draw，核对 shader eye index、
  viewport/texture view、屏幕坐标和 MSAA resolve。flags 高位仍需单独标定，不能只按外观改参数。
- 完整导入：[Tetris 2603 行 / 358 refused 行 / 290 唯一项](beatsaber-hmd-log-20260921/tetris2-imports.json)，
  [Beat 1939 行 / 256 refused 行 / 243 唯一项](beatsaber-hmd-log-20260921/beat-imports.json)。
  本轮为日志/ABI 校验，未声称剩余功能族不阻塞。

## 最终构建与清理

后续字段命名及上述定向测试版 APK `6ae68ead168dd6319241a8c6aad05c1e0a71743e8e0cac7570efbb753b1f3e53`，
host `f2a1a6a18d757a0e8321bee88eed95a84be0c4aad1587e3804eaba85df22d271`，JNI 未变。
native、Gradle native refresh 和 APK 构建通过，APK 内 host 与本机产物一致，
安装后整包 SHA 核验通过；**最终字段命名版只验 Library，没有重跑游戏**。
实景结论绑定上文诊断 APK，不冒充最终版本实景验收。
首次 assemble 曾漏打 host 库，安装前发现并重跑 native packaging；该包未安装。

Tetris 787 presents 后 UIStop / user_stop，guest return `68724916224`；
Beat 13104 presents 后 UIStop / user_stop，guest return `4`；均不写 return 0。
正常停止后重开空闲前端以清除既有旧画面残留。
最终 Library PID28846、session none、TracerPid 0；自有 logcat/replay/调试 forward 已清理，HMD log 属性清空。
global.json / host/config.json 前后逐字节一致，Turnip / 0.5 / High / SBS / gyro 保留。
无 Beat 双目修复、全游戏回归、性能收益或 commit/push 声明。
