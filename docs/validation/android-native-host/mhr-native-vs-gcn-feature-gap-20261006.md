# MHR（RE Engine）原生 D3D12 帧与 PS4 GCN 帧对照：特性清单、两条翻译路径的差异与待迭代项（2026-10-06）

目的（用户 10-06 要求）：不再逐个修现象，而是借 xrgame 在同一台 AYN Thor 上运行 PC 版 MHR 的原生帧，反推 RE Engine 实际用到哪些 GPU 功能，对照 PS4 版同一画面的 GCN 命令与着色器，找出 shadPS4（GCN → SPIR-V → Turnip）相对 vkd3d-proton（DXIL → SPIR-V → Turnip）在架构和特性上的差异，以及可能没有完整实现、需要迭代的部分。

本文只做分析与代码核对，不含修复。标记：**[核对]** 读过对应代码；**[实测]** 有 trace 或画面证据；**[推断]** 尚未用测试或画面证实。源码路径相对 `src/`。

## 0. 结论摘要

两帧的渲染流程一一对应：PC 132 个着色器（67 PS / 49 VS / 16 CS），PS4 147 个（73 FS / 58 VS / 16 CS），16 个计算着色器按派发尺寸和资源一一对上。差异不在 RE Engine 用了什么算法，而在同一算法在 GCN 上被编译成了什么：

- PC 版把意图写在 API 和 DXIL 里：ExecuteIndirect、Clear*View、`[earlydepthstencil]`、Wave/Quad 内建、`SampleGrad`、typed view、bindless 堆。vkd3d 只需逐项映射。
- PS4 版把同样的意图落成硬件机制：PM4 多重间接绘制、填充核、HTILE 写入与解压 draw、EXEC 与 `ds_swizzle`、手算梯度后 `image_sample_cd`、按 R32F 读写深度内存、描述符表。
- shadPS4 必须把机制反推回意图，缺口集中在这些反推上。

按影响排序的确定缺口（详见第 4、5 节）：

1. **`image_sample_cd*`（粗导数采样）翻译错误** [核对]+[实测]：解码器只设 `CoarseDerivative`，翻译器只认 `Derivative`（`frontend/decode.cpp:1162-1186`、`frontend/translate/vector_memory.cpp:629`），梯度寄存器被当作坐标读。MHR 的 8 个特效 PS（PC 名 `VfxPixelShader`）用这条指令，覆盖本帧粒子 pass 的全部 12 个 draw（#689–#700，PC 同一 pass 也是 12 个 draw），香炉烟柱就在这一 pass；PS4 版烟柱是饱和绿色光柱，PC 原生为淡白色烟。该缺陷来自上游最初导入的重编译器（#142），上游 shadPS4 同样存在。
2. **强制 early-Z 未实现** [核对]：`DB_SHADER_CONTROL` 的 `Z_ORDER`/`DEPTH_BEFORE_SHADER` 只有声明，没有任何位置读取，也没有发出 SPIR-V `EarlyFragmentTests`。RE Engine 的 `PS_MiniClusterOcclusionTest`（PC DXIL 标志带 ForceEarlyDepthStencil）在早期深度测试下把可见簇写入间接绘制参数；shadPS4 中被遮挡的包围盒也写“可见”，遮挡剔除失效。影响是多画、损失性能 [推断]。
3. **`v_cvt_pkrtz_f16_f32` 丢失向零舍入** [核对]：翻译为 `PackHalf2x16`，并与导出时的 `UnpackHalf2x16` 一起被常量传播消掉，压缩导出最终以 fp32 写入，由附件做 fp32→fp16 转换。≥65520 的值在 RTE 下变成 +Inf，PS4 为 65504。同一消除也去掉了 `PackUnorm/Snorm` 的 [0,1] 钳位。
4. **HTILE 只模拟“已登记深度目标的整体清除”** [核对]+[实测]：RE Engine 用填充核写 HTILE 做快速清除、再发 `DB_RENDER_CONTROL=0x60` 的解压 draw。只有地址已在深度目标创建时登记（`texture_cache.cpp:1643`）才会被识别成清除，且一律视为全部切片清除，到下次绑定为深度目标时才以 load-op 生效。本帧 #723/#724 用这套机制批量初始化随后被 bloom 复用的瞬态内存（`0x2019000000`），shadPS4 中两者都不产生写入。
5. **bindless 只有有界兼容路径** [核对]：PC 前向 PS、光照参数 CS、IBL CS 用 `bindlessTexture2D/Cube`。shadPS4 的动态图像表在 CPU 上快照描述符堆，单表超过 32 个不同 T# 或 2 MiB 即抛异常（`ir/passes/resource_patching_pass.cpp:58-118`）。每像素查表的开销此前已测到（材质 FS 约 600 次 flat buffer 读取）。
6. **内部缩放与 RE Engine 的配合** [核对]+[实测]：
   - 主 pass 实际在原生 1080p 渲染，0.5 对主场景基本不省工作。已确认根因（4.9.1）：RE Engine 在帧末用 CAS 计算着色器把锐化结果以存储图像写回 HDR 场景颜色所在的同一块内存。shadPS4 规定存储写入一律原生，且按内存身份单向固定，于是场景颜色在整个会话中都是原生；再经 mixed attachment pass 规则带动主深度、RGBA16F、R8、第二个深度全部提升。
   - 阴影深度数组反而被缩到 384×384，阴影精度下降 [推断]。
   - 帧中途提升会让紧随其后的 EQUAL 深度测试错一帧（10-05 已证实）。
7. **helper lane 与 EXEC 的模型和 GCN 不一致** [核对]+[推断]：
   - GCN 片元着色器不进 WQM 时，初始 EXEC 只含有效像素；shadPS4 对所有调用（含 helper）都把 EXEC 初始化为真（`frontend/translate/translate.cpp:89`），`s_wqm_b64` 是空操作（`frontend/translate/scalar_alu.cpp:104-105`）。
   - Turnip 的 ir3 会让 helper 参与 ballot/广播（`references/mesa-turnip/src/freedreno/ir3/ir3_legalize.c:1660-1673`：“Subgroup operations … will use helper invocations if they are present”）。
   - 遮挡测试 PS 用 `readfirstlane` + ballot + `mbcnt` 选出“每个簇唯一的写入 lane”，若选中的是 helper，这次写入被丢弃，簇可能被误判为不可见、整簇不画 [推断，需 GPU probe 证实]。
   - `v_readfirstlane` 被当作不受 EXEC 影响（`frontend/control_flow_graph.cpp:59-66`），可能被放到 EXEC 分支块之外，取到的“第一个 lane”不是 GCN 意义上的第一个有效 lane。
8. 前向 pass 同时依赖 wave64 OR 归约（已识别为 clustered 归约）、`isfinite` 守卫（45 处 `v_cmp_class 0x1F8`，经 SGPR 传入，走 IsFinite，正确）、三顶点原始插值（140 个属性）、阴影比较采样与 FS `image_load`，是已知亮斑/黑面的所在 pass。三顶点插值在 Turnip 上由 fork 补丁模拟：每个 32 位值拆成两个 16 位、以浮点变量传递、在三角形顶点处插值后取整还原。结果逐位精确，但每个属性要多占两个变量槽、读三个顶点要插值六次，槽位（VAR0–VAR31）用尽时管线创建失败（`references/mesa-turnip/src/freedreno/vulkan/tu_shader.cc:3888-4012`）。
9. 其余中低优先级项：
   - 立方体按 2D 数组采样，没有跨面过滤。
   - 可编程采样位置与 EQAA 未建模，按 `SPI_PS_INPUT_ADDR` 而非 `PS_ITER_SAMPLES` 开启逐样本着色。
   - 未开 `drawIndirectFirstInstance`。
   - min/max 归约过滤模式被忽略。
   - 正反面多边形偏移合一。
   - 动态顶点输入下越界顶点不置零。
   - `DMA_DATA` 的 CPU 路径不 `SettleWaits`；label 写入绕过页跟踪。

已观察的画面差异：
- 烟柱颜色（第 1 条，可信度高）。
- 金属/皮肤亮斑与黑面：10-02 已定位到法线为 0 → `rsq(0)` → NaN → isfinite 守卫，0 值来源未定。
- 抓帧后设备上静态场景几何消失：桌面回放同一 trace 画面完整，说明 guest 数据一致、问题在设备侧缓存状态，未归因（4.8）。

## 1. 证据来源与方法

