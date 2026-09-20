# 性能分析工具缺口与调优 spec（Litep / KGSL / hle_sync，2026-09-20）

状态：**待实施 spec**。面向另一个 session 在 `C:/workspace/spatial_mcp_publish`（`dev_tools/mcp/litep`，分支 `codex/litep-profiler`，当前 `30b40566`）调优 Litep 并发布一版 Windows 本地包。依据是 [Tier B 之后的瓶颈归因](../validation/android-native-host/tier-b-litep-bottleneck-20260920.md) 与 [Tier B A/B/A](../validation/android-native-host/fex-sync-tier-b-20260920.md) 两轮实际使用中**必须手写脚本才能得到结论**的地方。规则不变：不伪造数据、不把时间重叠当因果、保留失败证据；工具改动只在 Litep / 发布脚本 / shadPS4 诊断命令内，不改 FEX、Foundation、SDK 协议。

## 0. 本轮实际链路与手工补洞（缺口的来源）

| 步骤 | 现有工具 | 本轮实际做法 | 缺口编号 |
|---|---|---|---|
| 到达诊所、预热、A/B 属性切换 | 无 | `run_condition.sh` / `capture_litep.sh`（按 `host_present` 差分轮询 FPS，root `sendevent` 按 Cross） | G2 |
| PROF + KGSL 同窗 | `capture_profile`（PROF）、`tools/capture_kgsl.py`（KGSL，独立） | 后台起 KGSL（窗口 = PROF 秒数 + 6），前台 `profiler_capture file 512 20`，手工对齐、手工 pull/sha、手工写 manifest | G2、G3 |
| 身份绑定 | sidecar 绑定 PROF SHA / PID / boot / 锚点 | APK/host Build ID、`debug.shadps4.*` 属性、`hle_sync.fast_path`、Session context/gen 全部手抄进 `identity.txt`/manifest | G3 |
| GPU busy / 频率 / inflight | `analyze_cpu_gpu_bottlenecks` 只有 submit→retire 与 wait | `kgsl_busy.py` 自算 `kgsl_pwrstats` busy 85.7%、`gpu_frequency` 时间份额、inflight 直方图、按 ctx 的 batch 数 | G4 |
| CPU 频率 / 大小核 / 热 | sidecar 采了 `power:cpu_frequency`，工具不消费 | 手工从 trace 算 cpu7 1.84 GHz、cpu3–6 2.7–2.8 GHz；未采热区 | G5 |
| 线程唤醒/短 reblock/wake→run | 无 | `sched_wake_analysis.py`（Guest-1 20k wakeups、13.5% reblock<20 µs、wake→run p90 167 µs） | G6 |
| 每帧 HLE 预算（957 次/帧 ≈ 15 ms） | `analyze_frame_contributions` 给中位/最慢帧的 exclusive 差异 | 用 `find_scope_hotspots(tid)` 全窗口除以帧数手算；"其余 48 ms 是 JIT" 是人工推断 | G7 |
| 帧 owner 等待 74 ms/帧 | `analyze_wait_chains` 逐条 | 手工按 (waiter, notifier, object) 汇总再除以帧数 | G8 |
| `hle_sync` 窗口比较（A→B 汇总、B→C detail） | `hle_sync status` JSON | `compare_sync.py` **硬编码 op 列表，漏掉了 `Sync.AddrWait/AddrWake` 行**（tierB2/B3 补测时靠另写脚本才看到） | G9 |
| GPU 分 pass | `gpu_timing detail` 有 `GPU.GuestRenderPass` 等 zone；`list_gpu_zones/get_gpu_timeline` | 本轮未做；Turnip 无校准时间戳（±8 ms），无法与 KGSL batch 对齐 | G10 |
| Windows 运行 | 包只有 osx-arm64 / linux-x64 | 源码检出 + `dotnet build` 引擎 + `claude mcp add` 指向 `server.py`；`rootctl-win` 包装 | G1 |

