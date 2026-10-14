# KH3：loading 提速（线程私有数据走 guest 快路径）/ MemoryManager 锁等待 / 10-12 晚错误弹窗（2026-10-13）

- 设备与驱动：AYN Odin3（Adreno 830），Turnip 主干 `65b63f66bf5`（驱动 `a847eb5a…`）。
- 游戏：KH3（CUSA15072，Ver 1.10）；错误弹窗一节是 MHW（CUSA09554）。
- 证据：本机忽略目录 `build/validation/odin3-kh3-preempt-20261012/` 下 `M-memlock`、`N-memlock3`、`O-poolmap`（锁）、`P-load-ledger`、`Q-load-tls`、`R-load-ab`（loading），以及 `build/validation/odin3-guest-error-20261013/`（错误弹窗）。

## 1. KH3 loading

### 1.1 时间花在哪里

读档 loading 约 2.5–3 分钟（`loadwatch.py` 截图计时：从第一帧 “Now Loading” 到出现“開始遊戲”提示）。

- **CPU 并不满**：loading 中 simpleperf `--app` 30 s 采样（52,355 个样本、0 丢失），平均只有约 1.75 个核在忙。
- **Guest-50 占了 38%**：它是 UE4 的异步加载线程，约 0.66 个核。其中只有约 30% 在 JIT 生成的游戏代码里，其余是进出 HLE 的开销：
  - `fesetenv` 6.5%（每次 HLE 保存/恢复浮点环境）；
  - FEX `RunInternal` 约 8%、`DispatchNative` 2.8%；
  - 动态 TLS 解析 `tlsdesc_resolver_dynamic` 2.7%；
  - `CaptureSnapshot` 1.8% + `ReconstructXMMRegisters` 1.5%；
  - 各类 mutex/原子约 8%，`std::mutex::unlock` 包含时间 9.1%，基本是 futex 唤醒；
  - 每次 HLE 的 profiler span（`Profiler::Scope`、`LiteTrace::begin/end`、`encode_span_*`）约占宿主部分的一半。
- **调用最频繁的 HLE**：loading 中途 20 s PROF（`profiler_capture file`，877 万个 scope）统计，14.8 s 内：

  | 线程 | HLE | 次数 | 每秒 |
  |---|---|---|---|
  | Guest-50 | `scePthreadGetspecific` | 4,937,150 | 约 33.4 万 |
  | Guest-57 | `scePthreadGetspecific` | 712,099 | 约 4.8 万 |
  | Guest-1 | `scePthreadGetspecific` | 673,143 | 约 4.6 万 |
  | Guest-50 | `scePthreadGetthreadid` | 260,725 | 约 1.8 万 |

  各线程合计 getspecific 约 47 万次/秒。UE4 的分配器每次 malloc/free 都通过 `FPlatformTLS::GetTlsValue` 取线程缓存。在 PS4 上这是几条指令；在这里每次都要走一次完整的 HLE 往返：退出 JIT、浮点环境、寄存器快照、分发、全局 `threads_mutex`、profiler span。handler 本身只有 0.12 µs。

### 1.2 修复：getspecific / getthreadid 在 guest 侧完成

沿用已有的 app 自带 guest 同步快路径（`guest/runtime/sync`，首次 `Bind()` 时发布）：

- **数据搬到 guest 内存**（`guest_sync_abi.h`）：
  - key 表放在宿主分配的一页里，地址写进 payload 的 `shad_sync_window[ShadSyncWindowTlsKeys]`。每个 key 一个 u64：`(sequence << 1) | allocated`，宿主在 key 锁内写。
  - 每线程 256 条 `{sequence, value}` 放在 TLS 块里，正好结束于线程句柄（`fs:[0x10]`）之前。线程 id 写在句柄 +0x10，errno 仍在 +0x08。
  - 只有所属线程会写自己的记录（set，以及退出时的析构轮）。
