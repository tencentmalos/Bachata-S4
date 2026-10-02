# GCN compatibility paths and experimental-mode cleanup — 2026-10-02

本轮优先回应“系统性检查 software 模拟并清理”，暂停新的性能采集。对象为当前 `feature/malos/swan_performance`，源码基线 `85a39824d` 加未提交修改；实机是 **AYN Thor / Adreno 740**，不是 Swan/A840。具体身份和校验见 [manifest](evidence/gcn-software-audit-20261002/manifest.json)。

跨平台长期实现说明见 [GCN 模拟参考](../../guides/gcn-emulation.md)。本报告保留本次设备与时点证据。

## 结论与已完成清理

1. 血源此前确实继承了 MHR 实验留下的全局 `debug.shadps4.software_interp=1`。12:08:49 的 `GuestFault` 前报 `software interpolation currently requires VS-only triangles`；附近编译记录含 HS/LS，支持“不适用绘制阶段组合”的定位，但日志没有完整 pipeline key，不能据此证明具体 primitive 类型。不是本次 GPU query 错误，也没有本次 GPU DeviceLost 的证据。
2. 已清空该属性并重启血源。当前 PID 13762 的启动日志以及实际 shader cache profile 均确认 `emulate_fragment_interpolation=false`。**这是已在设备生效的清理。** 没有发现另一项误开的软件插值/强制整数展开开关。
3. 源码增加游戏隔离：`software_interp` 必须等于当前完整 CUSA ID 才能请求实验；旧值 `1`、不匹配游戏、空 ID/通配符均不生效并对非空请求记录警告。未来 MHR 定向诊断可用 `debug.shadps4.software_interp=CUSA34119`，仍受硬件能力及 VS-only triangles 限制；本轮没有开启它。probe 中显式设置 `Profile` 的测试不受影响。
4. 修正两处 Int64 `native` 日志，改为 **driver SPIR-V Int64**；增加启动时的实际 compiler policy 日志。Vulkan feature 支持不能当成硬件单指令支持：当前 Mesa IR3 明确对 Int64 做 lowering。
5. 必需的 GPU 兼容转换、CPU 内存一致性维护和用户画质设置保留。没有用跳过运算或伪造能力来提高帧率；本轮无 FPS 改善声明。

源码防复发改动在新构建中；运行中的 APK 仍为 `9483b4f4…`，其属性已经关闭。新包的构建/安装状态见文末。

## 实际设备状态

读当前 process environment、Android properties、UI 全局/每游戏配置、启动日志及持久化的**本次 compiler profile**，再用 `android_gcn_capabilities_probe` 加载与 app 相同的私有 Turnip 查询生产 `Instance` 能力。probe 不录制、不提交 GPU 工作。[能力输出](evidence/gcn-software-audit-20261002/gcn-capabilities.txt)、[属性](evidence/gcn-software-audit-20261002/debug-properties.json)、[完整 profile](evidence/gcn-software-audit-20261002/shader-cache-summary.json)。

| 项目 | 实际状态 | 含义 |
|---|---|---|
| `software_interp` | 属性空，profile false | 没有额外 GS 插值补偿 |
| `lower_int64` | 属性 `0`，`support_int64=true` | shadPS4 不强制 u32-pair；Turnip 仍自行 lowering |
| `wave_reduction` | 无覆盖，clustered=true | 没切回整波 reduction 的旧 shuffle 展开 |
| compute subgroup | device 默认 128；实际 profile 64，required-size 支持 | 没触发主仓 wave64 LDS/barrier 拼接路径 |
| NVIDIA 手动插值、clip discard、额外 LDS barriers | 全 false | 非本机使用路径 |
| 强制扩展隐藏、RenderDoc、GPUReshape | 未启用/无覆盖 | 没有借诊断层强制进入能力回退 |
| GPU query / detailed / per-draw fine / draw log / pass log | 全 off/0 | 本轮清查时没有这些采样开销；常驻 CPU ring 保留 |
| upload 诊断 /忽略 storage dirty / DMA bounds 实验 | off | raw sync/copy、compute-fill、SRT batch 正常启用；SRT verify off |
| 画质 | render 50%，texture medium，FSR1，强制禁用 MSAA | 是用户已有画质设置，不是 CPU 软件光栅化 |
| shading quality / FDM | High；guest FDM off | 本轮不是 XR/ETFR/FDM 工作负载 |
| direct memory access | true | shader 动态资源访问功能，不代表省去所有同步或回读 |

