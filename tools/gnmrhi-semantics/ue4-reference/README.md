# 与 UE 接口直接关联的 KH3 GnmRHI 参考实现

这版直接使用 UE 的头文件、继承关系和 `RHI*` 方法签名。目标语义是 **KH3 CUSA15072 01.10 的 `++UE4+Release-4.17` 分支**；公开接口由本地 **UE 4.18-GameWorks `b50e6338a7c5b26374d66306ebc7807541ff815e`** 源码校对。没有取得 KH3 原始 GnmRHI 源码，也没有在完整 UE 4.17/PS4 SDK 工程中编译。

它是供读代码、继续逆向和移植时参考的 **UE 风格实现片段**。不是 Epic/Sony 原代码，也不是可以直接替换 PS4 后端的完整模块。`FGnmCommandContext` / `FGnmDynamicRHI` 仍是抽象类，未恢复的方法保留继承来的纯虚要求。

## 从哪里读

| 文件 | 与 UE 的连接 |
|---|---|
| [UECallSite.cpp](UECallSite.cpp) | 使用真实 `FRHICommandList` 的 compute / graphics 调用示例 |
| [GnmRHIReference.h](GnmRHIReference.h) | `FGnmCommandContext : IRHICommandContext`、`FGnmDynamicRHI : FDynamicRHI`；shader、UB、VB 继承相应 `FRHI*` 类 |
| [GnmCommands.cpp](GnmCommands.cpp) | shader / UB setter、packed resource map、SGPR / LCUE 描述符绑定、direct / indirect compute |
| [GnmResources.cpp](GnmResources.cpp) | `RHICreateUniformBuffer`、资源引用、loose shadow / upload、Static / Dynamic / Volatile Lock |
| [GnmGraphics.cpp](GnmGraphics.cpp) | BSS / stream / VS、PS、GS参数资源setter、vertex→ES路由、ring快照、graphics LCUE、direct draw |
| [GnmGraphicsTypes.h](GnmGraphicsTypes.h) | 真实 UE shader / declaration / BSS 派生对象和 graphics 状态 |
| [GnmGraphicsCore.h](GnmGraphicsCore.h) | 可独立编译的共享 C++14 算法：descriptor、Set/Update、普通/Neo linker、LCUE selectors、draw 状态恢复 |
| [GnmStagesCore.h](GnmStagesCore.h) | copy shader/profile、GS ring/片上预算、六stage差异、LS/HS常量与提交重放 |
| [UE 与二进制逐层对照](../../../docs/gnm/kh3-gnmrhi-ue-reference.md) | UE 源码位置、KH3 模块偏移、时序、版本边界和新发现 |
| [Graphics 深入分析](../../../docs/gnm/kh3-gnmrhi-graphics.md) | 本轮新增入口、BSS / stream 布局、VS→PS linkage、LCUE 包序列和 GS/tess 边界 |
| [GS/ES 与 tessellation 深入分析](../../../docs/gnm/kh3-gnmrhi-stages.md) | r6的ring owner/descriptor、copy VS、LS/HS/DS资源关联、公式、证据与未闭合入口 |

代码使用 C++14 范围内的语法及 UE 类型；没有依赖前一版的 C++20 `std::span` / `std::optional`。新代码没有另造 `FRHICommandList` 或 `FDynamicRHI` 替身，也没有拷入 UE 头文件。

## 关联关系

```text
Renderer / FShader 参数设置
  FRHICommandList::SetShaderParameter / SetShaderUniformBuffer / DispatchComputeShader
    Bypass: 直接调用 GetContext()
    非 Bypass: FRHICommand...::Execute → GetContext()
      IRHICommandContext 虚方法
        FGnmCommandContext::RHI...              ← GnmCommands.cpp
          CPU shadow / UB 引用 / dirty mask
          反射映射 → SGPR 或 staging → LCUE 快照
          SDK dispatch / PM4 录制

RHICreateUniformBuffer / RHILockVertexBuffer
  GDynamicRHI → FDynamicRHI 虚方法
    FGnmDynamicRHI::RHI...                      ← GnmResources.cpp
      FGnmUniformBuffer / FGnmVertexBuffer      ← 真实 FRHIResource 派生对象
```