- **guest payload**：新增 `shad_pthread_getspecific` 和 `shad_pthread_getthreadid`，结果与 HLE 相同：越界、未分配或 key 重建过的都返回 0；key 只取低 32 位。`scePthreadGetspecific`/`pthread_getspecific` 和 `scePthreadGetthreadid`/`pthread_getthreadid_np` 在两种库名下都路由到 payload。
- **宿主**：setspecific、HLE getspecific 和线程退出时的析构都改为读写 guest 记录；key 的创建和删除同步更新 guest 表。
- **开关**：`debug.shadps4.tls_fastpath=0`（桌面 `SHADPS4_TLS_FASTPATH=0`）让这四个 NID 回到 HLE，mutex 快路径不受影响。安装日志里有 `tls_keys=… tls_routes=guest|hle`。
- **测试**：`guest_sync_fastpath_tests` 在 Odin3 上用真 FEX 执行 payload，37/0。原 29 项全过；新增 8 项覆盖：
  - 各线程取到自己的值；
  - rdi 高半部分为垃圾时只取低 32 位；
  - 旧 sequence 的记录读为 0；
  - 删除后读为 0；
  - key 255 可用，256 与负数读为 0；
  - getthreadid 返回句柄里的 id；
  - 全程 0 次 HLE。

### 1.3 效果

同包 A/B（见 1.4）：loading 159–166 s 降到 27.8–27.9 s。下面是 A/B 之前的单次测量：

- 修复前一次（APK `f0c5acaf`，其中 30 s 同时在跑 simpleperf）：162.5 s。
- 修复后第一次（APK `4e5c3583`）：27.8 s。
- 进游戏后奥林匹斯场景：35.3、36.3 FPS（10 s 窗口）；同一区域修复前为 14.7–22.7 FPS（站位不同，仅供参考）。修复前同一场景整体严重过曝发白，修复后颜色正常，原因未查（可能是低帧率下自动曝光的表现）。

### 1.4 同包 A/B

APK `d2183ede`（host `5c64d5cc`），每轮冷启动、读档，`debug.shadps4.tls_fastpath` 交替 0/1。片头 GPU 故障（KH3 #11）的轮次作废重来。

| 轮次 | 开关 | 安装日志 | loading |
|---|---|---|---|
| 1 | 0 | `tls_routes=hle` | 166.4 s |
| 2 | 1 | `tls_routes=guest` | 27.8 s |
| 3 | 0 | `tls_routes=hle` | 159.3 s |
| 4 | 1 | `tls_routes=guest` | 27.9 s |

- 计时误差约 ±3 s（截图间隔）。
- 作废 3 次：第 1 轮前两次和第 3 轮第一次都是片头 GPU 故障，faultcount 79 → 82。
- 进游戏后在奥林匹斯同一出生点看 10 s 窗口：关 15.0、16.0 FPS，开 39.8、43.4 FPS。

## 2. MemoryManager 锁等待

问题：GpuComm 采样中约 20% 在 `sched_yield`，即在等 MemoryManager 的读写锁（`SharedFirstMutex`）。

### 2.1 诊断工具（DebugBus `memory_lock`）

- `memory_lock status|reset`：按“GpuComm / 其它线程”分开统计共享与独占等待，包括次数、睡眠次数、总时长、最长时长和分桶（<10 µs、<100 µs、<1 ms、≥1 ms），以及各写锁持有时长和锁内分步耗时（`Common::StepStats`）。
- `memory_lock holds on|off`：统计读者最外层持有时长。
- `memory_lock spin syscall|pause|timed [us]`：切换 ARM64 上的自旋方式。
- `memory_lock pool_map locked|unlocked`：把池提交/回收的宿主映射放回写锁内，用于对照。

### 2.2 原因

- KH3 每 10 s 有约 4800 次池提交和 4800 次池回收（觉醒之所，不封顶），写锁共持有约 1.65 s。GpuComm 每 10 s 等锁约 5000 次、0.70–0.78 s，大多 100 µs–1 ms，而且进入了 futex 睡眠。
- 写锁里主要是宿主映射：Android 上 `impl.Map/Unmap` 走 `GuestAddressSpace::UpdateDataMapping`。分步计时（每次）：
  - 总计约 171 µs；
  - 账本整表复制约 110 µs，账本约 2 万条；
  - 执行权限扫描约 27 µs；
  - 系统调用约 21 µs；
  - GPU 页状态回放约 8 µs。

  另外池提交在写锁内线性扫描 `dmem_map`，约 130 µs。
