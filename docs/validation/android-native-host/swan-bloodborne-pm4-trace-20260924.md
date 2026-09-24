# Swan 血源：guest GPU PM4 trace、image store 填充 HLE 与 pass barrier 提前（2026-09-24）

分支 `feature/malos/swan_performance`，代码提交 c694b179（基于 512849d1）。设备 Swan PB3110PGL6240001G，血源 CUSA03023 诊所内部（`launch_bb_pad.sh` + 右摇杆转向，约 1500 draws/帧），Turnip，Internal Scale 0.5。证据：[evidence/swan-bloodborne-pm4trace-20260924](evidence/swan-bloodborne-pm4trace-20260924/)。

## 1. 为什么先做 trace

上一轮逐 pass GPU 计时只能说明“哪类 pass 贵”，说不清“谁写了这张图、为什么 pass 被切”。RenderDoc 在血源上抓不到帧（见 §5），所以按 [图形调试工具 spec](../../specs/android-graphics-debugging-toolkit.md) §4.2–§4.3 先补 guest 命令 trace；此前 `guest_command_trace`/`gpu_command_trace` 只是注册的“未实现”占位。

## 2. Trace 实现（`src/video_core/amdgpu/pm4_trace.*`，`tools/ps4-gpu-trace/ps4_gpu_trace.py`）

- 控制：DebugBus `gpu_command_trace`（`guest_command_trace` 同一引擎）`status | arm [frames=2] [delay=0] [max_mib=64] | save | cancel`。状态 idle/armed/capturing/writing/ready/cancelled/failed；armed 30 s 无 flip 失败，capturing 20 s 或字节超限即结束并标 `ended_by`（partial），不阻塞、不改变 guest/GPU 执行。帧边界是 guest flip（EOP flip 在 GpuComm 线程上，与 PM4 消费同序）。
- 三层记录共用一个序号，完成后 worker 线程拆写两个文件到 `CapturesDir/gpu-trace`：
  - `<uuid>.gpu.pm4.trace`（`ps4.pm4.command`）：提交信封（submission、queue、DCB VA/长度、CCB 长度、提交线程）、每次进入 IB（DCB/CE/ACB/嵌套，含 VA 与长度）、每个被消费 packet 的原始 DWORD（CE 拷贝与 compute ring 跨段 packet 的 VA 记 0）、每个 shader hash 一次的 GCN 机器码（binary info 长度，与 pipeline cache 同一代码段）。
  - `<uuid>.gcmdtrace.ps4`（`ps4.guest-command`）：每个 draw/dispatch 的来源 packet VA、shader hash/基址、参数；从本次解析的 sharp 读出的 image（地址、尺寸、格式、写/原子/精确访问）与 buffer（地址、大小、写/格式化）；draw 的实际 RT/深度（host 格式与实际缩放倍率）；以及挂在该 action 上的 host 事件：pass begin（natural/resumed 及原因、描述含 `native(reason,mask,触发原文)`）、pass end（原因、draws、load 像素、break 细节）、buffer barrier 来源（访问掩码、shader）、HLE 拷贝（提前/切断/pass 外）、native 决定、被 HLE 替代的 dispatch、被提前的 barrier。
  - 文件头 JSON：capture/run UUID、pid、generation、帧范围、预算、`ended_by`，以及运行时用 `offsetof` 生成的寄存器偏移表（decoder 据此给 SET_*_REG 命名）。
- 关闭时每 packet/action 仅一次 relaxed 原子读；采集时 mutex 追加到预留 buffer。
- 离线 decoder：`check`（magic/版本/记录边界/序号）、`summary`（每帧提交、IB、packet opcode 分布、action、pass 结束原因）、`frame`（按帧时间线，可 `--packets` 穿插原始 PM4）、`packets`（寄存器名解码）、`passes`、`who <addr>`（谁写/读这个地址；>256 MiB 的 buffer 视图只算精确起点，除非 `--wide`）、`shader <hash>`、`selftest`（合成文件，含截断检测）。
- 实测（[trace-status](evidence/swan-bloodborne-pm4trace-20260924/trace-status.txt)、[check](evidence/swan-bloodborne-pm4trace-20260924/trace-check.txt)、[summary](evidence/swan-bloodborne-pm4trace-20260924/trace-summary.txt)）：2 帧采集 95 ms，每帧约 74 次提交、5 万 packet（SetContextReg 2 万、SetShReg 1.3 万）、1640 draw、171 dispatch（118 个被 HLE 替代）、53 次 HLE 拷贝；两文件 7.6 MB + 2.2 MB，校验无问题。

