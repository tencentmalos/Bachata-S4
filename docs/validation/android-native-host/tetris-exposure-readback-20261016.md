# Odin 3：Tetris 三帧闪黑与 UE4.26 曝光读回

2026-10-16。结论：本轮复现的闪黑/错误颜色由 **三槽曝光 staging 纹理的 GPU 结果没有回写 guest CPU 内存** 引起。补充小型线性 storage image 的读回后，同一进程由 151 帧中100黑帧恢复为152帧中0黑帧，CPU PreExposure 同时恢复正常。验证覆盖警告页、标题/地球粒子场景；尚未完成实际关卡、长时间游玩和头显 XR 回归。

[UE4.26 GnmRHI 与 KH3 比对](../../gnm/tetris-ue426-gnmrhi-readback.md)及 [C++14 参考子集](../../../tools/gnmrhi-semantics/ue426-reference/README.md)记录了完整地址、层次与边界。

## 环境与保存

- 用户指定设备：Odin 3，ADB `6edb0593`，Adreno 830 / Android API35；未操作同时连接的 Swan。
- 标题 CUSA13427，设备元数据 VERSION02.05 / APP_VER01.00。实际 ELF 为 `++UE4+Release-4.26`，SHA256 `157845ba4297a000ac9cfeb26b7b78925d7d903a2ee34575c74d0f5ede6e9677`。
- 初始 APK SHA256 `b2640a6cc20545fc8dffdb4886068fc4d534518b3d62fcc74f3eb7c232b156f5`，本地保存 `baseline-b2640a6c.apk`。
- A/B APK `974d6ebcd12e4d3b86ef90aa1cee0ba531c8e95eaf08301186590316d32eae75`；最终 APK（补充 DebugBus help，运行逻辑相同）`b2d42cd74b9cba27f3351ddf155792e05766562d4def5c9360c9a9b37a21c31d`，host SHA `fa2c11671f9262de53698745ec4c83e19be4a01f5548c1359950fb97cc3b0a45`。
- Turnip 始终为 `39a4a65f43bf680802acf55698d7d9c9b9e757b84a172611b76ae6cc0b714a5f`，Mesa 26.3.0-devel；保留既有 `TU_DEBUG=csalign64k`。构建绑定 `build/turnip-lrz-noreuse`，没有换驱动作对照。
- SBS、scale0.75、High shading/texture、MSAA off、FSR 等设置保持原样。最终按字节核对 host config、global.json 和 CUSA13427.json 均与备份一致。
- 全部原始样本：`build/validation/tetris-gnmrhi-20261016/`，不入仓。包括录屏、逐帧亮度、CB dump、PM4、导出 ELF、反汇编、构建日志与配置备份。详见下面工件索引。

## 故障帧为何与 GnmRHI 相关

PM4 trace `66513230408ac1285b2e85b3a386cca9` 覆盖8个完整 flip 区间。每帧49个 dispatch，DrawIndexAuto31次，indexed101–103次；无 host skip。Tonemap context/config/uconfig 寄存器除目标 atlas 地址/CMASK 外一致，draw 没有 predication。不能据此证明所有输入都正确，但没有看到按黑帧周期少发 draw 的证据。

copy CS `b127b333163f2155` 每帧 dispatch(1,1,1)，复制如下曝光纹理；三个目标是 **1×1、32_32_32_32/Uint、tiling8（linear aligned）**。这里 Uint 是按位搬运 float4，不应将其解释为曝光算术使用整数。

| trace frame / action | GPU 源（tiling14） | 线性 CPU staging 目标 |
|---|---|---|
| 0 / 890 | `0x202bbd0000` | `0x30000c0800` |
| 1 / 1073 | `0x2028d30000` | `0x30000c0000` |
| 2 / 1255 | `0x202afa0000` | `0x30000c0400` |

地址是该会话实例，下一次启动不能硬编码。此 copy 是实际 shader dispatch，并非 host 已识别替换的 copy HLE。GPU image stats 中三个源的 R 均为约6.66666；Z 有1/16差异，但 tonemap VS 只读 R，不能把 Z 差异当闪烁原因。

与此同时，tonemap PS `94e1d38583d30b84` 的 View UB（size1060）连续12次绑定、左右眼成对，+874/+878 出现：

| 三帧周期 | PreExposure 位模式 / 值 | OneOverPreExposure |
|---|---|---|
| A | `6e754675` / 约1.8977e28 | 约5.2695e-29 |
| B | `616c7073` / 约2.7260e20 | 约3.6684e-21 |
| C | `0000000e` / 约1.9618e-44 | `7f800000` / +Inf |

