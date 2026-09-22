# Swan 血源诊所复测（去 fulldump 后）与 mutex 快路径失效根因（2026-09-22）

分支 `feature/malos/swan_performance`，设备 Swan `PB3110PGL6240001G`（2026-09-22 重刷系统，用户随后去掉 ROM 的 fulldump 调试策略：`ro.boot.debugpolicy=minidump`，崩溃不再落 tombstone，只剩 `logcat -b crash` 一行）。锁频仍用 spatial-debug-tool `swan-evt-legacy`（cpu0–5 1.90 GHz、cpu6–7 1.50 GHz、GPU 726 MHz），Render 0.5 / Texture medium（重刷后 `global.json` 默认是 high，先改回）、mainline Turnip `86ca472f`。证据在 [evidence/swan-bloodborne-baseline-20260921/](evidence/swan-bloodborne-baseline-20260921/) 的 `bb-clinic-R-postfulldump/`、`bb-clinic-S-syncwindow/`（PROF、host 日志、原始 BMP 只留 SHA）。前情见 [Render Scale 覆盖率与融合回读](swan-render-scale-coverage-20260921.md)。

## 结论

1. **复测本身发现了一个静默回归**：Tier B 的 guest mutex 快路径自 PSVR 内存工作把 `ServiceAllocationBase` 从 64 GiB 挪到 112 GiB 之后就一直没有生效。`hle_sync status` 仍报 `fast_path: installed`，但 host 日志有一行 `GuestSyncObjects block 0x1c00210000 is outside the fast-path arena window`：payload 里 `SHAD_SYNC_ARENA_BASE/LIMIT` 写死在 `[0x1000000000, 0x1010000000)`，所有 arena 对象都落在窗口外，guest 端逐个回退 HLE。run R 里 Guest-1 每帧 2625 次 `pthread_mutex_lock` + 2660 次 `unlock` HLE（35.7 ms/帧），整个进程 14.5k 次同步 HLE/帧，没有任何 `Sync.AddrWait/AddrWake` 行。此前 J–Q 全部轮次（含融合回读 A/B）都是在这个状态下测的。
2. **修复**：窗口不再是编译期常量。host 在其它 service 分配之上 4 GiB 处预留一整块 256 MiB `GuestSyncObjects`，arena 16 KiB block 从中顺序切出，publication 时把 `[base, limit)` 写进 payload 新增的 `shad_sync_window` 表；预留失败时状态变为 `installed_no_arena_window`，出窗 block 的告警带上窗口范围。真机 `guest_sync_fastpath_tests` 29/0（游戏运行时 28/29，SF13 是停车时序检查，空闲重跑通过）。
3. **同场景 A/B（诊所可操作场景，锁频）**：修复前 run R 9.45 / 9.85 / 9.76 FPS（1083 draws/帧指纹），修复后 run S **14.25 / 14.29 / 14.39 FPS**（1484 draws/帧重指纹，比 R 更重）。对照修复前的重指纹 run L 8.49 / 8.45 / 8.77，合计 +65%（其中融合回读约 +10%，其余来自快路径）。Guest-1 的 HLE 从 49.3 ms/帧降到 23.8 ms/帧（6164 → 862 次/帧）。
4. **瓶颈迁移**：修复后帧 69 ms，`shadPS4:GpuComm` 的 `PM4.Resume` 占 67.9 ms/帧（98%），guest GPU 命令 48.6 ms/帧，帧 owner Guest-19 在 `GNM.SubmissionGate` 等 55 ms/帧。下一步的主矛盾是 **PM4→Vulkan 的每 draw CPU 成本（约 46 µs/draw @1.9 GHz）**，其次是 GPU 每帧 ~50 ms；见 [下一步 spec](../../specs/swan-bloodborne-cpu-gpu-next-20260922.md)。
5. 顺带修正一个错误假设：把 256 MiB 窗口直接预留在 `ServiceAllocationBase` 起点会把首批栈/TLS 往上挪 256 MiB，血源在启动 10 s 内于 guest RIP `0x1020b6ac2` 空指针崩溃（3/3，`debug.shadps4.sync_fastpath=0` 同样崩）；窗口挪到 +4 GiB 后正常。那个依赖地址的消费者尚未定位（寄存器里 RSP=0x1c1020b840、RBP=0x1c1020b960，R10=0x80020000），保留为待查项。

## 过程记录

### 重刷后的前置