- 自旋方式只能缓解：把 `sched_yield` 换成 `yield` 指令或限时自旋，GpuComm CPU 约 −13%，帧率不变。

### 2.3 修复

- **池提交**（`MemoryManager::PoolCommit`）：所有改元数据的路径都先持 `unmap_mutex`，持有它时 VMA 表与物理表不会变。因此选物理页和宿主映射都在写锁外完成，最后只短暂持写锁，一次性登记 VMA、物理页和预算。宿主映射完成前，读者看到的仍是“已保留”，不会访问这段内存。
- **池回收**（`PoolDecommit`）：写锁内只改账本，释放写锁后再解除宿主映射，与 `UnmapMemory` 已有的延后解除映射是同一做法。`unmap_mutex` 仍持着，物理页在旧映射消失前不会被别的提交复用。
- **账本原地修改**（`GuestAddressSpace::PlanLedgerEditLocked/CommitLedgerEditLocked`）：
  - 系统调用前扫一遍，记下重叠记录、算出新分片，并预留容量。这一步完成全部内存分配，所以失败时账本不变。
  - 成功后用“末尾元素填空位”删除旧记录，再追加分片，不再整表复制。
  - 用于 `UpdateDataMapping` 和 `ProtectData`；持 token 的代码发布路径不变。
  - 账本本来就无序，M33 只要求纯追加时顺序不变。
  - 合约测试 Odin3 47/47。

### 2.4 效果

同进程交替，10 s 窗口，APK `5e18264a`：

| 场景 | pool_map | GpuComm 等锁 | 其中睡眠 | 等待总时长 | 写锁持有（提交+回收） |
|---|---|---|---|---|---|
| loading | locked | 1022–1340 | 988–1302 | 209–273 ms | 280–471 ms |
| loading | unlocked | 565–678 | 3–4 | 13–17 ms | 53–55 ms |
| 奥林匹斯 | locked | 977–1590 | 961–1580 | 220–315 ms | 280–372 ms |
| 奥林匹斯 | unlocked | 583–1002 | 0–1 | 5–6 ms | 8–11 ms |

- GpuComm 每帧 CPU 约 10.5 → 10.2 ms。这个场景的帧率不受 GpuComm 限制，看不出帧率差别。
- 读者持有（`holds on`）：GpuComm 每秒约 8.7 万次共享持有，绝大多数 <10 µs，每 10 s 约 12 次 ≥1 ms。
- 回收的账本部分偶有 6–9 ms 的单次持有，未定位（可能是持锁线程被抢占）。

## 3. 10-12 晚的错误弹窗

- **现场只剩一个会话**：日志按会话轮转，只保留 3 份，10-12 晚的会话日志只剩 MHW 的一次（PID 6236，00:10:45–00:15:27）。更早的会话日志已被今天的会话覆盖。
- **致命 guest 故障为 0**：`files/host/log/fex-fault-*.txt` 全部 0 字节；这些文件每个会话开局创建，只在致命故障时写入。
- **MHW 会话里的弹窗**：
  - 00:11:17，游戏打开一个 MsgDialog（mode 2，正常显示的 ImGui 模态框）。
  - 00:11:20，`sceNpGetAccountIdA` 返回 `0x80550006`（`ORBIS_NP_ERROR_SIGNED_OUT`）后，游戏发出 “An error has occurred. Code: 0X80550006” 和“无法连接至PlayStation™Network。”，被静默对话框自动确认。
  - 这是离线未登录时游戏自己的提示，与桌面离线语义一致，之后游戏又正常运行了 4 分钟。
- **会话结束原因**：GPU 设备丢失。KGSL 时间戳等待失败（context 15，timestamp 35626，errno 71），Present 线程断言，进程 SIGABRT。
- **之前的 GPU 故障**：前一个会话（PID 5578）00:10:30 也是 Present 断言退出。设备上未读取的 KGSL snapshot（fault 78，时间戳折算约 00:10:28）就是它。

  第一次拉取时，Git Bash 把设备路径改写成了 Windows 路径，设备上的 `cat` 没有读到 dump 节点，snapshot 仍在设备上，本地只留下无效的 `.partial` 和 `.pull.lock`。因此没有重试。