这里的 compute 片段是普通图形 context 的 compute 路径。通用 UE 的 async compute command list 另走 `GetComputeContext()`；没有把本片段冒充完整 async compute 队列实现。

## 这版保留的 KH3 行为

- `RHISetShaderParameter(CS, ...)` 忽略传入 shader / BufferIndex，在入口将 BaseIndex、NumBytes 各截为 uint16，再写 context 的 CS shadow；不是给每个参数分配 GPU buffer。
- `RHISetShaderUniformBuffer` 当场绑定 UB 常量 descriptor；保存原 UB 引用、置对应 dirty 位；到 dispatch 才按 shader 的 packed maps 展开 texture / SRV / sampler。
- 空 UB 或 constantSize=0 使用全局默认 UB 的 **常量部分**。参考构造函数要求传入这个对象，不能把默认 descriptor 猜成全零。resource slot 仍保存原 UB，因此 resources-only UB 的资源仍可在 dispatch 时展开。
- `RHISetComputeShader` 更新程序/反射、标记 LCUE resources dirty，程序指针不同才累积 shader dirty；该函数本体没有把所有 UB 或 loose constants 标脏。参考版不擅自补这种策略。
- loose upload、UB backing、LCUE table 是三个不同的存储/寿命问题；新表不会因后续 CPU staging 修改而改变。
- Dynamic 普通 buffer 三份、每次写 Lock 轮换；Dynamic|UAV 单份；Volatile 每次写 Lock 新分配。间接 dispatch 在执行时读当前 backing。
- `FRHIResourceTableEntry` 的解码直接调用 UE 定义；UE `SF_Compute=5` 与 GNM 硬件 CS=0 分开。
- `RHISetBoundShaderState` 重标 graphics UB masks、vertex dirty 与 discard-loose-extent；它不强制 loose dirty。VS/PS 的 Set/Update 条件包括 emitted bit 和不同长度的 native program key。
- stream 在 `RHISetStreamSource` 执行时快照 current backing；FirstInstance 变化会重建 vertex descriptor。direct indexed draw 在执行期间取当前 index backing。
- normal graphics LCUE 实现了反射 +4…+f 的分支差异；两种 shader 的 SDK Set/Update 在两次 table commit 之前，最后匹配 VS/PS semantic WORDs，再 draw。
- GS路径中UE vertex的UB/loose引用仍属逻辑VS，实际descriptor送ES4；GS3绑定自己的资源，copy shader占VS2并负责与PS匹配。ES/GS程序与表先于copyVS/PS提交。
- ring backing归全局manager：ESGS/GSVS各4MiB、64KiB对齐；布局改变时另在DCB NOP内生成192字节descriptor表。on-chip模式使用预算、LDS寄存器与scalar参数。
- direct draw 完成后恢复 NumInstances=1、BaseVertex=0；topology/index type 缓存保留。只接受已闭合的 triangle list/strip 与 line list。

## 仍需要平台实现的部分

`IGnmReferencePlatform` 是明确的接入边界：