没有在当前进程中发现 `TU_`/`MESA_`/`IR3_`/`SHADPS4_`/`VK_` 环境覆盖；实际驱动为 `Mesa 26.3.0-devel (git-9b8a35676e)`，私有 `vulkan.ad07xx.so` SHA `82943754…`。没有 SwiftShader/llvmpipe CPU renderer 的当前选择证据。

## 已编译 shader 的静态交叉核查

只读拉取本次血源 cache，校验 **1045 个 blob 的 magic/version/长度/XXH3**，反汇编 **375 个 SPIR-V 模块**。cache tar SHA `e0ceb4c5f5ad1663fb8828c62f730bb04406427d0dd10e12447b6826a468f313`。目录在本地 `build/validation/gpu-query-recovery-20261002/`，`audit_cache.py` 可重做；原 tar 和逐模块 SPIR-V 文本保留。

- 36 compute、172 fragment、153 vertex、5 TCS、7 TES、2 geometry。这是编译模块集合，不是每帧 draw 数；GS 模块本身不能证明软件插值，必须结合 profile（当前 false）。辅助 rect/quad stages 存在另一种缓存记录中，不能用 `.spv` 模块数量推算其 draw 次数。
- **0 个 `ssbo_shmem`**：当前缓存未触发 LDS→storage buffer；此生产 emitter 对每个这类 buffer 都使用该名字。
- 0 个 FP32 min/max 回退所需的 signed/unsigned integer atomic min/max 指令；不能将此结论扩展到未加载场景或其他游戏。
- 5 个模块声明 `gds_buffer`。这是 PS4 GDS 的 GPU buffer 映射。
- 375 个模块都声明 Int64 类型，但只有 **2 个模块**含非恒定的 64 位结果指令（shift、bitcast、OR、phi）。类型声明不能算作运算次数；这些数也不是 ISA 周期、动态调用数或 GPU 时间。
- 当前 profile 关闭 software/manual interpolation、clip emulation、LDS barriers、buffer alignment fixup、UNORM fixup。

没有用这些静态统计直接宣布性能瓶颈；尤其不能把旧 `software_interp=1` 的采样和现在不同场景比较为 A/B。

## 兼容路径总表

“GPU 展开”指编译后在 Adreno 执行，CPU 只做翻译/录制；它仍可能增加指令、寄存器、带宽或额外 pass。