## 3. 用 trace 定位的问题与修复

### 3.1 1080p 后处理链不缩放（主收益）

上一轮逐 pass 计时显示每帧约 6.5 ms 花在 5 个左右 1920x1080 RGBA16F 的 1–2 draw pass 上，pass 描述标 `native(semantic-native,…,created as storage)`。`who` 与 `frame` 给出链条（[postprocess-chain-before](evidence/swan-bloodborne-pm4trace-20260924/postprocess-chain-before.txt)）：dispatch cs=0x9a9cf8a9（240x135 组）以 storage image 写整张 0x263e80000，之后 draw 以 LOAD 把它当 RT 继续画，后续全屏 pass 链条的目标都因首次 storage 使用被定为 native。trace 导出的 15 个 dword（[fill-kernel-code](evidence/swan-bloodborne-pm4trace-20260924/fill-kernel-code.txt)）反汇编为：`x=tgid.xy*8+tid.xy`，`s_buffer_load_dwordx4` 读 16 字节颜色，`image_store` 写入，无其它副作用——常量清图。

修复 `Rasterizer::TryComputeImageStoreFill`：8x8x1 线程组、恰 1 个写 image + 1 个 guest buffer（recompiler 的内部 buffer 不计）后逐字比对全部代码；2D 单采样、Float/Unorm/Snorm/Uint/Sint（sRGB 与 scaled 格式保留原 dispatch）、dispatch 覆盖描述符选中的整层、颜色 buffer 非 GPU 写且可安全读出，才 `Image::Clear` 该 (mip, layer)；图以 Texture 身份查找/创建，首次当 RT 时走既有“首次附件重规划”带内容 blit 缩放。`upload_diag fill_clear off` 同时关闭它与 TMNT 填充 HLE。

结果（[pass-gpu-by-run](evidence/swan-bloodborne-pm4trace-20260924/pass-gpu-by-run.txt)，未锁频、不同会话，非严格 A/B）：`created as storage` 从 pass 描述中消失，0x263e80000 以 960x540 渲染；每帧 pass GPU 38.0（同条件 HLE 未命中那次）→ **30.4 ms**，1080p 原生 pass 6.5 → **0 ms/帧**，FPS 21–22 → **24–25**。DumpLayer 截图诊所画面、光照、HUD 正常。仅血源诊所验证；TMNT 未复测该 HLE。

### 3.2 G-buffer 被切断的来源

pass_log 的 `end_detail`（[gbuffer-break-sources](evidence/swan-bloodborne-pm4trace-20260924/gbuffer-break-sources.txt)）与 trace 给出三类原因，随镜头不同：compute 写过的两个 vertex buffer（各约 60 KB）在 G-buffer 内首次作顶点属性读时才懒惰发 barrier；400 draw 的 G-buffer 命令超过录制线程 8 块持有上限后不能再提前；以及非提前 HLE 拷贝/dispatch 的 `StageAccess` 暂存访问泄漏到下一个 draw 的 pass 写集合，使后续同目标拷贝连锁冲突。

修复：draw 入口清暂存访问；持有上限 8 → 32 块（128 KiB/块）；buffer barrier 提前——draw 延续当前 pass、每个 barrier 都经 `ClassifyBarrier`（按 buffer 整个 guest 范围）确认 pass 内没有 draw 写过（写访问还要求没读过）时，barrier 记到 pre-pass 槽而不结束 pass；未分类的 barrier（间接参数等）一律按原路径。`gpu_memory` 的 `pass_hoist` 行新增 `barriers=`，`vk_recorder hoist off` 同时关闭 HLE 与 barrier 提前。