| 接口 | 适配器必须提供的语义 |
|---|---|
| `Allocate` | CPU/GPU 地址、容量和所有权；临时 UB / Volatile 的 1 KiB 对齐与 8 MiB 块；loose 的 64 KiB 块；独立分配对齐和 LCUE rollover；不能只返回栈地址 |
| `RetireUniform` | 持久分配进 ticket 队列，完成后释放；临时池不随单个 UB 析构立即释放 |
| `ResolveDescriptor` | texture reference 解析、sampler；buffer SRV 从当前 multibuffer backing 刷新地址；未恢复分支明确拒绝 |
| `EmitPacket` | 预留空间、跨块处理、按给定 dword 写 DCB；保留 referenced backing |
| `SetCsShaderWithModifier` / `Dispatch*` | SDK 层调用及真实 packet buffer 管理 |
| `FlushPriorGraphics` / `FlushAfterCompute` | label 的真实 DCB NOP 存储与 EOP/EOS → WAIT → ACQUIRE 顺序；已识别包字见 [C++20 实现](../cpp/gnm_rhi_semantic.cpp) |
| `FinishComputeBookkeeping` | dispatch 后尚未完全还原的纹理/状态处理，不能用于宣称完整状态闭环 |
| `SetGraphicsShader` / `PrefetchGraphicsShader` / `DrawIndex*` | SDK 的 shader Set/Update、预取、draw emitter 与容量处理；shader header 地址与 code 地址不同 |
| `ApplyGraphicsSelectionSideEffects` | PS enable/shared register 和 pending PS资源等固定状态；GS模式转换与ring寄存器现由共享core实现，不能重复清理 |
| `GetGeometryRings` / `EmbedGeometryRingTable` | manager backing及两个aux descriptor；在DCB预留NOP+192byte表并保持至GPU完成，不能把普通LCUE upload当成DCB内嵌表 |
| `SetExportShader` | 将ES native body复制，按已计算的word14替换，再发SetEsShader；不能修改共享shader header |
| `GetRawIndexType` / `ResolveCurrentIndexAddress` | 来自真实 index-buffer/multibuffer 对象；后者必须在 draw 执行时取当前 backing |

shader创建/反射解析、UE HS/DS选择、patch draw、tess factor backing、非compact copy辅助表的全部填充、UAV setter、完整提交/flip/query、并发调度与defrag不在此片段中。VS/PS及GS/ES的UE绑定与graphics LCUE已实现，资源解析仍接`ResolveDescriptor`。BSS入口仅拒绝尚未闭合的tessellation，以及没有alternate ES的GS组合。LS/HS通过`GnmStagesCore.h::CommitLsHs`重放给定状态，未伪造UE选择入口。已还原的提交/query/回收行为见[第一篇](../../../docs/gnm/kh3-gnmrhi-reconstruction.md)。

规范化对象使用 `TArray` / `TRefCountPtr`，**不可 reinterpret_cast 到游戏内存**。UB layout 在参考版内部复制持有，KH3 原对象保存外部 layout 指针；该差异是为了让示例明确管理寿命。CS 的32个UB槽仍是mask位域参考容量；graphics使用新确认的20槽间距/掩码。构造初值、边界 `check` / `assert` 和资源容量检查是参考代码防误用措施，不是二进制中都存在的检查；关闭检查的 Shipping 配置不在验证范围。graphics 使用前需填入完整的 shader metadata、staging、loose shadow、extras、初始 draw state 与 Neo/prefetch 设置；默认值不代表恢复了 KH3 context 构造函数。

## 验证

对本机已保存的原始 UE 通用头文件运行：

```powershell
python tools/gnmrhi-semantics/ue4-reference/check_ue_interfaces.py --ue-runtime-root build/validation/kh3-gnmrhi-20261016/ue418/files/Engine/Source/Runtime --report build/validation/kh3-gnmrhi-20261016/ue-interface-check-r6.json
cmake --build build/validation/kh3-gnmrhi-20261016/cpp-build --config Debug
ctest --test-dir build/validation/kh3-gnmrhi-20261016/cpp-build -C Debug --output-on-failure
```

检查29个override的返回值和参数类型，报告保存源头文件SHA256。共享graphics/stages core以C++14编译，6组graphics与8组stages场景通过；CTest连同旧13场景为3/3通过。**签名检查和共享core测试不等于UE引擎编译**，不验证`GnmGraphics.cpp`的全部UE集成，也不验证游戏GPU执行。完整UE4.17/PS4 SDK工程未构建。

接入有权限的 UE 4.17 工程时，应先用其真实 RHI 头文件复核这些方法，再补齐其余纯虚方法、shader loader、分配器和平台模块注册；最后做双 dispatch 的 backing 快照、resources-only UB、shader 切换重绑定和多次 Lock 场景验证。