- **crash 缓冲区**：10-12 17:48–10-13 00:15 共有 9 次 `shadPS4:Present` SIGABRT，都是 GPU 设备丢失后的断言退出。

## 4. 其它

- `Unimplemented IT_SET_PREDICATION` 每个包一条 Warning，MHW 4.5 分钟写了 35 万行、46 MB，已改为 Debug（计数仍在 `dispatch_diag.predications`）。
- `shadPS4:Waker`（唤醒代理）在 Odin3 上占约 4.5–4.7 s/10 s CPU。Odin3 没有小核，它在中核上自旋，未处理。
- 每次 HLE 往返仍有固定开销（`fesetenv`、寄存器快照、profiler span），对其它高频 HLE 仍然适用，未处理。

## 5. 午间后续：叶子 HLE 走 guest、PKG 读取硬件 AES、rwlock 表（本地未提交）

证据：`build/validation/odin3-kh3-preempt-20261012/` 下 `W-load-waker2`、`X-load-pkg`、`Y-game-leaf`、`Z-game-prof`。

- **叶子 HLE 在 guest 侧完成**（`debug.shadps4.leaf_fastpath`，默认开；桌面 `SHADPS4_LEAF_FASTPATH`）：同步 payload（`guest/runtime/sync/mutex.c`）新增 `pthread_setspecific`、`pthread_equal`/`scePthreadEqual`、`sceKernelGetProcessTimeCounter`、`sceKernelReadTsc`、`sceKernelGetTscFrequency`；时钟用 guest `rdtsc` 减去会话原点（与宿主 `GuestClock` 同一 TSC 缩放），频率与原点由窗口表下发。Gnm payload（`guest/runtime/gnm/shader.c`）新增 `sceGnmDrawIndex`、`DrawIndexAuto`、`DrawIndexOffset`（含 Neo 变体）、`DispatchDirect`、`InsertPushMarker`/`InsertPopMarker`，编码与桌面 `gnmdriver.cpp` 逐字节一致。同包同存档点：场景内 leaf=0 为 29.6/34.0、35.4/34.6 FPS，leaf=1 为 38.3/38.4 FPS（GPU 频率各轮不同，后者已 98% 忙）；loading 27.9–29.0 s → 23.0 s。单测补在 `tests/host_runtime/guest_sync_fastpath_tests.cpp`、`guest_gnm_fastpath_tests.cpp`、`tests/guest_cpu/sync_fastpath_guest/entry.c`。
- **唤醒代理对照**（loading 时间）：on 27.8/27.7 s，off 31.2/30.2 s，保留默认开。
- **PKG 读取**（`pkg_reader.cpp`、新增 `src/core/crypto/aes_xts.{h,cpp}`）：整段 `pread`、AES-128-XTS 用 ARMv8 Crypto 扩展（x86 用 AES-NI，构造时与 LibreSSL 自检，失败回退），4 块交织；32 MiB 解压块缓存，解压不持锁。`debug.shadps4.pkg_reader=reference` 恢复旧的逐扇区串行路径。`pkg_reader_tests`（IEEE 1619 向量、硬件与 LibreSSL、真实 PKG 随机/并发/顺序读与参考路径逐字节比较）：随机读 134 → 341 MB/s，4 线程 1058 MB/s，结果与参考一致。loading A/B（fast 24.2/20.9 s，reference 24.5/24.3 s）差别在误差内，PKG 读取不是 loading 瓶颈。
- **rwlock**（`guest_rwlock.h`）：对象表改为 `shared_mutex` + 每锁 mutex/condvar、读者小向量与销毁标记，API 与语义不变。主包（APK `588e3b13`）Guest-44 的 CPU 2.9 → 1.6 s/10 s，Guest-1 3.9 → 2.9 s/10 s。
- **之后的状态**：场景内 GPU 98–99% 忙并因温控停在约 832 MHz，帧率受 GPU 限制；Guest-57 在 rwlock 读锁/解锁循环里占满一个核（约 3.1 万次 HLE/s，很少真正阻塞），调用方见第 6 节。GPU 侧的后续见 [kh3-scaled-storage](kh3-scaled-storage-20261013.md)。