| | 原生 PC（参考） | PS4（shadPS4） |
|---|---|---|
| 设备与软件 | AYN Thor；xrgame：Wine proton-11.0-2-arm64ec + FEX 2608-3f1f30a + **vkd3d-proton 11.0-212991f** + **Turnip d15b7c0（Mesa 26.2.99）**；游戏 16.0.2.0 | AYN Thor；shadPS4 Android，Turnip 351a4847（Mesa 26.3.0-devel，带 barycentric 补丁） |
| 采集 | GFXReconstruct 在 D3D12 API 层录 3 帧（`capture_trim_trigger_20261005T174419.gfxr`）。桌面 `gfxrecon-replay` 用 AMD D3D12 驱动回放，RenderDoc 抓第 2 帧：`D:\workspace\xrgame-native-evidence\api-replay\run-20261005\rdoc\mhr_save_select_frame2.rdc`（仓库外） | DebugBus `gpu_command_trace`（10-05 18:07，设备），`a10bbbc5…gpu.pm4.trace` + `.gcmdtrace.ps4`；另有 GPU 回放 trace `mhr_ss4/ss5`（桌面回放） |
| 场景 | 选存档界面，1280×720 | 选存档界面，1920×1080，内部分辨率 0.5 |
| 规模 | 1076 个事件：540 draw（全部经 ExecuteIndirect）、99 dispatch、19 clear、11 copy；132 个着色器，DXIL 保留入口函数名 | flip 9342 之后一帧：634 DrawIndexIndirect（含 CountMulti）、35 DrawIndirect、111 dispatch；147 个着色器 |

注意事项：
- RDC 里的着色器、资源和状态是游戏在设备上发出的 D3D12 调用，但像素来自桌面 AMD 驱动回放，不是 Turnip。原生在设备上的实际画面见 `screens/device-at-trigger.png`。
- 两边存档内容不同（角色不同），镜头相同，分辨率不同，所以只做结构与功能对照，不做逐像素比较。
- 代码核对由四个并行审查完成（间接绘制/DMA/同步、渲染后端状态、跨 lane/浮点、纹理/采样），关键结论都已复查原代码。

脚本与精简清单在 [evidence/mhr-feature-gap-20261006](evidence/mhr-feature-gap-20261006/)：
- `native_inventory.py`：qrenderdoc `--python`，逐事件管线状态、反射与 DXIL 反汇编；路径写死在脚本头。
- `native_shaders.json`、`native_timeline.txt`。
- `ps4_inv.py`：PM4 包与寄存器统计、着色器转储，并用 `tools/gcn-disasm` 反汇编。
- `ps4_state.py`：逐 draw 的特殊渲染状态。
- `htile.py`、`cs_list.py`、`feat_by_pass.py`、`fills.py`（每次填充是否被替换）、`vs_by_pass.py`、`mdi.py`、`ps4_inventory.json`。
- `replay_scale_log_mhr_ss4.txt`：桌面回放 `mhr_ss4`（不预置原生决定）中缩放提升、填充与拷贝诊断的日志摘录（4.9.1）。

## 2. 两条路径的架构差异

| 方面 | DX12 → vkd3d-proton → Turnip | GCN → shadPS4 → Turnip | 对 shadPS4 的含义 |
|---|---|---|---|
| 输入层次 | DXIL：带类型的资源、结构化控制流、Wave/Quad/导数内建有定义的语义 | GCN 机器码：EXEC 掩码、标量化、手写跨 lane、手算梯度、打包导出、fetch shader、SRT 表 | 必须从指令序列识别语义（归约、waterfall、去重、导数），识别失败时退回逐 lane 模拟，在 SPIR-V 中常是未定义行为 |
| 资源绑定 | 根签名 + 描述符堆，bindless 映射到描述符缓冲/索引 | V#/T#/S# 在内存里由 SGPR 指针取，动态索引即按 lane 从表里取描述符 | 静态 SRT 展平 + 有界动态图像表（32 项）；缩放表只有 30 个绑定位，超出即钉为原生 |
| wave 与 subgroup | 着色器按 `WaveGetLaneCount` 自适应 | 写死 wave64（`readlane 31/63`、64 位 EXEC/VCC） | Turnip FS/CS 默认 128（`references/mesa-turnip/src/freedreno/vulkan/tu_shader.cc:4194-4222`），只有 FS/CS 能指定 64（`tu_device.cc:1140-1143`），VS 恒为 64。shadPS4 对 FS/CS 都请求 64，正确性没问题，但放弃了 Adreno 片元默认的 128 宽 wave [推断：有性能代价] |
| 导数与 helper | D3D 规定 helper 参与导数与 quad 操作；PC 前向 PS 不用导数指令，用 Quad 读取手算后 `SampleGrad` | WQM（`s_wqm_b64`）下 helper lane 在 EXEC 中；`ds_swizzle` QUAD_PERM 手算后 `image_sample_cd` | Vulkan 中 helper 对非 quad 的 subgroup 操作是否活跃由实现决定；`_cd` 当前翻译错误 |
| 浮点语义 | DXIL 规则 | MODE 寄存器（f32 FTZ，f16/f64 保留，IEEE=0，DX10_CLAMP=1）、`v_cvt_pkrtz` 向零舍入、`rsq(0)=inf`、`max` 返回非 NaN | 需逐条按 GCN 规则发射；10-02 已改 NMin/NMax 与 med3，PKRTZ 未处理 |
| 光栅/输出状态 | PSO 显式；early-Z 是 PS 属性；采样位置用 API 设 | 上百个上下文寄存器（`DB_SHADER_CONTROL`、`DB_EQAA`、`PA_SC_AA_SAMPLE_LOCS_*`、`SPI_PS_INPUT_ENA`、`SPI_SHADER_COL_FORMAT` 等） | 未建模的寄存器被忽略：0xA201、0xA2F5–0xA30F 在 `amdgpu/regs.h` 中是填充字 |
| 深度与元数据 | 压缩由驱动管理，API 只有 Clear/Resolve/Discard | HTILE/CMASK 是游戏可见内存，游戏自己写 HTILE、发解压 draw、读写深度内存 | Vulkan 没有 HTILE，只能识别“清除”一种用法并转成 load-op |
| 图像布局与别名 | 资源 + typeless/castable 视图；深度只能经 DSV/SRV | 深度就是内存：按 R32F 纹理读、按 R32F 存储图像写、按 buffer 填充 | 纹理缓存要在不同格式和类型的图像之间判定重叠并同步内容 |
| 内存一致性与同步 | 显式资源状态与屏障 | 统一内存；PM4 的 ACQUIRE_MEM/EOS/EOP/WAIT_REG_MEM/DMA_DATA | 由 buffer/texture cache 页跟踪推断屏障；同步包本身多为空操作 |
| GPU 驱动渲染 | ExecuteIndirect | `DRAW_INDEX_INDIRECT_COUNT_MULTI`，基顶点/起始实例写 SGPR | 映射到 `vkCmdDrawIndexedIndirectCount` + BaseVertex/BaseInstance |
| 分辨率缩放 | 无 | shadPS4 自有内部 0.5 缩放 | 存储写、整数格式、绑定位超限等都会钉为原生并连带同 pass 附件；中途提升会错一帧 |
| Turnip 能力缺口 | vkd3d 用到的都具备 | 缺 `VK_AMD_shader_explicit_vertex_parameter`、`VK_EXT_depth_range_unrestricted`、混合附件采样数、`VK_AMD_shader_image_load_store_lod`、`VK_AMD_gcn_shader`、`VK_AMD_shader_trinary_minmax`；`maxComputeSharedMemorySize` 32 KiB | 全部走模拟：KHR barycentric（fork 补丁）、深度范围用裁剪距离、每 mip 存储视图、立方体与三元 min/max 用 ALU。设备日志 `fp64_to_fp32=true compute_subgroup=64 shared_limit=32768 cube_alu=true trinary_alu=true storage_lod_views=true` |

### 2.1 哪条路径更容易做到无错模拟

结论：PC 路径（DXIL → vkd3d → Turnip）明显更容易做到无错，运行时开销也更小。原因是结构性的，不是投入多少的问题。

| | PC：D3D12/DXIL → Vulkan | PS4：GCN 机器码 + PM4 → Vulkan |
|---|---|---|
| 翻译性质 | 同一抽象层级之间的转换，D3D12 与 Vulkan 几乎同构，信息基本不丢 | 反编译：游戏拿到的是编译器针对一块固定 GPU 降级后的结果，意图已经丢失，只能从指令序列和寄存器反推 |
| 依据的规范 | D3D12 规范与 DXIL 语义有文档，实现规范即可 | “规范”就是 GCN 硬件的实际行为，很多没有公开文档（HTILE 格式、EXEC/helper 语义、PM4 包、精确舍入） |
| 目标端有无对应物 | 绝大多数一一对应：early-Z 属性、Wave 内建、`SampleGrad`、Clear、bindless、深度视图 | 很多机制 Vulkan 不暴露（HTILE、可编程采样位置、读取不活跃 lane 的 swizzle、统一内存），即使认出来也只能模拟 |
| 出错的形态 | 映射有错，修一次所有游戏受益 | “没认出这种写法”，按引擎、按游戏有长尾：清除核、拷贝核、归约、去重、深度拷贝、动态描述符表都靠模式匹配 |
| 成熟度 | vkd3d-proton 经过大量 PC 游戏验证，同一 Turnip 路径也被很多安卓 PC 模拟器使用 | shadPS4 在 Android/Turnip 上还很新 |