反射注册和 CPU 指令分别证实字段名、读取 ViewState 历史以及1/x的计算。PS 真正读取 +878 并乘 scene/bloom RGB。GPU 读回 → CPU 暴露 → 下一帧 HDR/tonemap 形成反馈环，残留数据不仅导致黑帧，也会造成巨大颜色值/溢出。因此不必先修改 LUT 或 GS/copy shader。

## 修复

`TextureCache::FindTexture` 的 Storage 分支之前只在配置 `readback_linear_images` 开启时加入 download 队列；默认关闭。已有 KH3 `tiny_writeback` 只处理 render targets，还会跳过连续每帧写的目标，不能覆盖本例。

新增 **默认开启** 的 `tiny_linear_writeback`：单 sample、单层/单 mip、非深度、非 tiled、depth1、≤16 pixels、有效 guest 地址的 storage image 加入既有 download 队列，按 image ID 去重。没有游戏 ID、shader hash 或地址特判，也不受 render-target 写入 streak 限制。更大的/带层级的 staging 读回仍保留既有配置行为，未宣称解决全部 GPU→CPU coherent memory。

复用现有 Liverpool EOP/EOS 处理：在 SignalFence 前 `ProcessDownloadImages`。parse 模式同步等 copy 完成并写回，再写标签；completion 模式将读回数据回写和标签完成放入有序的 deferred 路径。后者也实测无黑帧。

开关：

```text
debugbus upload_diag tiny_linear_writeback on|off
Android: debug.shadps4.tiny_linear_writeback=0|1（启动读取）
Desktop: SHADPS4_TINY_LINEAR_WRITEBACK=0|1
```

`upload_diag status` 的 `writebacks` 统计加入 download 队列的次数，不是 Vulkan fence 次数。临时关闭后必须考虑 staging 已含有效结果；不能指望实时关闭立刻再次产生垃圾数据。

## 对照结果

录屏5秒，按完整游戏区域 `crop=1920:980:0:100`（剔除顶部状态层）统计 FFmpeg YAVG<16.15 的近全黑帧，结合截图检查周期。早期较小 ROI 会漏掉随陀螺仪漂移移动的画面，因此统一用全宽 ROI。计数是录屏解码帧，不是假称 guest GPU 连续帧编号。

| 条件 | 解码帧 | 黑帧 | 结论 |
|---|---:|---:|---|
| 原 APK parse | 151 | 50 | 约每3帧黑1帧 |
| 原 APK completion | 149 | 49 | 单改完成标签无效 |
| 原 APK frame_finish on / off | 151 / 151 | 51 / 51 | 整帧等待无效 |
| 原 APK full_barriers on / off | 151 / 151 | 50 / 50 | 全屏障无效 |
| 原 APK texture_bind cache/fast off | 151 | 50 | 关闭绑定缓存无效 |
| 修复 on，第1次启动警告页 | 151 | 0 | UB为2/0.5，文字正常 |
| 修复 off，第1次冷启动标题 | 151 | 0 | UB恒1/1；残留为非正值时走fallback，不能算成功负对照 |
| 修复 off，第2次冷启动警告页 | 151 | **100** | 周期1、约1.847e25、约1.831e25；两帧黑一帧可见 |
| **同一进程**直接 on | 152 | **0** | UB全部2/0.5；警告页稳定 |
| 同一进程 on，继续到标题地球场景 | 136 | 0 | 颜色/粒子恢复正常 |
| 同一进程 on，completion 模式 | 148 | 0 | deferred路径正常；结束恢复parse |
| 最终 APK 默认设置冷启动，警告页 | 151 | 0 | 临时属性为空；连续12次 UB 均为2/0.5 |

最强因果样本是 PID18551 / generation1 / run UUID `e4a13c82dfa7614c23753c8b8890d051` 的 off→on；只切换新开关，不重启、不换场景。off1 的不复现说明缺读回的表现依赖原始内存内容；它不否定 off2 的同进程对照。

第1次 on 的标题录屏有22个**开头连续**黑帧（0..21），是警告页切换的淡出/入场过程，随后无周期黑帧；没有将这一段写成全程0黑帧。标题 View 常量随后约9.15→9.17、倒数约0.109，正常随画面变化。

最终包冷启动 PID22740 / run UUID `2c772ee58cc97f187cf30730be44ee30`，没有设置新属性，也没有手动打开修复；状态确认默认 on。警告页151/0、12次常量均2/0.5。随后 Cross 输入被 guest 消费，但第二段截图仍是警告页，该段151/0已按实际场景命名 `final-default-warning-after-input`，不算标题回归。标题验证采用前面的同进程 A/B 样本。完成后正常停止游戏。