## 6. Guest-57 的调用方（10-13 晚）

证据：`build/validation/odin3-kh3-preempt-20261012/AA-guest57/`（APK `e11822f5`）。

- **新增诊断** `debug.shadps4.hle_caller_log=<名字片段>[,…]`（`guest_runtime.cpp` `LogHleCaller`，会话开始时读取，`0` 为关）：profile 名包含任一片段的 HLE 函数，每线程每 997 次调用记一次 guest 返回地址、参数与最多 6 层 RBP 链父帧，每进程最多 256 行（`HLE caller …`）。
- **不是 rwlock 空转**：simpleperf（`--app` + `task-clock`，Odin3 不支持 `cpu-clock`）对 Guest-57 采 10 s、16,697 个样本：83% 在 JIT 的游戏代码，`libshadps4_host.so` 7%，内核 6.5%。静止场景中持续 86–88% 一个核，HLE 约 3.8 万次/s（同时 Guest-1 约 7.6 万、Guest-44 约 8.7 万次/s）。
- **rwlock 的来源**：Guest-57 的读锁/解锁全部来自 eboot `fn_3d7e6a0`（返回地址 0x417e723/0x417e7b1，模块偏移 0x3d7e723；0x19f2 字节），锁对象始终是同一个全局 rwlock `0xc20de80`（模块偏移 0xbe0de80）；上层调用链各不相同，是被到处调用的公共查找函数。调用链的公共根为线程入口 `fn_3666820`（0x3a669a8）→ `fn_4476b50` → `fn_44752a0`（0x4875354，73 个样本中 47 个）。Guest-44 用的是另一组函数（读锁 0x7bad2a/解锁 0x7bb021，写锁 0x4172bf6）。
- **guest debugger 首次在 Windows 主机上实际使用**：attach、暂停、列线程、读寄存器可用；暂停时 Guest-57 停在 HLE veneer（RIP 0x180002c00f），读栈需要另加 `allowNativeMemoryRead` 权限。一次约 26 s 的暂停后游戏自行退出（Guest-1 依次 `sceHttpTerm`、`scePlayGoTerminate`、拆会话），暂停要尽量短。
- **IDA 识别结果：Guest-57 是 UE4 渲染线程，这份工作是真实负载，不是轮询。** 依据为反编译结构与 UE4 4.18 源码逐段对应（IDA 库已命名，快照 `D:\workspace\kh3-export\20261013-094604-205d63dc14cc4b8394228bdd04e52590\review.i64`，不入仓库）：

  | 偏移 | 识别为 | 依据 |
  |---|---|---|
  | `0x446f1a0` | `FRendererModule::BeginRenderingViewFamily`（游戏线程） | `Canvas->Flush_GameThread`；遍历 ViewExtensions；按 FeatureLevel 分配 552/568 字节的移动/延迟场景渲染器；`USceneCaptureComponent::UpdateDeferredCaptures`（含 `UObjectArray.cpp` 断言、按 +1108 优先级排序）；最后 `ENQUEUE_RENDER_COMMAND`：非线程渲染时在 `0x446fd90` 直接调用 `0x44752a0`，否则包成 TGraphTask 投给渲染线程 |
  | `0x4476b50` | `TGraphTask<FDrawSceneCommand>::ExecuteTask` | 调用 `0x44752a0` 后析构任务、放回 TLS/无锁空闲表 |
  | `0x44752a0` | `RenderViewFamily_RenderThread` + `FlushPendingDeleteRHIResources_RenderThread` | `FMemMark`；只执行一次的全局链表 `0xbf792d0` 调 `UpdateDeferredResource(RHICmdList, true)`（即 `FDeferredUpdateResource::UpdateResources`）；`SceneRenderer->Render(RHICmdList)`（vtable +0x10）；删除场景渲染器；末尾刷新待删 RHI 资源 |
  | `0x3d7e6a0` | `PipelineStateCache::GetAndOrCreateGraphicsPipelineState` | 可选 `ApplyCachedRenderTargets`；读锁查 160 字节初始化器为键的哈希表；未命中时解锁、加写锁、再查一次（`RaiseLockToWrite`），新建 184 字节 `FGraphicsPipelineState`，必要时投异步编译任务；581 个调用点（各个 mesh drawing policy 的 `SetGraphicsPipelineState`） |
  | `0xbe0de80` | `GGraphicsPipelineCacheLock` | 上述函数唯一使用的 rwlock |

  也就是说，Guest-57 每帧执行整份 `FSceneRenderer::Render`（可见性、mesh pass、RHI 命令生成），每次 draw 设置 PSO 时对全局 PSO 缓存加一次读锁、解一次锁：约 3.8 万次/s 的 HLE 基本就是“每个 draw 两次 rwlock”。这份工作在 PS4 上同样每帧发生；PSO 新建（写锁）只在首次遇到新状态组合时出现。
