# KGSL 架构剖析（文字版）

本文是视频 [kgsl-architecture.mp4](kgsl-architecture.mp4)（约 12 分钟）的文字版，也是《[Turnip 架构剖析](../turnip-architecture/turnip-architecture.md)》的续集：
上一集讲到 Turnip 经 KGSL 提交命令、分配显存、等待 fence，这一集往下一层，读高通 GPU 内核驱动（KMD）本身。
画面里的表格、代码和数字在这里都有出处。字幕原文在 [kgsl-architecture.srt](kgsl-architecture.srt)，生成方法见 [README](README.md)。

| 章节 | 视频位置 | 内容 |
|---|---|---|
| [01 全景](#01-全景kgsl-在架构中的位置) | 0:06 | KGSL 的三层、GMU、Turnip 的内核后端、设备与源码 |
| [02 对象与 ioctl](#02-对象与-ioctl) | 1:19 | 进程/上下文对象、上下文标志实测、厂商优先级钩子、ioctl 表 |
| [03 提交与时间戳](#03-提交与时间戳) | 2:31 | GPU_COMMAND → HFI 上下文队列 → GMU；退休链；超时 0 = 永远等待 |
| [04 显存与 MMU](#04-显存与-mmu) | 4:03 | 页表与 VA 布局、64 位内存标志、FORCE_32BIT、VBO 稀疏绑定、页故障 |
| [05 调度与抢占](#05-调度与抢占) | 6:02 | 优先级 → ringbuffer、抢占级别与风格、trace 里 level 字段的真实来源 |
| [06 故障与恢复](#06-故障与恢复) | 7:39 | GMU 错误码、硬复位、判责与 EPROTO、快照的覆盖范围 |
| [07 功耗与可观测性](#07-功耗与可观测性) | 9:35 | IFPC/下电、CPU 侧 TrustZone 调频、sysfs、tracepoint |
| [08 收尾](#08-能力清单与边界) | 11:01 | 能力清单、对 shadPS4 的影响、看不到的部分 |

## 源码与约定

- **KMD 源码**：Swan ROM 中的 `vendor/qcom/opensource/graphics-kernel`，位于 Ubuntu 主机
  `fangfang@10.37.108.94:/data00/swan_source/`（本机 ssh 别名 `workbench-hub`）。这是单个提交的快照
  `a0a588dca`（2026-07-02，“log ctx priority, GPU busy and freq on syncpoint deadlock”），约 15 万行、235 个文件。
  工作区另有一处未提交修改：`adreno_drawctxt.c` +54 行（XR / 全局抢占风格覆盖补丁）。**下文行号都按提交版本（HEAD）**，
  该文件的行号用 `git show HEAD:adreno_drawctxt.c` 核对过。
- **设备树**：同一 ROM 的 `vendor/qcom/opensource/graphics-devicetree/gpu/`（canoe-*、sun-*）。
- **Turnip**：本仓 `references/mesa-turnip`，行号按父仓引用 `c5a0d6fd990`（含 `92c96071dc5` 与计算状态重发修复）。
- **设备**：Swan 是 canoe 平台（`device/qcom/swan/BoardConfig.mk`：`TARGET_BOARD_PLATFORM := canoe`），
  Adreno 840 v2，`qcom,adreno-gpu-gen8-2-1`，实测 chip_id `0x44050a31`、GMEM 18 MiB、kernel 6.12.58。
  Odin3 是 sun 平台，`gen8-0-0`，chip `0x44050000`，Adreno 830。Odin3 与 Thor 跑的是 AYN 的内核，**本文没有核对**；
  只是 fault53 的 dmesg 打印格式与这份源码一致。
- 下文路径不带前缀的都相对 `graphics-kernel/`。“推断”“未验证”的内容单独标出。

---

## 01 全景：KGSL 在架构中的位置

```
用户态   shadPS4 ──► Turnip (libvulkan, tu_knl_kgsl.cc)        .openxr_runtime (compositor, 另一个进程)
               │ ioctl · /dev/kgsl-3d0                                │
内核     ┌─────▼────────────────┬────────────────────┬────────────────▼──────┐
(msm_kgsl│ kgsl 核心             │ adreno 层           │ gen8 hwsched          │
   .ko)  │ 对象 · 显存 · 页表     │ GPU 型号表 · 快照    │ 上下文队列 · 依赖       │
         │ 事件 · fence · timeline│ 调频 · 性能计数器    │ HFI 消息 · 门铃        │
         └──────────────────────┴────────────────────┴───────┬───────────────┘
                                                              │ HFI 队列 + 门铃
固件     SQE / AQE 微码（CP 执行 Adreno PM4）       GMU（Cortex-M3）：调度 · 抢占 · 上下电 · 调频投票
硬件     CP（BR · BV · DDE · LPAC）   SP · GMEM · UCHE   SMMU（按进程页表）
```

- KGSL 是高通的 GPU 内核驱动 `msm_kgsl.ko`，用户态只通过 `/dev/kgsl-3d0` 上的 ioctl 和它打交道。
- Turnip 有三种内核后端：`tu_knl_kgsl.cc`（KGSL）、`tu_knl_drm_msm.cc`（上游 freedreno 的 DRM msm）、
  `tu_knl_drm_virtio.cc`（虚拟机 virtio-gpu）；`tu_knl.cc` 先探测 `/dev/kgsl-3d0`。高通安卓设备的内核只提供 KGSL。
- 内核里分三层：kgsl 核心（`kgsl*.c`）管对象、显存、页表、事件；adreno 层（`adreno*.c`）管具体 GPU 型号、快照、调频；
  gen8 用 `adreno_gen8_hwsched_gpudev`，把调度交给 GMU 固件。
- **GMU** 是 GPU 旁边的一颗 Cortex-M3 微控制器（`adreno_gen8_gmu.c:282` 解除其复位；代码装进 ITCM/DTCM，`:29-40`），
  负责硬件调度、GX 上下电、RPMh 投票。固件文件：Swan `gen80200_gmu.bin`、`gen80200_sqe.fw`、`gen80200_aqe.fw`；
  Odin3 `gen80000_*`（`adreno-gpulist.h:3582-3585`、`3189-3192`）。
- 用户态做不了、由内核负责的：页表与 IOMMU、多个进程之间的调度与抢占、故障检测与恢复、上下电与调频、调试快照与 trace。

| 设备 | 平台 | GPU | KGSL 型号 | 调度 |
|---|---|---|---|---|
| Swan（Pico） | canoe | Adreno 840 v2 | gen8_2_1 · `0x44050a31` | GMU 硬件调度 |
| Odin3（AYN） | sun | Adreno 830 | gen8_0_0 · `0x44050000` | GMU 硬件调度（按此源码的型号表） |
| Thor（AYN） | kalama | Adreno 740 | gen7_2_x | GMU 硬件调度（按此源码的型号表） |

这份源码里，所有 gen8 型号以及 gen7_2（A740）都用硬件调度（hwsched）；软件派发器 `adreno_dispatch.c`
只给 a5xx/a6xx 和部分 gen7 用（`adreno-gpulist.h:2391、2428、3181、3574`；`adreno_gen8_hwsched.c:2091-2093`）。

主要文件：

| 文件 | 内容 |
|---|---|
| `kgsl.c` · `kgsl_ioctl.c` | 对象、ioctl 表 |
| `kgsl_drawobj.c` · `kgsl_events.c` · `kgsl_sync.c` · `kgsl_timeline.c` | 提交对象、同步点、事件、sync file、timeline |
| `kgsl_iommu.c` · `kgsl_mmu.c` · `kgsl_sharedmem.c` · `kgsl_vbo.c` | 页表、VA 分配、物理页、稀疏绑定 |
| `adreno_hwsched.c` · `adreno_gen8_hwsched_hfi.c` | 硬件调度、HFI 消息、故障处理与恢复 |
| `adreno_gen8_gmu.c` · `adreno_hfi.h` | GMU 启停、HFI 消息格式与错误码 |
| `kgsl_snapshot.c` · `adreno_gen8_snapshot.c` · `adreno_hwsched_snapshot.c` | 快照 |
| `kgsl_pwrctrl.c` · `kgsl_pwrscale.c` · `governor_msm_adreno_tz.c` | 功耗、调频 |

## 02 对象与 ioctl

### 打开设备时建立的对象

- `kgsl_open`（`kgsl.c:1535-1586`）建 per-fd 的 `kgsl_device_private`（只含 `{device, process_priv}`，`kgsl_device.h:672-675`），
  再取或建 **per-process** 的 `kgsl_process_private`（按 `current->group_leader`，`kgsl.c:1214-1218`）。
  同一进程第二次打开只增加 `fd_count`（`:1229-1231`）。
- `kgsl_process_private`（`kgsl_process_private_new`，`kgsl.c:1210-1300`）：`mem_idr`（显存条目，id → gpuaddr）、
  `syncsource_idr`、**按 pid 建立的 GPU 页表**（`kgsl_mmu_getpagetable`，`:1278`）。
- **上下文表是全设备共享的** `device->context_idr`（`kgsl_device.h:294`），但每个上下文只属于创建它的 fd
  （`kgsl_context_get_owner`，`kgsl_device.h:1073-1086`）。每进程最多 200 个（`KGSL_MAX_CONTEXTS_PER_PROC`，`kgsl.h:64`，检查在 `kgsl.c:726`）。
- Turnip 的一个 `VkQueue` 对应一个 KGSL 上下文（`kgsl_submitqueue_new`，`tu_knl_kgsl.cc:51`）。

### DRAWCTXT_CREATE：请求与返回

Turnip 请求 `SAVE_GMEM | NO_GMEM_ALLOC | PREAMBLE`（`tu_knl_kgsl.cc:53-56`），不设优先级和类型。
`adreno_drawctxt_create`（HEAD `adreno_drawctxt.c:442` 起）：

- 只保留接受列表里的位，`SAVE_GMEM` 不在其中，被丢弃；
- `PREAMBLE` 与 `NO_GMEM_ALLOC` 缺一个就失败：“legacy context switch not supported”（HEAD `:474-478`）；
- 强制打开 `PER_CONTEXT_TS`（HEAD `:510`）；
- 未设优先级 → `KGSL_CONTEXT_PRIORITY_MED = 8`（HEAD `:432`、`:521`）；
- 把生效后的 flags 写回调用者。

Swan 实测（09-29，`TU_DEBUG=kgsl_preempt_rb`，额外请求 RB 风格）：

| 位 | 字段 | 请求 | 返回 |
|---|---|---|---|
| 27:25 | PREEMPT_STYLE | 1 | 1 |
| 24:20 | TYPE | 0 | 0 = ANY |
| 15:12 | PRIORITY | 0 | **8** |
| 6 | PER_CONTEXT_TS | 0 | **1** |
| 4 | PREAMBLE | 1 | 1 |
| 1 | NO_GMEM_ALLOC | 1 | 1 |
| 0 | SAVE_GMEM | 1 | **0** |
| | 合计 | `0x0200_0013` | `0x0200_8052` |

默认（不开 `kgsl_preempt_rb`）时 STYLE = 0，其余位相同。TYPE 取值：0 ANY、1 GL、2 CL、4 RS、5 VK（uapi `msm_kgsl.h:67-75`）；
Turnip 没写类型，所以 fault53 的 dmesg 写着 `ctx 6 ctx_type ANY`。

### 厂商优先级钩子

`adreno_drawctxt_hook`（HEAD `adreno_drawctxt.c:396-430`，名字与常量在 `kgsl.h:549-558`）：

| 调用者 | 条件 | 优先级 | ringbuffer |
|---|---|---|---|
| `.openxr_runtime` 的 compositor 线程 | uid < 10000 | 0（SUPER） | RB0 |
| `.openxr_runtime` 的 main_loop 线程 | uid < 10000 | 1 | RB0 |
| 第三方应用（含 shadPS4） | uid ≥ 10000 | 数值至少 8，即最多拿到 MEDIUM | RB2 |

KGSL 里**数值越小越优先**（0 最高、15 最低）。另有进程级 `dynamic_prio`，可经厂商 ioctl `GPU_PRIO_SHIFT` 设置（`kgsl.c:752-757`、`5583-5591`）。

### ioctl 表

分发表 `kgsl_ioctl_funcs[]`（`kgsl_ioctl.c`），不在其中的转给 `adreno_ioctl`（`adreno_ioctl.c:298-306`）。

| 类别 | ioctl | Turnip |
|---|---|---|
| 上下文 | `DRAWCTXT_CREATE` · `DRAWCTXT_DESTROY` | ✓ |
| 显存 | `GPUMEM_ALLOC_ID` · `GPUOBJ_ALLOC` · `GPUOBJ_IMPORT` · `GPUOBJ_INFO` · `GPUMEM_GET_INFO` · `GPUMEM_FREE_ID` · `GPUOBJ_FREE` | ✓ |
| 稀疏绑定 | `GPUMEM_BIND_RANGES` · `GPU_AUX_COMMAND(BIND)` | ✓ |
| 提交 | `GPU_COMMAND` | ✓ |
| 时间戳 | `CMDSTREAM_READTIMESTAMP_CTXTID` · `DEVICE_WAITTIMESTAMP_CTXTID` · `TIMESTAMP_EVENT` | ✓ |
| 属性 / 计数 | `DEVICE_GETPROPERTY` · `PERFCOUNTER_READ` | ✓ |
| timeline | `TIMELINE_CREATE / WAIT / FENCE_GET / QUERY / SIGNAL / DESTROY` | — |
| 旧接口 | `RINGBUFFER_ISSUEIBCMDS` · `SUBMIT_COMMANDS` · `SHAREDMEM_*` · `SPARSE_*`（处理函数为 NULL，未实现） | — |
| 厂商 / 其它 | `GPU_PRIO_SHIFT` · `RECURRING_COMMAND` · `GET_FAULT_REPORT` · `SYNCSOURCE_*` · `PERFCOUNTER_GET/PUT/QUERY` | — |

**拷回规则**（`kgsl_ioctl.c:167-172`）：处理函数返回 0 时才把结构体拷回用户态。所以一次提交即使在内核里已经入队，
只要返回值非 0（例如 `-EPROTO`），用户态就拿不到它的 timestamp（见第 06 章）。

## 03 提交与时间戳

### 一次提交的路径

| 步骤 | 内容 | 出处 |
|---|---|---|
| 1 | `vkQueueSubmit` → `kgsl_queue_submit` → `IOCTL_KGSL_GPU_COMMAND`（`SUBMIT_IB_LIST`，每项 `KGSL_CMDLIST_IB`） | `tu_knl_kgsl.cc` |
| 2 | 分类：有同步点 + 命令 → SYNCOBJ\|CMDOBJ；最多 2000 个 IB、32 个同步点 | `kgsl.c:2182-2215`，`kgsl.h:151-152` |
| 3 | 建 draw object（`kgsl_drawobj_cmd` + 可选 `kgsl_drawobj_sync`） | `kgsl.c:2301-2392` |
| 4 | 分配时间戳：每上下文一个递增计数 `drawctxt->timestamp`，即 **QUEUED** | `adreno_hwsched.c:1635-1655` |
| 5 | 同步点挂起：等别的上下文的时间戳（事件）、sync file（fence 回调）、timeline（dma_fence） | `kgsl_drawobj.c:580-870` |
| 6 | 进上下文 drawqueue（128 项，满了最多睡 10 s），任务挂到 `jobs[priority]`，按优先级 0–15 派发 | `adreno_hwsched.c:1657-1690`、`1563-1570` |
| 7 | 首次提交时向 GMU 注册上下文（`H2F_MSG_REGISTER_CONTEXT` 带页表与 flags，`CONTEXT_POINTERS` 带 memstore 地址和上下文队列地址） | `adreno_gen8_hwsched_hfi.c:2999-3112` |
| 8 | 把 `ISSUE_CMD` 写进该上下文 4 KiB 的 GMU 上下文队列，再敲门铃 `DISPQ_IRQ_BIT(level)` | `adreno_gen8_hwsched_hfi.c:3759-3878` |

```c
struct hfi_submit_cmd {               /* adreno_hfi.h:1032-1041 */
    u32 hdr; u32 ctxt_id; u32 flags; u32 ts;
    u32 profile_lo, profile_hi;
    u32 numibs; u32 big_ib_gmu_va;
};  /* 后接 numibs 个 {addr, size}；约 82 个以内内联，再多走“big IB” */
```

**关键点：gen8 上 KGSL 不写 ringbuffer。** hwsched 的 gpudev 没有 `ringbuffer_submitcmd`/`rb_start`
（`adreno_gen8.c:3365-3397`，对比旧 GMU gpudev `:3399-3428`）。由此**推断** RB 与 IB 包由 GMU 固件写入（固件不在源码里）。
每个 IB 的 flags 在 hwsched 下被丢掉，只把 `{addr, size}` 交给 GMU（`adreno_gen8_hwsched_hfi.c:3137-3141`）。

Swan 的 gen8_2_1 把硬件 fence 关掉了：`// Disabled (Fence Stability Issues): ADRENO_HW_FENCE`（`adreno-gpulist.h:3561-3574`），
所以 fence 类同步点由 CPU 在内核里解决。（同一份源码里 gen8_0_0 是开的，但 Odin3 的实际内核未核对。）

### 时间戳与退休

```c
struct kgsl_devmemstore {      /* uapi msm_kgsl.h:301-312，每上下文 40 字节 */
    soptimestamp;              /* CONSUMED：开始执行时由 GPU 写 */
    eoptimestamp;              /* RETIRED：执行完成后写 */
    preempted; ref_wait_ts; current_context; …
};                             /* QUEUED = KGSL 自己的计数 */
```

退休链（hwsched）：

1. GMU 发 `F2H_MSG_TS_RETIRE`（id 133，带 submitted_to_rb / sop / eop / retired_on_gmu 等计时）并触发中断；
2. `gen8_hwsched_hfi_handler` 唤醒 `gmu_f2h` 线程（SCHED_FIFO），读消息后排调度（`adreno_gen8_hwsched_hfi.c:1110-1113`、`1185-1213`）；
3. 调度线程 `adreno_hwsched_work` → `retire_drawobj_list` 比对 memstore 的 RETIRED（`adreno_hwsched.c:2926-2965`）；
4. `kgsl_process_event_groups` 触发事件：唤醒等待者、signal sync file 与 timeline。

RB_DONE_TS 等 GPU 中断在 hwsched 模式下被屏蔽（`adreno_gen8.h:314-320` 对比 `298-312`），所以每次退休通知都要经过
GMU → 中断 → 两个内核线程 → 事件回调，每一跳都有延迟。

### 超时 0 = 永远等待

```c
/* HEAD adreno_drawctxt.c:163-168（adreno_drawctxt_wait） */
/*
 * If timeout is 0, wait forever. msecs_to_jiffies will force
 * values larger than INT_MAX to an infinite timeout.
 */
if (timeout == 0)
        timeout = UINT_MAX;
```

`TIMELINE_WAIT` 相反，超时 0 才是轮询（`kgsl_timeline.c:432-433`）。旧版 Turnip 的 `wait_timestamp_safe` 在剩余时间为 0
时照样发 `WAITTIMESTAMP_CTXTID(timeout=0)`，于是“查询一下是否完成”变成了等 GPU 完成：

| 设备 · 操作 | 旧 Turnip | 改读 RETIRED |
|---|---|---|
| Thor · `vkWaitSemaphores(timeout=0)` | 66.8 ms，SUCCESS | 0.062 ms，TIMEOUT |
| Thor · 第二次 `vkQueueSubmit` | 66.8 ms | 0.039 ms |
| Swan · 零超时等待 | 195.9 ms | 5.5 µs，TIMEOUT |

数据见 [frame-tail-wait-2026-09-16](../../validation/android-native-host/frame-tail-wait-2026-09-16.md) 与
[swan-turnip-20260920](../../validation/android-native-host/swan-turnip-20260920.md)。修复：剩余 0 毫秒时改读
`READTIMESTAMP_CTXTID(RETIRED)`，按 32 位有符号差比较（我们的 fork `be8ebc54`；上游 `e984ef294ea` 同样修了，
10-09 合并上游时取上游实现）。当前代码：

```c
/* tu_knl_kgsl.cc:957-970（c5a0d6fd990）wait_timestamp_safe() */
while (true) {
   int timeout_ms = get_relative_ms(abs_timeout_ns);
   if (timeout_ms == 0)
      return kgsl_poll_timestamp(device, context_id, timestamp);
   wait.timeout = timeout_ms;
   int ret = ioctl(device->fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &wait);
   …
}
```

等待成功之后，若上下文自上次检查以来出过故障，返回 `-EPROTO`；作废的上下文返回 `-EDEADLK`；已分离的返回 `-ENOENT`
（`adreno.c:3268-3285`）。Turnip 把这三种都当作设备丢失（`tu_knl_kgsl.cc:920-925`）。

## 04 显存与 MMU

### 页表与 VA 布局

- SMMU 是 `qcom,qsmmu-v500` + `qcom,adreno-smmu`，KGSL 用**每进程页表**（`KGSL_MMU_IOPGTABLE`，`kgsl_iommu.c:2797-2806`）。
  TTBR0 切到当前进程的页表，TTBR1 指向所有进程共用的全局页表。context bank 有 `gfx3d_user`、`gfx3d_lpac`、`gfx3d_secure`
  三个（canoe-gpu.dtsi:150-166；其中的 0x0/0x1/0x2 是 stream ID，不是 bank 号）。
- 64 位进程的 GPU 虚拟地址（`kgsl_iommu.h:16-77`，`kgsl_iommu.c:1624-1656`）：

| 区段 | 范围 | 用途 |
|---|---|---|
| 兼容区 | `ALIGN(GMEM, 64 MiB) + GMEM` … `0xEFFF_F000` | `FORCE_32BIT`、32 位进程 |
| SVM 区 | `0x10_0000_0000` … `0x40_0000_0000`（64–256 GiB） | `USE_CPU_MAP`：mmap 时让 GPU 地址等于 CPU 地址 |
| 普通区 | `0x40_0000_0000` … `0x80_0000_0000`（256–512 GiB） | 默认；按递增提示顺序摆放（`kgsl_iommu.c:2162-2189`） |
| 全局区 | `0x1_FF80_0000_0000` 起（TTBR1），90 MiB 位图分配 | memstore、GMU 申请的 ringbuffer、抢占记录、scratch、固件 |

fault53 的两个地址正好落在这里的两段：BR 的 IB1 `0x42_5F88_B000`（普通区，约 265 GiB），BV 的 IB2 `0x1_FF80_02A3_6190`（全局区）。

### 物理页

按 canoe 的构建配置（`config/canoe_*_gpuconf`：`CONFIG_QCOM_KGSL_USE_SHMEM=y`、`IOCOHERENCY_DEFAULT=y`），
每个 BO 一个 shmem 文件，mapping 设为不可换出（`kgsl_sharedmem.c:1263-1288`）。页池 `kgsl_pool.c`（设备树里有 6 个池、约 100 MiB 预留）
和进程回收 `kgsl_reclaim_qcom.c` 在源码里，但这个配置下没有编入。**设备上实际 `.ko` 的配置未核对**
（主机上一份 bazel 构建产物与此一致）。

### 64 位内存标志与 FORCE_32BIT

| 位 | 名字 |
|---|---|
| 3 | SECURE |
| 8–15 | MEMTYPE（用途） |
| 16–23 | MEMALIGN（log2） |
| 24 / 25 | GPUREADONLY / GPUWRITEONLY（后者内核没用） |
| 26–27 | CACHEMODE：WC / UNCACHED / WT / WB |
| 28 | USE_CPU_MAP |
| 31 | IOCOHERENT |
| **32** | **FORCE_32BIT** |
| 33 | GUARD_PAGE |
| **34 / 35** | **VBO / VBO_NO_MAP_ZERO** |

（uapi `msm_kgsl.h:136-225`）

上一集说 `FORCE_32BIT`（`1ULL << 32`）被 `kgsl_gpumem_alloc_id` 的 32 位 `flags` 截断。内核这边更彻底：

```c
/* kgsl.c:4469-4477 · ALLOC_ID */
uint64_t flags = param->flags;
if (is_compat_task())
        flags |= KGSL_MEMFLAGS_FORCE_32BIT;
```

64 位进程走 `ALLOC_ID` 既传不进第 32 位，内核也不会加。只有 64 位 flags 的 `GPUOBJ_ALLOC` 能设上，分配才会落进兼容区：

```c
/* kgsl_iommu.c:2459-2491 · kgsl_iommu_get_gpuaddr() */
if (memdesc->flags & KGSL_MEMFLAGS_FORCE_32BIT) {
        start = pagetable->compat_va_start;
        end = pagetable->compat_va_end;
} else { start = va_start; end = va_end; }
```

兼容区起点 = `ALIGN(GMEM, 64 MiB) + GMEM`（`adreno.c:1356-1359` 与 `kgsl_iommu.h`）：

| 设备 | GMEM | 兼容区起点 | 上一集 IB32 实验 |
|---|---|---|---|
| Odin3 · gen8_0_0 | 12 MiB | `0x4C0_0000`（76 MiB） | IB 在 `0x4CB_2D80`、`0x50F_B000` —— 正好在起点之后 |
| Swan · gen8_2_1 | 18 MiB | `0x520_0000`（82 MiB） | — |

Turnip 的 `92c96071dc5` 不再追求低 4 GiB，只保证命令 BO 不跨 4 GiB 边界（`GPUOBJ_ALLOC` → `GPUOBJ_INFO` → 检查高 32 位 → 跨界就留作占位重试）。

### VBO 与稀疏绑定

- **VBO** 是只有一段 GPU 地址、没有物理页的对象，`KGSL_MEMFLAGS_VBO`（第 34 位），所以只能用 `GPUOBJ_ALLOC` 建
  （`gpumem_alloc_vbo_entry`，`kgsl.c:4224-4294`）。不能带 `USE_CPU_MAP`、只读、缓存模式；大小只受普通区 256 GiB 限制。
- 未绑定的范围都映射到同一个零页 `kgsl_vbo_zero_page`（SMMU PRR，`kgsl_iommu.c:1752-1777`）；建 VBO 时整段先映射零页，
  除非带 `VBO_NO_MAP_ZERO`。读零页得到什么值源码没写，**未验证**。
- **同步绑定** `GPUMEM_BIND_RANGES`（`kgsl_vbo.c:681-740`）：排到 workqueue，调用方等完成；可选异步加 fence。
- **排队绑定** `GPU_AUX_COMMAND(BIND)`（`kgsl.c:2394-2577`）：一次只收 1 条命令；内核在它前面插一个隐式同步对象，
  等**同一上下文**最后一个已排队时间戳退休，之后在 CPU 上改页表（`kgsl_sharedmem_bind_ranges`），完成前挡住该上下文后面的命令
  （`adreno_hwsched.c:1068-1093`）。跨上下文只靠显式同步点排序。
- Turnip：`vkQueueBindSparse` 的区间按 VMA 分组，每个 VMA 一次 `AUX_COMMAND`。shadPS4 的 `BufferCache::SubmitPendingArenaBinds`
  走的就是这条路。

### 页故障

`kgsl_iommu_fault_handler`（`kgsl_iommu.c:1206-1270`）按 TTBR0 找到进程，打印
`GPU PAGE FAULT: addr = %lX … context pid = %d ctx_type=%s`，附读写、translation/permission、出错硬件块（FSYNR1 解码）、
附近的 buffer、刚释放时的 premature free 提示，并发 `kgsl_mmu_pagefault` tracepoint。

`ft_pagefault_policy` 默认 0（`kgsl_mmu.h:37-47`）：SMMU 终止出错的那次访问（CFCFG=0），其余继续（HUPCF=1），
**不触发恢复**（`kgsl_iommu.c:1781-1815`）。只有 GMU 另外报了带 SMMU 位的软件故障，才进入第 06 章的恢复流程。

例子（Swan，Mapper5 修好之前，APK `204adb4b`，PID 13313，context 54）：gralloc 新的 handle 布局没被识别，
Turnip 按错误的 tiling 写，CCU 写到 `0x40_01BF_E700`，超出已映射的 `[0x40_0140_0000, 0x40_01BF_8FFF]`，
先报 write translation fault，随后 GMU 报 GPU hang（ts 2847）→ DeviceLost。Mapper5 修复（`86ca472f`）后不再出现。
见 [swan-turnip-20260920](../../validation/android-native-host/swan-turnip-20260920.md)。

## 05 调度与抢占

### 优先级到 ringbuffer

```c
/* adreno.h:1950 · adreno_get_level() */
level = context->priority / KGSL_PRIORITY_MAX_RB_LEVELS;   /* 4 */
return min(level, 3);                                       /* LPAC 另用 RB4 */
```

| 优先级 | ringbuffer | 谁在用 |
|---|---|---|
| 0–3 | RB0 | XR 合成器（0）、main_loop（1） |
| 4–7 | RB1 | |
| 8–11 | RB2 | 第三方应用默认 8，shadPS4 在这里 |
| 12–15 | RB3 | |

- RB0 上的工作总是跑完，不被抢占，也没有用户上下文的抢占记录（HEAD `adreno_drawctxt.c` 中 “RB0 always runs to completion”）。
- GMU 收到的是**原始优先级**（上下文注册时 `cmd.flags = HFI_CTXT_FLAG_NOTIFY | context->flags`，`adreno_gen8_hwsched_hfi.c:3016`）；
  KGSL 只按 level 选门铃位 `BIT(level + 2)`（`:3147-3161`）。**何时调度、何时抢占在 GMU 固件里，源码看不到。**

### 抢占配置

全局，启动时发一次 `HFI_FEATURE_PREEMPTION`（`adreno_gen8_hwsched_hfi.c:1583-1593`）：

| 位 | 含义 | Swan |
|---|---|---|
| 1:0 | 抢占级别 | 1（`adreno-gpulist.h` gen8_2_1 `.preempt_level = 1`） |
| 2 | usesgmem | 1（强制，`adreno_gen8.c:2720-2722`） |
| 3 | skipsaverestore | 1（强制） |
| 超时 | `HFI_VALUE_BIN_TIME` | 10000 ms（`adreno.h:353`） |

每上下文：flags 第 27:25 位请求风格，0 = DEFAULT/ANY、1 = RINGBUFFER、2 = FINEGRAIN，原样进上下文注册消息。
Turnip 默认不请求；我们的 fork 有默认关闭的 `TU_DEBUG=kgsl_preempt_rb` / `kgsl_preempt_fg`。
源码只说明 L1 抢占时跳过保存/恢复（`adreno.h:406`），**0、1、2 各级的确切含义没有公开**。

### trace 里的 “level” 不是抢占级别

Swan 的三次 DDE 故障（devcd13/14/15）都发生在 rb2 被 rb0 抢走、再经 rb1 切回 rb2 之后约 0.1 ms，trace 里写着 `level=1`
（[swan-cp-opcode-firmware-20260930](../../validation/android-native-host/swan-cp-opcode-firmware-20260930.md)）。
源码给这条线索加了注脚：硬件调度下，`adreno_preempt_trigger/done` 的 level 参数是 GMU trace 包里的 `ctx_switch_cntl`：

```c
/* kgsl_gmu_core.c:876-881 · stream_trace_data() */
case GMU_TRACE_PREEMPT_DONE: {
   struct trace_preempt_done *data = …;
   trace_adreno_preempt_done(data->prev_rb, data->next_rb,
                             data->ctx_switch_cntl,   /* → 打印成 level */
                             pkt->ticks);
```

它的含义没有公开，**不能直接读成一级（L1）抢占**。非 hwsched 路径才把 `CP_CONTEXT_SWITCH_LEVEL_STATUS` 放在这里（`adreno_gen8_preempt.c:112-115`）。

| 实验（Swan · 09-29） | 结果 |
|---|---|
| 请求 RB 风格（`kgsl_preempt_rb`） | 返回 `0x0200_8052` → style 1 已生效 |
| 同一轮 trace：rb2 被切出 22634 次 | level=0 20456 · level=1 2172 · level=3 6 |
| 故障 | 仍发生（DDE BR opcode error） |

风格位与这个字段的关系仍未知；抢占与故障的关联是时间上的强相关，没有因果证明。

### ROM 目录里的 XR 抢占实验补丁

`/data00/swan_source/` 下：

| 补丁 | 行 | 作用 |
|---|---|---|
| `graphics_kernel.patch` | 164 | 导出抢占初始化与改优先级前的准备（分配每级记录），修 GMEM 记录的错误检查 |
| `xr_context_preempt_style_ab_release` | 60 | `.openxr_runtime` 上下文的风格覆盖（模块参数 + 计数） |
| `global_context_preempt_style_ab_release` | 55 | 全局风格覆盖，优先于 XR 覆盖 |
| `global_context_preempt_style_default_any_release` | 6 | 全局默认改为 ANY（源码工作区就是这一版） |
| `xr_preempt_probe_release` | 223 | RB0 XR 上下文分配记录 + `kgsl_xr_preempt_probe` 日志 |
| `xr_preempt_probe_once_release` | 51 | 探针每个上下文只打一次 |
| `xr_preempt_style_hfi_ab_release` / `_counters_release` | 56 / 45 | 只改 HFI 注册时的风格位，加计数 |

**是否进了设备上的量产内核：未验证。** Turnip 日志里的 `returned_style` 能反映内核实际应用的风格。

## 06 故障与恢复

### 检测

gen8 上 CP 的错误由 GMU 检测（hwsched 的中断掩码只剩 AHB/ATB/UCHE/TSB 这类非致命错误，`adreno_gen8.h:314-320`）。
GMU 发 `F2H_MSG_CONTEXT_BAD`（150）：

```c
struct hfi_context_bad_cmd {   /* adreno_hfi.h:998-1012 */
   u32 hdr; u32 version;
   struct fault_info gc;       /* ctxt_id · policy · ts */
   struct fault_info lpac;
   u32 error;                  /* 600 … 700 */
   u32 payload[];              /* 寄存器 · RB · 抢占超时 · 复位策略 */
};
```

`gmu_f2h` 线程 → `process_ctx_bad()` → `log_gpu_fault()`（`adreno_gen8_hwsched_hfi.c:268`、`586`）→
`scheduler_fault |= HARD`，唤醒调度线程。错误码（`adreno_hfi.h:1312-1396`）：

| 错误码 | 含义 |
|---|---|
| 600 · 601 | CP 硬件错误 · GPU 硬件挂死 |
| 602 · 603 | 抢占超时 · 长 IB 超时（SW_HANG） |
| 604–609 | BR：操作码 / 保护 / 非法指令 / 微码 / 硬件故障 / GPC |
| 610–614 · 615–620 | BV 同类 · LPAC 同类（620 为 LPAC 长 IB） |
| 621 | 软件熔丝违规 |
| 622–629 · 630 | AQE0/1 同类 · 同步对象超时 |
| 631–635 | DDE_BR：硬件故障 / **632 操作码（CP DDE BR opcode error）** / 微码 / 保护 / 非法指令 |
| 636–640 · 641–649 | DDE_BV 同类 · 各流水软件故障 |
| 650–656 | 非致命：AHB · ATB · UCHE 越界 / trap · TSB · DBGC |

`"CP DDE BR opcode error | opcode=0x%8.8x"` 的打印在 `adreno_gen8_hwsched_hfi.c:442-447`。
dmesg 里 `Fault id:%d` 是 `scheduler_fault` 位掩码（`adreno.h:317-324`：SOFT 1、HARD 2、TIMEOUT 4、IOMMU 8、PREEMPT 16、GMU 32、
CTX_DETACH 64、SKIP_SNAPSHOT 128），fault53 的 `Fault id:2` 即硬故障；`policy` 是 GMU 给的 `gc.policy`，
hwsched 下内核不解读（`adreno_hwsched.c:2766`）。

### 恢复

| 步骤 | 内容 | 出处 |
|---|---|---|
| 1 | 拍快照 `kgsl_device_snapshot` | `adreno_hwsched.c:2810` |
| 2 | 按 GMU 报的 (ctx, ts) 找出错命令，把它的时间戳强制写进 memstore sop/eop —— **不会重跑** | `:2559`、`2295-2312` |
| 3 | 硬复位：软复位在这份源码里没打开（`gen8_hwsched_soft_reset` 返回 `-EOPNOTSUPP`），所以每次致命故障都是停 HFI、GMU 冷启动、重新注册上下文 | `adreno_gen8_hwsched_hfi.c:4249`，`adreno_hwsched.c:3118-3137` |
| 4 | 重放其它在途命令 | `adreno_hwsched_replay`，`:2345-2370` |

**判责**（`adreno_hwsched.c:2814-2827`）：出错的上下文只有在带 `INVALIDATE_ON_FAULT` / `NO_FAULT_TOLERANCE`、错误是长 IB 超时
（SW_HANG）或熔丝违规、或 2 秒内超过 3 次故障时，才被判有责并作废。没有任何路径设置“无辜”状态。

用户态看到的：

| 情况 | 返回 | 出处 |
|---|---|---|
| 在作废的上下文上提交 / 等待 | `-EDEADLK` | `kgsl_device.h:996-1004`，`adreno.c:3275` |
| 已分离的上下文 | `-ENOENT` | 同上 |
| 故障后存活，第一次提交 | **`-EPROTO`，命令已经入队** | `adreno_hwsched.c:1996-1997` |
| 故障后第一次成功等待 | `-EPROTO` | `adreno.c:3283-3284` |
| `KGSL_PROP_GPU_RESET_STAT` | 返回并清除 reset_status | `kgsl.c:1798-1821` |

```c
/* adreno_hwsched.c:1996 · 提交路径末尾 */
if (test_and_clear_bit(ADRENO_CONTEXT_FAULT, &context->priv))
        return -EPROTO;
/* kgsl.c:2171 */
/* -EPROTO is a "success" error - it just tells the user that the
 * context had previously faulted */
if (result && result != -EPROTO) kgsl_drawobj_destroy(drawobj);
```

所以 **EPROTO 说明上下文挺过了这次故障**，命令也已入队；但按 02 章的拷回规则，用户态拿不到它的 timestamp。
fault53 正是这样：KGSL 给 shadPS4 的稀疏绑定 `AUX_COMMAND` 返回 EPROTO；Turnip 对 AUX_COMMAND 的任何失败都调用
`vk_device_set_lost("bind submit failed")`（`tu_knl_kgsl.cc:1797`），于是 shadPS4 收到 DeviceLost。DeviceLost 是 Turnip
按 Vulkan 语义做的判定，不是内核宣布设备失效。

### 快照

8 MiB 静态区（gen8_2_1 `snapshot_size = SZ_8M`），经 sysfs `snapshot/dump` 读取；**读走之前新的故障不会覆盖它**
（`kgsl_snapshot.c:664`），但 `faultcount` 照样加。内容依次是：头部与固件版本；GMU 的 ITCM/DTCM、日志、HFI 队列、寄存器；
debugbus 与 IB1/2/3 基址（只读 BR 流水）；CP 索引寄存器（`CP_SQE_STAT`、`CP_ROQ_DBG`、DDE FIFO 等）；抢占记录与每上下文 GMU 队列；
ringbuffer、memstore、出错命令的 IB（解析）；冻结的 buffer；事件日志。“冻结”只是不让 buffer 被释放，内容稍后在 work 里拷贝
（**推断**：看到的是 work 运行时的内容，不一定是故障瞬间）。

draw state 载荷可能缺失的原因（都有代码依据）：

1. 只解析出错的那一次提交（`CMDOBJ_FAULT`，`adreno_hwsched.c:3100`）；
2. type-7 包里只解码 `CP_SET_DRAW_STATE`，组只跟随 DIRTY / LOAD_IMMED 的，DISABLE 的跳过（`adreno_cp_parser.c:233`、`262-292`）；
3. 遇到零包头或越界就停止解析该 IB（`:434-451`）；
4. `CP_DRAW_STATE` 索引寄存器只采 BR/BV/LPAC，不采 DDE 流水（`adreno_gen8_2_0_snapshot.h:2139-2178`）；
5. 单个 IB 最多 1000 个对象；导入的 USERMEM、纹理类 buffer 不冻结（`adreno_cp_parser.c:15`、`kgsl_snapshot.c:188-200`）。

fault53 的 IB2 本身采到了（上一集已解码），组指向的载荷没采到 —— 候选是 2、3、5；第 4 条还意味着看不到 DDE 当时在读什么。
**具体是哪一条还没逐一核对。**

### “might be blocked for itself”

同步对象在队首 5 秒仍未满足，`syncobj_timer`（`kgsl_drawobj.c:154-257`）打印：

```
kgsl: possible gpu syncpoint deadlock for context %u priority %u timestamp %u owner %s[%d]
      tid %d gpu_busy %u%% gpu_freq %lu Hz
```

随后 `kgsl_context_dump()` 打出 `Context %u might be blocked for itself`（HEAD `adreno_drawctxt.c:84`）并列出每个未满足的同步点。
所以这句是 5 秒看门狗的副产品，不是独立的“自己等自己”检查。priority、gpu_busy、gpu_freq 三个字段对应快照提交 `a0a588dc` 的说明。
Odin3 血源开局挂死前出现过 “Context 16 might be blocked for itself”。另有厂商的卡死上下文看门狗（`adreno_hwsched.c:41-80`），默认关闭。

## 07 功耗与可观测性

### 下电

- **IFPC**：GX 单独断电，GMU 和 CX 仍在。主机只下发迟滞 `0x1680` 个 19.2 MHz 周期 ≈ 300 µs（`adreno_gen8_gmu.c:1957-1961`），
  进入与退出由 GMU 固件决定。
- **SLUMBER**：KGSL 空闲计时 80 ms（`CONFIG_QCOM_KGSL_IDLE_TIMEOUT=80`）到期且没有活跃引用时，让 GMU 整个下电：
  `PREPARE_SLUMBER`、RSC 睡眠序列撤掉 RPMh 投票（`adreno_gen8_hwsched.c:515-565`）。有新提交再唤醒；触摸输入会把空闲计时推后到 100 ms。

### 调频

Swan 的 gen8_2_1 特性表里 `ADRENO_GMU_BASED_DCVS` 被注释掉（“Using sys DCVS”），所以是 **host 侧调频**（`adreno.c:1635-1636`）：

```
GMU 忙碌计数（19.2 MHz 周期 → µs）→ kgsl_pwrscale（最多每 10 ms 一次）→ msm-adreno-tz 治理器
  → TrustZone qcom_scm_dcvs_update_v2(level, total, busy) → 新档位 → H2F GX_BW_PERF_VOTE → GMU 投票
```

累计忙碌超过 50 ms 直接跳到最高档，否则交给 TrustZone 选（`governor_msm_adreno_tz.c:350-431`），策略本身是黑盒。

Swan（canoe v2）speed bin 0 的频率表（`canoe-v2-gpu-pwrlevels.dtsi`）：

| 档 | MHz | 档 | MHz | 档 | MHz | 档 | MHz |
|---|---|---|---|---|---|---|---|
| L0 | 1050 | L4 | 726 | L8 | 461 | L12 | 282 |
| L1 | 1000 | L5 | 646 | L9 | 422 | L13 | 222 |
| L2 | **902** | L6 | 578 | L10 | 382 | L14 | 191 |
| L3 | 826 | L7 | 500 | L11 | 342 | L15 | 160 |

更高的 bin 到 1200 / 1300 MHz。我们日志里的上限 902 MHz 来自 `max_gpuclk`，它显示的是当前温控 / PM QoS 的限制值
（`kgsl_pwrctrl.c:492-533`），**Swan 实际用哪个 bin 未核对**。Odin3（sun）的 bin 最高 900 或 1150 MHz，同样未核对。

温控与限流：`gpuss-0` 105 °C 被动降频（devfreq cooling）；GMU 侧 `tj_limit` 90 °C，固件节流 50/37/20；CLX / IFF 电流限制；
BCL 电池电流 62/75/83 %；SoC 放电（socd）时限到 L4。

### sysfs

| 节点（`/sys/class/kgsl/kgsl-3d0/`） | 内容 |
|---|---|
| `gpubusy` | `busy_us total_us`：忙碌时间与总时间（微秒），total ≥ 1 s 才更新一次；忙碌率按时间算，与频率无关（`kgsl_pwrctrl.c:617-636`、`1231-1251`） |
| `gpu_busy_percentage` | busy × 100 / total |
| `gpuclk` | 当前档位的 Hz —— 驱动记账，不是硬件回读（`:559-566`） |
| `max_gpuclk` | 当前温控 / QoS 上限 |
| `min_pwrlevel` / `max_pwrlevel` | 档位范围 |
| `devfreq/gpu_load` | 按最高频折算的负载 |
| `ft_pagefault_policy`、`preempt_count`、`preempt_info`、`snapshot/faultcount` | 页故障策略、抢占统计、快照计数 |

shadPS4 的状态层（Foundation `perf_metrics`，`DeviceMetrics.cpp:289-310`）读 `gpubusy`（退而求其次 `gpu_busy_percentage`），
频率依次读 `gpuclk`、`devfreq/cur_freq`、`clock_mhz`。GPU 空闲时读一次 `gpubusy` 会把两数清零，Swan 上 HUD 偶尔读到的 `0 0`
很可能就是这样来的（Foundation 已对此做了短暂保持）。

### tracepoint 与 Litep

| tracepoint | 关键字段 |
|---|---|
| `adreno_cmdbatch_submitted` | ctx · prio · ts · rb_id · ticks |
| `adreno_cmdbatch_retired` | start / retire（GMU 的 sop / eop）· retired_on_gmu |
| `kgsl_pwrlevel` · `kgsl_gpubusy` | 档位变化 · ≥ 1 s 窗口的 busy / elapsed |
| `kgsl_mmu_pagefault` | 地址 · 页表 · 读写 |
| `adreno_preempt_trigger` / `done` | cur · next · “level”（= ctx_switch_cntl） |
| `kgsl_hfi_send` / `receive` | HFI 消息 id · size · seqnum |

`ticks` 来自 GMU 的 always-on 计数器（19.2 MHz，推断）。`adreno_cmdbatch_fault` 只在旧路径发，gen8 不会有；`kgsl_clk` 在有 GMU 时也不发。
Litep 的 KGSL 侧车就是在独立 tracefs 实例里采这些事件，并与 PROF 按 SHA / PID / 时钟绑定。例：Thor 血源诊所（09-20），
GPU 忙 85.7%，主批次提交到退休 p90 80 ms，说明 GPU 已饱和
（[tier-b-litep-bottleneck-20260920](../../validation/android-native-host/tier-b-litep-bottleneck-20260920.md)）。

性能计数器 ioctl 默认关闭：`PERFCOUNTER_READ` 返回 `-EPERM`，需先写 sysfs `perfcounter=1`（会让 GPU 重新上电，
`adreno_ioctl.c:131-147`、`adreno_sysfs.c:506-512`）。

## 08 能力清单与边界

| 能力 | KGSL 里怎么做 | 对 shadPS4 的影响 |
|---|---|---|
| 提交与等待 | GPU_COMMAND → HFI 上下文队列 → GMU；QUEUED / CONSUMED / RETIRED | 零超时等待会阻塞，Turnip 已改读 RETIRED；退休通知绕经 GMU |
| 显存 | 每进程页表 · 三段 VA · shmem 物理页 · 64 位 flags | FORCE_32BIT 在 64 位进程里从未生效；稀疏绑定排在同一上下文的提交之后 |
| 调度与抢占 | 优先级 / 4 → RB0–3；GMU 固件决定抢占 | 合成器在 RB0、游戏在 RB2；抢占与 DDE 故障的关系未定 |
| 故障与恢复 | GMU 报错 → 快照 → 强制退休 → 硬复位 → 重放 | EPROTO：上下文挺过了故障；DeviceLost 是 Turnip 的判定 |
| 功耗与观测 | IFPC · 80 ms 下电 · CPU 侧 TZ 调频 · sysfs · ftrace | HUD 的 gpubusy / gpuclk；Litep KGSL 侧车 |

**源码之外看不到的**：GMU 固件内部（RB 构建、抢占策略、policy 位的含义）；trace 里 level 字段（`ctx_switch_cntl`）的含义；
Odin3 / Thor 的实际内核；Swan 用的频率 bin；设备上 `.ko` 的实际构建配置与是否带 XR 抢占补丁。

## 对上一集的修正

读完 KMD 源码后，《Turnip 架构剖析》有三处说法需要补充：

1. 第 05 章说“KGSL 维护几条 ringbuffer”。gen8 硬件调度下 KGSL 只维护每上下文的队列，RB 由 GMU 固件写入（推断，见本文 03 章）。
2. 第 05 章引用 Swan trace 的 “level = 1”。这个字段在 hwsched 下是 GMU 的 `ctx_switch_cntl`，含义未公开（本文 05 章）。
3. 第 06 章说 EPROTO “表示上下文出过故障”。更准确地说：上下文挺过了故障、没被作废，命令也已入队；DeviceLost 是 Turnip 的判定（本文 06 章）。

## 资料

- 设备与 Turnip 的实测：[swan-turnip-20260920](../../validation/android-native-host/swan-turnip-20260920.md)、
  [frame-tail-wait-2026-09-16](../../validation/android-native-host/frame-tail-wait-2026-09-16.md)、
  [swan-cp-opcode-20260929](../../validation/android-native-host/swan-cp-opcode-20260929.md)、
  [swan-cp-opcode-firmware-20260930](../../validation/android-native-host/swan-cp-opcode-firmware-20260930.md)、
  [odin3-kh3-performance-20261011](../../validation/android-native-host/odin3-kh3-performance-20261011.md)、
  [tier-b-litep-bottleneck-20260920](../../validation/android-native-host/tier-b-litep-bottleneck-20260920.md)
- 未结问题：[bugs/odin3-kh3-cp-dde-opcode-20261011](../../../bugs/odin3-kh3-cp-dde-opcode-20261011/README.md)
- 上一集：[Turnip 架构剖析（文字版）](../turnip-architecture/turnip-architecture.md)