测试中曾试改 host/config.json 的 `readback_linear_images_enabled`，但未证实 Android 将其加载到本轮运行状态，因此 `config-readback-attempt.mp4` 不用于任何结论；配置已经恢复并逐字节校验。

## 构建、测试与边界

- NDK29/API33 ARM64 `shadps4_host` 增量构建成功，Gradle `:app:assemblePlaystoreDebug` 成功；最终包已安装 Odin3。
- 最初全量 `build-host-android` wrapper 因 VS 环境缺 `kernel32.lib` 在 tools-configure 失败；随后使用现有 host tools 和直接 native build 完成。`build/android-host-api33/result.json` 保留 wrapper 的 FAIL，不能当此次成功增量构建的证明；以本目录 `build-host-final.log` / `build-apk-final.log` 为准。
- 新 C++14 readback 子集 MSVC Debug 编译、CTest4/4通过（包含已有KH3三个目标）。完整 UE/PS4 SDK 未编译；PC模拟器未回归。
- 现场保留默认修复开启；`debug.shadps4.tiny_linear_writeback` 已恢复为空，配置三份逐字节一致，`gpu_fences=parse/flip_wait on/frame_finish off`，其它负对照开关已复原。无仍在运行的录屏或 debugger。
- 这里解决的是本轮 Tetris 症状；没有据此改变 KH3 跑跳闪黑的未解决结论，也未验证其它游戏小图 readback 的性能影响。

## 工件索引

本地相对 `build/validation/tetris-gnmrhi-20261016/`：

- `final-video-comparison.json`、`*-full-luma.txt`：全部逐帧指标和黑帧索引。
- `fix-off2.mp4`、`fix-on2.mp4`、`fix-on2-title.mp4`、`fix-completion.mp4`：决定性验证。
- `tonemap-cb-dump.txt`、`fix-off2-cb.txt`、`fix-on2-cb.txt`、`*-constants.json`：修复前后原始常量。
- `66513230408ac1285b2e85b3a386cca9.gpu.pm4.trace` / `.gcmdtrace.ps4`、`completion-resources.json`、`tonemap-register-states.json`：提交/绑定。
- `tonemap-vs.bin/.txt`、`tonemap-ps.bin/.txt`：GCN字节及反汇编。
- `field-ref-35485c0.asm.txt`、`field-ref-3548500.asm.txt`、`fn-2170ba0.asm.txt`、`fn-fa13d0.asm.txt` 等：4.26 静态链。
- `config-restore-verification.json`、`final-apk-libraries.json`：恢复和二进制身份。
- `final-default-results.json`、`final-default-warning.mp4/.png`、`final-default-cb.txt`：最终 APK 默认开关的冷启动复核。

源工作区开始时已有大量未提交修改，本轮没有覆盖或提交其它改动。此次修复的三个源文件也原本有修改；本轮开始前副本为 `texture_cache-before.cpp`、`upload_diagnostics-before.cpp/.h`，可据此单独核对本轮差异。

## 后续：graphics还原、16像素范围与Swan部署

[4.26 graphics深入还原](../../gnm/tetris-ue426-gnmrhi-graphics.md)补出pipeline、资源绑定、普通LCUE、direct/indirect draw及KH3对照；C++14编译和CTest5/5通过，新增graphics8组场景。没有再修改或重打此前通过Odin验证的APK。

用户询问将16像素扩大至32×32×32 LUT。核查现有8帧trace：LUT `0x2028d80000` 由32层RT draw写入，随后tonemap按tiling19的3D texture采样；非线性storage staging。仅扩大大小阈值仍被入口/布局/depth/layers条件排除，且共享TinyTargetPixels还影响RT补偿和dump。底层download能处理更大图和tiled layout，但默认无条件写回每帧LUT会引入同步，不替代同帧GPU alias/copy。故本轮保留≤16默认规则；详细容量估算、同步路径及后续按需扩展条件见专文§9，本地新增`lut-path-summary.json`。

按用户要求由subagent将Odin3的CUSA13427四文件复制到Swan PB3110PGL6240001G，合计5,246,883,930bytes，源/本机/目标SHA一致；安装APK `b2d42cd7`并核验设备base.apk全SHA。按用户明确授权，仅删除`/storage/emulated/0/XRGameNative/Steam/steamapps/common/MonsterHunterRise`，释放约34.54GiB，最终剩约30.92GiB。外部存档/容器配置及99个旧诊断目录保留。原16份配置/数据库安装前后逐字节一致，游戏库扫描后10份设置也一致。库识别Tetris正常，未启动游戏，核验后应用已关闭；Swan XR/闪烁回归未做。证据在`build/validation/tetris-transfer-swan-20261016/README.md`及`summary.json`。