- **能做的**：
  - `pthread_rwlock` 读锁/解锁的 guest 快路径（同 mutex Tier B，无竞争时只做原子操作）：去掉这 3.8 万次/s HLE 往返，估计省约 10% 的 Guest-57 时间；Guest-1、Guest-44 的 rwlock 调用同样受益。写锁很少，走慢路径即可。
  - 不能用 guest 补丁直接去掉这把锁：PSO 缓存也可能被并行渲染任务等其它线程读写。
  - 其余约 83% 是游戏自身的渲染线程代码在 JIT 下的执行时间，只能靠 JIT 效率或降低场景负载（画质设置/cvar）减少。静止场景约 47 FPS 时它占一个核的 86–88%（约 18 ms/帧），已接近 CPU 侧的帧时间上限。

## 7. rwlock guest 快路径与渲染线程的查询等待（10-13 晚，本地未提交）

证据：`build/validation/odin3-kh3-preempt-20261012/` 下 `AB-rwlock`（含 `perf-on`/`perf-off`、`prof-on`、`dbg`）、`AB-qwait`；APK `c77c3c98`（rwlock 快路径）、`c4d0d1e6`（再加格式修复，存于 `apk/`）。

### 7.1 rwlock 快路径

- **做法**：`pthread_rwlock_{rd,tryrd,wr,trywr}lock/unlock` 与 `scePthreadRwlock*` 的 10 个入口由同步 payload（`guest/runtime/sync/mutex.c`）在 guest 内完成。锁对象放进第二个 256 MiB 窗口 `GuestRwlockObjects`（窗口表槽 5/6），前缀为 u32 锁字（bit31 写锁、bit30 有写者在宿主等待、bit29 有读者在宿主等待、bit0–28 读持有者数）、u32 类型、u64 写者。每线程在 TLS 记录下方有 256 字节读持有表（15 项 {锁, 深度} + 溢出计数），递归读只改深度，锁字里每个线程只计一次；EDEADLK、EPERM、类型 1/2 的写者优先与“已在读的线程可再读”都由这张表判断，与原 HLE 一致。阻塞、超时、错误、静态初始化器与第 16 个持有都走 HLE 回退（导入槽 5–9）。宿主 `guest_rwlock.h` 改为在同一锁字上实现（每锁 mutex 管等待计数和等待位，等待者经 mutex 域的 `GuestAddressWaiters` 按锁字地址停车，即 `shadSyncWake` 唤醒的同一实例），rwlock 的属主标识改为线程 handle。`debug.shadps4.rwlock_fastpath=0`（或 `SHADPS4_RWLOCK_FASTPATH=0`）、`rwlock_log` 时仍走 HLE。
- **测试**：FEX 实跑 `guest_sync_fastpath_tests` 78/0（4 轮，新增 32 项：读写互斥、递归、类型 2 拒绝、溢出、写者优先、取消、混合读写一致性），宿主 `guest_rwlock_tests` 123/0，诊断 1233/0。1 万对无竞争读锁/解锁约 0.33–0.47 ms。
- **效果**（隔会话 A/B，各两窗口；GPU 98% 忙）：on 46.5/47.8、49.7/48.2 FPS（GPU 883/879、866/849 MHz），off 44.5/44.3、47.2/46.5 FPS（846/846、836/871 MHz），平均约 +5%，区间有重叠。Guest-57 的 rwlock HLE 由约 3.8 万次/s 降到 10 s 内约 416 次读锁慢路径、解锁 0 次；Guest-1 4.2–5.0 → 3.2–3.4 s/10 s，Guest-44 3.4–3.8 → 1.5–1.6 s，Waker 5.2–6.1 → 3.8–4.0 s。
- **但 Guest-57 的 CPU 反而升高**（off 7.9–9.0 → on 10.7 s/10 s）。simpleperf：off 7.21 s 在 CPU、主动切换 6.3 万次/10 s、JIT 70%、内核 12.9%；on 9.86 s、5.5 千次、JIT 92%、内核 1.8%，样本集中在约 20 个连续的 JIT 地址（紧凑循环）。PROF 中 Guest-57 的阻塞等待合计只有约 0.8 s/10 s。即省下的 HLE 时间变成了 guest 内的空转。

