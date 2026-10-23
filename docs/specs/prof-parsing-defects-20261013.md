# PROF 采集与解析缺陷（交接，2026-10-13）

KH3 loading 分析时，两份 `profiler_capture file` 采集（Odin3）用现有工具都没能直接完整解析，最后只能自己写流式脚本统计。下面列出遇到的问题，供系统性修复。前一份工具缺口文档是 [litep-perf-tooling-gaps-20260920](litep-perf-tooling-gaps-20260920.md)，本文只记这次新确认的问题。

## 复现材料（本机，忽略目录）

| 名称 | 文件 | 说明 |
|---|---|---|
| A | `build/validation/odin3-kh3-preempt-20261012/P-load-ledger/load-run2.prof` + `.prof.json` | 139,465,857 B；20 s；修复前 loading，约 877 万个 scope |
| B | `build/validation/odin3-kh3-preempt-20261012/T-load-prof/load-tls.prof` + `.prof.json` | 81,700,810 B；30 s；修复后 loading；9,598,855 个事件、990 个 chunk、0 丢弃 |

- 两份 sidecar 都写 `sdk_revision = 0b467a861569345a64fd88cb2e5daa8ff542c14f+slow-frame-v1`。B 的 sidecar 还写 `capture_complete=true`、`max_seconds=30`。
- 绕过用的脚本：`build/validation/odin3-kh3-preempt-20261012/prof_agg.py`。它调用仓内 Python decoder 的私有函数，按（线程名、最外层 scope 名）统计次数、总时长和平均时长。
  - 用法：`python prof_agg.py foundation/third_party/profiler_sdk/skills/spatial-trace-analyzer/scripts <file.prof> [top]`
  - B 的输出在 `T-load-prof/agg.txt`，可作为修复后的对照，例如 Guest-50 `HLE.sceKernelGetProcessTimeCounter` 693,212 次，Guest-44 `HLE.pthread_rwlock_unlock` 174,665 次。

## 1. Litep 拒绝当前构建产出的采集

- **位置**：`spatial_mcp_publish/dev_tools/mcp/litep/analysis_engine/Litep.Core/Prof/ProfTraceImporter.cs:18-43`。
- **现象**：
  - `CompatibleRevisions` 写死了 8 个 commit，不包含当前 Foundation 产出的 `0b467a86…+slow-frame-v1`（带本地扩展后缀），所以 A、B 都在 `load_prof` 被直接拒绝。
  - 三种情况报同一句 “unknown or conflicting SDK wire provenance”：缺 sidecar、revision 不在表里、显式参数与 sidecar 冲突。用户无法区分是哪一种。
- **期望**：
  - 按格式能力判断能否解析（见第 2 条），而不是只认精确 revision。
  - 至少收录当前 revision，并把三种失败分开报。

## 2. 线格式变了，文件头里的版本号没变

- **现象**：
  - 上游 `f34a1c8` 改了 Bookmark 布局，文件头仍是 `PROF` v3 / `PROF-v3-track-id`。
  - 本地的 slow-frame 扩展也只体现在 revision 字符串后缀上。
  - 读取端只能靠文件旁边的 sidecar 判断布局。文件单独复制出来就无法正确解析。
- **期望**：文件头自带格式小版本或特性位，以及事件 tag 的长度描述，让读取端能跳过不认识的 tag，并能从文件本身判断布局。

## 3. 强制指定旧 revision 时，只解析出一小段且不报错

- **现象**：
  - 为绕过第 1 条，指定 `sdk_revision=4c65699e…`。解析在第一个不认识的 tag 处停止：A 只解析出 20 s 中的 14.8 s，B 只解析出 30 s 中的 1.5 s。
  - Litep 只给一条 warning，后续摘要和排名照常输出，看起来像完整结果。
- **Python decoder 同样有问题**：
  - `_read_chunks` 把所有 chunk 解压后拼成一个缓冲区（`decoder.py:266-300`）。
  - `_decode_payload` 遇到未知 tag 就 `break`（`decoder.py:741-750`），后面所有 chunk 一起丢失，不能从下一个 chunk 重新同步。
- **期望**：
  - 确认 SDK 写入时事件是否与 chunk 边界对齐。如果对齐，就逐 chunk 解码，坏块只丢自身。
  - 输出覆盖率：解析出的时间范围除以 sidecar 声明的采集窗口。低于阈值时报错，而不是给 warning。

## 4. 采集边界与真正的数据丢失混在一起

- **现象**：
  - 采集开始前就已开始的 span，在采集窗口内结束时，会被报成 “unbalanced span end”。decoder 在这种情况下置 `truncated = True`（`decoder.py:378-384`）。B 是完整采集（`capture_complete=true`、0 丢弃），结果仍是 `truncated=True`，所以 `truncated` 无法用来判断是否真的丢了数据。
  - Litep 对每个线程都报 “unbalanced span end / N unclosed spans / scope end without begin”，另有 “37 open scopes excluded”。这些提示列的是 tid 而不是线程名。
- **期望**：
  - 区分边界遗留与流中间的真实不配对。边界遗留指线程首个事件之前已开始的 span，或采集结束时仍未结束的 span；流中间的不配对算错误。
  - 每类按线程名汇总计数。`truncated` 只表示数据真的不完整。

