# Gnm 绑 shader 的 guest 快路径 / HLE 编码器减负 / multiblock 默认开启（2026-09-28）

分支 `feature/malos/swan_performance`，基于 `8adbf8c3`，本轮改动已本地提交、未推送。
设备：AYN Thor `9c2841a4`（快路径首轮与原生对照）、Pico Swan `PB3110PGL6240001G`（其余全部；Thor 中途断开，按用户要求改用 Swan）。
证据：[`evidence/gnm-fastpath-20260928/`](evidence/gnm-fastpath-20260928/)。

## 1. 背景

[Thor 瓶颈报告](thor-bloodborne-bottleneck-20260928.md) §4 发现：逐 draw 的 `sceGnmSet{Vs,Ps,...}Shader` 与
`sceGnmIsUserPaEnabled` HLE 在各线程合计约 20 ms/帧，`DrawInitDefaultHardwareState200` 每次约 282 µs。
这些函数只把一小块寄存器编码成 PM4 写进游戏的命令缓冲，不碰宿主状态，但每次都要走一遍 HLE 包装：
admission lease、`graphics()`、两次堆分配、16 KiB 保护缓冲的填充与扫描、整段容量的拷进拷出。

## 2. 实现

### 2.1 guest 快路径（新增）

- `guest/runtime/gnm/shader.c`：自由环境 x86-64 C，15 个导出，逐字节复刻
  `src/core/libraries/gnmdriver/gnmdriver.cpp` 的编码器，并包含 HLE 包装的前置检查
  （空缓冲、`size==0` 或 `size>0x100000` 返回 -1，不写）：
  `SetCsShader`、`SetCsShaderWithModifier`、`SetEs/Gs/Hs/Ls/Ps/Vs Shader`、`SetPsShader350`、
  `UpdateGs/Hs/Ps/Vs Shader`、`UpdatePsShader350`、`IsUserPaEnabled`（恒 0）。
  每个 dword 用一次对齐的 32 位存储写（见 §4.2）。
- `src/core/host_runtime/guest_gnm_abi.h`：NID → 导出名路由表（测试共用）。
- `GuestRuntime::InstallGnmFastPath`：首次 `Bind` 时与 sync 快路径同样的方式发布
  （`CodePublication`、`Allocate` + RX 保护），只路由本会话已准入的 Gnm NID（`graphics_gnm_nids`），
  `hle_status` 记 `guest_fastpath`。日志 `Guest Gnm fast path installed: ... routed=15`。
- 关闭：`setprop debug.shadps4.gnm_fastpath 0`（会话启动时读取），桌面 `SHADPS4_GNM_FASTPATH=0`。
- CMake：`guest_gnm_payload` 目标用 `scripts/android/build-guest-payload` 生成 `guest_gnm_payload.h`。
- 血源实际导入其中 12 个（没有 `SetCsShaderWithModifier` 与两个 `350` 变体），`guest-imports.json`
  中全部为 `guest_fastpath`。

与 HLE 的唯一有意差别：命令缓冲或寄存器块未映射时，HLE 返回 -1，快路径在 guest 里缺页（与主机真实库行为一致）。
血源实测 HLE 路径从未拒绝过这类调用（§3.3）。

### 2.2 HLE 编码器减负（`guest_graphics_hle.cpp`）

- 编码器改用独立的 `install_encoder`：不再取 graphics admission lease、不调 `graphics()`。
  编码器只写已 pin 的命令缓冲，不提交 GPU 工作；pin 本身保证映射在写期间有效。
- 寄存器块改为栈上 `std::array<u32,16>`；scratch 改为线程局部缓冲（不再每次分配 16 KiB 并释放）。
- `HWINIT`（`DispatchInitDefaultHardwareState`、`DrawInitDefaultHardwareState200/350`）：这三者对任意
  `size >= 0x100` 都只写前 `HwInitPacketSize`(0x100) 个 dword，游戏传的是剩余容量。现在只拷
  `min(size, 0x100)` 个 dword 进出 scratch；仍按完整容量 pin（语义不变：容量不可写时照旧拒绝）。
- 拒绝时有界记录（每个编码器最多 64 条）：`encoder <nid> refused (<原因>): cmdbuf=... size=... regs=...`。

### 2.3 诊断（默认关闭）

- `LogBadPacket`：Liverpool 遇到未知 type-3 opcode 时，在断言前把前 96/后 32 个 dword 打进日志。
- `debug.shadps4.pm4_validate=1`：`SubmitGfx` 拷贝 DCB 后逐包检查头；发现坏包则打印上下文，并在 2 ms 后
  重读 guest 内存统计变化的 dword（区分"写错"与"提交时仍在写"）。进程内首次读取属性。

