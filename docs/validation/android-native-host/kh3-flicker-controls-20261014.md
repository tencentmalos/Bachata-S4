# KH3：Controls 面板帧率限制与可调渲染选项 / 场景闪黑排查（2026-10-14）

设备 Odin3（`6edb0593`），KH3 CUSA15072 奥林匹斯 Continue 存档。

## 1. Controls 面板：帧率限制与运行时渲染选项

- 新增 `Core::GameOptions`（`src/core/game_options.*`）：帧率上限 Off / 50 / 45 / 40 / 30 / 20，以及运行时修改后的保存回调。
- VideoOut PresentThread 按 vblank 累计额度（每个 vblank 加上限值，满 vblank 频率才放行一次 flip），不改游戏的 flip 请求。
- 状态层 Controls 面板新增"游戏"页（首页）：帧率上限、管线编译模式（Wait / Background / Skip until ready）、Android 屏幕超分（Off/FSR1/SGSR1）与锐度；桌面为 FSR/RCAS 开关与 RCAS 强度。
- 逐游戏保存：Android 经 `JniHelper` 回调 `SessionSettingsBridge.onNativeSettingChanged`，只接受运行时设置目录里存在的键和取值，由会话服务写入该游戏的 profile；桌面写 `EmulatorSettings.Save(id)`。启动时 `gpu.frame_limit` 由 JNI `nativeSetFrameLimit` 下发，Launch 面板也有这一行。
- 单测：`FrameLimitTest`、`SessionSettingsBridgeTest`、`GameLaunchOptionsTest` 通过。Odin3 上面板切换与生效已人工确认；重启后的保留、桌面编译未验证。

## 2. 场景闪黑（未修复，排查中）

现象：跑动/跳跃时单帧异常，常成串出现（间隔 5–300 帧）。有三种表现：

- 整帧缺直射阳光和阴影；
- 大部分黑屏，只剩一条斜边带状区域；
- 右侧一条黑边。

用户确认 10-12 晚上没有这个问题。

### 方法

- SDK 录制最终渲染目标（`capturectl record`）。`dipstats.py` 统计整帧亮度骤降（本帧 < 0.75 × 两侧帧较小值），`localflash.py` 统计局部变暗。
- 自动跑跳脚本：`jumploop.sh` 150 s + `runloop.sh` 145 s，用 5 分钟录像对比各配置。四项全开时自动输入约每分钟 1–3 次闪黑。

### 二分结果

`debug.shadps4.leaf_fastpath` 改为可按组设置 `clock,tls,draw,marker`（`0`/`1` 含义不变）。

| 配置（scaled storage 开、guest 补丁开） | 闪黑次数 / 5 min |
|---|---|
| 四项全关（用户手动） | 0 |
| 仅 guest 补丁（用户手动） | 0 |
| 仅 scaled storage（用户手动） | 0 |
| rwlock 快路径（自动） | 0 |
| 叶子 HLE 全组（自动） | 0 |
| rwlock + 叶子 HLE 只开 marker | 0 |
| rwlock + 叶子 HLE `draw,marker` | 8 |
| rwlock + 叶子 HLE 全组（多轮） | 6 / 8 / 11 / 11 / 12 |

结论：必须同时打开 rwlock 快路径和叶子 HLE 的 draw 组（DrawIndex / DrawIndexAuto / DrawIndexOffset / DispatchDirect）才会闪。两组编码器与宿主逐字节一致（单测），rwlock 锁字协议逐项核对过，没有找到读写同时持有的路径。目前判断是渲染线程每个 draw 不再进 HLE 之后、时序变化暴露出的竞争。

### 已排除（四项全开，运行时切换后照样闪）

- `gpu_fences completion`（标签等 GPU 真正完成才写）：8 次。
- `upload_diag watch_predict/watch_coalesce/watch_content off`：交替 4 窗 1/4/9/2 次。
- `memory_lock pool_map locked`：12 次。
- 管线未编译跳过：本局 0 次跳过。
- 之前已排除：`pool_undefined`、`pool_revive`、`expand_twin`、`tiny_writeback`、`adopt_mips`、pass hoisting、predication、提交 HLE。

### 新增诊断（默认关，`upload_diag <名> on|off`）

- `frame_shape`：每帧一行 draw/dispatch/pass 计数与结构哈希，另有单帧结构异常转储。正常帧之间哈希也一直在变，单帧检测噪声太大，未能定位。
- `vb_watch`：draw 录完后重读顶点缓冲 V#，比对是否在 draw 期间被改；基址未映射时记录并跳过该范围。6 次闪黑的一轮中，V# 变化 0 次、未映射 0 次。
- `draw_pairs`：新出现的 (VS, PS, 图元类型) 组合连同单调时钟时间打日志，可与录像 PTS 对齐。11 次闪黑中只有热身阶段那次附近有新组合，所以不是用了从未见过的 PSO。
- `sharp_watch`：每次绑定检查所有缓冲 V# / 纹理 T# 基址是否在已映射的 guest 内存内。7 次闪黑的一轮中 0 次报告。
- `full_barriers`：每个 render pass 结束后、每次 dispatch 前后插入完整内存屏障，用来验证"缺屏障"的假设。尚未实测。

### 其它观察

- 一次 GpuComm 断言：`MemoryManager::ProtectGpu` 收到 `0x9000–0x11000` 的顶点缓冲（V# 基址是垃圾值）。只出现过一次，之后加了 `vb_watch` 和 `sharp_watch` 也没再抓到。
- 测试中出现 3 次 GPU 故障，均为已知类型，snapshot 留在本地证据目录待后续分析：
  - DDE opcode=0：11:06（PID 7160）、12:23（PID 5010）；
  - 片头保护模式写入 #11：12:18（PID 21418）、12:35（PID 13653，KGSL 未生成新 snapshot，因上一份尚未读出）。
- 录像 `frames.csv` 的 PTS 是设备 CLOCK_MONOTONIC 微秒，与 dmesg、`draw_pairs`、`frame_shape` 的时间同一时基。

### 下一步

- `full_barriers on` 对照：如果不再闪，按 pass 边界 / dispatch 前 / dispatch 后逐项缩小。
- 用户建议同时用 GPU Reshape 查 GPU 侧问题。本机源码在 `my_mcp_tools/gpu-reshape`（带 Android SDK 构建选项），需带 `--gpu-reshape-sdk-root` 编译 host。

证据（本地，不入库）：`build/validation/kh3-flicker-1014/`（ab* 各轮录像、日志、分析结果，ab11/ab16/ab20 下的 KGSL snapshot）。
