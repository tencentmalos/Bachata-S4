# GCN v2 阶段 A：PM4 自有副本与真实完成的 fence（2026-10-04）

依据 [gcn-translation-layer-v2-20261003.md](../../specs/gcn-translation-layer-v2-20261003.md) 第 3.6 节与第 5 节阶段 A。回放工具见 [gpu-replay-20261004.md](gpu-replay-20261004.md)。

环境：Windows，clang-cl RelWithDebInfo，RX 7600M XT（Thunderbolt 外置），血源 CUSA03023 1.00 + 60 FPS 包，中央亚楠出生点。测试目录 `D:\workspace\shadps4-win-test`，每轮先把 `shadps4-win-bb\user\home` 同步过去。最终 exe `fc4ad84c`，已提交。

---

## 1. 结论

| 项 | 状态 |
|---|---|
| PM4 自有副本 | 桌面也默认开（原先只有 Android 调用 `UseOwnedSubmissions`）。提交时复制 DCB/CCB/ACB，从副本解析；REWIND 在 guest 置位后从原缓冲重读其后的包；计算队列的 DMA 补丁按原环地址映射到副本 |
| 真实完成的 fence | EOP/EOS/RELEASE_MEM 的写入与中断进有序队列，GPU timeline 到达后由调度器完成线程执行；CP 的 `WAIT_REG_MEM` 读待定值；flip 完成等此前排队的 fence |
| 修复前实机 | 进世界加载时 GpuComm 读空的 VS 顶点表指针崩溃：completion 模式 8 轮崩 4 轮，parse 模式 2 轮 0 崩 |
| 原因 | CP 用待定值放行等待后，紧接的 `WRITE_DATA` 立即可见；游戏据此回收命令内存，迟到的 label 写进了新命令（§2） |
| 修复后实机 | completion 模式 8 轮 0 崩溃，fence 目标被改写 0 次（§4） |
| 回放 | 最终 exe 与基线逐帧、逐事件图像哈希全同；修复后 14 次 completion 回放 13 次全同（§6） |
| 帧率 | 同一会话交替 A/B/A，completion 与 parse 的帧率、GpuComm 时间差别在噪声内（§5） |
| 默认值 | 桌面 `GPU.completion_fences=true`；Android `false`（未上设备） |
| 未完成 | 回读默认打开（§7）；真实完成语义在血源上被提前执行覆盖（§7） |

---

## 2. 崩溃定位

**现象**：选“继续”后加载世界约 1 秒，GpuComm 线程 `0xc0000005 reading 0x0`，`mov rcx, [r9+rdx*4]`，r9=0。符号化为 `StageSpecialization` → `VertexAttribute::GetSharp` → `Info::ReadUdReg`：VS 用户数据里顶点表指针为 0。崩溃前约 0.25 s 游戏打印 `== Stall during rendering at flush N`；反汇编 `eboot+0x11d9590` 可知这是游戏自旋等待“已完成 flush 计数”（`*[ctx+0xc30]`）满 1000 万次后的提示，打印后继续等，不会放弃。parse 模式下同样出现（flush 468），本身不是原因。

**归因**：同一 exe 改配置交替（`crashprobe`），parse 2 轮正常、completion 2 轮中 1 轮同签名崩溃；连同此前 1 次，completion 3 轮崩 2 轮。

**机制**：给每个 fence 记下 CP 解析时目标内存的值，生效时若已不同就计数并打日志。每轮都在同样的地址报告：

| fence | 地址 | 解析时 | 生效时 |
|---|---|---|---|
| EOS | `0x241d5d670` | 0 | `0xc0016900`（SET_CONTEXT_REG 包头） |
| EOS | `0x241d5d6d0` | 0 | `0x1d0c41` |
| EOP | `0x241d606b8` | 0 | 2 |
| EOP | `0x241d68768` / `0x241d693a8` | 0 | `0xc00b6900` |
| EOS / EOP | `0x2414c1608` / `0x2414c1668` / `0x2414c48c0`，`0x241d5d238` / `0x241d5d298` / `0x241d604f0` | 1 | 0 |

游戏在 label 生效前就把这块内存当作空闲重录了命令（或把 label 复位），迟到的 label 写进新命令，CP 解析出错，VS 用户数据未被设置。