本文的每个确定缺口，本质都是“意图在 PC 版写明、在 PS4 版要倒推”：

| PS4 上要倒推的机制 | PC 版对应的写法 |
|---|---|
| `image_sample_cd` | `SampleGrad` |
| 强制 early-Z | DXIL 的一个标志位 |
| HTILE 清除 + 解压 draw | 一次 `ClearDepthStencilView` |
| 计算着色器按 R32F 写深度 | 写 SV_Depth 的 PS |
| wave64 去重循环 | `WaveReadLaneFirst`，语义由 API 定义 |

也要看到两面：

- **PC 路径并非没有难点**：带状态切换的 ExecuteIndirect、依赖 Windows 驱动的未定义行为、移动 GPU 缺失的特性；重度 PC 游戏在手机 GPU 上的性能同样是问题。MHR 起点是 Switch 游戏，负载轻，所以表现特别好。
- **PS4 路径的长处是目标固定**：GCN 行为一旦模拟正确，对所有游戏都成立，没有按显卡厂商的差异。而且 PS4 独占游戏只有这条路。

### 2.2 为什么设备上 PC 版帧率高得多

同一画面在设备上，PC 版约 60 FPS（GPU 78%，60 可能是帧率上限），PS4 版约 12.5 FPS（GPU 100%）。分辨率与存档不同，不是严格对等的比较，但 PS4 路径确实是 GPU 受限。有证据的因素：

1. **主 pass 像素多 2.25 倍**（trace 实测 pass 尺寸）：主 pass 在 1080p 原生渲染，PC 为 720p。根因已确认（4.9.1）：帧末 CAS 以存储图像写回场景颜色所在内存，存储写入按内存身份固定为原生，再连锁提升同 pass 附件。
2. **draw 更多**（计数实测，原因推断）：669 对 540，阴影约 365 对 264；early-Z 未实现导致遮挡剔除失效可能是原因之一。
3. **着色器中的模拟开销**：
   - 材质 FS 每像素约 600 次动态描述符表查找（10-02 实测，该 FS 占 draw 时间 37%）；
   - 三顶点插值每个属性插值 6 次；
   - 立方体与三元 min/max 用 ALU 模拟；
   - FS 强制 wave64 [推断]。
4. **render pass 被打断**：每帧 66 个 pass 实例，被采样、拷贝、状态变化切断；在 Adreno 分块架构上每次打断都要额外读写片上内存。
5. **CPU 侧**（这个场景不是瓶颈）：PS4 路径要单线程逐包解析 PM4、按地址查资源，并用写保护缺页跟踪统一内存；PC 的资源、上传和屏障都是显式的，没有这些开销。

差距中相当一部分可以修，而且都是通用改进：主 pass 被钉为原生、early-Z、动态描述符表改为真正的 bindless、手动插值还原为硬件插值、pass 打断。但统一内存一致性、HTILE、精确的 wave 语义这类工作，代价是这条路径固有的，只能降低，不能消除。

由此的取舍：
- 有 PC 版的多平台游戏，PC 路径在正确性和性能上都有结构性优势。
- shadPS4 的投入更适合放在 PS4 独占游戏，以及能“提升回意图”的通用机制上，也就是像 vkd3d 那样从语义出发，而不是逐条模拟硬件。
- 用原生帧确认“意图是什么”，对多平台引擎的排查很有效。

## 3. 帧结构对照

PC 名称取自原生 DXIL 入口函数名与资源名；PS4 为 trace 中的 pass、draw/dispatch 与着色器哈希。

| 阶段 | PC 原生 | PS4 GCN | GCN 特有机制 |
|---|---|---|---|
| 帧首 buffer 拷贝 | `CS_FastClear`（资源 `fastCopySource/Target`）约 60 次 | cs `0xf2218e57` 63 次（已 HLE 为图像拷贝）+ 451 个 `DMA_DATA`（内存→内存） | DMA_DATA 在命令处理器解析时执行（4.8） |
| 清除 | Clear*View；`GDSinitialize` | 填充核 cs `0xa1a2dfdc` 19 次（含写 HTILE，部分已 HLE 为整图清除）；cs `0xf9cecc7e` | 填充核与 HTILE（4.5） |
| IBL 探针 | `CS_ClearLightProbes` [4,2,64]（`bindlessTextureCube` → `RWOutdoorProbes`） | cs `0xd3dc78a2` (4,2,64)，读 BC6 立方体，写 64×32×384 R11G11B10 3D | 存储写 3D 纹理 |
| 实例剔除 | `CS_UpdateInstanceCount`、`CS_MiniClusterFrustumTest`×4、`CS_CulltestFirst` | cs `0x443a1895`、`0x7d90b58c`×4，写 `0x2048000000` | — |
| 遮挡剔除 | 128×64 **4×MSAA** 深度：`VS_WriteOccluderDepth` 1 draw；`PS_Culltest`、`PS_MiniClusterOcclusionTest`（ForceEarlyDepthStencil + WaveOps，写 `RWCountBuffer`/`RWDrawIndirectArguments`） | 128×64 D32 **4 样本 EQAA + 可编程采样位置**；#71 `fs 0xa6de2516`、#72–75 `fs 0x1a8286f0`，`DB_SHADER_CONTROL=0x1410` | 强制 early-Z、EQAA、采样位置（4.1） |
| 压缩 | `CS_InstancingCompaction`×3、`CS_MiniClusterCompaction`、`CS_DrawIndirectArgumentFill`×16 | cs `0x0f658d2c`×3、`0x33e0e9b0`×8 | — |
| 光源剔除 | `CS_LightSphereTransform`、`CS_LightFrustumTransform`、`CS_LightCulling2` [16,8,1] → `LightCullingVolumeUAV`（3D） | cs `0x4e367a48`、`0xc240d63f`、`0x4a1afb3c` (16,8,1) 写 32×80×32 R32 Uint 3D | 存储写 3D |
| 深度预通道 | `VS_StaticMeshShadowCast`/`VS_SkinningMesh8ShadowCast` + `PS_ShadowCast`，1280×720 D32S8，70 draw | 1920×1080 D32S8，98 draw；打包 f16 插值、WQM、kill | HTILE 快速清除（#88，已识别） |
| AO | `SSAO_PS`、`PreFilterHalfAOResultH/V`、`TemporalDenoiserLWHalfLowPS`、`UpscaleAOResult` | 480×270 R16F ×4 + 1920×1080 R16F（原生，绑定位超限） | 深度按 R32F 读 |
| 阴影 | 768×768×32；`CopyDepthPS` 写 SV_Depth 拷贝缓存阴影，再分片绘制，片间 `CS_MiniClusterFrustumTest`/`Compaction` | 768×768×32 D32；**计算着色器 #87（`0x2ed692a8`）以 R32F 存储图像写深度数组并写 HTILE**；`DB_DEPTH_VIEW` 逐片；片间 cs `0x91ac0150` | 深度/颜色别名、HTILE、多边形偏移 |
| 光照参数 | `PreCalculateLighting`（`LightParameterSRV`、`ShadowMapSRV`、`IESLightTableSRV`、`bindlessTexture2D`…） | cs `0x27fc5f3b` | — |
| 天空/远景 | `CubemapFarPlane2DPS` | 1080p 单 draw | — |
| 前向着色 | `PS_Forward`/`ForwardPS` + `VS_*Velocity`：RT0 R11G11B10 + RT1 RGBA16F，深度 Equal + 模板，70 draw；WaveActiveBitOr/ReadLane、Quad、`SampleGrad`、isfinite | c0 B10G11R11 + c1 RGBA16F，深度 EQUAL，99 draw；ds_swizzle OR 归约 + readlane、`v_cmp_class 0x1F8`、三顶点插值、`image_load`、`image_sample_c_lz` | 4.2–4.4 的大部分内容集中在这里 |
| 反射去噪 | `DepthDownSamplingPS` + `TemporalReflectionDenoiserPS`（320×180） | 240×135 深度（#682 Z 导出）+ B10G11R11 | Z 导出 |
| 雾 | `FogScatteringSunMaskCS` → `FogBufferUav`；`FogPS` | cs `0xe8194a55` 以 R32F 读 1080p 深度；雾合成 | 深度平面读 |
| 特效/粒子 | `VfxVertexShader`/`VfxPixelShader`：RT [场景, R8]，12 draw，`ExtendParticleVertexBuffer`、`PreCalcParticlesResult`、`CutoutTableSRV`，`SampleGrad` | c0 场景 + c1 R8，#689–#700 共 12 draw，8 个 FS（`0x6f92bbc4`、`0x8e638d38`、`0x31c357ee`、`0x5fc38605`、`0xa9eaf44f`、`0xa2c7445c`、`0x442189df`、`0xf9581d87`）全部用 `image_sample_cd`，`ZCLIP_NEAR/FAR_DISABLE` | **`image_sample_cd` 翻译错误；烟柱颜色差异在此** |
| 时域 | `PreTonemap2_PS`（`HDRImage`、`PrevHDRImage`、`Velocity`） | 1080p B10G11R11 单 draw | — |
| Bloom | `NewReduction`/`NewFiltering`/`NewBlendingHorizon/Vertical`/`NewFinal`，640×360 → 40×22 → 回到 640×360 | 480×270 → 60×33 → 回到 480×270 | 部分小目标按尺寸保护为原生 |
| 色调映射与锐化 | `LwLDRPostProcess`、`FXAA_PS`、`CAS_CS` [80,45,1] | 1080p PS + cs `0x0f696cac` (120,68,1) 以存储图像写场景颜色（B10G11R11） | B10G11R11 存储写 |
| UI 与输出 | `VSGUI`/`PS2D`、`ScreenOutputPS` | 960×540 R8 等；最终 A2B10G10R10 | — |

