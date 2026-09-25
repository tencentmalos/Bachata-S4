# Guest GPU command trace / PM4 trace 操作入口

## 已集成版本与运行时确认

实现提交 `c694b179a`，经 `a562e8100` 合入本仓主干 `malos/main`。`gpu_command_trace` 和 `guest_command_trace` 是同一采集引擎的两个命令名；一次采集同时生成两层文件，不要分别 arm。源码见 `src/video_core/amdgpu/pm4_trace.*`、`src/core/diagnostics/diagnostics_commands.cpp`，离线工具见 `tools/ps4-gpu-trace/ps4_gpu_trace.py`。

先核对实机包/host SHA、游戏、PID、generation、run UUID、驱动身份，再查询实际安装包，不能仅凭源码已合入假定运行包已更新：

```sh
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService debug_status
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService gpu_command_trace status
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService gpu_command_trace arm 2 0 64
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService guest_command_trace status
```

## 有界采集与导出

- `arm [frames=2] [delay=0] [max_mib=64]`：frames 1–16，delay 0–600 个 flip，max_mib 1–512。已 armed/capturing/writing 时返回 busy。以 Guest flip 为边界；armed 的 30 秒超时在 status 查询时检查；capturing 的 20 秒和字节预算在记录写入时检查并保存 partial。没有独立 watchdog，完全停住时须主动 save。
- 状态 `idle → armed → capturing → writing → ready`，也可能 `failed/cancelled`。`save` 提前保存当前捕获，并标记 `ended_by: save requested`；`cancel` 丢弃未完成捕获。
- GPU timeout 调查要在故障前 arm；若命令停止推进但进程还存活，可立即 `save`，避免进程退出导致内存中的 trace 丢失。这不是自动滚动故障记录器，采集期间也有开销，须保留无采集的对照。
- `ready` 的 `pm4_file` / `gcmd_file` 是实际路径，通常在 app-private `files/host/captures/gpu-trace/`。只拉取本次 capture UUID 对应的两个文件，保留同名相邻存放和原始状态回执。不要整包导出 host 日志目录；持续 Debug 日志可能非常大。

```sh
adb -s SERIAL exec-out run-as com.shadps4.android cat DEVICE_PM4_PATH > CAPTURE.gpu.pm4.trace
adb -s SERIAL exec-out run-as com.shadps4.android cat DEVICE_GCMD_PATH > CAPTURE.gcmdtrace.ps4
python3 tools/ps4-gpu-trace/ps4_gpu_trace.py check CAPTURE.gpu.pm4.trace
python3 tools/ps4-gpu-trace/ps4_gpu_trace.py check CAPTURE.gcmdtrace.ps4
python3 tools/ps4-gpu-trace/ps4_gpu_trace.py summary CAPTURE.gcmdtrace.ps4
python3 tools/ps4-gpu-trace/ps4_gpu_trace.py frame CAPTURE.gcmdtrace.ps4 --frame 0
python3 tools/ps4-gpu-trace/ps4_gpu_trace.py packets CAPTURE.gpu.pm4.trace
python3 tools/ps4-gpu-trace/ps4_gpu_trace.py passes CAPTURE.gcmdtrace.ps4
python3 tools/ps4-gpu-trace/ps4_gpu_trace.py who CAPTURE.gcmdtrace.ps4 0xGUEST_ADDRESS
python3 tools/ps4-gpu-trace/ps4_gpu_trace.py shader CAPTURE.gpu.pm4.trace 0xSHADER_HASH --out shader.bin
```

`--help` 查看各子命令过滤/限制参数。保留两文件 SHA256、header、结构检查结果、输入操作、时间线及故障前最后一次状态，原始游戏 shader/trace 留在本地证据目录，不提交 Git。

## 证据语义与 KGSL 关联

`.gpu.pm4.trace` 是 **PS4 Guest AMD PM4**：submission/queue、DCB/CCB/ACB 与嵌套 IB、实际消费的 packet DWORD、shader hash/基址/GCN 代码。`.gcmdtrace.ps4` 记录 draw/dispatch 参数、shader、RT 实际倍率、image/buffer sharp 及 pass/barrier/HLE/native 决策。两层共用 capture UUID 和序号；decoder 会自动读同名 sibling。

KGSL 快照里的 PM4 是 **宿主 Adreno 命令流**，地址空间、提交号、opcode 都不同，不能把它与 Guest PM4 混为一谈。关联时按同轮 PID/TID、generation、驱动、内核 ctx/ts、IB 地址与故障时间核对，再用 dispatch 维度、shader 身份、资源及执行顺序交叉验证。静态 IB 内容、寄存器中的 shader 基址和 L0 指令匹配不单独证明精确故障 PC。

先归档并读出已有 KGSL snapshot，确认 timestamp 清零后再复现；旧快照可能阻止新现场保存。快照 OS process_id 可能是创建上下文的线程，需用本轮线程日志与 kernel TGID 交叉核对。A7xx 的 CP ROQ 预取补偿参考 Mesa `crashdec.c` / `crashdec.h`，不能直接将 IB size − REM 当作正在执行的 packet。

当前 action 层不覆盖全部 DMA/CopyData/WriteData/事件，这些仅在原始 PM4 层；没有完整 host 资源版本链，也没有自动关联到 Vulkan host serial。trace 中已翻译/已消费不等于 GPU 已执行完成。

## 实测案例

- [Swan 血源：原始实现、trace 定位 image-store 清图和 pass 切断](validation/android-native-host/swan-bloodborne-pm4-trace-20260924.md)。
- [AYN MHW：trace + KGSL 关联与寄存器掩码调查](validation/android-native-host/mhw-gpu-trace-20260925.md)。
