# Tier B 之后的血源诊所瓶颈：Litep PROF + KGSL 同窗归因

日期：2026-09-20。分支 `feature/malos/fex_sync_performance`（`8dabb361` 之后）。承接 [Tier B](fex-sync-tier-b-20260920.md)。设备 AYN Thor `9c2841a4`（Adreno 740v2、内核 5.15.123、Turnip `5ac41be677`），Bloodborne CUSA03023 诊所，Internal Scale 0.5，guest mutex 快路径 **installed**，APK c0a7cd36 / host 77d0e9f8。分析工具是 Litep 源码检出（见"工具"一节），不是历史 macOS 包。

## 结论（按对帧时长的约束排序）

帧 ≈ 95 ms（10.3 FPS）。三条 lane 各自都接近整帧，CPU 同步已经不是主矛盾：

| # | lane | 同窗测量（14.4 s 有效交集） | 每帧折算 | 说明 |
|---|---|---:|---:|---|
| 1 | **GPU** | `kgsl_pwrstats` busy **85.7%**，频率 615/680 MHz（Adreno 740 上限）；ctx 16 主 batch submit→retire p90 **80.3 ms**（275 个 >20 ms 的 batch ≈ 帧数），KGSL wait 82–85 ms 内 on-CPU 0.03 ms | ≈ 80 ms | GPU 已接近饱和，单靠 CPU 无法超过 ~12 FPS |
| 2 | **Guest-1（游戏主线程）** | on-CPU **9.30 s（64.5%）**，其中 HLE 内 1.22 s、非 HLE（JIT）8.08 s；**runnable 2.58 s（17.9%）**；sleep 2.55 s | on-CPU ≈ 63 ms + 等核 ≈ 17 ms | 帧 owner Guest-19 每帧等它 74 ms（cond `0x19999BEE80`），notify→resume 仅 0.06 ms |
| 3 | **GpuComm（PM4→Vulkan）** | on-CPU **10.20 s（70.7%）**，全部非 HLE；5.5 s 落在 cpu7，而 cpu7 大部分时间只有 1.84 GHz（cpu3–6 为 2.7–2.8 GHz） | ≈ 70 ms | 与 Guest-1、5 个 worker 争用 5 个大核（小核 0–2 几乎不用） |

其它：VkSubmit 线程每帧一次 ~78 ms 的阻塞 `vkQueueSubmit`（1043 次提交中 219 次 >20 ms，合计 17.2 s / 19.9 s），`Vulkan.PostSubmission→WorkerSubmit` 排队 p90 74 ms——这是 GPU 饱和在 CPU 侧的投影，不是独立原因。ctx 3（另一进程，prio 1）的 653 个 batch p50 55 ms 说明系统合成也排在我们的 batch 后面。

Guest-1 每帧仍有约 **957 次 HLE**（`sceGnmSetPs/Vs/CsShader` ≈ 300、`sceGnmIsUserPaEnabled` ≈ 250、`pthread_cond_broadcast` ≈ 50、`WaitSema/SignalSema` ≈ 73、rwlock ≈ 60、`shadSyncWait/Wake` ≈ 34），合计约 15 ms/帧；其余 ~48 ms 是 guest JIT 代码。

## 下一步的杠杆（未实施，按预期收益排序）

1. **GPU 每帧工作量**：这是硬上限。需要 `gpu_timing detail` / RenderDoc 分 pass 归因（之前 TMNT 的 replay 已证明 host detile/copy pass 与未归属间隙占比不小）；Internal Scale 已是 0.5，再降分辨率是最后手段。
2. **CPU 争用与频率**：GpuComm 常驻的 cpu7 被压在 1.84 GHz（可能是热限或 governor），同时 Guest-1 有 18% 时间在等核。先核实是否热限（长时间会话后采集），再考虑线程放置（GpuComm/Present 与 Guest-1、worker 分核，小核利用）。
3. **剩余 HLE**（~15 ms/帧）：`sceGnmSet*Shader` 300 次/帧是 PM4 写入，`IsUserPaEnabled` 是常量返回，`pthread_cond_broadcast` 50 次/帧可走 Bionic seq 协议进 guest（Tier B 二期）。收益上限约 10–15%，且被 GPU 上限压制。
4. 不建议再做 host 侧锁/唤醒优化：cond notify→resume 0.06 ms、reacquire 0.01 ms，已无空间。