已知 API 怪癖（本轮踩到，需在文档/schema 里固定或修掉）：`query_indexed_frames` 返回字段是 `rows` 不是 `frames`；`analyze_wait_chains` 的 `top` 上限 100；`query_async_flows` 分页要求 `limit ≤ 400`；`capture_kgsl.py` 的相对路径在 MSYS 下会被当前目录前缀成 `C:\c\workspace…`（必须绝对路径）；rootctl 拒绝 `rm -rf` 清理远端目录，`/data/local/tmp/litep_kgsl_*` 会残留。

## 1. 目标与交付

1. **一条命令得到同窗证据包**：PROF + KGSL/sched sidecar + 身份 + 截图 + manifest，内含 SHA/大小，可直接进仓库 `evidence/` 目录（大文件只留 SHA）。
2. **`analyze_cpu_gpu_bottlenecks` 一次输出三条 lane 的判定**（GPU busy/频率/batch 延迟、主线程 on-CPU/runnable/频率加权、PM4 线程放置），不再需要 `kgsl_busy.py` / `sched_wake_analysis.py`。
3. **每帧预算视图**：任意线程在所有完整帧上的 scope 次数/时长均值与分布，HLE 与非 HLE 残差分开，并显式标注残差不是 CPU idle。
4. **Windows 本地发布**：`win-x64` 包（自带 .NET 引擎、Perfetto processor）经 `mcp_package_manager.py` 安装，Claude/Codex 注册指向安装目录而不是源码检出；源码测试与打包后 `initialize`/`tools/list` 都通过。
5. 用本轮已有数据做**回归夹具**：不接设备也能把归因报告里的数字复现出来。

## 2. 缺口清单与要求

### G1（P0）Windows 打包与运行

- `Scripts/Release/build_litep_mcp.py`：允许 `rid=win-x64`；`dotnet publish -r win-x64 --self-contained`；拷贝 `Ptracy.Core/Perfetto/runtimes/win-x64/trace_processor_shell(.exe)`；启动器用 `.cmd` + `LITEP_PYTHON`。`release_mcp.py` 的 `PRODUCT_LITEP_MCP` rid 列表加 `win-x64`。
- 引擎 pin 必须是 `codex/litep-analysis-engine 4f5a316` 或更新且**包含 `load_prof_window`**；`server_get_version` 要报告引擎 SHA，并在缺工具时给出具体工具名（本轮安装包 Ptracy `7b85bd0` 缺该工具，报错只说“unknown tool”）。
- `engine.py` 的 Windows 读线程已提交（30b40566）；补：`tests/test_server.py` 中 `os.killpg` 的 POSIX-only 跳过改为 Windows 等价（`taskkill /T` 或 Job Object），使 Windows 上 87/87 无跳过。
- `LITEP_ALLOWED_ROOTS`：Windows 用 `;` 分隔、统一大小写与斜杠（`C:\workspace` 与 `C:/workspace` 视为同根）；错误信息要打印规范化后的根和被拒路径。
- `LITEP_ADB`/`--rootctl`：`capture_kgsl.py` 已接受解释器前缀；补 `--rootctl` 缺省从 `LITEP_ROOTCTL` 读；文档写明 MSYS `TMPDIR` 问题与 `rootctl-win`（`C:/workspace/devices/tools/android-root/rootctl-win`，devices `44c77f1`）。
- 验收：`python -m unittest discover -s tests` 在 Windows 全过；打包后从安装目录启动 `initialize` + `tools/list`，工具数与源码一致；`claude mcp list` 与 `codex mcp list` 都能连。

### G2（P0）同窗采集编排 `collect_evidence`

新 MCP 工具（或 `tools/collect_evidence.py` + MCP 包装），参数：`target_id`、`seconds`、`kgsl:{total_mib, extra_events[]}`、`sched:bool`、`preconditions:[{counter, min_rate, max_rate, hold_seconds}]`、`properties:{name:value}`（采集前设置、结束后恢复）、`screenshots:bool`、`output_directory`。行为：