## 4. 特性差异与实现状态（按功能族）

### 4.1 GPU 驱动渲染与遮挡剔除

- **多重间接绘制** [核对]+[实测]：
  - 541 个 `DRAW_INDEX_INDIRECT_COUNT_MULTI`，`maxCount` 1–6，198 个从内存读 count，stride 20。
  - `draw_index_en` 全为 0。源码不实现 draw index SGPR（`amdgpu/pm4_cmds.h:1146-1153` 只声明），但不影响 MHR。
  - 基顶点/起始实例写 SGPR 0x53/0x54，映射为 SPIR-V `BaseVertex`/`BaseInstance`（`frontend/translate/translate.cpp:101-123`），每个子 draw 各自正确。
  - 参数与 count 缓冲经 `ObtainBuffer` 取 GPU 常驻副本（`renderer_vulkan/vk_rasterizer.cpp:584/591`），计算着色器写过的页按 GPU 修改跟踪，随后有全局屏障（`renderer_vulkan/vk_runtime.cpp:321-347`）。不读 CPU 端旧数据，不做 CPU 解析。
  - 未启用 `drawIndirectFirstInstance`，而 RE Engine 的间接参数带起始实例。按 Vulkan 规范属未定义用法；Turnip 与 AMD 实际照常传递，风险低，但应开启。
  - 步进率实例 ID（v1/v2）用未减基址的 `gl_InstanceIndex` 计算（`translate.cpp:136-155`），与 v3 不一致。MHR 的 VS 都请求 v0–v3，但只有遮挡包围盒 VS 读 v3，其余从 v0/SGPR 取实例数据，不受影响。
  - stride 不等于 20 时 `ASSERT` 中止（`vk_rasterizer.cpp:633/646`）。
- **遮挡测试 PS 与 early-Z** [核对]：
  - PS4 `fs 0x1a8286f0` 开头 `exp null done`，读平坦的簇编号，用 `v_readfirstlane` + `v_cmp_eq` ballot + `s_and exec` 循环和 `v_mbcnt` 让每个簇只有一个 lane 写 1；`DB_SHADER_CONTROL=0x1410`（EARLY_Z_THEN_LATE_Z、EXEC_ON_NOOP、DEPTH_BEFORE_SHADER）。
  - PC `PS_MiniClusterOcclusionTest` 是同一算法：WaveReadLaneFirst 循环 + WavePrefixBitCount + 两次 QuadOp 取最小，DXIL 标志 `0x80118` 含 ForceEarlyDepthStencil。
  - shadPS4 从不读取 `z_order`/`depth_before_shader`（`amdgpu/regs_depth.h:30-38`）。`DB_SHADER_CONTROL` 唯一的读者只取 Z/模板/掩码导出位（`vk_pipeline_cache.cpp:200-205`）。片元执行模式只有 Origin* 与 `DepthReplacing`（`backend/spirv/emit_spirv.cpp:459-471`）。
  - 带存储写的 FS 按 Vulkan late 测试执行：被遮挡的包围盒也写“可见”，剔除失效，Turnip 还会为这类管线关闭 LRZ [推断]。影响是多画、性能下降，不会少画。
- **4 样本 EQAA 遮挡深度 + 可编程采样位置** [核对]：
  - 深度采样数取自 `DB_Z_INFO.NUM_SAMPLES`，4× MSAA 是真实的；`PA_SC_AA_CONFIG` 只用来定 SAMPLE_COVERAGE 宽度。
  - `DB_EQAA`、`PA_SC_AA_SAMPLE_LOCS_*`、`AA_MASK`、`CENTROID_PRIORITY` 都是填充字（`amdgpu/regs.h:102-104, 149-151`），从未使用 `VK_EXT_sample_locations`（Turnip 提供）。
  - 本帧采样点 (−6,6)、(6,−6)、(−2,−2)、(2,2)/16，不是 Vulkan 标准 4× 图样；EQAA 为 4/4/4，不需要额外模拟。
  - PC 版同一 pass 也是 4×MSAA（标准位置），所以只影响剔除精度。修了 early-Z 之后才会显现为簇边缘的跳变 [推断]。
- **逐样本着色**：按 `SPI_PS_INPUT_ADDR` 的 sample 位开启（`renderer_vulkan/vk_graphics_pipeline.cpp:168-174`），而 PS4 由 `DB_EQAA.PS_ITER_SAMPLES` 决定（此处为 0）。MSAA pass 中主机可能每像素着色 4 次 [核对]。

### 4.2 跨 lane 与 wave64

- PS4 用法 [实测]：
  - `ds_swizzle` QUAD_PERM 66 处（梯度、quad 交换）。
  - XOR 1/2/4/8/16 蝶形 229 处，后接 `v_or_b32`，即 32 lane 内的 OR 归约。
  - `v_readlane` 常数 31/63 各 44 处，把两个半区合并：手写的 `WaveActiveBitOr`，用于前向 PS 让整 wave 统一遍历光源簇。
  - 动态 lane 的 `v_readlane` 64 处（waterfall），`v_readfirstlane` 115、`v_mbcnt` 22、`s_wqm_b64` 50。
- PC 对应 [实测]：`WaveActiveBitOr` 53 处/18 个着色器，`WaveReadLaneFirst` 61，`WaveReadLaneAt` 28，`WavePrefix*` 4，`QuadReadAcross*` 42。PC 前向 PS 没有任何导数指令，梯度由 quad 读取手算后用 `SampleGrad`（143 处）。
- **subgroup 大小** [核对]：Turnip `minSubgroupSize=64`、`maxSubgroupSize=128`，只有 FS 与 CS 能指定所需大小，不指定时 API 视为 128。shadPS4 对设备允许的每个阶段都链上 `requiredSubgroupSize=64`（`renderer_vulkan/vk_graphics_pipeline.cpp:362-371`、`vk_compute_pipeline.cpp:41-43`），所以 FS/CS 实际是 wave64。VS 不能指定，但 ir3 从不让几何阶段用双倍 wave，实际也是 64。wave 大小本身不是缺口。
- **指令映射** [核对]：
  - `ds_swizzle` QUAD 模式：同一选择 → `QuadBroadcast`；逐 lane 选择在 FS 中 → 四个常量 `QuadBroadcast` 加 `OpSelect`，其他阶段 → `Shuffle`（`frontend/translate/data_share.cpp:302-318`、`backend/spirv/emit_spirv_warp.cpp:21-38`）。
  - `ds_swizzle` 位模式：纯 XOR → `ShuffleXor`，其他 → 动态 `Shuffle`。
  - `v_readlane` → `Shuffle`，`v_readfirstlane` → `BroadcastFirst`，`v_mbcnt` → `BitCount(src & SubgroupLtMask)`，`s_ff1_i32_b64` → `BallotFindLSB`（掩码为 0 时结果未定义，GCN 为 −1）。
  - EXEC 是每个调用一个布尔，64 位读取 = `Ballot(exec)`，写入 = `InverseBallot`；64 位标量运算后的 SCC 取本 lane 的位，不是“任一 lane”。
- **整 wave 归约** [核对]：识别 `s_orn2_saveexec` → `v_cndmask` 单位元填充 → XOR 16/8/4/2/1 → `readlane 31/63`，改为 `ClusteredReduce(op, fill, 32)` 加半区合并（`translate.cpp:1506-1672`），能匹配 MHR 前向 PS 的光源掩码 OR 循环。只支持整数 umin/umax/smin/smax/and/or/xor；浮点 min/max/add 归约不识别，退回原样 shuffle。
- **inactive 与 helper lane** [核对]：
  - shuffle 的源 lane 若不活跃，结果未定义，代码不做屏蔽。FS 中只有 quad 操作保证 helper 参与。
  - 仓库笔记 `references/psvr-public-api/vulkan-subgroup-notes.md:21` 记录 GCN3 对无效 swizzle 源返回 0。
  - EXEC 初值、`s_wqm_b64` 空操作、ir3 让 helper 参与 ballot、`readfirstlane` 放置，见第 0 节第 7 条。
  - 实现了 `FindLiveMask`（10-04）：识别“开头保存存活掩码 → WQM → 只被 kill 清位 → `exp vm` 用掩码”的片元着色器，kill 时 demote 为 helper（`translate.cpp:1269-1408`）。