| 路径 / 代码入口 | 执行位置与触发条件 | 当前判断 / 清理策略 |
|---|---|---|
| GCN→IR→SPIR-V；EXEC/VCC/SCC、打包格式、MBCNT/CMP_CLASS 等：`shader_recompiler/recompiler.cpp`、`frontend/translate`、`ir/passes/lower_*` | CPU 编译，GPU 执行展开的指令；跨架构基础工作 | 不能整体关闭；应优化热 shader 的展开和重复翻译。不是逐像素 CPU GCN 解释器 |
| GS per-vertex/barycentric 插值：`backend/spirv/emit_spirv_interpolation.cpp`、`vk_graphics_pipeline.cpp` | 额外 GPU geometry stage 和更多 varying；显式实验开启且 FS 需要 | **关闭并隔离到指定游戏**。当前 Turnip 不提供 KHR barycentric/AMD explicit vertex；普通 smooth/flat 仍使用 Vulkan 插值，不等于完整支持所有 GCN 显式顶点语义 |
| NVIDIA manual interpolation / clip-discard / LDS barriers：`vk_pipeline_cache.cpp` | fragment ALU/discard/barrier，按 driver ID | 本机全部不启用，保留其他驱动兼容逻辑 |
| Int64：`backend/spirv/spirv_int64.cpp`；Mesa `ir3_compiler.c` / `ir3_nir.c` | 主仓可做 u32-pair；当前提交 Int64 SPIR-V 后由 Turnip `nir_lower_int64` 展开 | 没有一键变为 Adreno 原生 Int64 的模式；优先减少不必要的 64 位表达式，不能改地址/原子语义 |
| FP64→FP32：`ir/passes/lower_fp64_to_fp32.cpp` | Turnip `shaderFloat64=false` 时主仓降精度到 GPU FP32 | **不是完整的软 FP64 精确模拟**，是既有精度边界；不是“软件双精度很慢”的证据，不能伪造 Float64 feature |
| wave64 ballot/readlane：`lower_wave64_pass.cpp`、`emit_spirv_warp.cpp` | 不满足 64-lane compute 时共享内存+barrier；readlane/DS swizzle 也可展开 shuffle | 当前 compute profile=64，LDS 拼接不触发。单个 shuffle/quad permutation 仍是 GPU 语义适配，clustered 也不保证一条 ISA 指令 |
| compute LDS→SSBO：`shared_memory_to_storage_pass.cpp` | 请求 LDS>32 KiB，或无 explicit layout 且混合类型时，用全局 storage buffer 替代 | 潜在高成本路径；本机 explicit layout=true，本次缓存无 `ssbo_shmem`。不能把整个实现删掉，否则更大 LDS 的游戏不正确 |
| fragment LDS→私有值：`fragment_lds_pass.cpp` | 证明 lane-private 访问后转寄存器/SSA | 优化路径，不引入 fragment 全局内存/barrier；不满足证明时拒绝，不做任意 CPU LDS 模拟 |
| GDS：`BufferType::GdsBuffer`、`vk_rasterizer.cpp` | 共享 GPU storage buffer，GCN GDS 没有通用 Vulkan 对等资源 | 本次 5 模块；保留一致性和原子操作，热 shader 再评估 |
| FP32 atomic min/max：`emit_spirv_atomic.cpp` | 无 `VK_EXT_shader_atomic_float2` 时展开 integer atomics | 当前能力选择回退，但本次缓存没有使用证据。代码 `OpSelect` 两侧均发原子操作的 FIXME 是额外开销/正确性待查项，应独立以并发 GPU 对照修复，不能宣称本轮已修或已归因 |
| 64-bit atomics | buffer 支持，shared 不支持；生产 emitter 检查 | 不支持时抛明确错误，**没有用两个 32 位原子冒充 64 位原子** |
| cube face/三元 minmax：`emit_spirv_image.cpp`、integer/floating_point emitters | AMD 扩展缺失，比较/select/两级 minmax 等 GPU ALU | 当前适用；普通转换可被驱动优化，不能跳过；需要热点 ISA 后再决定专门改写 |
| storage image LOD：`vk_rasterizer.cpp`、`emit_spirv_image.cpp` | 无 AMD storage LOD 时按 mip 建 view/descriptor array，store 索引选择 | GPU descriptor/CPU 绑定开销；不是 CPU 逐像素处理。storage read+LOD 的某一未支持分支仍明确拒绝，不是已经完整 fallback |
| RectList/QuadList、环访问、GS/HS/DS：`vk_graphics_pipeline.cpp`、`ring_access_elimination` | GPU 辅助 TCS/TES 和阶段接口转换；Vulkan 没有原生 rect/quad list | 可能增加阶段和管线成本；按真实 draw 数评估，不能直接关掉额外阶段 |
| depth range、user clip、scaled min/max blend：`lower_user_clip_planes.cpp`、`emit_spirv_special.cpp`、`emit_spirv_context_get_set.cpp` | 特殊 guest 状态时增加 VS/FS ALU/clip 距离；当前无 unrestricted depth range | 必需且条件触发；不等于所有 draw 都走最慢模式 |
| texture tiling/detiling、深度/格式互拷：`texture_cache/tile_manager.cpp`、blit helper | 主要 GPU compute/transfer；不同硬件内存布局无法原样采样 | 保留；优先减少重复转换、复用 scratch、减少 pass break，不可删去布局转换 |
| medium 纹理缩放/ASTC或BC7 重编码：`image.cpp`、`blit_helper.cpp` | GPU compute；asset 需要缩放且不能直接丢 mip 时 | 当前 medium 策略适用，依纹理触发；会影响上传/加载成本，不是 CPU BC 解码。原生 BC 和 ASTC sampled 能力已确认 |
| FSR/SGSR、ETFR/FDM、影院合成 | GPU 后处理/合成；用户选项和 XR 状态决定 | 当前 FSR1 开，guest FDM off；不是 GCN CPU 模拟，不擅改用户画质 |
| copy shader HLE：`vk_shader_hle.cpp` | CPU 读控制表/合并区段，GPU buffer copy；CPU mirror 或完成后 download/commit 维护 guest RAM | **真实 CPU 参与路径**。当前 `hle_guest_copy=on`，观测 mirror 58,339 regions / 5,842,153,072 bytes；commit 0。这是会话累计量，不能推算稳态带宽。直接关掉会破坏血源蒙皮矩阵一致性 |
| PM4 处理、SRT、资源缓存和页脏跟踪：`liverpool.cpp`、rasterizer/buffer/texture cache | CPU 处理 guest GPU 命令与共享内存模型 | 是可能的 CPU 提交瓶颈，不是 GCN shader 在 CPU 执行；查重复查表、锁、同步和 Vulkan 录制 |
| FEX x86→ARM、音视频解码 | CPU 架构翻译/媒体，不属于 GCN | 单独归因，不能混算为 software GPU |