## 5. Python decoder 处理不了大文件

- **现象**：
  - `read_bytes` 把每个事件生成为一个 `DecodedEvent` dataclass 放进列表，动态名字的字符串也逐事件重复保存。上千万个事件时内存和时间都不可行。
  - 后处理默认事件是列表：Region track 命名（`decoder.py:757` 起）、`normalize_flows`、`normalize_dependencies`。
  - 没有公开的流式接口。`prof_agg.py` 只能调用 `_read_session_header`、`_read_chunks`、`_decode_payload` 三个私有函数，并塞一个带 `append`/`__iter__`/`__len__` 的假事件容器。
- **期望**：
  - 提供公开的流式接口（回调或生成器），后处理也改成流式。
  - 自带常用聚合（见第 7 条）。

## 6. Litep 输出过大、查询慢

- **现象**：
  - A 的 `load_prof` 返回约 6.1 万字符，超过 MCP 单次输出上限。diagnostics 数组在顶层和 `profilerOverhead` 里各出现一次。
  - `query_sql` 要先导出 pftrace，877 万个 slice 约 42 s 后才能执行第一条查询。
- **期望**：
  - `load_prof` 只返回紧凑摘要和诊断计数，明细分页获取。
  - 导入时直接建好常用聚合表。

## 7. 缺少按线程统计 scope 的表，等待与计算不分

- **现象**：
  - 最常见的问题是“哪个线程调了哪个 HLE、多少次、总时长和平均时长”，现在只能写 SQL 或自写脚本。
  - 按总时长排序时，空闲线程的等待 span 排在前面。例如 B 中 Guest-20/24/25 的 `HLE.sceKernelUsleep` 各约 30 s。等待与计算没有区分。
- **期望**：
  - 内置“（线程名，scope 名）→ 次数 / 总时长 / 平均 / p99”表，区分最外层与自身时间，并支持时间窗过滤。
  - 已知会阻塞的 HLE（usleep、cond/sema 等待、equeue 等）单独归类。或者按采样记录 span 的线程 CPU 时间，不靠 KGSL/sched sidecar 也能区分在 CPU 上的时间和等待时间。

## 8. loading 阶段没有合适的分析入口

- **现象**：
  - 帧按 Guest `sceGnmSubmitDone` 周期划分。loading 期间慢帧等按帧的分析没有意义。
  - 也没有 loading 起止标记。这次靠截图判定（`loadwatch.py`）loading 起止，再手工对齐时间。
- **期望**：
  - 支持用外部时间点切窗口，例如截图判定的时刻或 DebugBus 打的 bookmark。
  - 以时间窗为主做线程、HLE 和等待统计。

## 9. 生产端：每次 HLE 都把名字字符串写进事件

- **位置**：
  - shadPS4 `src/core/host_runtime/guest_runtime.cpp:260-268`：每次 HLE 都执行 `Common::Profiler::Scope{profile_name.c_str()}`。
  - `src/common/profiler.cpp:22`：调用 `spatial::LiteTrace::begin`。
  - Foundation `basic/underlying/core/private/spatial/core/utils/LiteTrace.cpp:27-29`：一律走 `span_begin_dyn_args`，即 `encode_span_begin_dyn`，把整个名字字符串写进事件（`profiler_sdk/sdk/src/Encoder.h:143`）。
- **影响**：
  - 修复前 loading 约 59 万个 span/s，A 是 20 s 写了 139 MB。
  - simpleperf 中 `Profiler::Scope`、`LiteTrace::begin/end`、`encode_span_*`、`TlsBuffer::ensure` 约占 Guest-50 宿主部分采样的一半。
  - 高频 HLE 的 handler 本身只有 0.1–0.2 µs。
- **期望**：
  - HLE 适配器的名字在会话内地址稳定，可以在创建时登记一次，此后走 `encode_span_begin` 的 name_id 路径（固定 25 字节）。
  - 注意：SDK 按指针登记字符串（`StringIntern`）。会话结束后适配器释放，地址可能被别的名字复用，所以要用 `stableName` 那样的常驻池。
  - 对亚微秒级的 HLE，可以只计数、不记 span：按（线程，NID）定期发布 counter。

## 10. 拉取采集文件

- **现象**：
  - 文件在 app 私有目录 `files/host/log/profiler/file-<时间>-<n>.prof`，只能用 `run-as … cat` 拉取。
  - sidecar 必须与 `.prof` 同名，放在同一目录。单独拉 `.prof` 或改名都会失去配对，进而触发第 1 条。
  - Git Bash 下设备路径会被改写，需要 `MSYS_NO_PATHCONV=1`。
- **期望**：拉取工具同时拉 `.prof` 和 sidecar，校验大小和 SHA。最好把身份信息写进文件本身（同第 2 条）。

## 验收建议

- A、B 在 Litep 中不指定 `sdk_revision` 也能完整导入。B 解析出的窗口约 30 s；`truncated` 为假，或只报边界遗留。
- 按线程的 HLE 统计与 `T-load-prof/agg.txt` 一致。
- Python 接口流式解析 B 时内存有上限，不随事件数线性增长。
- 生产端改为登记名字之后，同一 loading 场景的 PROF 体积和 Guest-50 宿主侧 profiler 采样占比明显下降，解析结果中的 HLE 名字不变。