- **WQM 与发散控制流中的隐式导数** [核对]+[推断]：EXEC 按 lane 建模、WQM 不做事，GCN 中被 WQM 保活的 quad 成员在 SPIR-V 中会跳过坐标计算和采样，隐式 LOD 未定义。只有 `FindLiveMask` 与“受保护图像”路径（在逐 lane 描述符分支前先取 `OpDPdx/DPdy`）两处特殊处理。Turnip 提供 `VK_KHR_shader_quad_control`，但未使用 `QuadDerivativesKHR`/`RequireFullQuadsKHR`。可能表现为分支、alpha 测试边缘的 mip 噪点与闪烁。
- `v_writelane` 只转发常量 lane 的写→读链，其余情况目标 VGPR 变为 0；MHR 未使用。

### 4.3 插值与片元输入

- PS4 用法 [实测]：
  - 140 个属性以 `v_interp_mov p0/p10/p20` 读三顶点原始值，配合 `v_cvt_f32_f16`（341 处）解包，由游戏按重心坐标插值。这是 PC 版 min16float（24 个 PS）在 GCN2 上的形态，Liverpool 没有 f16 插值指令。
  - 392 个 draw 用 `PERSP_SAMPLE`，79 个用 `ANCILLARY`；`SPI_BARYC_CNTL.POS_FLOAT_LOCATION=at sample`。
- 已实现 [核对]：
  - 经 `VK_KHR_fragment_shader_barycentric`（Turnip 为 fork 补丁 `64817e11155`）提供逐顶点值；无该扩展时有默认关闭的软件 GS 插值。10-01/02 核对过 P10/P20 语义、缓存元数据与重心范围。
  - 有该扩展时**所有** `v_interp_mov`（包括普通 flat 属性）都走 `PerVertexKHR`：P0 = 顶点 0，P10/P20 = 顶点 1/2 减顶点 0，“直通”输入取原值（`frontend/translate/vector_interpolation.cpp:108-120`）。因此 flat 的 P0 是逐顶点索引 0，不一定是 provoking vertex；Turnip 报告 `triStripVertexOrderIndependentOfProvokingVertex=false` [推断风险]。
  - fork 补丁的实现与代价见第 0 节第 8 条。PC 版不需要这一层：min16float 插值由硬件完成。可以考虑在着色器中识别“三顶点值 × 重心权重”的手动插值模式，改回硬件插值（在 VS 侧解包），同时去掉插值次数与变量槽的放大。
  - `v_interp_p2` 用硬件插值，限定词由其 J 源取自哪组重心 VGPR 决定；一个属性只保留最后写入的限定词 [推断风险]。
  - sample/centroid 重心映射为 `Sample`/`Centroid` 修饰或 InterpolateAt*（`frontend/translate/vector_interpolation.cpp:11-30`）。
  - FLAT_SHADE 的 P0 读取映射为 Flat 或 `PerVertexKHR[0]`。
- 不完整 [核对]：
  - `ANCILLARY` 只降级常量位段提取：8–11 位映射为 SampleId，16–26 位映射为 Layer；其他字段走 UNREACHABLE，非位段提取的用法读到 0（`ir/passes/lower_hardware_intrinsics.cpp:27-61`）。
  - `POS_FLOAT_LOCATION` 被忽略。
  - `FRONT_FACE_ALL_BITS` 时正面给 1/0，而硬件可能给全 1。
  - 平坦输入用 P1/P2 读取会 ASSERT。

### 4.4 浮点语义

- PS4 用法 [实测]：
  - `v_cmp_class_f32` 掩码全为 `0x1F8`（即 isfinite），45 处，集中在前向 pass 的 99 个 draw；PC 对应 `isSpecialFloat` 74 处。
  - 浮点模式：f32 非规格化冲零、f16/f64 保留，IEEE=0，DX10_CLAMP=1（少数 PS 为 0）。
  - `v_cvt_pkrtz_f16_f32` 174 处，`v_rsq_f32` 337 处，`v_rcp_f32` 730 处。
- 已实现（10-02）：`v_max/v_min` 改用 NMin/NMax，`med3` 按 GCN 规则；Turnip 上 SignedZeroInfNanPreserve 已打开。此前定位的亮斑机制：法线恰为 0 → `rsq(0)=inf` → `0·inf=NaN` → 游戏 isfinite 守卫把结果写黑，或保留为彩色。
- **PKRTZ** [核对]：见第 0 节第 3 条（`frontend/translate/vector_alu.cpp:849-853`、`ir/passes/constant_propagation_pass.cpp:376-379`）。MHR 的 B10G11R11 目标不受影响：Vulkan 把有限值钳到格式最大值 [推断]。RGBA16F 目标（前向 pass 的 RT1、AO 等）受影响。前向 PS 在导出前先用 isfinite 去掉 NaN/inf，再经 PKRTZ 导出；去掉 RTZ 之后，有限的大值仍可能在附件转换时变成 inf。
- **`v_cmp_class_f32`** [核对]（`vector_alu.cpp:1328-1348`、`ir/passes/lower_hardware_intrinsics.cpp:10-25`）：
  - 内联常量掩码：含两个 NaN 位 → `IsNan`；否则含两个 ±inf 位 → `IsInf`；否则含四个负类位 → `x <= -0.0`（会把 +0 也算作负）。
  - SGPR 掩码只支持 NaN / Inf / Finite，其他走 `UNREACHABLE`。
  - 超集会退化为第一个匹配项：0x207 只测 NaN，0x3FC 变成 `IsInf`。
  - MHR 只用 0x1F8（Finite），正确；其他游戏的掩码可能崩溃或漏判。
- **执行模式** [核对]（`backend/spirv/emit_spirv.cpp:491-676`）：非规格化处理按 guest FLOAT_MODE，RTZ 仅当 guest 要求，SignedZeroInfNanPreserve32 在支持时总是开启。Turnip 支持 FP32 flush，FP32 preserve 只在 softfloat drirc 下支持；A7xx 上 FP16 flush 支持、preserve 不支持；不支持 RTZ。`IEEE_MODE`、`DX10_CLAMP` 位从不读取：clamp 修饰一律经 NClamp 把 NaN 写成 0，等同 DX10_CLAMP=1；本帧少数 PS 为 0，这些 PS 的 clamp(NaN) 行为与 PS4 不同 [推断]。
- **乘加** [核对]：`v_mad`/`v_mac`/`v_madmk` 为分开的乘与加（NoContraction，与 GCN 一致）；**`v_madak_f32` 被当作 `V_FMA_F32`（融合）**（`vector_alu.cpp:76-77`）。MHR 前向 VS 的 6 个 `v_madak` 都在 `exp pos0` 之后，只用于法线等参数解码，深度预通道 VS 不用 `v_madak`，所以不破坏 EQUAL 测试，只造成约 1 ulp 的差异。`OpFma` 在 Mesa 中是 `ffma_weak`，不保证融合。`v_mad_legacy`/`v_mac_legacy` 丢了 0·x=0 规则。
- **超越函数** [核对]：rcp = `1/x`，rsq/sqrt/log2/exp2 用 Vulkan 精度；`rcp_clamp`/`rsq_clamp`/`rsq_legacy`/`log_clamp` 映射为不钳位版本（`vector_alu.cpp:172-189`），GCN 钳位处会产生 inf（MHR 未用这些变体）。`v_fract` = `OpFract`，对很小的负数可能返回 1.0 [推断]。
- **其他转换** [核对]：`v_cvt_f32_f16` 用 `OpFConvert`，A7xx 上 FP16 非规格化被冲零 [推断]；`v_cvt_off_f32_i4` 只接受立即数源，VGPR 源会 ASSERT；`v_sad_u32` 正确（MHR 多用作三操作数加法）。
- **位置不变性**：MHR 的深度预通道 VS 与前向 VS 是不同的着色器，位置计算都只用 `v_mad`/`v_mac`（均为 NoContraction 的乘加），在同一驱动上应得到相同结果；未见需要 `Invariant` 修饰的证据。

### 4.5 深度、HTILE 与深度/颜色别名

- **深度测试** [实测]：反向 Z（GEQUAL）；前向 pass 在预通道后用 EQUAL；阴影用多边形偏移；部分粒子 draw 用 `ZCLIP_NEAR/FAR_DISABLE`；深度预通道与阴影 VS 导出用户裁剪距离 0（`PA_CL_VS_OUT_CNTL=0x400001`）。
- **裁剪与钳位** [核对]：
  - ZCLIP 关闭 → `depthClipEnable=false`，`depthClampEnable=!DISABLE_VIEWPORT_CLAMP`；DX 裁剪空间 → `negativeOneToOne=false`（`vk_pipeline_cache.cpp:623-625`、`vk_graphics_pipeline.cpp:148-158`）。主机按视口变换范围钳位，PS4 按 `PA_SC_VPORT_ZMIN/ZMAX`，两者在 [0,1] 时一致。
  - 用户裁剪距离映射为真实 `ClipDistance[0]`，可与深度范围回退共存；但在 RectList/QuadList 的辅助 TCS/TES 中只有回退在裁剪时才转发，guest 裁剪距离会丢失。MHR 只用 TriList/TriStrip，不受影响。