1. 设置属性 → 等待前置条件（例如 `host_present` 速率在 [2,15) 且持续 60 s，或 `presents ≥ 250` 预热）——只做**轮询判定**，不做按键/场景推进（那是 emulator 侧脚本，见 G2b）。
2. 先 arm KGSL/sched instance（窗口 = seconds + 6），再 `profiler_capture file`，PROF 在 KGSL 窗口内；记录两侧的 wall/mono 起止与锚点。
3. 结束：等 `capture_active=0`，pull PROF（经 rootctl 复制到可读目录，保留设备端 SHA），pull trace，写 `capture.json`；恢复属性；可选 STOP session；**清理**远端目录与 tracefs instance（用精确路径的 `rm -r`，不用 `rm -rf` 通配；失败要报告残留路径）。
4. 写 `manifest.json`：设备（serial/model/kernel/GPU/Turnip SHA）、APK/host/JNI Build ID（`dumpsys package` + 从 APK 抽 DSO 算 Build ID，或读 emulator `identity` 命令）、PID/gen/context、属性快照、`hle_sync.fast_path`、PROF/trace SHA 与字节数、失败/作废原因。
5. 失败保留：任何一步失败都写 manifest 的 `failed_steps`，不删除已得到的文件。

G2b：场景推进脚本留在 shadPS4 仓（`docs/validation/.../evidence/*/run_condition.sh` 已有），只要求 Litep 暴露 `wait_for_counter`（对 `dumpsys` 任意 `name=value` 行做速率判定）供它调用。

### G3（P0）身份 sidecar 扩展

`collect_capture` 的 `capture_identity` 增加：`build_ids{apk, host_dso, jni_dso}`、`properties{}`（按前缀 `debug.shadps4.` 采集）、`session{context, generation, stage}`、`emulator_status_json`（透传一条 `dumpsys` 命令的 JSON，如 `hle_sync status`）。`load_kgsl_sidecar` 除现有 PROF SHA/PID/boot 校验外，把这些字段原样带进 `cpu-gpu-bottlenecks.json`，报告里不再手抄。

### G4（P1）GPU lane：busy、频率、深度、按 ctx

`analyze_cpu_gpu_bottlenecks` 输出增加 `gpu`：

- `busy_percent`（`kgsl_pwrstats` busy/total，安全窗内）、`gpubusy_percent`（`kgsl_gpubusy`，若有）、`freq_share{khz: fraction}`、`freq_max_khz` 与达到上限的时间份额。
- `inflight_at_submit{depth: count}`、`batches_by_context{ctx: {count, p50, p90, max}}`、`context_owner{ctx: {pid, comm}}`（来自 `kgsl_context_create`），并标出目标进程的 ctx；其它进程（合成器）单列，不与目标相加。
- 每 batch `retire-start` tick 折算的执行时长用 `kgsl_pwrstats` 校验 tick 频率（本轮假设 19.2 MHz），不一致时报告不解释。
- 判定字段 `interpretation.gpu_bound`：busy ≥ 80% 且 p90 submit→retire ≥ 帧长 × 0.8 才成立，否则写明未满足哪个条件。

### G5（P1）CPU 频率 / 大小核 / 热

- 消费已采的 `power:cpu_frequency`：安全窗内每 CPU 的频率时间份额；每线程 `freq_weighted_on_cpu_ghz`（on-CPU 时间按所在 CPU 当时频率加权）；`on_cpu_by_cpu_ms` 已有，补 `cluster`（读 `/sys/devices/system/cpu/cpu*/cpu_capacity` 或 `cpufreq/related_cpus` 分簇，采集时由 `capture_kgsl.py` 保存）。
- `capture_kgsl.py` 采集起止各采一次：`/sys/class/thermal/thermal_zone*/{type,temp}`、`cpufreq/*/cpuinfo_{cur,max}_freq`、`scaling_governor`、以及 KGSL `/sys/class/kgsl/kgsl-3d0/{gpuclk,max_gpuclk,thermal_pwrlevel,throttling}`（存在才读）。可选 `--thermal-sample-seconds N` 周期采样写 `thermal.jsonl`。
- 输出 `interpretation.cpu_frequency`：列出被压在低于簇最大频率 ≥ 20% 的 CPU 与其上运行最多的线程（本轮：GpuComm 5.5 s 在 cpu7@1.84 GHz），并注明“热限还是 governor 未证实”，除非热区数据支持。