## 3. 验证

### 3.1 逐字节对照

详见 `evidence/.../tests.txt`。

- 主机探针 `guest_gnm_fastpath_tests`（链接 `shadps4_host`）：shader.c 原生编译，与生产编码器经
  HLE 包装模型（相同的前置检查、寄存器块读取、保护缓冲）对比返回值与命令缓冲全部字节；覆盖每个
  阈值附近的 size、空/非空缓冲与寄存器、`regs[1]!=0`、各种修饰位与随机值；路由 NID 与 aerolib 名对上、
  与 payload 导出一一对应。Thor 127894/0，Swan 127918/0（新增 24 项 HwInit 写入上界）。
  负对照：把 `SetVsShader` 的 `TrailingNop(11)` 改成 10，得 758 项失败（源码已还原，逐字节比对）。
- FEX 测试 `gnm_fastpath_guest_tests`（`cmake/fex`）：x86 payload 与一个 guest 驱动在真实 FEX 下执行，
  与同一源码的原生编译逐例比对。Swan 终版：16 字节对齐 21000/0、各 dword 相位 21000/0、
  4 个 guest 线程各 12 遍 1008000/0；首版 payload 在同一测试下 3 次均 0 不一致。
  热态每次调用约 22 ns（Thor）/ 28 ns（Swan，含一次 InvokeGuest 摊销）。

### 3.2 血源实测：CPU 开销（Swan，诊所门口，30 FPS 封顶）

血源帧率封顶 30、此处 GPU 忙 88–94%，看的是各线程每帧 on-CPU（`evidence/.../ab/`、`ab-trim/`）。
每行是一个会话；快路径开关每次都重启会话。

| 构建 | 会话 | 快路径 | Guest-1 | Guest-20 | Guest-21..25（各） | GpuComm |
|---|---|---|---|---|---|---|
| 快路径（`e9b6588f`） | off1 | 关 | 25.2 | 7.8 | 3.7–3.8 | 13.9 |
| | off2 | 关 | 22.6 | 7.8 | 3.8–4.1 | 14.9 |
| | on2 | 开 | 25.3 | 5.8 | 2.5–2.6 | 14.2 |
| + 编码器减负（`0ae5fd46`） | trim-on1 | 开 | 25.5 | 4.3 | 2.4–2.5 | 13.7 |
| | trim-off1 | 关 | 22.3 | 6.4 | 4.1–4.2 | 14.6 |
| | trim-on2 | 开 | 26.4 | 4.1 | 2.5–2.6 | 14.4 |

- 绑 shader 的调用集中在工作线程（Guest-20..25，延迟命令缓冲构建）：快路径使 Guest-21..25 各少约
  1.4 ms/帧、Guest-20 少约 2 ms/帧，合计约 8.5 ms/帧 CPU。
- 编码器减负再使 Guest-20 少约 1.5 ms/帧（开关快路径两种情况一致），应主要来自 HwInit 不再整段拷贝。
- Guest-1 在 22–26 ms 之间波动，与开关无对应关系；GpuComm 不变。帧率被 30 FPS 上限封住，本场景看不到 FPS 变化。
- 每种配置只有 1–2 个会话，只说明方向。on1、trim-off2 两个会话因 §3.4 的 GPU fault 作废。

### 3.3 HLE 路径的拒绝情况

关快路径的冷启动会话（`e9b6588f`，启动 → 菜单 → 读档 → 诊所门口约 4 分钟）中，HLE 编码器**没有任何一次拒绝**
（无 `encoder ... refused` 日志）。即游戏从未传过未映射或容量不足的命令缓冲，快路径与 HLE 的有意差别在该流程中不触发。

### 3.4 崩溃调查（与快路径无关的已有 GPU fault）

- Thor 首次开快路径即在进程启动 12 秒崩溃：GpuComm `ProcessGraphics` 遇到未知 PM4 type-3 opcode。
  当时没有坏包转储（§2.3 的诊断是之后加的）；Thor 随后断开，未能复测。
- Swan：开快路径的首轮在读档中 device lost。KGSL 为 `CP (DDE BR) opcode error | opcode=0x00000000`，
  即 **Turnip 自己的 Adreno 命令流**里读到 0，不是 guest PM4。