- **HTILE** [核对]+[实测]：
  - 快速清除 = 填充核写 HTILE。只有地址已登记（深度目标创建时）才跳过 dispatch，且按“全部切片已清除”处理（`texture_cache.h:335-342`），在下次绑定为深度目标时以 load-op 清除、使用当时的 `DB_DEPTH_CLEAR`（`vk_rasterizer.cpp:1294-1328, 2460-2465`）。CP 填充同样不看填充值就 `ClearMeta`（`vk_rasterizer.cpp:2678-2679`）。
  - 本帧 #69（遮挡深度）、#88（主深度）已识别。
  - #723 写 `0x2058400000`，属于 `0x2019000000` 的 D32 表面；紧接着 #724 是 `DB_RENDER_CONTROL=0x60` 的解压 draw；此后这段内存不再作为深度，而作为 bloom 的 240×135 B10G11R11 目标。也就是 RE Engine 用“HTILE 清除 + 解压”批量初始化瞬态别名内存。shadPS4 中 #723 按普通 buffer 写执行；#724 因深度测试关闭不绑定深度，成为空 pass（`vk_rasterizer.cpp:353-365`），内存不会被写成清除值。
  - #87 写阴影深度数据的同时写其 HTILE，不是清除；shadPS4 不读 HTILE，无影响。
  - 设备上本帧 19 次填充只有 8 次被替换：6 次整图清除，#69、#88 两次元数据清除。直接执行的有：
    - #61（场景颜色，见 4.9.1）；
    - #89（RGBA16F 整图 `0x201f2f0000+0xff0000`）；
    - #722（瞬态内存 `0x2019900000`）、#723；
    - #245/#308/#358/#393/#481/#529：阴影第 1–6 片各自的 HTILE（`0x20ef810000` 起每片 0x10000）。
  - 阴影这 6 次是相对 HTILE 基址有偏移的逐片写入，正好是审查指出的“按切片偏移的清除识别不到”。对应切片 pass 的深度是 Load 而非 Clear（trace 中 `L:rw`）。这些写入是清除还是“置为未压缩”取决于填充值，本次没有读取值。若是清除，切片保留上一帧的深度，动态投影物会留下残影 [推断]。
  - 以格式化方式读 HTILE 时返回 `0xf`（未压缩，MinZ=MaxZ=0，`buffer_cache/buffer_cache.cpp:969-974`）。反向 Z 下等于“处处远平面”，若有基于 HTILE 的 Hi-Z 会出错；MHR 本帧未见读取。
  - `FLUSH_AND_INV_DB_META` 为空操作。
- **深度拷贝惯用法** [核对]：`Z_READ_BASE≠Z_WRITE_BASE` + `DB_RENDER_OVERRIDE=0xf8000000` → `DepthStencilCopy`，做整范围、全部视图切片的 `vkCmdCopyImage`。10-05 修了目标图像 Dirty 标记导致的旧数据覆盖。`regs_depth.h:111-112` 中 `COPY_SAMPLE` 位宽有误，只有跳过判断读它。
- **深度当颜色用** [核对]：
  - R32F T# 的比较采样会把视图换成同一图像的 D32 深度视图，比较在硬件中按逐纹素先比较后过滤完成，属正确 PCF（`texture_cache/image_view.cpp:123-128`、`texture_cache/sampler.cpp:58`）。
  - R32F 存储写深度数组（#87）走 `ResolveDepthOverlap` → `CopyColorAndDepth` 重建并用 twin 避免来回（`texture_cache.cpp:1061-1143`、`vk_runtime.cpp:174-209`）。
  - 已知边角：`ImageResource` 去重忽略 `is_depth`；重建判断用 `{levels,layers}` 的字典序比较而非包含关系。
- **Z 导出**：映射为 `FragDepth`；`CONSERVATIVE_Z_EXPORT` 未映射，只影响性能。
- **清除 draw** [核对]：深度/模板清除对整个渲染区域执行（`vk_scheduler.cpp:169-173`），对局部矩形清除不正确；MHR 本帧未见。

### 4.6 纹理与采样

- PS4 用法 [实测]：
  - BC6 立方体（mip，1056 次读取）、BC7/BC4/BC1/BC3。
  - 3D 纹理：10_11_11 Float、32 Uint、16_16_16_16 Float、8_8_8_8 sRGB LUT，tiling 19（Thick）。
  - 1DArray 16F；2DArray R32F（阴影，`image_sample_c_lz da` 83 处）。
  - `image_gather4_lz(_o)`；FS 中 `image_load` 188 处；`image_sample_cd` 10 处。
- **`image_sample_cd`** [核对]：见第 0 节第 1 条。特效 PS 的指令序列：`ds_swizzle QUAD_PERM` 求差得到 du/dx、dv/dx、du/dy、dv/dy 放在 v5–v8，坐标放在 v9–v10，再 `image_sample_cd v[..], v5, …`。shadPS4 把 v5/v6 当坐标读。
- **立方体** [核对]：所有立方体以 2D 数组视图采样，坐标由 AMD 立方体指令结果换成面内坐标加面索引（`image_view.cpp:24-25`、`backend/spirv/emit_spirv_image.cpp:113-131`），没有跨面过滤，隐式 LOD 来自面内导数。BC6 IBL 立方体的低 mip 可能出现接缝 [推断]。
- **bindless** [核对]：见第 0 节第 5 条。
- **S# 字段** [核对]：
  - 已支持：过滤、各向异性、比较函数、LOD 偏移（只有上限钳位）、min/max LOD、边框颜色（含自定义表）、`force_unnormalized`、`force_degamma`（在过滤之后做 gamma）。
  - 近似：`mip_filter`=None 映射为 nearest 且不钳到基础 level。
  - 忽略：min/max 归约过滤模式、`lod_bias_sec`、`aniso_threshold/bias`、`z_filter`、`perf_mip/z`、`trunc_coord` 等（`amdgpu/resource.h:413-440`）。
- **T# 字段** [核对]：mip 范围、`min_lod`（`VK_EXT_image_view_min_lod`）、`dst_sel`、数组范围、`pow2pad` 都已支持，Thick 3D 解 tiling 已实现（`texture_cache/tiling.cpp`、`host_shaders/tiling.comp`）。已知偏差：
  - 深度 <4 的 mip 不按 addrlib 退化为 THIN [推断]，只影响带 mip 的体纹理。
  - 11_11_10 只按 R/B 交换重映射，位宽不一致 [推断解码错误]，MHR 未用。
  - SnormNz/Uscaled/Sscaled 用整数视图加着色器转换，失去线性过滤。
- **运行时纹素偏移** [核对]：非常量偏移在不支持的采样指令上被丢弃并告警（`emit_spirv_image.cpp:209-214`）。MHR 的设备日志无此告警，偏移都是常量。
- **存储写** [核对]：B10G11R11 与 3D R32 Uint 在 Turnip 上可作存储图像（所有可渲染非深度格式都给 STORAGE），无需重解释；不支持时会 ASSERT。
- **`image_load` 与缩放** [核对]：可缩放 2D/2DArray/Cube 绑定在着色器中重映射整数坐标与 LOD，`FragCoord` 换算为 guest 像素，`image_get_resinfo` 返回 guest 尺寸。钉为原生的条件：写入、原子、无法换算的偏移或 LOD 查询、立方体当 2D 数组读、整数格式、统一绑定序号 ≥30。30 个绑定位由 push 常量中的缩放表决定，且前面阶段的 buffer 与 sampler 也计数（`vk_rasterizer.cpp:969-1011`），纹理多的 FS 后部纹理会被钉住并连带提升。
- **顶点输入** [核对]：fetch shader 转为 Vulkan 顶点属性，格式在 Adreno 上都支持。使用动态顶点输入时不传 buffer 大小（`vk_rasterizer.cpp:1239-1240`），越界索引读相邻内存而非 0。非 fetch 的 typed 读降为 raw 读加解包，2_10_10_10 SNORM 不钳到 −1。

### 4.7 渲染目标与导出

- 目标格式两边一致：B10G11R11（场景）、RGBA16F、R8、R16F、D32S8、D32 数组；PS4 最终输出 A2B10G10R10。
- 导出 [实测]：`mrt compr`（打包 16 位）89 处、`mrtz` 1 处、null 30 处。混合以预乘 alpha（ONE, 1−SRC_ALPHA）为主，另有加法和 SRC_ALPHA/1−SRC_ALPHA。`CB_COLOR*_INFO` 未开 CMASK/快速清除/压缩，`CB_COLOR_CONTROL` 只有 Normal 与 Disable。
- [核对]：
  - 压缩导出按 `SPI_SHADER_COL_FORMAT` 解包（FP16、UNORM16、SNORM16、UINT16、SINT16），见 PKRTZ 一条。
  - 写掩码 = 交换后的 `CB_TARGET_MASK`；`CB_SHADER_MASK` 只用于在混合因子中模拟 alpha=1。
  - `BLEND_CLAMP`、`ROUND_MODE`、`SIMPLE_FLOAT` 未使用（`amdgpu/regs_color.h:161-164`）。