### 7.2 空转的位置：UE4 GnmRHI 等查询结果

- guest debugger 暂停取样 Guest-57 的 RIP（6 次，2 次落在同一函数、同一栈帧）：eboot `0x3a0d10`。IDA：它只被 `0x3955d0` 调用，后者是 GnmRHI 的 `RHIGetRenderQueryResult`（RHI vtable +728）：类型 2 为时间戳（结果 / 800 或 911 MHz），类型 1 为遮挡查询（8 个、Pro 上 16 个 render backend 的 begin/end 计数都带 bit63 有效位时求差之和）。结果未到且调用方要求等待时进入 `0x3a0d10`：处理渲染线程队列里的任务、检查查询字、检查超时，循环，从不睡眠。成功时把等待计入每帧空闲统计（计数 `0xbe0e20c`、时间 `0xc02ba50`）。调用方为 `0x44919e0`（遮挡测试，总是等待）和 `0x455f0f0`（以查询实现的围栏/计时对象，按参数等待）。
- 模拟器在 GPU 命令处理线程解析到查询的 `EVENT_WRITE ZPASS_DONE`（`liverpool.cpp`，写入固定递增的假计数）或 EOP 包时才写这些字。GPU 受限的场景里该线程被宿主 GPU 节流，渲染线程于是空转大半帧；rwlock 快路径让渲染线程更早到达这里，空转随之变长。

### 7.3 guest 补丁：等待时睡眠

- `guest/games/CUSA15072/query_wait.cpp`（最初为单独的包 `kh3_query_wait_sleep_01_10`，A/B 用的是它；后与 7.4 合并为 `01.10/cpu_waits.recipe.json`，按代码签名识别版本）：在循环回边 `0x3a0eee`（`cmp byte [r15],0; jz 0x3a0d70`）放 `before` site，结果仍未到时调用游戏自己的 `sceKernelUsleep(100)`（PLT `0x77499e0`）。超时检查与循环内的任务处理不变，结果最多晚一次睡眠被看到。`guest-patch.log` 每 16384 次睡眠记一次累计数（实测约 3.8 千次/s，即每次实际睡约 190 µs）。
- 顺带修复：宿主包格式检查只允许 sdk_version 2 带 site 和代码补丁，sdk_version 3（2 的超集）的包被拒（`guest_patch_format.cpp` 改为 ≥2，`scripts/android/guest-patch` 接受 3）。新增 gtest `SignedBuildsKeepSitesAndCodePatches`，本机没有桌面构建，未编译运行；Android host 编译通过。
- **同会话交替 A/B**（rwlock 快路径开，`guest-patch enable|disable`，每窗口 10 s）：

  | 窗口 | FPS | GPU MHz | Guest-57 CPU | 全进程 CPU |
  |---|---|---|---|---|
  | on ×3 | 51.3 / 49.2 / 49.9 | 922 / 927 / 922 | 3.41 / 3.36 / 3.37 s | 23.96 / 23.18 / 23.36 s |
  | off ×3 | 48.9 / 46.4 / 46.8 | 880 / 900 / 906 | 10.67 / 10.73 / 10.72 s | 30.26 / 29.50 / 29.71 s |

  Guest-57 −69%，全进程 CPU −21%（每 10 s 少约 6.3 s CPU），帧率约 +6%，区间不重叠；GPU 始终 98% 忙，开补丁时频率高 20–40 MHz，推测是 CPU 功耗下降后 GPU 得到的余量（未单独测功耗/温度）。开补丁的场景截图正常。
