# SP、wave 与 eviction buffer（文字版）

本文是视频 [adreno-sp-wave.mp4](adreno-sp-wave.mp4)（约 11 分钟）的文字版，接《[Turnip 架构剖析](../turnip-architecture/turnip-architecture.md)》
和《[KGSL 架构剖析](../kgsl-architecture/kgsl-architecture.md)》。前两集讲用户态驱动和内核驱动，这一集讲着色器在
Adreno 840 上的执行：SP、wave、指令缓存，以及按序输出用的 eviction buffer。全片以 2026-10-13 Swan 上 XR 合成器的
一次 GPU 挂死为例，最后给出根因分析和证据边界。字幕原文在 [adreno-sp-wave.srt](adreno-sp-wave.srt)，生成方法见 [README](README.md)。

| 章节 | 视频位置 | 内容 |
|---|---|---|
| [01 SP 在 GPU 里的位置](#01-sp-在-gpu-里的位置) | 0:07 | 流水线上的 SP、slice / SP / uSPTP 三级结构 |
| [02 wave](#02-wave一组线程同步前进) | 1:18 | fiber 与 wave、dual wave dispatch、wave slot、占用率、(sy) |
| [03 指令从哪里来](#03-指令从哪里来) | 2:26 | 指令长度寄存器、128 字节的行、指令缓存的 tag、L0、early preamble |
| [04 乱序完成与 eviction buffer](#04-乱序完成与-eviction-buffer) | 3:40 | 为什么要按序输出、专利描述、EVICT_BUF_MODE、驱动的选择公式、KGSL chicken bit |
| [05 现场](#05-现场xr-合成器挂死) | 5:12 | 挂死时间线、RBBM 状态、在跑的着色器、LayerMerge 组合、两个变体 |
| [06 解码 384 个 wave](#06-解码-384-个-wave) | 6:38 | SP_HWAVE_RAM 的字段、序号、PC 512 与 PC 0、差 136 |
| [07 卡在取指](#07-卡在取指) | 7:54 | PC 512 是行首、指令缓存装不下、缺行与待填充条目 |
| [08 结论与边界](#08-结论与边界) | 9:21 | 三个条件、配置为什么没生效、规避方法、证实 / 推断 / 未证实 |

## 数据、源码与约定

- **现场**：Swan（Pico，Adreno 840 v2，chip `0x44050a31`，KGSL 型号 gen8_2_1），进程 `com.pico.xr.openxr_runtime`
  PID 5438，合成器线程 5541，GLES 上下文 13。10-13 16:56:45.880 起反复 GPU hang，留下 20 份 KGSL 快照
  `devcd4`–`devcd23`。
- **快照解码**：原始快照放在 `C:\Users\Admin\Downloads\gpu_snapshot\kgsl-*-devcd*.bin`；gpu-snapshot MCP 生成的
  `devcd*.sqlite` 在 bug_reports 仓库 `analysis/shader_eviction_hang_20261013/snapshot/`。本目录的
  [`analysis/sp_snapshot.py`](analysis/sp_snapshot.py) 只用 sqlite 的 `shader_states` 表定位各 SP 的状态块
  （state 23 指令数据、52 指令 tag、57 wave、58 L0），字段含义都是从数据本身推断的，没有厂商文档。
- **Mesa**：本仓 `references/mesa-turnip`，行号按父仓引用 `65b63f66bf5`。
- **KGSL**：Swan ROM 的 `graphics-kernel`（`workbench-hub:/data00/swan_source/vendor/qcom/opensource/graphics-kernel`），
  提交 `a0a588dca`。
- **高通用户态驱动**：EVICT_BUF_MODE 的选择逻辑来自 bug_reports
  `qualcomm_gpu_driver/docs/umd_minidebug/settings/README.md` 第 9 节（0855.0.9 的静态还原）。仓库已提交的版本只到第 8 节，
  本文依据的是 2026-10-13 的本地版本。
- **设备日志**：`aplog.029`（klog，挂死时间与 Fault id）、`aplog.034`（SELinux avc）。
- 标注：**【证实】** 直接来自源码、日志或快照读数；**【推断】** 有数据支持但字段含义没有文档；**【未证实】** 目前没有证据。

---

## 01 SP 在 GPU 里的位置

```
CP ──► PC · VFD ──► HLSQ ──wave──► SP ──► RB ──► GMEM / 系统内存
解析命令流   图元 · 顶点    打包成 wave   │ ▲      按图元提交顺序混合
                                       ▼ │
                               VPC · TP · UCHE ──► 显存
```

- SP（shader processor）执行顶点、像素、计算着色器的指令。HLSQ 把工作打包成 wave 派发给 SP；SP 采样纹理经过 TP，
  读写显存经过二级缓存 UCHE；顶点阶段的输出存进 VPC，交给像素阶段；像素着色器的输出交给 RB 混合。
- **Adreno 840 有 3 个 slice，每个 slice 2 个 SP，每个 SP 2 个 uSPTP，共 12 个执行单元。【证实】**
  - KGSL 快照把着色器状态按 `(slice_id, sp_index, usptp)` 分组导出，正好 12 组。
  - Mesa 的 A840 条目 `num_slices = 3`（`freedreno_devices.py:1553-1572`）。
- wave slot 在 uSPTP 上，每个 uSPTP 32 个。指令缓存的 tag 表在同一个 SP 的两个 uSPTP 里逐项相同，所以按每个 SP 一份来看。

## 02 wave：一组线程同步前进

- 一个像素或顶点是一个 fiber；一个 wave 是 64 个 fiber，共用一个 PC。
  - Mesa `a8xx_base`：`supports_double_threadsize = False`、`has_dual_wave_dispatch = True`（`freedreno_devices.py:1403-1404`），
    即 A8xx 的 wave 固定 64 线程，两个 wave 成对派发。
  - A840 条目 `wave_granularity = 2`、`fibers_per_sp = 128 * 2 * 16` = 4096（`:1570-1571`）。4096 / 64 = 64，
    与快照里每个 SP 64 个 wave slot 吻合。
- **wave slot**：SP_HWAVE_RAM 每个 uSPTP 32 个 slot，一个 SP 64 个，整颗 GPU 384 个。wave 从派发到退休一直占着 slot。【证实】
- **占用率**：`ir3_get_reg_dependent_max_waves()`（`ir3.c:317-324`）

  ```c
  waves = reg_size_vec4 / reg_count * wave_granularity;   // A840: 128 / r * 2
  ```

  `reg_size_vec4 = 128` 来自 `a8xx_gen2`（`freedreno_devices.py:1466`）。结果再与 `max_waves` 取较小值
  （`ir3.c:624-628`；A6xx 起默认 16，`freedreno_dev_info.py:159`）。Mesa 没有说明这个数按什么单位计，它和快照里
  “每个 uSPTP 32 个 slot” 之间的换算没有文档。

  | 每个 wave 的寄存器 r（vec4） | 8 | 16 | 32 | 64 |
  |---|---|---|---|---|
  | 128 / r × 2 | 32 | 16 | 8 | 4 |
  | Mesa 的 max_waves（上限 16） | 16 | 16 | 8 | 4 |

- **等待**：采样指令发出后 wave 继续往下执行，遇到带 `(sy)` 的指令才等全部采样返回。这次的像素着色器里：

  ```
  500  gather4g.a (f16)(xyzw)hr2.z, r40.x, s#3, t#3
  504  gather4g.a (f16)(zw)hr3.x, r0.x, s#3, t#3
  507  gather4g.a (f16)(xy)hr0.x, r0.x, s#3, t#3
  508  (ss)(nop3) add.f hr7.z, hr1.w, hr2.x
  509  (sy)(nop3) add.f hr7.z, hr2.z, (last)hr7.z
  510  (nop3) add.f hr7.z, hr3.y, (last)hr7.z
  511  (rpt3)mad.f16 hr1.z, hc42.y, (neg)hr7.z, (r)(last)hr1.z
  512  (rpt3)mad.f16 hr2.z, hc42.y, (neg)hr7.z, (r)(last)hr2.z
  ```

  所以一个 wave 停在某个 PC 上，可能在计算，也可能在等纹理、等取指，或者已经结束、在等退休。

## 03 指令从哪里来

| 寄存器 | 地址 | 现场读数 | 含义 |
|---|---|---|---|
| `SP_PS_BASE` | `0xa983` | `0x40_6920a000`（上下文 0、6、7）· `0x40_69211000`（上下文 1–5） | PS 机器码起点 |
| `SP_PS_INSTR_SIZE` | `0xab05` | `0x3d` = 61 | PS 长度，单位：行 |
| `SP_VS_INSTR_SIZE` | `0xa824` | `0x15` = 21 | VS 长度，单位：行 |

寄存器定义见 `a6xx.xml:3821`、`:3985`、`:4313`。读数来自快照各硬件上下文的寄存器。

- 每条指令 8 字节，16 条构成一个 128 字节的行。这次的 PS 有 976 条指令，正好 61 行。
- **SP 指令缓存**（每个 SP 一份）【证实 / 推断】：
  - `SP_INST_TAG`（state 52）256 项，实际用到前 128 项。每项 = `flags << 24 | 行号`。
  - `SP_INST_DATA_RAM`（state 23，4 × 8 KiB）第 i 项对应 128 字节数据。后 128 行是看似随机的内容，推断是未用的 SRAM。
  - 观察到的 flags：A 变体 `0x18/0x58`、B 变体 `0x08/0x48`、VS `0x40/0x50`、其他程序 `0x20`、待填充 `0x1a`。
    这个对应关系是用数据内容比对出来的，flag 各位的含义是推断。
  - 每个 uSPTP 还有一个 512 字节的 L0 指令缓冲（state 58）。
- wave 走到一行的开头，如果这一行不在缓存里，就要分配一个缓存行、向显存取 128 字节，数据回来填入后才能继续。
- **early preamble**：A8xx 的着色器开头有一段只交给一个 wave 执行的代码，用来计算公共常量。

  ```
    0  shps #l55
    1  getone #l53
    2  sam.a (f32)(xyzw)r63.x, r48.x, s#3, t#3
   …   计算常量，stc 写进常量区
   54  (sy)(ss)shpe
   55  (rpt1)bary.f r2.z, (r)10, r0.x
   …
  972  bary.f (ei)r63.x, 0, r0.x
  973  end
  ```

  其他 wave 从第 55 条开始。这个 PS 的 `shpe` 在第 54 条，`end` 在第 973 条。

## 04 乱序完成与 eviction buffer

- 同一个像素上的多个图元必须按提交顺序混合，所以像素着色器的输出要保持派发顺序。但 wave 的耗时不同，后派发的可能先算完。
- 最简单的做法是让先完成的 wave 原地等：结果留在寄存器里，slot 也不释放。
- 高通专利 [WO2022235402A1](https://patents.google.com/patent/WO2022235402A1/en)（*Wave slot retirement procedures*，
  2022-11-10）描述了另一种做法：已完成 wave 的结果被搬进 eviction buffer，slot 立即交给新 wave；eviction buffer 用空闲的
  GPR 空间，不够时再从 wave slot 的 GPR 里划出；序列器记录各结果的位置，按原顺序输出。
- **`SP_PS_CNTL_1`（`0xa9ae`，A7XX 起）**，`a6xx.xml:4658-4664`：

  | 位 | 字段 |
  |---|---|
  | 20:13 | `NUM_TOTAL_VAR` |
  | 12:11 | `WAVE_PAIR_MODE` |
  | **10:9** | **`EVICT_BUF_MODE`** |
  | 8 | `DEFER_WAVE_ALLOC_DIS` |
  | 7:0 | `SYSVAL_REGS_COUNT` |

  Turnip 对每个 PS 固定写 `.defer_wave_alloc_dis = true, .evict_buf_mode = 1`（`tu_shader.cc:2643-2644`）。
- **高通驱动的选择**（0855.0.9，GLES 与 Vulkan 相同；见上文 bug_reports 文档 §9.3–9.8）：

  ```
  N = ceil(shader_bytes / 128)          // 即 SP_PS_INSTR_SIZE
  M = hwInfo.SHPEOffset                 // shpe 的 PC
  L = 16 × N − (M ? M − 1 : 0)
  G = shaderBehavior.hasLargeDivergentBB
  T = 配置键 0xd2979a10                  // 阈值
  mode = (!G || L < T) ? 3 : 0
  ```

  - 显式键 `0x76a7f278` 取十进制 0/1/2/3；设为 3 时跳过自动选择。设为 0 并不保证最终为 0，非零阈值仍可能把它改成 3。
  - 0 和 3 都只是这个 2 位字段的数值，**硬件含义没有公开定义**，不能叫作“开 / 关”。
  - L = 923（本例）时：阈值 256、512 选 0；1024 选 3。

- **KGSL 的 SP chicken bit**（`adreno-gpulist.h`）【证实】：

  | 寄存器表 | `SP_CHICKEN_BITS_1` | `SP_CHICKEN_BITS_2` |
  |---|---|---|
  | gen8_0_0（`:3015-3032`）、gen8_4_0、gen8_6_0 | `BIT(26)`：Limit the number of wave-slots for Eviction buffer to 1 per ALU GRP；`BIT(30)`：Disable LPAC auto-promotion | `BIT(22)`：Disable PS out of order retire；`BIT(23)` |
  | gen8_2_0（`:3440-3452`，gen8_2_1 即 Swan 也用这张表） | `BIT(3)` | `0xc20800`（含 `BIT(22)`），注释 “Disable PS out of order retire” |
  | gen8_9_0（`:4013-4015`） | `BIT(3)` | `0xc21800` |

  Swan 快照实读：`SP_CHICKEN_BITS`（`0xae03`）= `0x04000000`、`SP_CHICKEN_BITS_1`（`0xae08`）= `0x8`、
  `SP_CHICKEN_BITS_2`（`0xae09`）= `0xc20800`，与表一致。也就是说，Swan 关掉了 PS 乱序退休，但没有设 gen8_0 那个
  “eviction buffer 每个 ALU 组只用 1 个 wave slot” 的限制位。

## 05 现场：XR 合成器挂死

- **时间线【证实】**：

  ```
  16:56:45.880928 [gmu_f2h] adreno-gen8-gmu 3d37000.qcom,gmu:MISC: GPU hang detected
  16:56:45.880946 [kgsl_hwsched] kgsl kgsl-3d0:Fault id:2 and GX is ON with Curr GPU freq:902000000
  16:56:47.266 / 48.355 / 48.895 / 49.987 / 50.956 / 51.464 / 52.564 / 53.464 / 54.467 / 54.996 / 55.528 …
  ```

  GMU 判定挂死、KGSL 复位后约 0.5–1.4 秒再次挂死，一直持续到 16:57 之后；KGSL 留下 20 份快照 devcd4–devcd23。
- 快照里没有页故障，命令流已经取完；`RBBM_STATUS = 0x00FE1707`，HLSQ、UCHE、VPC、PC 都忙。
- 384 个 wave slot 全部被占用（devcd4：VS end 26、PS end 355、PC 512 两个、PC 0 一个）。
- **在跑的像素着色器**：按 tag 从 SP 指令数据里拼出完整程序，976 条、61 行、4 张纹理：
  - t3 是数组纹理：`getsize` 一次、`gather4g` 四次（第 481、500、504、507 条），是 SGSR 超分的边缘检测；
  - t0–t2 上是 DFS 的 `conv.3d` 卷积采样，每张纹理 4 条，共 12 条；
  - 没有回跳分支，不存在循环。
- **合成器日志**：挂死前 1.6 秒（16:56:44.306）请求了两个 4 层 LayerMerge 组合：

  ```
  mask 37b87 | 4_0000_1005 | 1004 | 0a20_1000 | 4_0000_1900
  mask 37bbb |   0000_1005 | 1004 | 0a20_1000 | 4_0000_1900
  ```

  | 层 | 掩码 | 类型 | 特性（XRRuntime `pxr_shader_enums.h`） |
  |---|---|---|---|
  | 1 | `0x(4)_0000_1005` | quad | 2D；第一个组合多一个 bit 34 `fScaleRgbByTransitionAlpha` |
  | 2 | `0x1004` | cylinder | 2D |
  | 3 | `0x0a20_1000` | projection | Projected · SpaceConvert · LASIK · DFS |
  | 4 | `0x4_0000_1900` | projection | Projected · ArrayTexture · SuperResolution |

  两个组合只差 quad 层的透明度缩放和混合掩码。编译出的两个 PS 结构相同、寄存器分配不同（例如第 137 / 141 条的
  `conv.3d` 源寄存器 `r5.y` / `r2.y`，第 440 条 `br p0.x` 的目标 `#l928` / `#l929`），下文称 A、B。
  两份程序的 `SP_PS_BASE` 在 8 个硬件上下文里交替出现（见第 03 节的表）。
- **代入驱动公式**：N = 61，M = 54，L = 16 × 61 − 53 = **923**；快照读到 `SP_PS_CNTL_1 = 0x00000000`，即 mode 0。
  若这个着色器带 `hasLargeDivergentBB`，阈值 ≤ 923 选 0、≥ 924 选 3。

## 06 解码 384 个 wave

- **SP_HWAVE_RAM**（state 57）：每个 uSPTP 32 个 slot × 16 dword。【推断】
  - dword 0：`[31:8]` = PC（以指令计），`[7:0]` = 序号。
  - dword 2：状态字，按 PC 归类：`0x0004010b` PS end（PC 973）、`0x0004000a` VS end（PC 329）、`0x00040133` PC 512、
    `0x00000133` PC 0。各位的含义没有公开。
- **序号**：按 256 回绕，在同一个 SP 的约 60 个像素 wave 里几乎不重复。20 份快照 × 6 个 SP = 120 组，其中 5 组各有一对
  相同的序号（devcd6、19、20 的 SP 1.1，devcd11 的 SP 2.1，devcd14 的 SP 0.1）。推断它是 wave 的分配顺序。
  另外 devcd17、devcd23 各有一个 slot 是乱码（例如 `0xfbfb4f4f`），按快照读取错误处理。
- **每份快照恰好一个 SP 不一样【证实】**：1–3 个像素 wave 停在 PC 512、1 个停在 PC 0。20 份快照中是 SP 1.1 11 次、
  SP 2.1 4 次、SP 0.1 4 次、SP 1.0 1 次。
- **devcd4 的 SP 2.1**：PC 512 的 wave 序号 `0x0a`、`0x2b`（uSPTP 0 的 slot 20、7），PC 0 的 wave 序号 `0x92`
  （uSPTP 0 slot 1）。其余 57 个 PS end 的 wave 中，52 个序号在 `0x3c`–`0x91`，5 个在 `0xac`–`0xc6`。
- **差 136【证实】**：PC 0 的序号 = 某个 PC 512 的序号 + 136（mod 256），20/20：

  | 快照 | PC 512 | PC 0 | 快照 | PC 512 | PC 0 |
  |---|---|---|---|---|---|
  | devcd4 | `0x0a` | `0x92` | devcd14 | `0x62` | `0xea` |
  | devcd5 | `0x4e` | `0xd6` | devcd15 | `0x6f` | `0xf7` |
  | devcd6 | `0x0a` | `0x92` | devcd16 | `0x84` | `0x0c` |
  | devcd7 | `0x69` | `0xf1` | devcd17 | `0xe8` | `0x70` |
  | devcd8 | `0xac` | `0x34` | devcd18 | `0xeb` | `0x73` |
  | devcd9 | `0x69` | `0xf1` | devcd19 | `0xe8` | `0x70` |
  | devcd10 | `0x82` | `0x0a` | devcd20 | `0x2b` | `0xb3` |
  | devcd11 | `0x4d` | `0xd5` | devcd21 | `0x5b` | `0xe3` |
  | devcd12 | `0x4d` | `0xd5` | devcd22 | `0x48` | `0xd0` |
  | devcd13 | `0x68` | `0xf0` | devcd23 | `0x1e` | `0xa6` |

  按序号排，这个 PC 512 的 wave 在 20 份快照里都是本 SP 最老的 4 个之一（名次 0–3，7 份是最老的）。
- **解读【推断】**：每个 SP 有一个宽 136 的按序窗口。最老的 wave 不退休，窗口里其余 wave 算完也只能停在 end 等退休；
  序号排到 +136 的 wave 拿到了 slot，却一条指令都没执行。136 ≈ 60 个驻留 slot + 约 76 个结果暂存在别处的已完成 wave，
  与专利描述的 eviction buffer 相符，但没有直接证据。
- 另一种可能【未证实】：Mesa `f7ca1f551510`（2026-09-30，上游 MR 44840）把 A750 的 `SP_CHICKEN_BITS_2` bit 30
  （`INDEPENDENT_ICACHE_MISS`）描述为“置位时只有缺失的 wave 等，清零时一次缺失让所有 wave 一起停”。Swan 该位为 0。
  如果 A840 上这一位含义相同，PC 0 的 wave 不执行也可能是整个 SP 在等同一次取指。
- 其他 SP 的 wave 也全部停在 end，推断它们在等同一个输出顺序点。

## 07 卡在取指

- PC 512 = 16 × 32，是第 32 行的第一条；PC 0 也是行首。第 509 条带 `(sy)`，wave 已越过它，四次 gather 的结果都已返回；
  第 512 条是普通乘加，没有同步标志。
- **指令缓存装不下【证实】**：A 61 行 + B 61 行 + VS 21 行 = 143 行，指令缓存只有 128 行。两个变体同时在跑，互相把对方的行
  挤出去。devcd4：

  | SP | A 缺的行 | B 缺的行 | 待填充条目 |
  |---|---|---|---|
  | 2.1（卡住） | 0–5、35–54 | **32–40** | 第 93、94、95 项：tag `0x1a000022/21/20`（第 34、33、32 行） |
  | 0.0（健康） | 无 | 0–14 | 无 |

- **待填充条目【证实 / 推断】**：卡住的 SP 里有 3 项 tag 已经是第 32、33、34 行，flag `0x1a`（比正常条目多 bit 1），
  数据却是别的着色器的旧代码（反汇编是 `saml … s#5, t#4`，不属于 A、B、VS 的任何一行）。推断这是一次已分配缓存行、
  已写 tag、但数据始终没有返回的取指。20 份快照里卡住的 SP 都有：B 缺 32–40 行（devcd11、22 缺得更多）、
  第 32–34 行的待填充条目。
- **健康的 SP 里 B 的第 32 行都在。** 另有 3 份快照里，没有卡住 wave 的 SP 也挂着同样的 32–34 待填充条目；那里的 wave
  走了跳过 SGSR 边缘检测的分支，直接到 end，不需要第 32 行。这说明这类填充普遍回不来，不是单个 SP 的偶发错误。
- **最合理的解释【推断】**：最老的 wave 跑的是 B 变体，走到 PC 512 需要第 32 行，而这次取指一直没有返回。它不退休，
  后面算完的 wave 只能停在 end；窗口满（+136）之后新 wave 不执行；384 个 slot 全被占满，GPU 停住，GMU 判定挂死。
  复位后约 1 秒走到同一处，于是留下 20 份几乎一样的快照。

## 08 结论与边界

- 这次挂死不是着色器死循环（没有回跳分支，wave 都停在 end），也不是页故障（快照无页故障、命令流已取完），
  而是一次取指不返回，加上按序退休，堵住了整颗 GPU。
- **三个条件同时成立**：
  1. 两个 61 行的变体加 VS 超出 128 行指令缓存；
  2. `EVICT_BUF_MODE = 0`，且 KGSL 关了 PS 乱序退休，按序窗口很深（窗口的存在是推断）；
  3. 最老的 wave 正好缺行，且这次取指没有返回。
- **阈值配置为什么没起作用【证实】**：

  ```
  16:53:23.071 5438 auditd: avc: denied { read } for comm="binder:5438_4" name="esx_config.txt"
      scontext=u:r:system_app:s0 tcontext=u:object_r:vendor_data_file:s0 tclass=file permissive=0
  ```

  合成器进程读配置文件被 SELinux 拒绝，阈值键没读到，用的是默认阈值。日志里阈值 2048 的回显只出现在
  16:43:00–16:52:38，4096 在 17:02:43–17:11:48，都不是 PID 5438。其他进程读到了配置，不能说明合成器进程也读到了。
  阈值 ≥ 924 时这个着色器改选 mode 3；配置真正生效的那几次开机（2048、4096）没有挂死；此前的试验中 1024 不挂、
  512 和 256 挂。
- **规避（未实测）**：
  1. 用显式键 `0x76a7f278 = 3`（十进制）固定 mode 3，不依赖阈值，也不依赖配置文件能否读到；
  2. 把两个变体合成一个程序（第 1 层的透明度缩放和混合改成 uniform），PS 61 + VS 21 = 82 行，装得进 128 行。
- **证据边界**：

  | 已证实 | 推断 | 未证实 |
  |---|---|---|
  | wave 停在哪里（PS end 973、VS end 329、PC 512、PC 0） | 序号是 wave 的分配顺序 | 取指为什么不返回 |
  | PC 0 = 某个 PC 512 + 136，20/20 | tag 的 bit 1 表示填充未完成 | EVICT_BUF_MODE 0 / 3 的硬件含义 |
  | 卡住的 SP 里 B 缺 32–40 行 | 每个 SP 有宽 136 的按序窗口 | A840 上 `SP_CHICKEN_BITS_2` bit 30 的含义 |
  | 第 32–34 行的条目 flag `0x1a`、数据对不上 | 卡住的 wave 跑的是 B 变体 | |
  | `SP_PS_CNTL_1 = 0` | 其他 SP 在等同一个输出顺序点 | |
  | 合成器进程读 `esx_config.txt` 被 SELinux 拒绝 | | |

  下一步：请高通确认 gen8_2 是否需要与 gen8_0 `BIT(26)` 等价的限制、是否有指令填充在按序退休下回不来的 erratum；
  或者用同一个着色器分别固定 mode 0 / 3 做对照。

## 制作中更正的说法

- 序号并非在每个 SP 内严格唯一：120 组里有 5 组各有一对相同的序号。
- 每个 uSPTP 的 VS wave 是 2–4 个（devcd4 的 uSPTP 2.0.1 有 4 个），不总是 2 个。
- KGSL gen8_9_0 的 `SP_CHICKEN_BITS_2` 是 `0xc21800`，不是 `0xc20800`。
- Mesa 的 `max_waves` 上限是 16，单位没有文档；不能直接换算成“每个 uSPTP 32 个 slot”。