- 排查过的假设：
  - payload 本身：§3.1 原生与 FEX（单线程、多线程、各对齐相位）全部一致，排除编码或 FEX 执行差异；
  - 快路径写到 HLE 会拒绝的地址（进而写坏宿主内存）：§3.3 零拒绝，不成立；
  - FEX 未对齐 TSO 存储回补（SIGBUS backpatch）：多线程、各相位测试一致，不成立；
  - 冷缓存：装新 APK 后关快路径的冷启动正常，不成立。
- 统计（Swan，同一流程）：开快路径 10 个会话中 2 次 GPU fault，关快路径 6 个会话中 2 次
  （一次约 6 分钟时在诊所门口静置，一次读档 85 秒）。四份 KGSL snapshot 的 SHA 与解析结论见
  `evidence/.../gpu-fault/snapshots.txt`：devcd3 的 IB2 905 dword 全是合法包（26 个 `CP_SET_DRAW_STATE`、
  9 个 draw），抓到的 draw-state 组也正常；推测 0 来自未被抓进 snapshot 的管线 draw-state（管线/BO 生命周期），
  未进一步定位。
- 结论：这是当前构建在 Swan 上已有的宿主 Vulkan 侧问题，开关快路径都会发生。Thor 那次坏 PM4 只有一个样本，
  不能归因。已另起任务跟进。

## 4. 其它说明

### 4.1 multiblock 默认开启（`fex_context.cpp`）

- 默认 `MULTIBLOCK=1` 且循环头 poll 开。`debug.shadps4.fex_multiblock=0` 回到单块；
  `debug.shadps4.fex_loop_poll=0` 只关循环头 poll（保留 multiblock，仅供测量 poll 开销，此时热循环不能暂停）。
  logcat `FexCore` 标签打印当前组合，如 `MULTIBLOCK enabled, loop-header polls on`，非默认组合为 WARN 级。
- 默认模式下 Thor 设备测试：guest 执行测试全部通过（0 失败），调试器测试 86/0；血源默认配置运行正常
  （Thor 较凉时 21.48 FPS，APK `1cc0460c…`）。本轮 Swan 全部会话都在默认 multiblock 下运行。

### 4.2 payload 写法

首版让编译器合并存储（8 字节 `movq`、16 字节 `movups`）。命令缓冲只保证 4 字节对齐，FEX 在 TSO 下把
8 字节存储翻成 `stlr`，跨 16 字节时 SIGBUS 并回补成 `dmb; str`。测试表明这条路径结果正确，但为了避免依赖它，
终版每个 dword 用一次对齐的 32 位存储写，热态开销无可测差别。

### 4.3 工具记录

- Swan 上 `adb root` 后可用 `am startservice ... STOP_EMULATION` 正常停止（`user_stop`）；
  会话停止后画面停在最后一帧，需要 force-stop 空闲 app 才回到 Library（`launch_bb_pad.sh` 已改为总是 force-stop）。
- `gpu-snapshot` MCP 的数据库路径必须在 `build/gpu-snapshot/` 这类允许的目录下，否则 `artifact_root_denied`。
- Swan 的 `android-host_1.log` 有 22 GB（09-24 的调试级日志），占用设备存储，本轮未动。

## 5. 身份

- 最终 APK `0ae5fd46894e14b7f72e67c01ebc3499e20b2c4deddf4dfee4d48581a6cab3bf`：
  `libshadps4_host.so` Build ID `71c3fe034aad16ca6902b2c82f477775ac3995b9`（SHA `1001bca7…`），
  `libshadps4_fex_session.so` Build ID `251287a6e324e80f3b3cd084fa98d1daa7e9509c`（SHA `1e08fbc4…`）。
  生产 payload（终版）3288 字节，`image_sha256=f1d019db459af94d889317c2b210ae03e9faaa255bdd52538d589c10f3d90059`
  （首版为 3072 字节 `dcf588fc…`）；FEX 测试镜像另含测试驱动，3668 字节 `75ba604a…`。
- 过程 APK：`5a53463b`（首版快路径）、`fb7ef71c`（坏包转储）、`1a842f7b`（提交校验）、`e9b6588f`（拒绝日志 + 终版 payload）。
- Swan 最终装的是 `0ae5fd46`；`debug.shadps4.gnm_fastpath`、`debug.shadps4.pm4_validate` 已清空（快路径默认开）；
  `/data/local/tmp` 下本轮的测试文件已删除；最后一个会话因 GPU fault 退出，app 未在运行。
- Thor 在快路径首轮崩溃后断开，未能收尾：设备上仍装着首版快路径 APK `5a53463b`，
  `debug.shadps4.gnm_fastpath` 仍为 `1`（与默认相同，但属性未清空），下次连接时需清理。