**原因**：回放 trace（世界里 5 帧）中每帧都有一处 `WAIT_REG_MEM` 等 EOP label（`0x2414c48c0` 与 `0x241d604f0` 两组交替），紧接 `WRITE_DATA` 把 `0x256cb36e0` 清 0；这两个 label 正是上表被复位的地址。真机上 CP 要等 EOP 真正写入才执行这条 `WRITE_DATA`；这里 CP 读待定值通过等待，`WRITE_DATA` 解析时就写进内存，游戏看到后回收这一组命令内存，而 label 要等 GPU 完成才写。`0x256cb36e0` 是“这一组已用完”的标志，是根据 trace 与检测到的地址推断的，未在游戏代码中确认。

**排除**：翻页完成（flip status、flip 事件、VO label 释放）早于该帧 fence。加了 flip 等待后 completion 模式仍 4 轮崩 2 轮，检测器照样报告改写；flip 等待作为真机顺序保留（§3）。

---

## 3. 修复

- `FenceLabels`（`video_core/amdgpu/fence_labels.*`）：延后的 fence 进有序队列。`PerformThrough(seq)` 按序执行到 `seq`，每个只执行一次，完成线程与 CP 线程都可调用（执行期间持 `perform_mutex`）。
- `Liverpool::TestWait`：等待条件由待定值满足时，记下提供该值的 fence 序号（`wait_debt`）。
- `Liverpool::SettleWaits`：`WRITE_DATA`、信号量 signal 之前，CP 先执行到 `wait_debt`（计入 `early`），之后的立即写不会先于等待所依赖的 fence 可见。EOS 的 GDS store 在 `Finish` 后执行全部排队的 fence。
- flip：VideoOut 请求记下准备时已排队的 fence 数（`Liverpool::FenceMark`），`VideoOutDriver::Flip` 在更新 flip 状态、触发 flip 事件、释放 VO label 之前等它们执行（`Liverpool::WaitFences`，profiler scope `GPU.FlipFenceWait`，上限 5 s）。
- 检测：`Fence::before` 记录解析时目标的值，生效时不同就计 `overwritten`，前 16 次打 Warning。
- DebugBus：`gpu_fences [status | completion | parse | flip_wait on|off]`。status 给出 deferred、performed、early、outstanding、pending_dwords、pending_reads、overwritten 与三类 flush 计数。
- 默认：`GPU.completion_fences` 桌面 true、Android false。

---

## 4. 实机轮次

每轮：启动 → 手柄过菜单 → “继续” → 出生点稳定后测 3×8 s → 再留 60 s → 关闭。

修复前（completion 模式未结清等待）：

| 轮 | exe | 模式 | 结果 |
|---|---|---|---|
| fc | 未结清、无 flip 等待 | completion | 崩溃 |
| cp1 | 同上 | parse | 正常 |
| cp2 | 同上 | completion | 崩溃 |
| cp3 | 同上 | parse | 正常 |
| cp4 | 同上 | completion | 正常 |
| fw1 | 加 flip 等待与检测 | completion，flip_wait off | 正常，改写 16 次 |
| fw2 | 同上 | completion，flip_wait on | 崩溃 |
| fw3 | 同上 | completion，flip_wait on | 崩溃 |
| fw4 | 同上 | completion，flip_wait off | 无效：启动时 `vk_swapchain.cpp:95` 创建交换链等 33 s 后返回 `ErrorUnknown` |
| fw5 | 同上 | completion，flip_wait on | 正常，改写 646 次 |
| fw6 | 同上 | completion，flip_wait on | 正常，改写 8 次 |

修复后（结清等待；completion，flip_wait on）：

| 轮 | 崩溃 | 改写 | deferred | early 占比 |
|---|---|---|---|---|
| 1 | 无 | 0 | 2,881,861 | 99.79% |
| 2 | 无 | 0 | 2,240,582 | 99.70% |
| 3 | 无 | 0 | 2,519,416 | 99.81% |
| 4 | 无 | 0 | 2,915,927 | 99.81% |
| 5 | 无 | 0 | 3,041,672 | 99.75% |
| 6 | 无 | 0 | 2,960,753 | 99.67% |
| `d5962257` | 无 | 0 | 3,137,433 | 99.80% |
| `fc4ad84c`（提交） | 无 | 0 | 3,153,532 | 99.76% |

各轮出生点帧率在 19–60 FPS 之间波动（同一 exe、同一配置），轮间数据不能用来比较模式，见 §5。

