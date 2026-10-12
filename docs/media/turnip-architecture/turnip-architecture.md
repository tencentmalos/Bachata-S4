# Turnip 架构剖析（文字版）

本文是视频 [turnip-architecture.mp4](turnip-architecture.mp4)（约 8 分钟）的文字版：同样七章，补上画面里的表格、
代码和每个例子的出处，便于检索和引用。字幕原文在 [turnip-architecture.srt](turnip-architecture.srt)，
生成方法见 [README](README.md)。KGSL 内核驱动本身的结构在续集
[KGSL 架构剖析](../kgsl-architecture/kgsl-architecture.md) 里展开。

| 章节 | 视频位置 | 内容 |
|---|---|---|
| [01 全景](#01-全景从-gcn--pm4-到-adreno) | 0:06 | 六层结构，两种 PM4 |
| [02 Vulkan 层](#02-vulkan-层vkcmd-如何变成-pm4) | 0:55 | PM4 流、state IB、`CP_SET_DRAW_STATE`；KH3 loading 挂死 |
| [03 着色器](#03-着色器spir-v--nir--ir3) | 2:15 | wave64/128、Int64 |
| [04 渲染模式](#04-渲染模式gmem-分块与-sysmem) | 3:09 | GMEM 分块、BV/BR、sysmem；ASTRO BOT VSC 溢出 |
| [05 CP 内部](#05-cp-内部rb--ib1--ib2-与固件流水) | 4:31 | RB/IB1/IB2、CP 微控制器；fault53 `CP DDE BR opcode error` |
| [06 内核接口](#06-内核接口tu_knl_kgslcc) | 5:56 | `tu_knl_kgsl.cc`；fault53 的后果链、4 GiB 边界 |
| [07 已证实与推测](#07-已证实推测与未定位) | 7:14 | 三类结论 |

设备：Odin3（AYN，Adreno 830，gen8_0_0）、Swan（Pico，Adreno 840 v2，gen8_2）、Thor（AYN，Adreno 740）。
所有例子来自 shadPS4 Android 的实测记录（2026-09～10）。

---

## 01 全景：从 GCN / PM4 到 Adreno

PS4 游戏交给 GPU 的是 GCN 着色器和 AMD PM4 命令包（经 `sceGnmSubmit*` 提交），安卓设备上没有 AMD 显卡。
一帧画面要经过六层：

| 层 | 角色 | 关键模块 |
|---|---|---|
| PS4 游戏 | 产生 GCN 着色器与 AMD PM4 | `sceGnmSubmitCommandBuffers` |
| shadPS4 | 解析 PM4（Liverpool），GCN → IR → SPIR-V，Vulkan 渲染器 | `video_core/` |
| SPIR-V + Vulkan | API 边界 | `vkCmdDraw/Dispatch`、`vkQueueSubmit`、`vkQueueBindSparse` |
| Turnip（Mesa） | 把 Vulkan 命令编码成 **Adreno 的 PM4**，把 SPIR-V 编译成 Adreno 机器码 | `tu_cmd_buffer`、`ir3`、`tu_knl_kgsl` |
| KGSL | 高通 GPU 内核驱动 `msm_kgsl.ko`：提交、fence、显存、调度 | `/dev/kgsl-3d0` |
| Adreno GPU | 命令处理器 CP（SQE、BV、BR、DDE、LPAC）执行命令，SP/GMEM/UCHE 干活 | 固件 + 硬件 |

**两种 PM4。** AMD 的 PM4 和 Adreno 的 PM4 名字相同，指令集完全不同。shadPS4 只解析前者；后者由 Turnip 生成、由 Adreno CP 的固件执行。下文说的 PM4、IB、
`CP_*` 操作码，除特别说明外都是 Adreno 的。

后面几章按层往下看：02 Vulkan 层、03 着色器、04 渲染模式（都在 Turnip 里），05 CP 内部（GPU），
06 内核接口（Turnip 与 KGSL 之间）。

## 02 Vulkan 层：vkCmd* 如何变成 PM4

### Turnip 的命令流

Turnip 的命令缓冲（`cmd->cs`）就是一段 Adreno PM4 流：

- **type4 包**写寄存器，例如 `GRAS_SC_SCREEN_SCISSOR`；
- **type7 包**是 CP 操作码：`CP_SET_DRAW_STATE`、`CP_DRAW_INDX_OFFSET`、`CP_INDIRECT_BUFFER`、`CP_EXEC_CS` 等。

管线状态在**创建管线时**就预先编码进一块块 **state IB**（各自是一小段 PM4）。绘制前发一条
`CP_SET_DRAW_STATE`，每组 3 个 dword，给出组号、长度、地址和生效阶段。dword0 的位域：

| 位 | 字段 | 含义 |
|---|---|---|
| 29:24 | `GROUP_ID` | 组号（0 = PROGRAM_CONFIG、1 = VS、8 = FS …） |
| 22 / 21 / 20 | `SYSMEM` / `GMEM` / `BINNING` | 在哪种 pass 中生效 |
| 17 | `DISABLE` | 禁用该组 |
| 16 | `DIRTY` | 强制重新加载 |
| 15:0 | `COUNT` | 长度（dword） |

例：`0x0161002C` = 组 1（VS）、GMEM+SYSMEM、DIRTY、44 dword，地址 `0x43_3182_9C00`。这是 fault53 中
IB2 第 1301 dword 那条包的第 2 组（字段定义见 `src/freedreno/registers/adreno/adreno_pm4.xml`）。

CP 的 **DDE** 按需读取这些 state IB；内容没变的组会被固件跳过，所以 Turnip 用 `DIRTY` 位强制重载。

**计算管线不走这条路。** 绑定计算管线时，Turnip 用 `CP_INDIRECT_BUFFER` 直接执行一次计算状态 IB，
之后假设 SP 的 CS 寄存器一直保持，直到下一次绑定。

### 例子：王国之心三（KH3）loading 挂死

shadPS4 默认跳过重复的 pipeline 绑定（B2 优化）。于是计算管线绑定后，中间录入了二十多个 render pass、
blit 和 LRZ 清除，才再次用同一管线派发。fault51 的 BR IB1 有界解码：

| IB1 dword | 内容 |
|---|---|
| 1697 | `CP_INDIRECT_BUFFER` → 计算管线状态 IB（44 dw），绑定 |
| 1728 | `CP_EXEC_CS 7168×1×1`，该管线第一次派发 |
| 1733–5039 | 约 23 个 sysmem pass、1 个 GMEM pass、blit、LRZ 清除，未重绑 |
| 5084 | `CP_EXEC_CS 104×1×1`，同一管线 —— **未完成的派发** |
| 5089–5269 | cache clean、WFI、A2D 拷贝、下一个管线的状态 IB |
| 5270 | `CP_WAIT_FOR_ME` —— **SQE 停在这里** |

挂死后采集的 SP 计算寄存器（MVC）：

| 寄存器 | 管线状态写入值 | 挂死时 |
|---|---|---|
| `SP_CS_CNTL_0` | `0x80104080` | 0 |
| `SP_CS_CNTL_1` | `1` | 0 |
| `SP_CS_BASE` | `0x40055a0980` | `0x40055a0980`（唯一保留） |
| `SP_CS_PVT_MEM_SIZE` | `0x80000000` | 0 |
| `SP_CS_CONFIG` | `0x105` | 0 |
| `SP_CS_INSTR_SIZE` | `2` | 0 |
| `SP_CS_CONST_CONFIG` | `0x304` | 0 |

NDRANGE 与工作组数和 5084 那次派发一致；只有 `SP_CS_BASE` 保留。MVC 是故障后的采集值，需结合对照实验解读。

**修复**：Turnip 提交 `2d96f4bc287`（分支 `codex/a8xx-compute-state-reemit`）在 `tu_dispatch()` 里，
A8XX 上若上次发出计算状态后又录入过别的命令，派发前重发状态 IB：

```c
   if (cmd->device->dbg_renderpass_stomp_cs) {
      tu_cs_emit_state_ib(&cmd->cs, shader->state);
+  } else if (CHIP >= A8XX &&
+             cs->cur != cmd->state.compute_state_end) {
+     tu_cs_emit_state_ib(cs, shader->state);
+     cmd->state.dirty |= TU_CMD_DIRTY_COMPUTE_DESC_SETS;
   }      … CP_EXEC_CS …
+  cmd->state.compute_state_end = cs->cur;
```

| | 结果 |
|---|---|
| 修复前 | 同流程 loading 约 52 s 必挂（fault 42/43/45/46/47/51） |
| 修复后 | loading 通过，240 s 以上无新 fault；`bind_skip off` 对照同样通过 |
| 推断 | gen8.0 固件的上下文切换没有保留 CS 寄存器 —— **无直接证据** |

`TU_DEBUG=nocsreemit` 负对照（预期恢复挂死）尚未执行。出处：
[odin3-kh3-compute-state-20261011](../../validation/android-native-host/odin3-kh3-compute-state-20261011.md)。

## 03 着色器：SPIR-V → NIR → IR3

```
GCN ISA → shadPS4 IR → SPIR-V (sirit) → NIR (spirv_to_nir) → NIR pass (降级/优化, nir_lower_int64)
        → IR3（指令选择、寄存器分配、调度）→ Adreno ISA（写入管线 BO）
```

前三步在 shadPS4，后四步在 Turnip/Mesa。两处与 GCN 的差异需要 shadPS4 主动处理：

**wave 宽度。** GCN 固定 wave64，EXEC/VCC 是 64 位掩码，一位对应一个 lane。Adreno 支持 64 和 128，
Turnip 默认报告子组大小 128（计算与片元可双倍线程宽度）。GCN 代码大量使用 64 位掩码，还会用
`v_readlane_b32 s0, v1, 63` 读第 31、63 号 lane 做归约；在 128 lane 的 wave 上，一个 host wave 相当于两个
GCN wave，两半会混在一起。所以 shadPS4 在设备允许的阶段要求 `requiredSubgroupSize = 64`
（`vk_compute_pipeline.cpp` / `vk_graphics_pipeline.cpp`）。

**Int64。** Adreno 没有原生 64 位整数运算，IR3 用 `nir_lower_int64` 拆成 32 位指令（`ir3_compiler.c`
里 `lower_int64_options = ~0`）：

```c
uint64 c = a + b;
  ↓ nir_lower_int64
lo    = a.lo + b.lo
carry = lo < a.lo
hi    = a.hi + b.hi + carry
```

| 驱动 | `shaderInt64` | 64 位整数由谁拆 |
|---|---|---|
| Turnip（Mesa） | 1，报告支持 | IR3：`nir_lower_int64` |
| 高通私有驱动 | 0，不报告 | shadPS4：u32 成对降级 |

`shaderInt64 = 1` 只说明驱动接受 Int64 SPIR-V，不代表硬件原生执行。

## 04 渲染模式：GMEM 分块与 sysmem

Adreno 是分块渲染（tiled）：片上有高速 **GMEM**，render target 切成若干 **bin**，一次渲染一个。

- **GMEM 模式**：先跑只算位置的 **binning pass**，把各 bin 可见的图元写进 **VSC 可见性流**（每个 pipe 一段）；
  A7XX 起由 **BV** 执行。随后 **BR** 逐 bin 渲染：LOAD 附件到 GMEM → 只画可见图元 → STORE 回系统内存。
  BV 可以领先 BR 并发分箱（concurrent binning，`TU_DEBUG=nocb` 关闭）。
- **sysmem 模式**：不分块，直接读写系统内存。Turnip 按 render pass 选择模式。

此外：**LRZ** 是低分辨率深度，在分箱/绘制前粗粒度剔除被遮挡像素；**UBWC** 是带宽压缩格式；
**FDM** 是片元密度图，不同区域不同着色密度，XR 注视点渲染在用。

### 例子：ASTRO BOT 在 Swan 上约 33 秒挂死

五份 KGSL snapshot 是同一个 pass：1440×1536，RGBA16F + D32F + S8，8× MSAA 时 16 个 bin、4× 时 9 个。

- BR 停在第 4–5 个 bin；`RBBM_STATUS 0x00FE0107`，CP 忙、`PC_BUSY`；SP/HLSQ/UCHE/VPC 空闲；
- 12 个 SP 指令缓存行都含 `end`，不是着色器死循环；BV 已越过最后一个 bin。

VSC 流还是 Turnip 的初始大小：每个 pipe 图元流 16 KiB（`VSC_PIPE_DATA_PRIM_LENGTH 0x4000`）、
draw 流 4 KiB（`VSC_PIPE_DATA_DRAW_LENGTH 0x1000`）。Turnip 的溢出处理是：命令缓冲 N 的 binning 写流溢出，
用 `CP_COND_WRITE` 记到全局 BO；命令缓冲 N+1 开始录制时检查，把流的 pitch 加倍。但在 A8xx 上，
**命令缓冲 N 里溢出的那个 pass 本身已让 PC 卡在截断的流上**。

| 设置（Swan，同场景） | 结果 |
|---|---|
| 默认 GMEM | 3/3 挂死（约 33 s） |
| `TU_DEBUG=sysmem` | 150 s 无故障 |
| `nolrz` | 仍挂死 |
| `nobin` | 81 s 无故障 |
| `gmem_warmup` | 91 s 无故障 |

**修复**：`vk_driver_android.cpp` 在加载 Turnip 前把 `gmem_warmup` 并入 `TU_DEBUG`，初始流每 pipe
512 KiB / 16 KiB × 32 pipe ≈ 16.5 MiB；150 s、4322 帧、约 30 FPS、无 KGSL 故障。`debug.shadps4.vsc_warmup=0` 关闭。

这是**规避，不是根治**：更重的场景仍可能超过 512 KiB；待做是让溢出的 pass 不依赖可见性流重画。
视频里溢出条的长度是示意，实际需求量未测。出处：
[astro-htile-firmware-20261008](../../validation/android-native-host/astro-htile-firmware-20261008.md)“GPU 挂死定位”。

## 05 CP 内部：RB / IB1 / IB2 与固件流水

命令按层嵌套：

| 层 | 内容 |
|---|---|
| RB 条目 | ringbuffer 里的 `CP_INDIRECT_BUFFER`，指向 IB1 |
| IB1 | Turnip 的命令缓冲（一次 `vkQueueSubmit` 提交的内容） |
| IB2 | draw IB、每个 bin 的 store IB、计算状态 IB |
| state IB | 由 `CP_SET_DRAW_STATE` 登记，DDE 读取 |

ringbuffer 按优先级分几条。Swan 上 RB0 是 XR runtime，RB2 是 shadPS4。

> **补充（读 KMD 源码后）**：视频说“KGSL 维护几条 ringbuffer”。gen8 用 GMU 硬件调度，KGSL 只把每次提交写进该上下文的
> GMU 队列并敲门铃，不自己写 RB；由此推断 RB 与 IB 包由 GMU 固件写入。优先级 0–15 按 `优先级 / 4` 映射到 RB0–3：
> XR 合成器优先级 0 → RB0，第三方应用（含 shadPS4）默认 8 → RB2。见
> [KGSL 架构剖析](../kgsl-architecture/kgsl-architecture.md) 第 03、05 章。

CP 里有多个运行固件的微控制器：

- **BR**：逐 bin 渲染、执行主命令流，跑 SQE 固件镜像；
- **BV**：分箱（可见性 pass），跑 BV 镜像；
- **DDE_BR / DDE_BV**：分别为 BR、BV 读取 draw state；
- **LPAC**：另一条异步计算流水。

DDE、BR、BV 没有公开全称，这里只按功能描述。

### 例子：fault53 `CP DDE BR opcode error`（Odin3，KH3 运行约 19 分钟后）

```
[171360.077765] adreno-gen8-gmu 3d37000.qcom,gmu: CP DDE BR opcode error | opcode=0x00000000
[171360.077789] kgsl kgsl-3d0: shadps4.android[28627]: ctx 6 ctx_type ANY ts 195670 policy 2
[171360.077852] kgsl kgsl-3d0: status 00880005 gfx_status 00880004 gfx_br_status 00880004
[171360.077854] kgsl kgsl-3d0: BR: rb 1b88/1c30 ib1 000000425F88B000/4b56 ib2 000000426BA05644/0317
[171360.077856] kgsl kgsl-3d0: BV: rb 1b8c/1c30 ib1 0000004038028000/0000 ib2 0001FF8002A36190/0000
```

BR 的 IB2 共 2118 dword，剩余 `0x317`（= 791），已取 1327。用 gpu-snapshot 解码这段 IB2
（截图 [ib2-decode-fault53.png](ib2-decode-fault53.png)）：

```
 dword  packet               fields
  1255  CP_SET_DRAW_STATE    n=3   组11 DESC_SETS 5 dw @0x431449adf8
  1259  CP_SET_DRAW_STATE    n=33  组10 CONST 33 dw @0x431449ad74 …
  1293  CP_DRAW_INDX_OFFSET  n=7   TRIANGLE_LIST · DMA_INDEXED · 4 B 索引
  1301  CP_SET_DRAW_STATE    n=33  11 组 · 包头 0x70438021
        组0 PROGRAM_CONFIG 25 dw @0x433182ae80  组1 VS 44 dw @0x4331829c00
        组2 VS_BINNING 65 dw  组3–5 HS/DS/GS  组6 GS_BINNING（禁用）
        组8 FS 79 dw @0x433182ac80  组7 VPC 40 dw  组18 PRIM_MODE_GMEM
  1327  ◀ 取指位置：已取 1327 dword，落在 1301 这条包内部
  1335  CP_SET_DRAW_STATE    n=3   组11 DESC_SETS 5 dw @0x431449b8d4
```

不能据此断定是 1301 这条包出错：

- **载荷未冻结**：这些 draw state 组指向的内存不在 snapshot 里，无法核对 DDE 实际读到了什么；
- **ROQ 预取**：取指位置领先于执行，真正出错的包可能早于 1301；
- 该芯片（`0x44050000`）没有已验证的 PM4 组合解码 profile；MVC 游标只做语义映射。

**Swan 的同类故障**（devcd13/14/15）经固件 PC 映射：DDE 停在非法包陷阱（`$19 = 0xbad4`），当前包头为 0。
三次都在 shadPS4 的 rb2 从 **level = 1** 抢占恢复后约 0.1 ms 发生：

| 故障 | rb2 离开时长 | 切回后到 `adreno_gpu_fault` |
|---|---|---|
| devcd13（`sds_page_align` 开） | 4.9 ms | 80 µs |
| devcd14（默认） | 4.7 ms | 115 µs |
| devcd15（请求 RB style） | 8.2 ms | 113 µs |

结构相同：rb2 被高优先级 rb0（XR runtime）以 level = 1 抢走，再经 rb1 切回 rb2。level = 1 只占 rb2 被切出
次数的约 9.6%，三次都碰上的巧合概率约 0.1%。这是**目前最强的线索，根因仍未定位**。

> **补充（读 KMD 源码后）**：硬件调度下，trace 里 `adreno_preempt_trigger/done` 的 level 参数取自 GMU trace 包的
> `ctx_switch_cntl`（`kgsl_gmu_core.c:876-881`），含义未公开，不能直接读成一级（L1）抢占。时间上的关联仍然成立，
> 只是“L1 抢占”这个解读需要打折扣。见 [KGSL 架构剖析](../kgsl-architecture/kgsl-architecture.md) 第 05 章。出处：
[odin3-kh3-performance-20261011](../../validation/android-native-host/odin3-kh3-performance-20261011.md)、
[bugs/odin3-kh3-cp-dde-opcode-20261011](../../../bugs/odin3-kh3-cp-dde-opcode-20261011/README.md)、
[swan-cp-opcode-firmware-20260930](../../validation/android-native-host/swan-cp-opcode-firmware-20260930.md)。

## 06 内核接口：tu_knl_kgsl.cc

`tu_knl_kgsl.cc` 是 Turnip 与 KGSL 之间的接口，全部通过 `/dev/kgsl-3d0` 的 ioctl：

| 类别 | ioctl | 作用 |
|---|---|---|
| 提交 | `IOCTL_KGSL_GPU_COMMAND` | cmdlist（IB 列表）+ synclist（时间戳或 fence fd），返回该上下文的 timestamp |
| fence | `WAITTIMESTAMP_CTXTID`、`READTIMESTAMP_CTXTID(RETIRED)`、`TIMESTAMP_EVENT` | fence = (context, timestamp)：阻塞等待 / 轮询已退休 / 导出 sync fd |
| 稀疏绑定 | `GPUMEM_BIND_RANGES`、`GPU_AUX_COMMAND(BIND)` | 虚拟 BO；同步绑定，或排队绑定（与普通提交同一条时间线排序） |
| 显存 | `GPUMEM_ALLOC_ID`、`GPUOBJ_ALLOC`、`GPUOBJ_INFO`、`GPUMEM_FREE_ID` | 分配 BO、查询 GPU 地址、释放 |

主要函数：`kgsl_queue_submit()`、`kgsl_syncobj_wait()`、`wait_timestamp_safe()`、`kgsl_sparse_vma_map()`、
`kgsl_bo_init()`。

### fault53 的后果链

| 层 | 事件 |
|---|---|
| GPU | 21:00:21 `CP DDE BR opcode error`，ctx 6 / ts 195670 —— **起因** |
| KGSL | 处理故障（dmesg：`Fault id:2 … policy 2`） |
| KGSL | 下一次 `IOCTL_KGSL_GPU_AUX_COMMAND(BIND)` 返回 `errno = EPROTO` |
| Turnip | `tu_knl_kgsl.cc:1797 bind submit failed: Protocol error` → `vk_device_set_lost()` |
| shadPS4 | `BufferCache::SubmitPendingArenaBinds`：`vkQueueBindSparse failed: ErrorDeviceLost` → 断言 |
| shadPS4 | Present 线程：`Device lost during waiting for a frame` → abort |

```c
ret = safe_ioctl(fd, IOCTL_KGSL_GPU_AUX_COMMAND, &req);
if (ret) {
   const int error = errno;
   kgsl_syncobj_destroy(&wait_sync);
   result = vk_device_set_lost(&queue->device->vk,
      "bind submit failed: %s\n", strerror(error));
   goto fail_submit;
}
```

同一函数 `GPU_COMMAND` 分支的注释（`:1756`）写明 `EPROTO - Device faulted since the last check.`
所以 bind 失败和 DeviceLost 都是 GPU 故障的**后果**，不是起因。

> **补充（读 KMD 源码后）**：KGSL 只在提交路径末尾对“出过故障但没被作废”的上下文返回一次 `-EPROTO`
> （`adreno_hwsched.c:1996`），注释称之为 “success” error：命令其实已经入队，只是 ioctl 返回非 0 时不拷回 timestamp。
> Turnip 的上下文不带 `INVALIDATE_ON_FAULT`，这类故障后上下文仍存活；DeviceLost 是 Turnip 对任何提交失败的处理，
> 不是内核宣布设备失效。见 [KGSL 架构剖析](../kgsl-architecture/kgsl-architecture.md) 第 06 章。

### 4 GiB 边界

固件在抢占恢复时，IB 预取地址的高 32 位不进位：IB 若跨过 `0x41_0000_0000` 这样的边界，预取会读到错误地址。
所以上游 `118ec7f45b1` 给命令 BO 加了 `TU_BO_ALLOC_NO_32B_ROLLOVER`（不跨 4 GiB）。

KGSL 后端原本用 `KGSL_MEMFLAGS_FORCE_32BIT` 实现这个约束。但这个常量是 `1ULL << 32`，而
`kgsl_gpumem_alloc_id.flags` 是 32 位的 `unsigned int`，第 32 位被截断，**从未生效**。（内核这边更彻底：`ALLOC_ID`
只给 32 位兼容进程自动加这一位，64 位进程无论如何都设不上，`kgsl.c:4469-4477`。）

我们曾用 `GPUOBJ_ALLOC`（64 位 flags）让它真正生效（IB32 实验）：命令/着色器 BO 全落在低 4 GiB，紧贴 GMEM
窗口 `0x4000000` 之后（IB 在 `0x4cb2d80`、`0x50fb000`）。兼容区起点 = `ALIGN(GMEM, 64 MiB) + GMEM`，Odin3 的 GMEM
是 12 MiB，所以从 `0x4C0_0000` 开始，与这两个地址吻合。结果字体复制计算着色器读错，游戏缺字，原因仍未查明。

**修复**：Turnip `92c96071dc5` 在 `kgsl_bo_init()` 里改为检查实际分配：`GPUOBJ_ALLOC` → `GPUOBJ_INFO` 取
gpuaddr → 起止地址高 32 位相同就使用；跨界则留作占位、重试，最多 4 次，最后释放所有占位。

```c
if (!info.gpuaddr || !alloc.size ||
    alloc.size - 1 > UINT64_MAX - info.gpuaddr ||
    (info.gpuaddr >> 32) !=
    ((info.gpuaddr + alloc.size - 1) >> 32)) {
   rejected[rejected_count++] = alloc.id;
   continue;
}
```

与上游 DRM 后端（VMA 堆 `nospan_shift = 32`）语义一致，不再强制低 4 GiB；push / set / copy 独立测试各 32/32。
出处：[odin3-kh3-compute-state-20261011](../../validation/android-native-host/odin3-kh3-compute-state-20261011.md) 第 8 节。

## 07 已证实、推测与未定位

**已证实**

- ASTRO：挂死与 VSC 初始大小有关；`gmem_warmup` 规避后 150 s 无故障。
- KH3：挂死时 SP 的 CS 寄存器为 0；派发前重发状态后 loading 通过（`bind_skip off` 对照一致；`nocsreemit` 负对照待做）。
- fault53：`bind submit failed`（EPROTO）与 DeviceLost 是 GPU 故障的后果。
- `FORCE_32BIT` 被 32 位 flags 截断，从未生效。

**推测**（都缺直接证据）

- KH3 寄存器丢失来自 gen8.0 固件的上下文切换。
- DDE 读到零包头与抢占恢复（level = 1）有关。

**未定位**

- `CP DDE BR opcode error` 的根因。
- 低 4 GiB 为何让着色器读错。

DDE、BR、BV 没有公开全称，本片只描述它们的功能。

## 读 KMD 源码后的补充（2026-10-12）

续集《[KGSL 架构剖析](../kgsl-architecture/kgsl-architecture.md)》读了 Swan ROM 里的 KGSL 源码（快照 `a0a588dc`），
对本片有三处补充，已在上文对应位置以引用块标出：

1. 第 05 章“KGSL 维护几条 ringbuffer”：gen8 由 GMU 硬件调度，KGSL 不写 RB；
2. 第 05 章 Swan 的 “level = 1”：该字段是 GMU 的 `ctx_switch_cntl`，含义未公开；
3. 第 06 章 EPROTO：上下文挺过了故障、命令已入队，DeviceLost 是 Turnip 的判定。

视频本身没有重新渲染。

## 资料

- [astro-htile-firmware-20261008](../../validation/android-native-host/astro-htile-firmware-20261008.md)
- [odin3-kh3-compute-state-20261011](../../validation/android-native-host/odin3-kh3-compute-state-20261011.md)
- [odin3-kh3-performance-20261011](../../validation/android-native-host/odin3-kh3-performance-20261011.md)
- [odin3-kh3-firmware-analysis-20261011](../../validation/android-native-host/odin3-kh3-firmware-analysis-20261011.md)
- [swan-cp-opcode-firmware-20260930](../../validation/android-native-host/swan-cp-opcode-firmware-20260930.md)
- Turnip 源码：`references/mesa-turnip/src/freedreno/vulkan/`（`tu_cmd_buffer.cc`、`tu_device.cc`、`tu_knl_kgsl.cc`），
  寄存器定义 `src/freedreno/registers/adreno/`
