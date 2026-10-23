# Guest / native 调试链路与数据断点（2026-10-15）

设备 Odin3（`6edb0593`），KH3 CUSA15072 奥林匹斯存档场景（基线约 12.5 帧/秒）。最终 APK `dade8d37…`。

## 链路状态

| 环节 | 结果 |
|---|---|
| guest RSP 连接、暂停、线程、寄存器、内存、混合栈 | 可用 |
| 软件断点 | 可用 |
| 单步 | 可用；从 MCP 已拥有的软件断点上直接单步仍需先删断点（MCP 缺 atomic step-over） |
| 模块身份（`qXfer:citron-modules` / `citron-debug-state`） | 新增；MCP `symbols prepare/load/resolve` 可绑定 `guest/games/CUSA15072/01.10/symbols` |
| 写 / 读 / 访问数据断点 | 修复后可用（见下） |
| IDA workspace、反编译、callers、bind/correlate | 可用 |
| native LLDB（`android_debug_*`） | `transport=tcp_gdbserver` 可用（attach、线程列表、`image lookup` 到源码行、detach 后 TracerPid 0）；默认 `abstract_platform` 在本机 attach 失败（CodeLLDB 连接被关闭），未查 |
| `guest_debug_frame_chain` | 是 Citron 的 GPU 输出采集接口，与调用栈无关；shadPS4 无此提供方 |

## 数据断点的问题与修复

1. **插桩把 64 位值截断（崩溃根因）**。FEX 寄存器分配器会把同时写入 PF/AF 的 64 位中间值放在 raw PF/AF 静态寄存器（x26/x27），例如 `dec qword [rdi+8]` 的结果与读出的原值，并在之后当数据用（这里是写回内存）。观察调用 `DebugMemoryAccess` 前后按 32 位溢出/回填 PF/AF，高 32 位丢失，写回内存的指针被截断（`0x100fb79d40` → `0xfb79d40`），随后别的线程用它访存崩溃。二分：只对普通 load 插桩不崩、只对 TSO store 插桩崩；插桩代码反汇编直接看到 `subs x26, x27, #1` → 调用 → `ldp w26, w27` → `stlur x26`。修复（主仓 `raw_flag_stores.h`）：插入调用后，每个 StorePF/StoreAF 改用紧挨着的 `Copy`，只有副本落在 PF/AF，原值留在会被完整保存的寄存器。`ProfilePass`（auto tag 探针）同样插调用，一并处理。
2. **x87 `fstp tword` 被拒绝**：`StoreStackMem`（x87 pass 降级前的形式）未识别，libc 的 long double 打印每次都被拒绝，主线程停在 Unsupported。现按 80/64/32/16 位写入观察。
3. **停止归属错误**：一个线程命中 watch 时，其它线程在各自单步结束处停下并报 StepComplete，RSP 按线程号选第一个，回包归到线程 1 且不带 `watch:`。现在命中/断点优先；旁观线程报 PauseRequested。
4. **太慢，UE4 渲染线程超时退出**：原方案是所有线程逐指令单步、每步编译一次（约千倍减速），30 秒后 `GameThread timed out waiting for RenderThread` 退出。新默认：watch 生效期间 FEX 每次翻译只含一条 guest 指令（`MaxInstPerBlock=1`，首个 watch 生效和最后一个移除时丢弃全部翻译），插桩翻译照常缓存；命中时保护本线程中断页，在下一个块入口（即访问指令的后继）停下，停止状态与单步一致，`watch-rip` 为访问指令。REP 计数、方向照旧从溢出状态读取。旧的逐步模式保留为 `debug.shadps4.watch_mode=0x4000`。
5. 其它：REP MOVS/STOS 按运行时长度观察、非临时存取、AVX128 成对 256 位访问（上一轮）；单步时最近 16 步的寄存器环形记录在致命 JIT 故障时写入 `fex-fault-*.txt`。

## 验证

- `guest_execution_tests --focused-debugger` 93/0（三次），完整套件 259/0。新增 DW23（`dec/add/sub/inc qword [mem]` 在观察下保持高 32 位；去掉修复的负对照得 4/6/4/6 → 失败）、DW24（`fstp tword` 记为 10 字节写）。
- 游戏内：写 watch `0xc20e20c`（`GnmRHI_QueryWaitSuccessCount`）命中 Guest-57（RSP 线程 0x39）：`watch-rip 0x7a0f5c`（4 字节）与 `0x4d8cc06`（写 `0xc20e208` 8 字节），停在 `0x4d8cc11`，寄存器可读；MCP 返回 `stopReason=watch`、`watchHit{instructionPc, accessAddress, accessBytes}`。
- 速度：watch 生效且不命中时约 1.3 帧/秒（基线 12.5，约 10 倍减速），45 秒不退出；移除后回到 12.5–12.7 帧/秒。每次命中都会停全部线程，持续高频命中十余秒后游戏会自行退出（AJM 报错后终止），建议命中后先删 watch 再继续。
- 读 watch 在游戏内 20 秒未命中（该地址可能只写不读），以 DW03 单测为准。

## 用法

1. 启动前 `setprop debug.shadps4.guest_debug_port 24680`，`guest_debug_wait 0`。
2. `guest_debug_start_session` → `pause` → `manage_breakpoints add type=write|read|access` → `continue` → `wait_for_event`。
3. 用完 `stop_session`，清空两个属性。
4. 符号：`guest/games/CUSA15072/01.10/symbols`（v2；函数无 size，函数内部地址暂不能解析）。

## 未做

- native `abstract_platform` attach 失败原因；MCP atomic step-over。
- 读/访问 watch 的游戏内命中。
- 进一步提速（只给访存指令开单指令翻译、IR 内范围预筛）。