结果：模拟器切断重开 24.3 → 18.2 次/帧（hle 重开 3.2 → 0），G-buffer 实例 7.2 → 4.2 个/帧；同会话 4 轮 on/off（[hoist-ab-same-session](evidence/swan-bloodborne-pm4trace-20260924/hoist-ab-same-session.txt)）FPS 25.25/25.55、GPU GuestFrame 35.75/36.27 ms，差异在轮间波动内——**中性**，与“tile load/store 不是大头”的既有结论一致。保留（正确、默认开、可关）。

### 3.3 Turnip GMEM/sysmem 选择（实验，未改代码）

`debug.mesa.tu.debug=sysmem`（Mesa 在 Android 上按 `debug.mesa.<name>` 读 `TU_DEBUG`，进程启动时读一次）全 sysmem：每帧 pass GPU 33.2 → 27.8 ms，G-buffer 10.9 → 8.7，深度只读 pass 2.5 → 0.7，但部分 pass 变慢（2-MRT B10G11R11 1.25 → 2.06），应逐 pass 选择；FPS 仍 24，说明 GPU 变快后 CPU 侧同时受限。实验后属性已恢复为空并停止会话。

## 4. 其它诊断改进

- pass_log 每条带 `gpu_us`：`gpu_timing detail` 的 render pass zone 以 pass 序号为 tag，时间戳经录制线程 `Custom` 按序写入（不再 `CheckOutRaw` 使录制线程退回立即模式），retire 时并入 pass_log。
- `renderdoc_capture` / `renderdoc_guest_capture` 增加 `[delay]`（跳过若干边界后再抓，≤600）。
- RenderDoc 标签：`shadps4.pass #N guest|resumed-after:<cause> <targets>`、`shadps4.draw`（shader hash、参数、深度状态、纹理）、`shadps4.hle.copy`。
- 无附件 pass 的渲染区域取有效 scissor（此前为设备最大 16384²）；D32 深度图不再挂 stencil 附件（此前把清除的深度 pass 变成 load）。
- Android 显式加载 RenderDoc 层：`debug.shadps4.renderdoc_layer=1`；debug APK manifest 加 `<queries>`（否则 Android 11+ 包可见性使层 app “未安装”，强制层后 HWUI 的 vkCreateInstance 失败而 abort）。
- buffer 分配失败时打印各 heap usage/budget。

## 5. RenderDoc 在血源上抓不到帧

`renderdoccmd` 从 `spatial_mcp_publish/renderdoc-for-pico` a99864ad 用 NDK 25.1 + MSVC 宿主编译器构建（APK SHA f654a8d6…）。层已加载（`Attached debugging tool: RenderDoc`），但首次 dispatch 创建 16 KiB DeviceLocal BDA buffer 时 ErrorOutOfDeviceMemory，0.5 与 0.25 同样；此时 heap 0 usage 1014 / budget 6010 MiB，不是 VMA 预算拒绝。Turnip 在 KGSL 上以 `KGSL_MEMFLAGS_USE_CPU_MAP` 实现 capture-replay BDA（GPU VA = CPU mmap 地址）；推测 guest 预留占据了可用地址区间，约 1 GiB 后耗尽——未用 KGSL 日志证实。先前 TMNT 在 AYN 上可抓（BDA 总量小）。回放端指定 Turnip 的办法（`cmake/renderdoc`）与此无关。

## 6. 边界与状态

- 所有 FPS/GPU 对比都在出厂 GPU 频率下（902 MHz）；上一轮锁频操作收尾后 policy0 max 被系统限在 2611200、min 保持记录基线 1900800（基线本身是锁定值），工具报 CleanupPending，未手改 sysfs。跨会话数字有 ±3 ms/帧的会话间波动，只有 §3.2 是同会话 A/B。
- trace 的 action 层只覆盖 Rasterizer 的 draw/dispatch；DMA/CopyData/WriteData/事件只在原始 PM4 层；资源的 host 版本/写入者关联、选择性字节探针、与 PROF/RDC 的统一证据包（spec §4.4）未做。
- `.gpu.pm4.trace`/`.gcmdtrace.ps4` 原始文件留在本地 `build/validation/swan-bloodborne-20260924/`，不进 Git。
- 设备：`debug.mesa.tu.debug`、`debug.shadps4.renderdoc_layer`、GPU debug settings 均为空/null，会话 `user_stop`，`global.json` 为 0.5（SHA 47d1d96c）。