### G6（P1）线程唤醒分析并入 bottleneck

从 `sched_waking/sched_wakeup/sched_switch` 得每线程：`wakeups`、`reblock_lt_20us_percent`、`wake_to_run_p50/p90_us`、`run_p50_us`、`migrations`、`waker_top[]`（谁唤醒它，次数）。放进 `cpu_threads[]`；`wait_chains` 里 notify→resume 已有，二者用同一 tid 命名。这是 Tier B A/B/A 的核心指标（89k→29k wakeups），必须在工具内可复算。

### G7（P1）每帧预算 `analyze_frame_budget`

参数：`index_id`、`tid`、`frames:"complete"|[range]`、`group_by:"scope"|"hle_op"`。对每个完整源帧：scope 次数与 elapsed（exclusive/inclusive 各一份），输出均值/中位/p90 与总和；`hle` 与 `non_hle` 合计；`unannotated_ms` = 帧长 − 该线程已标注 union，标注为“未标注，不等于 idle，也不等于 JIT”，但允许调用方用 `on_cpu`（若 sidecar 已加载）把它拆成 on-CPU/runnable/sleep 三部分——这才是本轮“48 ms JIT”的正确表述。HLE op 名来自 scope 名前缀（`HLE.` / `Sync.`），规则可配置。

### G8（P2）等待链按帧聚合

`analyze_wait_chains` 增加 `aggregate:"per_frame"`：按 (waiter tid, notifier tid, object) 汇总每帧等待时长均值/p90、每帧次数、`notify_to_resume` 与 `resume_to_reacquire` 均值；`top` 上限提到 1000 或改分页；`query_async_flows` 的 `limit ≤ 400` 与 `query_indexed_frames` 的 `rows` 字段写进 `docs/api-catalog.md`，并在 schema 里给出 `maximum`。

### G9（P2）`hle_sync` 窗口比较进 Litep

新工具 `analyze_hle_sync_windows`：输入 2–3 份 `hle_sync status` JSON（或一次 `runtime_control(subsystem:"debug", command:"hle_sync status")` 采样序列），输出每 op 的 calls/s、mean、≥1 ms/≥16 ms 计数、detail 相位（Lookup/Guard/Park/Publish/Reacquire）占比与均值、`phase_mask` 与实际相位计数不一致时报警（tierB2-on 的 `AddrWait` phase_mask=4 但相位计数为 0 就是这种情况）。**op 列表来自 JSON，不硬编码。** `runtime_control` 的 `subsystem` 枚举增加 `debug`（任意注册的诊断命令透传，名单由 target profile 声明），`hle_sync start/detail/stop <context>` 的 context 由工具从 `status` 读取后回填。

### G10（P2）GPU 分 pass 归因

- emulator 侧：`gpu_timing detail` 的 `GPU.GuestRenderPass` zone 要带可区分的标签（pass 序号、pipeline/shader hash 前 8 位、attachment 数、是否 host detile/copy/FSR）——先核对 `src/common/gpu_timing.h` 与 `vk_gpu_profiler.cpp` 现有字段，缺则加；不新增 GPU 等待。
- Litep 侧：`analyze_gpu_frames` 在无校准时间戳（Turnip）下改用 **KGSL batch 边界**做锚：每个 `adreno_cmdbatch_submitted/retired` 对内的 GPU zone 按 GPU 时钟相对排序、按 elapsed 归一化到 batch 的 retire−start，输出每帧 top pass 份额与不确定度；明确“份额，不是绝对时间”。
- 验收：诊所同窗一次 `gpu_timing detail` + KGSL，工具给出每帧前 10 个 pass 份额并能与 RenderDoc `timing_analyze` 的相对排序对照（允许 ±1 名）。

### G11（P3）证据包与仓库