---

## 5. A/B/A（同一会话交替）

出生点，`gpu_fences completion|parse` 切换后等 3 s，测 8 s 帧率，再测 6 s 各线程 CPU。

| 窗口 | 模式 | FPS | draws/帧 | GpuComm ms/帧 | 完成线程 ms/帧 |
|---|---|---|---|---|---|
| 1 | completion | 58.11 | 1103 | 3.30 | 0.07 |
| 2 | parse | 53.11 | 1580 | 15.71 | 0 |
| 3 | completion | 54.24 | 1574 | 15.42 | 0.08 |
| 4 | parse | 54.86 | 1574 | 16.01 | 0 |
| 5 | completion | 52.24 | 1573 | 16.05 | 0.08 |
| 6 | parse | 53.49 | 1577 | 16.18 | 0 |
| 7 | completion | 52.49 | 1577 | 15.92 | 0.13 |

窗口 1 场景未加载完（draws/帧 1103），剔除。parse 平均 53.8 FPS、GpuComm 16.0 ms/帧；completion 平均 53.0 FPS、15.8 ms/帧，范围重叠。会话开始时 CPU 有效频率 3.32 GHz。

GpuComm 20 s 栈采样（completion，6332 个样本）：含 `FenceLabels` 的栈 2.4%，`SignalFence` 1.6%，`SettleWaits` 1.2%，`DeferPriorityOperation` 0.3%（相互包含，非加和）。parse 模式同样要写 label，增量主要是排队与延后操作，约 0.3 ms/帧，A/B/A 中看不出。最高的 self 是 `CopySparseMemory` 里的 memcpy（9.5%，符号化成 `VCRUNTIME140!_NLG_Return2`）与 `BindTextures`（6.7%），与本改动无关。

---

## 6. 回放

`bb-final`（世界 5 帧，1875 个事件）与基线 `bb-final_lm1` 比较 `frames.txt`、逐事件 `image_hashes.txt`（2210 行）。

| exe | 模式 | 次数 | 全同 |
|---|---|---|---|
| 修复前 completion | completion | 2 | 2 |
| 修复版 | completion | 12 | 11 |
| 修复版 | parse | 4 | 4 |
| `d5962257` | completion | 1 | 1 |
| `fc4ad84c`（提交） | completion | 1 | 1 |

不一致的一次是修复版编译后的第一次回放：第 2 帧帧哈希不同，事件 908–936 中 image 22（`0x26f090000`，960×540 R8G8B8A8Srgb）7 行不同，其后各帧相同；回放强制 sync 编译，不是跳 draw。原因未定位，未再复现。

---

## 7. 结论与后续

- **血源上 99.7–99.8% 的 fence 由 CP 提前执行。** 每帧末尾都是“等 EOP → `WRITE_DATA` 标志”，CP 先行解析时要保证标志不先于 label，只能把此前的 fence 一起执行。label 对 CPU 的时机因此接近原先的解析时写入，崩溃前那种“GPU 完成后才写”只剩约 0.2%。
- **真实完成需要 CP 真正等待。** 现在 GpuComm 既解析又录制，真实等待会让每帧末尾 GpuComm 等 GPU 做完这一帧，录制与 GPU 执行串行。阶段 B/C 把 Vulkan 工作移出 GpuComm、降低解析成本后再评估。
- **回读默认打开未做。** 提前执行的 fence 可能先于异步下载（`ProcessDownloadImages(sync=false)`）生效；打开回读前，结清时若有未完成的下载要改为真实等待。
- **Android 未测。** 默认关，需在 Thor / Pocket DS 上做同样的轮次与 A/B/A 后再决定。
- **开销**：每个 fence 一次延后操作（约 370 个/帧）。若后续需要，可改为每个命令缓冲一次。
- **交换链**：fw4 那次启动失败发生在无人使用电脑期间，推测显示器休眠；与本改动无关，未复现。

同日顺带：桌面状态层（Only FPS / Summary）默认位置由左下改回左上（`StatusOverlay::status_anchor`，两个平台都是左上）。已保存的 `status-overlay.json` 会覆盖默认值，用户目录 `shadps4-win-bb\user\status-overlay.json` 的 `status_anchor` 已由 2 改为 0，原文件备份在 `shadps4-win-bb\backup-20261004-fpsanchor\`。