- 多边形偏移 [核对]：结构从 0xA2DF 开始，名为 `depth_bias` 的字段实为 `PA_SU_POLY_OFFSET_CLAMP`。`DB_FMT_CNTL` 未建模；正反面开启时只用一组（`vk_rasterizer.cpp:3001-3009`）。D32 的单位与 GCN 一致。

### 4.8 内存模型、同步与 DMA [核对]

- **`DMA_DATA`**（451/帧，源 L2，目标内存）→ `Rasterizer::CopyBuffer`：
  - 源与目标都没有 GPU 修改、且源处没有图像时，命令处理器解析时直接 `memcpy`（`vk_rasterizer.cpp:2704-2710`）；否则录制 `vkCmdCopyBuffer`。
  - `memcpy` 经 guest 地址写，被监视的页触发缺页并标记为 CPU 修改，顺序上与录制时快照一致。
  - `SAS/SAIC/DAIC/CP_SYNC` 被忽略。
  - CPU 路径不调用 `SettleWaits()`（WRITE_DATA 会），开 completion fence 时写入可能早于 fence。
  - ≤16 KiB 的只读源走流式快照读 guest 内存，不会把 GPU 写过的图像 tile 回 buffer。
- **同步包**：
  - `ACQUIRE_MEM` 是空操作。
  - `EVENT_WRITE_EOS` 忽略事件类型（CS_DONE/PS_DONE），只写 label；GDS 目标时先整 GPU `Finish()`。
  - EOP 写 label 并发中断。Android 默认 completion fence 关闭，label 在解析时立即写。
  - `WAIT_REG_MEM`/`COND_EXEC` 在 CPU 上读 guest 内存或待定 fence 值，看不到着色器写的标志。MHR 没有卡住，说明它的等待对象都是 CP 写的 label。
  - EOP/EOS label 经 `TryWriteBacking` 写，不经页保护，同页的 GPU 常驻副本会陈旧。
  - `SET_PREDICATION` 未实现，遮挡计数为假值。
- **队列**：计算队列与图形在同一线程、同一调度器上串行，没有真正的异步计算。
- **抓帧后设备场景几何消失**（10-05，未归因）：
  - 消失发生在抓帧开始之后（写回 GPU 结果、对可写内存设录制写保护），抓帧期间设备导出的两帧已缺几何。
  - 同一 trace 在桌面回放画面完整，说明 guest 内存与命令一致，问题在设备侧缓存状态。
  - 返回标题再进入仍缺失。

### 4.9 内部分辨率缩放（shadPS4 自有）[核对]+[实测]

- 10-05 证实：桌面回放 0.5 第 1 帧整体发暗。原因是主深度在帧中途由 0.5 提升为原生（mixed attachment pass），此时深度预通道已在 0.5 下画完，最近邻放大后的深度与原生光栅化的深度不完全相等，前向 pass 的 EQUAL 测试大面积失败。设备上这些决定在会话早期就已做出，只在首次出现时错一帧。回放 trace 现记录原生决定（`ScalePlans`）并在回放开始时预置，0.5 回放与 1.0 一致。
- 阴影深度数组按 0.5 缩到 384×384（深度目标与颜色同等缩放，保护阈值只有 “>64”，`image.cpp:278`），阴影精度下降 [推断]。

#### 4.9.1 主 pass 为什么是原生 1080p（10-06 确认）

证据：
- 桌面回放 `mhr_ss4`，不预置原生决定，按日志顺序记录（`user/log/gpu_replay.log`）。
- 设备 trace（`a10bbbc5…`）中 pass 的缩放标记，以及每次填充是否被替换。
- 桌面回放开启 `upload_diag` 时的逐 dispatch 诊断。

**一、根因：同一块内存每帧承载两份不同内容。** RE Engine 把 HDR 场景颜色缓冲 `0x201d100000`（1920×1080 B10G11R11）在帧末复用为最终图像：

| 帧内事件 | 作用 |
|---|---|
| #61 填充核 `0xa1a2dfdc` | 把整段 `0x201d100000+0x7f8000` 写成常量（帧首清除） |
| #581 起：天空 `fs 0x7c82bd42`（PC `CubemapFarPlane2DPS`）、前向、粒子 | 以颜色附件在其上渲染 HDR 场景 |
| #720 后处理 | 结果写到另一张图 `0x201d8f8000` |
| #721 cs `0x0f696cac`（PC `CAS_CS`），120×68 组 × 16×16 线程 = 1920×1088 | 读 `0x201d8f8000`，**以存储图像把锐化结果写满 `0x201d100000`** |
| UI | 画在其上，随后翻转显示 |

两份内容之间都有整张覆盖（#61 与 #721），互不依赖。本帧共 4 次存储图像写入：IBL 探针体、光源剔除体、阴影数组、#721。只有 #721 落在场景颜色上。

**二、规则：存储写入一律原生，且按内存身份单向固定。**
- `FindImage` 对 Storage 绑定调用 `ForceNative("storage or format alias")`（`texture_cache.cpp:1505-1507`，另见 `image.cpp:261, 554`）。
- 这是设计规定：写入型 storage 保持原生，不缩放计算派发（`docs/specs/internal-scale-resource-policy-20260920.md:78`）。
- 缩放计划按图像身份（地址、格式、尺寸等）保存。NativeRequired 是单向的，之后不再回到缩放（同文档“每个 identity 最多一次 native→render 重规划；之后……单向回 native”）。
- 回放日志第一条相关记录：`promote storage (semantic-native) 1920x1080 B10G11R11 0x201d100000 … history=0xe`（Texture|Storage|RenderTarget）。从下一帧起，前向 pass 用的场景颜色就是原生的。

**三、连锁：同 pass 附件缩放不一致时全部提升。** Vulkan 一个渲染 pass 的附件必须在同一像素网格上，所以 mixed attachment pass 规则把与原生附件同 pass 的缩放附件全部提成原生（`vk_rasterizer.cpp:1078-1082`）：

| 日志 | 着色器 | 同 pass 附件 | 被提升 |
|---|---|---|---|
| 873 | `fs 0x7c82bd42`（天空） | 场景颜色 + 主深度 | 主深度 `0x201e9f0000` |
| 875 | `fs 0xb0260f9a`（前向） | 场景颜色 + RGBA16F + 深度 | RGBA16F `0x201f2f0000` |
| 877 | `fs 0x6f92bbc4`（粒子） | 场景颜色 + R8 + 深度 | R8 `0x2058c00000` |
| 881 | `fs 0x6bd431c4` | 场景颜色 + R8 + 第二个深度 | D32S8 `0x20197f8000` |

结果：深度预通道、前向、粒子、雾、时域等全部在原生 1080p 上进行。0.5 只作用于 AO 半分辨率链、bloom 等本身就小的目标，以及阴影数组。设备 trace 中前向 pass 的标记一致：c0 `native(semantic-native,0x4,storage)`、c1 `native(mixed-pass)`、深度 `scale=1`。

**四、同一块内存上的其他每帧开销**（与缩放无关，但都源于内存复用）：
- #61 帧首填充在设备上没有被转换为整图清除（trace 中无 replaced 事件），作为 buffer 写入执行；GPU 上的场景颜色图像随之失效，下次使用时重新上传。桌面回放两帧 38 次填充中，10 次转为清除、26 次范围内无图像、2 次 partial，#61 属于哪一类未逐个记录。
- 原始拷贝核 `0xf2218e57`（诊断 #13，4080 组）把整张场景颜色当作 buffer 读出，拷进瞬态内存 `0x2019000000`，供粒子采样场景颜色（PC 为一次 `CopyResource`，对应 `primSceneTex`）。目标区域重叠着 bloom 的 480×270、960×270、960×540 等多张图像，同布局图像拷贝 HLE 不适用，于是走“GPU 图像 tile 回 buffer（`SynchronizeMemoryFromGpuImage`，每帧一次 8 MB）→ buffer 拷贝 → 目标图像重新上传”。

**五、PC 版为什么没有这个问题。** PC 上分辨率是游戏自己的设置（720p），没有模拟器缩放，也就不存在“存储写 → 钉原生 → 连锁提升”。PC 的 CAS 写 `OutputImage`、场景拷贝是 `CopyResource`，都是 API 级操作，驱动不需要推断。