`collect_evidence` 生成的目录直接匹配现有 `evidence/*/manifest.json` 约定（`schema`、`device`、`apk`、`conditions`、`files{path: sha256}`、`large_files_not_committed{}`），大于 3 MB 的文件只写 SHA/字节数。提供 `verify_evidence(dir)` 校验 SHA 与大文件存在性。

## 3. 回归夹具（不接设备）

用 `C:/workspace/emulations/shadps4/build/validation/tier-b-litep-20260920/litepB-sync/`：`capture.prof`（SHA `94315184d4d0…`，26,419,194 B）、`kgsl-capture/kgsl_trace.txt.gz`（SHA `5b8ca2144ca8…`，50,046,502 B）、`kgsl-sidecar.json`。工具改完后必须复现（容差 ±2%）：

| 指标 | 期望 |
|---|---|
| 有效交集 | 14.4 s，时钟不确定度 1083 ns，overrun 0 |
| GPU busy（pwrstats） | 85.7%；主 ctx submit→retire p90 80.3 ms，>20 ms batch 275 |
| Guest-1 | on-CPU 9.30 s（HLE 1.22 / 非 HLE 8.08），runnable 2.58 s，sleep 2.55 s |
| GpuComm | on-CPU 10.20 s，其中 cpu7 5.5 s；cpu7 频率份额以 1.84 GHz 为主 |
| Guest-19 → Guest-1 | 每帧等待 ≈ 74 ms（cond `0x19999BEE80`），notify→resume 0.06 ms |
| 每帧 HLE（Guest-1） | ≈ 957 次/帧、≈ 15 ms/帧；`sceGnmSet*Shader` ≈ 300、`IsUserPaEnabled` ≈ 250 |

以及 `evidence/fex-sync-tier-b-20260920/tierB3-on/sync-{A,B,C}.json`：`analyze_hle_sync_windows` 必须列出 `Sync.AddrWait` 491/s、Park 1928/5049、`Sync.AddrWake` 1536/s。

## 4. 发布步骤（Windows 本地）

1. `codex/litep-profiler` 上实现并跑 `python -m unittest discover -s tests -v`；`docs/api-catalog.md` 重新生成。
2. `python Scripts/Release/release_mcp.py --product litep_mcp --rid win-x64 --skip-publish --no-bootstrap --version 0.x.0-local.<date>`（参数名以脚本为准），产物 zip + manifest。
3. `Scripts/Release/mcp_package_manager.py install_from_manifest` 到 `%LOCALAPPDATA%\SpatialDebugTool\Mcp`，`current.txt` 指向新版本。
4. 更新 `~/.claude.json` 与 Codex 的 `litep` 条目：命令指向安装目录启动器，保留 `LITEP_ADB`、`LITEP_ALLOWED_ROOTS=C:\workspace`、`LITEP_DATA_ROOT`；去掉源码 `LITEP_ANALYSIS_ENGINE` 覆盖。
5. 新 session `initialize`/`tools/list` 冒烟，并用 §3 夹具跑一遍 `index_trace → load_kgsl_sidecar → analyze_cpu_gpu_bottlenecks → analyze_frame_budget`。
6. `dev_tools` 提交并 push；父仓 `spatial_mcp_publish` 推进 `dev_tools` gitlink（当前父仓在 `feature/ida-mcp-csharp`，`M dev_tools` 未提交——由实施者决定是否在该分支推进）。

## 5. 明确不做

- 不改 profiler_sdk 线协议、PROF v3 布局或 Foundation ring；不在 Litep 内伪造 GPU 校准。
- 不做游戏专用场景推进（按键序列）进 Litep；不把 `rootctl --allow-destructive` 写进任何自动化。
- 不把 `unannotated` 时间报告成 JIT 或 idle；不把不同 PID/gen 的窗口拼接比较。
- 不重做 Ptracy 已有的 `analyze_scheduling/analyze_synchronization`（PROF v3 无这些流），sidecar 路径继续走 `analyze_cpu_gpu_bottlenecks`。