- `global.json` 重刷后只剩语言，Texture 默认 high；按之前基线改回 `gpu.internal_scale=0.5`、`gpu.texture_quality=medium`（owner `u0_a152`、mode 600）。
- 老的 spatial-debug-tool 会话绑定旧 boot id，重新 `device_session_open`（`5d92e11b…`）。锁频操作的 baseline 这次记录到真实值（policy0 min 384000、GPU 160/902），`device_frequency_stop` 会正确还原；但曾出现 `gpu_max_clock` 回写 902 读回 826 和 policy0 max 被外部改成 2112000 的冲突，工具拒绝覆盖，重试后清理完成。
- 没有存档：血源从 New Game 走亮度页 → 操作页 → 开场动画（~2 min，21 FPS）→ 角色创建（必须起名，ImGui 虚拟键盘，圈键选字、R2 完成）→ 诊所。这个中文版 **圈键确认、叉键返回**，旧脚本只按圈键，在亮度/操作页不会前进。屏幕用 `pico_capture_dump_layer` 左眼 + `tools/panel_crop.py` 读，`screencap` 是旋转后的低分辨率画面。
- 第一次尝试（pid 15279）在标题菜单按到第 9 次时进程 `SIGSEGV SEGV_ACCERR fault 0x24d3c0040`（Guest-19），时间与 DumpLayer 第一次施加 "Enable DumpLayer Support" 预设在同一秒（host 14:08:08 UTC ↔ 设备 03:40:50）。无 tombstone，`fex-fault-15279-1.txt` 为空。之后 `debug.spruntime.etfr.subsample=0` 保持，再没重现；不作为根因结论。

### run R：修复前（APK 36528ab6 / host 71b8135e，pid 26489 gen1，PROF `6fca7458…`）

| 项 | 值 |
|---|---|
| FPS 3×10 s（锁频） | 9.451 / 9.85 / 9.755 |
| 331 帧差分 | 1083 draws/帧（scaled 897）；pass 实例 187.3/帧 = 天然 160.3 + 重开 26.0（hle 18.9、sampled_image 5.5、image_copy 1.0、download 1.0） |
| 融合回读 | `fused_readbacks` Δ = `image_buffer_syncs` Δ = 999（3.0/帧），34.7 MB/帧 |
| PROF（163 帧，detail + hle_sync 开着，8.2 FPS） | 帧 122.2 ms（p90 129.9） |
| GPU / 帧 | GuestCommands 51.1、RenderPass 36.0（≥5 ms 2.6 个 = 19.1）、HostReadback 4.8、OverlayRedraw 4.1、Dispatch 3.5、Transfer 2.3、Present 1.4 |
| Guest-1 / 帧 | HLE 49.3 ms、6164 次：mutex_lock 2625（26.7 ms）、unlock 2660（9.0）、CondWait 3.1（4.0）、WaitSema 31（2.5）、SetPs/Vs 133+91（2.6）、SignalSema 40（1.0）、cond_broadcast 50（0.8）、IsUserPaEnabled 219（0.17）、rwlock 62+62；未注解 72.7 ms |
| Guest-19 / 帧 | CondWait 99.0 ms 等 Guest-1；mutex HLE 271 次 |
| GpuComm / 帧 | `PM4.Resume` 70.5 ms（73 段） |
| hle_sync 20 s / 160 presents | Mutex.Lock 1,044,993、Unlock 1,054,189（各 ~6.5k/帧）、Rwlock 55,492×2、Sema.Wait 26,377、Signal 18,719、Mutex.Config 16,158、Usleep 10,551、Cond 8,548×2、Init/Destroy 5,708×2；合计 2.32M = 14.5k/帧；**无 AddrWait/AddrWake** |
| 进程 | RSS 2545 MB、swap 1101 MB、MemAvailable 1109 MB |

此时 GPU 只占 122 ms 帧的约 57 ms（含 overlay），是 CPU 侧限速：Guest-1 全帧在 HLE 与 JIT 之间，帧 owner 每帧等它 99 ms。

### 根因与修复

- 现象链：`InstallSyncFastPath` 正常发布 payload（`base=0x1800014000`），`GuestSyncArena` 的 block 由 `MapMemory(hint=ServiceAllocationBase=0x1c00000000)` 分配得到 `0x1c00210000`，而 payload 的 `Object()` 判断 `address - 0x1000000000 >= 0x10000000` → 全部走 HLE 回退导入。`sync_fastpath_status` 仍是 `"installed"`。
- 改动（`src/core/host_runtime/guest_sync_abi.h`、`guest/runtime/sync/mutex.c`、`guest_runtime.cpp`、`tests/host_runtime/guest_sync_fastpath_tests.cpp`、`guest/runtime/sync/README.md`）：
  - ABI 去掉 `SHAD_SYNC_ARENA_BASE/LIMIT`，新增 `SHAD_SYNC_ARENA_WINDOW_SIZE`（256 MiB）与 `shad_sync_window[2]` 表（`ShadSyncWindowBase/Limit`）。payload `Object()` 读表比较（两次 RIP 相对加载，无 HLE）。
  - host 在 `InstallHandlers` 里 `Allocate(256 MiB, "GuestSyncObjects", ServiceAllocationBase + 4 GiB)` 预留窗口（只占地址空间，页按需提交），arena block 从窗口顺序切出；窗口耗尽或预留失败回退旧的逐 block 映射并告警（带窗口范围与原因）。`InstallSyncFastPath` 校验并填表，状态 `installed` / `installed_no_arena_window`，日志带 `arena_window=[…)`。
  - 测试 harness 自定 `kArenaBase/kArenaLimit` 并像生产一样填表。
