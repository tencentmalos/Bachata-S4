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
- `full_barriers`：每个 render pass 结束后、每次 dispatch 前后插入完整内存屏障，用来验证"缺屏障"的假设。交替 4 窗 1/3/10/5 次（开/关/开/关），不是缺屏障。

### 下午追加（10-14）

更多已排除（四项全开）：

- `dcb_verify`：处理每个提交前，把提交时复制的 DCB/ACB（含 IB 内容）与 guest 内存比对，114 万次 0 次改动，guest 没有在提交后改写命令缓冲。顺带修正：Android 提交 HLE 传入的是宿主副本，REWIND 重读的原指针改用 guest 地址（`source`）。
- `cp_delay 8000`（命令处理器每帧开工前睡 8 ms，guest 领先更多）、`submit_drain`（`sceGnmSubmitDone` 等命令处理器把已提交的全部处理完）：两种都照样闪（3/3、3/8），与 guest 和命令处理器之间的领先量无关。
- 叶子 HLE 的 DrawIndex 系列在 guest 里每次编码后加全屏障（`leaf_fastpath` 项 `drawfence`，x86 `mfence`，FEX 译为 `DMB SY`）：5 min 2 次。
- FEX 的向量访问与 REP MOVS/STOS 也按 x86 TSO 排序（新诊断属性 `debug.shadps4.fex_tso=vector,memcpy`，FEX 默认只排标量访问）：5 min 3 次。不是 FEX 内存序。
- 叶子 HLE 只开 dispatch 组（DispatchDirect）：0 次；只开 DrawIndex 系列：7 次。

闪黑帧的样子（录像逐帧）：黑区与可见区之间是屏幕空间的直线（一条斜线、或几条直线围成的多边形、或右侧竖直黑边），可见区里的内容位置正确；也有全黑只剩 UI 的帧。

pm4 trace 新增每个 draw 的只读缓冲内容（宿主事件 `BufferData`=10：地址、大小、前 64 KiB 的 XXH3、前 192 字节；flatbuf 记地址 0；顶点缓冲记合并区段的前 64 字节）。一轮 5 min 抓到一次落在 trace 内的全黑帧（`buf1`，capture `41519d7f` 第 8 帧，对应 flip 5669）。与同奇偶的第 6、10 帧比：

- 最终合成 draw（索引全屏三角形，静态 VB/IB 在 `0x2000822xxx`，常量相同）及之前后处理 pass 的缓冲内容、各 draw 的上下文寄存器、render pass 切分/屏障/拷贝等宿主事件序列都没有异常差别；只有每帧轮换的 ring 地址（用户数据寄存器）不同，相邻帧本来就这样。
- 几个 16–32 字节的后处理参数缓冲每帧都在变（含明显不相关的值），正常帧也一样，不是闪黑的原因。

即 guest 提交的命令和 guest 内存里的数据在坏帧里是正常的，问题在宿主执行这一侧（图像内容、驱动/GPU 执行或宿主命令缓冲），不在 guest 写的数据。

宿主执行侧对照（四项全开，各 5 min）：

- `vk_recorder off`（命令直接录制、不走录制线程与 worker submit，确认录制计数不再增长）：2 次（其一是整帧缺阳光）。不是录制线程。
- `debug.mesa.tu.debug=sysmem`（Turnip 全部 render pass 走 sysmem，不分 bin、不经 autotune）：7 次 + 右侧黑边 4 次。不是 GMEM/分 bin/autotune。属性已恢复为空。

另：一次冷启动在片头之前停住（无 Guest-57、Guest-1 一直 `pthread_cond_timedwait`、黑屏 60 FPS、音频静音，3 分钟无进展），日志与线程转储存在 `rec1/`，与闪黑无关。原因：片头影片 StateReady 回调里游戏调 `sceAvPlayerStreamCount`，回调线程上的 API 调用只 `try_lock` 播放器 API 锁（避免与在 OnWorker 里等回调线程的 API 互锁），而打开影片的 Guest-1 这时还在 `AddSource` 里持有该锁，于是返回 OPERATION_FAILED（日志 `hdTyRzCXQeQ rejected: 0x806a0002`），游戏没有启用任何流、一直等影片。正常启动时回调晚一点到，没有争用。修复（`guest_avplayer.cpp`）：回调里拿不到锁时，先在回调线程上执行排队中的同步调用（文件读、分配、Close 的屏障，持锁方可能正等它们），事件仍按序排队，然后重试；关闭/停止时退出。待设备验证。

### 其它观察

- 一次 GpuComm 断言：`MemoryManager::ProtectGpu` 收到 `0x9000–0x11000` 的顶点缓冲（V# 基址是垃圾值）。只出现过一次，之后加了 `vb_watch` 和 `sharp_watch` 也没再抓到。
- 测试中出现 3 次 GPU 故障，均为已知类型，snapshot 留在本地证据目录待后续分析：
  - DDE opcode=0：11:06（PID 7160）、12:23（PID 5010）；
  - 片头保护模式写入 #11：12:18（PID 21418）、12:35（PID 13653，KGSL 未生成新 snapshot，因上一份尚未读出）。
- 录像 `frames.csv` 的 PTS 是设备 CLOCK_MONOTONIC 微秒，与 dmesg、`draw_pairs`、`frame_shape` 的时间同一时基。

### 下一步

- 宿主执行侧逐项对照：录制线程（`vk_recorder off`）、Turnip 的 GMEM/LRZ（`debug.mesa.tu.debug=sysmem` / `nolrz`）、纹理缓存是否在坏帧把 GPU 写过的目标从 guest 内存重传（`upload_diag start <行数>`，日志行带单调时钟 `t=`，可与录像 PTS 对齐）。
- 用户建议同时用 GPU Reshape 查 GPU 侧问题。本机源码在 `my_mcp_tools/gpu-reshape`（带 Android SDK 构建选项），需带 `--gpu-reshape-sdk-root` 编译 host。

证据（本地，不入库）：`build/validation/kh3-flicker-1014/`（ab* 各轮录像、日志、分析结果，ab11/ab16/ab20 下的 KGSL snapshot）。