**六、可选的修正方向**（未实施）：
1. **按内容生命周期分配**（推荐）：把“整张覆盖”视为上一份内容的结束——#61 的清除，以及覆盖整张图的存储写。为同一身份保留两份后备：渲染生命周期用缩放后备，存储生命周期用原生后备。两段之间都是整张覆盖，切换时不需要拷贝。纹理缓存已有深度/颜色别名用的 twin 机制可以借鉴。前提是让 #61 这类填充可靠地被识别为整图清除。
2. **对“整屏一线程一像素”的后处理计算核缩放派发**：缩小派发网格并重映射线程坐标。与现有规定“不缩放计算派发”冲突，通用性风险大，只适合识别出的固定模式。
3. **只放宽连锁规则不可行**：同一渲染 pass 的附件必须同一网格；连锁本身是被迫的，要从源头（第 1 点）解决。

**七、预期影响**（推断）：主 pass 像素在原生时为 2,073,600，0.5 时为 518,400（PC 720p 为 921,600）。10-02 剧情场景实测主场景 pass 约占 GPU 时间 84%，这是设备上 GPU 100% 的主要来源之一。

### 4.10 已观察的画面差异

| 现象 | 位置 | 状态 |
|---|---|---|
| 香炉烟柱为饱和绿色光柱（PC 为淡白烟） | 特效 pass #689–#700，设备与桌面回放一致 | 对应 `image_sample_cd` 翻译错误（第 0 节第 1 条），修复后需画面确认 |
| 金属/皮肤彩色亮斑、黑面 | 前向 pass `0xb0260f9a` 等 | 10-02 已定位到法线为 0 → `rsq(0)` → NaN → isfinite 守卫；法线由三顶点原始值与重心权重计算，0 值来源未定 |
| 大面积黑色（建筑、推车） | 前向 pass | 10-02 判断为真实的 0 光照，不经过 isfinite 守卫；未定位 |
| 抓帧后静态场景消失 | 设备 | 见 4.8 |

## 5. 迭代优先级

1. **`image_sample_cd*`**：翻译器把 `CoarseDerivative` 与 `Derivative` 同样处理（GCN 两者寄存器布局相同，只是硬件精度不同）。验证：特效 pass 逐 draw 导出，对比 PC `VfxPixelShader` 输出（烟应为白色、有纹理）。可推上游。
2. **强制 early-Z**：读取 `DEPTH_BEFORE_SHADER`/`Z_ORDER`（以及 `EXEC_ON_NOOP`、`KILL_ENABLE`、Z 导出的组合），对满足条件的 FS 发出 `EarlyFragmentTests` 并纳入管线键。验证：遮挡 pass 前后间接参数中可见簇数量与前向/阴影 draw 数（与 PC 同一 pass 的 `RWDrawIndirectArguments` 数量级对比），以及 GPU 时间。
3. **PKRTZ 向零舍入**：保留 PKRTZ 的舍入语义（不让 `PackHalf2x16`/`UnpackHalf2x16` 互消，或用 RTZ 执行模式），并保留 `PackUnorm/Snorm` 的钳位。验证：GPU probe 对 65504–1e6 区间与 [0,1] 外的输入逐值比较。
4. **HTILE 语义补全**：
   - HTILE 地址与深度表面的对应关系在看到 `DB_HTILE_DATA_BASE`/`DB_Z_INFO` 时就登记，而不只在创建深度目标时登记。
   - 清除按实际写入的切片/范围与填充值判断。
   - 解压 draw 在没有对应主机深度图像时，把清除值写入内存或对应的别名图像。
   - 验证：bloom 链读取 `0x2019000000` 时的内容。
5. **helper lane 与 EXEC**：
   - 不进 WQM 的片元着色器把初始 EXEC 设为 `!gl_HelperInvocation`；`s_wqm_b64 exec, exec` 恢复为整 quad。
   - `v_readfirstlane` 归为受 EXEC 影响的指令，不再被移出 EXEC 分支块。
   - 验证：GPU probe 构造部分覆盖的 quad，跑遮挡 PS 的“每簇唯一写入者”循环，确认写入不丢；再在设备上对比遮挡剔除后的可见簇数。
6. **bindless**：评估把动态图像表改为 GPU 侧描述符索引（descriptor indexing 或描述符缓冲），同时解决 32 项上限、CPU 快照的一致性和每像素查表开销。
7. **手动插值还原为硬件插值**：识别“三顶点原始值 → 解包 f16 → 按重心权重相加”的模式，在 VS 侧解包、FS 用硬件插值。这样可去掉 fork 补丁中每个属性两个变量槽、六次插值的放大，也避免槽位用尽导致管线创建失败。
8. **WQM 与发散中的隐式导数**：对需要隐式导数的采样，确保 quad 成员在采样点都执行过坐标计算（例如把导数提前计算，或使用 `VK_KHR_shader_quad_control` 的 `QuadDerivativesKHR`/`RequireFullQuadsKHR`）。
9. **缩放策略**：
   - 按内容生命周期分配缩放后备（4.9.1 第六点第 1 条），解除“帧末存储写 → 整个主 pass 原生”。前提是帧首填充 #61 能可靠地转换为整图清除。
   - 阴影深度数组与比较采样目标默认不缩放。
   - 在会话开始时预置已知原生决定（设备上也可复用 `ScalePlans` 的思路，按游戏持久化），消除首次出现时的错误帧。
10. **立方体跨面过滤**：评估改用真正的立方体视图（坐标从 AMD 立方体指令结果还原为方向向量）。
11. 中低优先级：
   - `v_cmp_class` 支持任意掩码组合（逐类判定后按位或），避免 UNREACHABLE。
   - `v_madak_f32` 改为不融合；`rcp/rsq/log` 的 clamp 与 legacy 变体按 GCN 钳位；`s_ff1_i32_b64` 对 0 返回 −1；`v_mad/mac_legacy` 的 0·x=0；按 `DX10_CLAMP` 位选择 clamp 对 NaN 的行为。
   - 浮点 min/max/add 的整 wave 归约识别。
   - 采样位置与 EQAA（`VK_EXT_sample_locations`）；逐样本着色改按 `PS_ITER_SAMPLES` 判断。
   - `drawIndirectFirstInstance`；步进率实例 ID 基址。
   - min/max 归约过滤（`VK_EXT_sampler_filter_minmax`）。
   - 正反面多边形偏移。
   - 越界顶点置零。
   - `DMA_DATA` 的 `SettleWaits` 与 SAIC/DAIC；≤16 KiB 快照不 tile 回图像；label 写入绕过跟踪。
   - `ANCILLARY` 其余位段；`FRONT_FACE_ALL_BITS` 取值。
   - 11_11_10 位宽。

## 6. 验证方法建议

- **同阶段结构对照**：原生 RDC 与 PS4 回放（`--gpu-replay-hash-images` 与逐 draw 导出）在同一阶段比较数量，不比较像素：遮挡剔除后的可见簇数、光源剔除体素中非空单元数、阴影切片写入的 draw 数、前向 pass draw 数、特效 pass 的输出颜色分布。
- **着色器级差分**：用现有 GPU probe 框架（`tests/video_core/android_*_probe.cpp`）在 Turnip 上逐 lane 比较 GCN 语义与生成的 SPIR-V，覆盖 `image_sample_cd`、PKRTZ、遮挡 PS 的去重循环、前向 PS 的归约段和 isfinite 段。
- **状态覆盖统计**：给 `ps4_state.py` 增加“寄存器被写入但 shadPS4 不读”的清单，作为新游戏接入时的固定检查项。本帧已知：`DB_EQAA`、`PA_SC_AA_SAMPLE_LOCS_*`、`PA_SC_AA_MASK_*`、`PA_SC_CENTROID_PRIORITY_*`、`DB_SHADER_CONTROL` 的 `Z_ORDER/DEPTH_BEFORE_SHADER/EXEC_ON_NOOP/CONSERVATIVE_Z_EXPORT`、`DB_RENDER_CONTROL` 的压缩位、`CB_COLOR*_INFO.BLEND_CLAMP`、`SPI_BARYC_CNTL.POS_FLOAT_LOCATION`、`PA_SU_POLY_OFFSET_DB_FMT_CNTL`。
- **指令覆盖统计**：用 `ps4_inv.py` 的指令直方图对照解码标志，凡是解码器设置而翻译器不检查的修饰位（如 `CoarseDerivative`）都应列出。

## 7. 未覆盖

- 原生 RDC 的像素来自桌面 AMD D3D12 回放，不代表 Turnip 原生输出；没有在 Turnip 上回放原生帧。
- PS4 trace 是 10-05 18:07 的一次设备采集，只分析了一帧；村庄、户外、战斗等场景未做同样盘点。
- 标为 [推断] 的影响（遮挡剔除失效、helper 抢占写入导致误剔除、PKRTZ 溢出成 inf、立方体接缝、阴影精度下降等）尚未用 GPU probe 或画面对照证实；第 5、6 节列出了对应的验证做法。
- 本文的分析没有改代码。此前的回放改动仍未提交：`ScalePlans`、`DepthStencilCopy` 的 Dirty 修复、`gpu_replay_probe`、`sgpurply.py` 对带内容的 Submit 记录的解析（见 [Android GPU 回放](android-gpu-replay-mhr-20261005.md)）。工作区里另有其他会话的未提交改动。