- 反例保留：窗口放在 `ServiceAllocationBase` 起点（APK 4ab95748 / host 02d1c83c）三次启动均在 ~10 s 崩溃（pid 26494 / 27056 / 28292，Guest-1，`SEGV_MAPERR fault 0x0`，block `0x1020b6ac2`），`sync_fastpath=0` 亦然，`fex-fault-27056-1.txt` 有寄存器。窗口挪到 +4 GiB（APK a863de67 / host cf2df6be）后 70 s 内 1972 presents 正常。

### run S：修复后（APK a863de67 / host cf2df6be，pid 14261 gen1，PROF `690ada9d…`）

| 项 | 值 |
|---|---|
| FPS 3×10 s（锁频） | **14.252 / 14.291 / 14.392**（锁频前 StatusLayer 15.0） |
| 484 帧差分 | 1484 draws/帧（scaled 1235）；pass 实例 255.5/帧 = 天然 221.3 + 重开 33.2（hle 23.0、sampled_image 6.0、buffer_upload 2.2、image_copy 1.0、download 1.0、buffer_barrier 1.0） |
| PROF（289 帧，detail 开着，14.5 FPS） | 帧 69.0 ms（p90 75.1） |
| GPU / 帧 | GuestCommands 48.6、RenderPass 33.4（≥5 ms 2.0 个 = 14.2；<200 µs 180 个 = 7.5）、HostReadback 5.3（4.0 个）、Dispatch 3.0、Transfer 2.8、Present 1.4、Overlay 1.3 |
| Guest-1 / 帧 | HLE 23.8 ms、862 次：CondWait 6（16.1 ms）、WaitSema 31（2.1）、SetPs/Vs/Cs 126+91+26（2.5）、SignalSema 42（1.0）、cond_broadcast 48（0.6）、shadSyncWait 11.6（0.46）、shadSyncWake 27.6（0.10）、IsUserPaEnabled 204（0.17）、rwlock 61+61（0.14）；未注解 45.2 ms |
| Guest-19 / 帧 | `GNM.SubmissionGate` 55.1 ms（11.9 次）；HLE 6.2 ms |
| GpuComm / 帧 | `PM4.Resume` **67.9 ms**（72 段，子 scope 仅 0.3 ms） |
| GpuDone / Present | `Vulkan.CompletionWait` 147.5 ms/帧（三条线程叠加）、`Present.Frame` 35.7、`RedrawFenceWait` 24.8 |
| hle_sync 20 s / 275 presents（每帧） | Rwlock.Read/Unlock 569+569、AddrWake 264、Sema.Wait 258、Sema.Signal 176、Mutex.Config 122、AddrWait 105、Cond.Wait/Broadcast 89+89、Usleep 60、Mutex.Init/Destroy 44+44、Cond.Init/Destroy 13+13、EpollWait 3.3；合计 2420/帧，**Mutex.Lock/Unlock 行消失** |
| 线程放置（`ps -T` 三次采样） | 两条最忙线程（70%、59%）在 cpu0–5 之间迁移，未上 cpu6–7（prime，未锁时 4.6 GHz） |
| 进程 | RSS 1455 MB、swap 925 MB、MemAvailable 1900 MB |
| 终态 | UI Stop → `Stopped / user_stop / guest return=8757845664`，14961 flips，TracerPid 0 |

### 边界

- R 与 S 的镜头不同（S 的角色被 intro 脚本多按的圈键挪动过），指纹分别是 1083 与 1484 draws/帧；FPS 只能与同指纹轮次比：S 对 L（都 ~1490 draws/帧）。没有完全同镜头的 A/B。
- 没有做 `debug.shadps4.sync_fastpath=0` 的同版本 A/B（需要重启会话再走一次存档加载），修复前数据取自 run R 与历史 L。
- PROF 是在 `gpu_timing detail` 与 `hle_sync` 开启下采的，FPS 比裸跑低 1–2；分解比例可用，绝对值以 fps.txt 为准。
- 未做全游戏回归、其它标题验证；启动地址依赖崩溃只验证了"挪开窗口就不崩"。