## 后续优化优先级

- 首先保住已经清掉的 GS 实验污染；旧采样不能证明关闭后提升多少，需要相同场景/同驱动/同包的测量。
- 当前更值得测：GCN host 提交中的逐 draw CPU 开销、copy HLE 镜像、纹理转换/pass break、特定热 shader 的 IR3 指令数/寄存器与内存访问。它们是候选，不是本轮已证实的根因。
- LDS spill / FP32 atomic fallback 在本次 375 模块里无证据，暂不把时间花在关闭它们；以后触发时再做专门语义和性能对照。
- 同步保留 GPU query 修复和 LiteP 工具修改。新的细粒度采集因 GuestFault 失败，其原始证据保留；用户改为优先本次清查后，没有重新制造一份采集结果。

## 验证与边界

- Android host、能力 probe、实验隔离测试已编译。
- Android 实机及 macOS 各 **11 checks / 0 failures**，覆盖旧布尔值、错误游戏、正确游戏、空值和非法 ID。
- 能力 probe 使用设备原有测试目录里与当前 app 相同 SHA 的 host/driver，不把候选 host 能力冒充已安装包。读取能力没有改游戏设置或提交渲染工作。
- 新日志和 title gate 不改变当前 `false` profile，缓存不需要仅因新增日志重建。开启实验导致 profile 变化时沿用已有完整 profile 校验。
- 本轮没有修改 Mesa、Foundation 或游戏/存档；没有 commit/push。候选 APK 状态单独记录于构建 manifest，不用构建成功代替游戏性能验收。

构建交付：候选 `build/validation/gpu-query-recovery-20261002/shadps4-gcn-audit.apk`，APK SHA `00de1f25f2d8ecc6bcc3b89b049cae80778805380b25e4853e152eb561d0d14e`，包内 host SHA `85a43613893651d6eb3c4a062edfaa9085f28c830f15020ba4465fd964644603` 与本地构建逐字节一致；Turnip 仍 `82943754…`。APK 构建 293 tasks 成功，**候选未安装**，当前已清理属性的血源会话持续 Running（末次 host_present=38,332），保留给用户操作。[包校验](evidence/gcn-software-audit-20261002/package.json)。