## 采集与工具

| 项 | 内容 |
|---|---|
| PROF | `profiler_capture file 512 20`，`debug.shadps4.profile_sync=1`（条件变量观察者），26.4 MB，SHA `94315184…`，3.07 M 事件 / 304 chunks / 0 skipped，205 个完整帧（mean 97.4 / median 94.4 / p90 116.4 / max 158 ms），PID 719 gen1（全新进程） |
| KGSL sidecar | Litep `tools/capture_kgsl.py`：独立 tracefs instance、384 MiB、26 s、mono 时钟、两组 CNTVCT/monotonic 锚点；`load_kgsl_sidecar` 校验 PROF SHA/PID/boot/start ticks，时钟不确定度 **1083 ns**，overrun 0 / dropped 0 / scheduling_complete；诊断：1243 条 zero-timestamp batch 观察、1 条未配对 wait exit（保留，不据此下结论） |
| 分析 | `index_trace` → `query_indexed_frames` → `analyze_frame_contributions`（中位帧 + 最慢帧）→ `analyze_wait_chains`（显式 enqueue/notify/resume/reacquire）→ `query_async_flows(order=queue_latency)` → `open_indexed_window` + `find_scope_hotspots(tid)` → `load_kgsl_sidecar` → `analyze_cpu_gpu_bottlenecks` |
| 另两份 PROF | `litepB-on`（profile_sync OFF，PID 9558 gen4）：219 帧 mean 90.7 ms，1043 次 Vulkan 提交；`litepB-sync-noskgsl` / `-attempt3`（profile_sync ON，无 sidecar）：owner 等待 68 / 74.4 ms，同一 cond 对象、同一 notifier，结论一致 |

Litep 在本机的落地（`C:/workspace/spatial_mcp_publish/dev_tools/mcp/litep`，分支 `codex/litep-profiler`）：`engine.py` 在 Windows 上不能用 `selectors` 轮询管道（WinError 10038），改为读线程 + 有界队列，POSIX 路径不变；`tools/capture_kgsl.py` 允许 `--rootctl` 带解释器前缀、`capture.sh` 强制 LF、push 后 `chmod 755` 时钟助手；测试 86/86（1 个 POSIX-only 跳过）。分析引擎按 pin `codex/litep-analysis-engine 4f5a316` 本地 `dotnet build`（安装包 Ptracy 7b85bd0 缺 `load_prof_window`）。已用 `claude mcp add --scope user litep …` 注册到当前 Claude 环境（新 session 生效）；本轮通过 stdio 客户端 `tools/litep_client.py` 直接驱动。`devices/tools/android-root/rootctl-win` 是 Windows 包装：MSYS 从非 MSYS 父进程启动时会把 `TMPDIR` 重置为 `/tmp`，Windows 版 adb/d8 打不开，桥接 jar push 失败会被 EXIT trap 的 `remote_jar: unbound variable` 掩盖。

失败/作废的采集保留在 manifest 里说明：首次 `capture_litep.sh` 少传了快路径参数（输出进了 `20/`，PROF 仍有效，即 `litepB-sync-noskgsl`）；两次 KGSL 失败分别是 CRLF 与 exec 位；一次把标题界面当成了诊所（`litepB-sync-menu`，30 FPS，不用于结论）。

## 证据

[evidence/tier-b-litep-20260920/](evidence/tier-b-litep-20260920/)：各采集的 `index-status`、帧列表、贡献分析、wait chain、async flow、`cpu-gpu-bottlenecks.json`、sidecar 绑定结果与 KGSL capture 元数据（capture.json、锚点、per-CPU stats、线程名、cleanup）、截图、脚本与 `manifest.json`（含未入库大文件 PROF / trace 的 SHA 与大小，留在 `build/validation/tier-b-litep-20260920/`）。