- **未做**：只验证了奥林匹斯存档点；遮挡结果本身仍是假的固定值（全部可见），这个补丁不改变它；更换版本需要重新核对签名。

### 7.4 游戏线程的帧率上限等待（`AC-guest1`、`AB-tick`）

- 先前把 `guest_watchdog dump` 的“500 ms 内 27960 次 HLE、当前在 `sceKernelUsleep`”误读成 usleep 循环：那是 Guest-1 全部 HLE 次数，当时恰好在 usleep 里。PROF（10 s）中 Guest-1 为 `scePthreadYield` 25.9 万次（每次 HLE 约 1.5 µs）、`pthread_cond_signal` 2.7 万、`pthread_cond_timedwait` 5965 次共 5.43 s、`pthread_cond_wait` 3625 次、`sceKernelUsleep` 140 次共 0.75 s；simpleperf：Guest-1 在 CPU 上 3.05 s/10 s，其中内核 14%，JIT 部分无集中热点。
- **调用者**：`hle_caller_log` 的 256 行预算会在 loading 中用完，新增 DebugBus `hle_caller_log status | arm [lines]`（`guest_runtime.cpp` `HleCallerLogCommand`），进场景后重新给预算。场景里 423 个 yield 样本全部来自 eboot `0x5d8e2ea`，所在函数 `0x5d8dfc0` 按日志字符串（UnrealEngine.cpp 1534/1634 行、“Detected negative delta time”）是 UE4 `UEngine::UpdateTimeAndHandleMaxTickRate`：剩余等待时间超过 5 ms 时先睡（剩余 − 2 ms），然后循环 `scePthreadYield` 直到结束时刻（`rbp-0x40`，进程时间计数 × `0xa7065f8` 处的秒/周期）；剩余 ≤ 5 ms 时整段都在 yield。`0x37e5b70` 是 `FPlatformProcess::SleepNoStats`（0 → yield，否则 usleep）。
- **补丁**：并入同一个包（两个 site 都调用游戏的 `sceKernelUsleep`，不同包调用同一段代码会被冲突检查拒绝）：`01.10/cpu_waits.recipe.json`，包 `kh3_cpu_waits_01_10`，`tick_wait.cpp` 的 replace site 代替 `0x5d8e2ea` 的 `call scePthreadYield`：剩余超过 0.4 ms 时睡到只剩 0.2 ms，之后照旧 yield；循环自己的结束判断不变。两个 site 可分别开关（`guest-patch SERIAL disable kh3_cpu_waits/tick_wait_round`）。
- **同会话交替 A/B**（查询等待 site 一直开，只开关 `tick_wait_round`；`gameab_patch.sh`）：

  | 窗口 | FPS | Guest-1 CPU | 全进程 CPU |
  |---|---|---|---|
  | on ×3 | 52.5 / 50.2 / 51.3 | 2.99 / 2.90 / 2.86 s | 23.96 / 23.28 / 23.27 s |
  | off ×3 | 49.1 / 50.2 / 51.5 | 3.37 / 3.49 / 3.48 s | 23.19 / 23.65 / 23.94 s |

  另取 10 s `hle_sync`：开 site 时 `Thread.Yield` 22954 → 1482 次/s，`Thread.Usleep` 4717 → 4766 次/s（每帧最多多一次睡眠）。Guest-1 少约 0.53 s/10 s（−15%）；全进程 CPU 与帧率的差别在波动范围内。

### 7.5 其它观察

- `shadPS4:Waker` 在 Odin3 上仍占约 3.8–4.0 s/10 s，现在是 GpuComm 之后 CPU 最高的线程。
- Guest-1 的 `pthread_cond_timedwait` 每帧约 11 次、平均 0.9 ms（共 5.4 s/10 s），是游戏线程等渲染线程，不耗 CPU。
