# Tetris UE4.26：GNM graphics与读回参考子集

[Graphics/draw、绑定和LCUE](../../../docs/gnm/tetris-ue426-gnmrhi-graphics.md)；[读回与 KH3 比对](../../../docs/gnm/tetris-ue426-gnmrhi-readback.md)；[Odin 3 闪烁验证](../../../docs/validation/android-native-host/tetris-exposure-readback-20261016.md)。

`GnmReadbackCore.h` 是基于 CUSA13427 实际 eboot 指令重写的 C++14 语义子集，不是 Epic/Sony 源码，不是完整 UE 插件。未用 4.18 头文件冒充 4.26 ABI；这里没有继承或伪造 `FRHI*` 基类。名称对应 UE 的职责，平台/资源接口明确保留为抽象接入点。

| 本文件 | UE / 二进制对应 |
|---|---|
| `RHIMapStagingSurface` / `RHIUnmapStagingSurface` | GnmDynamicRHI native 实现 `0xfa13d0` / `0xfa1430`；映射返回 guest 地址，不解引用成 host 指针 |
| `FGnmGPUFence::Clear/Poll` | `0xf8c380` / `0xf8c390`；armed + 32-bit label 等值检查 |
| `RHIWriteGPUFence` | context `0xf991a0` → SDK `0x3f37180`；实际 6 DWORD EOP |
| `FEyeAdaptationTextureHistory::Advance` | Renderer `0x2170ba0`；三纹理/三读回槽、读 next/写 current、始终轮换 |
| `PackOrdinaryViewExposure` | `0x2171340` 普通启用分支 + `0x22e8bbc` 常量写入；保留异常正数的实际行为 |
| `TextureReadback` / `ReadbackBackend` | 待接完整 UE 的 RDG、`FRHIGPUTextureReadback`、RHI command list 和实际 GPU/内存系统 |

`pending_writes` 仅模拟 RHI command enqueue/execute 的状态，不提供多线程原子实现。native 的 `Clear` 不清 label；`Poll` 不检查 pending，检查在外层。Map 本身也不等 fence，外层 `0x17c8110` 才处理必要的 RHI thread flush；这些职责不能合并。

真实后端必须按 **copy/dispatch → GPU 完成 → 数据对 CPU 可见 → 完成标签可见** 的契约实现。测试中的 `Complete(..., false)` 故意只发布标签，用来重现 payload 残留，并非推荐实现。

构建和测试：

```powershell
cmake -S tools/gnmrhi-semantics/cpp -B build/gnmrhi-reference
cmake --build build/gnmrhi-reference --config Debug
ctest --test-dir build/gnmrhi-reference -C Debug --output-on-failure
```

2026-10-16：MSVC Debug 编译通过，CTest **6/6**（三个已有KH3目标、readback426、graphics426、stages426）。读回测试覆盖实际EOP字、fence代次与pending、三槽顺序、禁用仍轮换、CPU残留导致Inf及写回恢复、地址映射。完整UE4.26/PS4 SDK未构建。

## Graphics参考

`GnmGraphicsCore.h` 实现本版reflection、vertex/UB/loose、普通VS/PS LCUE、userData PM4及direct/indirect draw；`GnmGraphicsReference.h` 把这些算法接成UE职责命名的pipeline、shader setter、Prepare与draw。没有继承伪造的UE基类。

```cpp
Tetris426::Graphics::CommandContext<MyBackend> context(backend, capturedConstantCapacity);
context.RHISetGraphicsPipelineState(pipeline, applyStaticUniforms);
context.RHISetStreamSource(0, &vertexBuffer, streamOffset); // 本版无Stride实参
context.RHISetShaderUniformBuffer(vertexShader, slot, uniformBuffer);
context.RHIDrawIndexedPrimitive(indexBuffer, baseVertex, firstInstance,
                               numVertices, startIndex, numPrimitives, numInstances);
```

`MyBackend` 必须提供源码注释中的接口：实际shader Set/Update、draw、prefetch、fixed state、引用保留、static UB校验，以及constants/table分配。表和常量须当场复制到新地址并保留到GPU退役；不能返回可继续覆盖的CPU shadow地址。`Emit`已给出userData真实PM4字，其余SDK emitter不得以空函数冒充成功。`graphics426_tests.cpp`的recording sink只是CPU测试器，不执行GPU。

输入的native program identity/key、shader reflection、prepared vertex elements、staging prefix/extras由解码者提供；默认0不证明游戏构造器清零。公共对象字段是normalized表示，不应memcpy到guest内存。pipeline仍由调用者/Retain接口保证存活。context现支持普通及GS pipeline；PSO stage生成、GS/copy/ring/完整LCUE阶段分支已补入。UE HS/DS选择桥仍拒绝，LS/HS只接受调用者提供的重放状态。shader loader、extended copy辅助数据、ring退役和queue submit worker仍待还原。

8组graphics测试覆盖实际包字、反射子表offset、immutable snapshot、shader Update资格、UB sampler→texture→SRV→PS UAV、loose extent、pipeline到draw、间接参数20字节stride及KH3差异。另对全部65536个PS semantic WORD的普通/Neo组合比较两版参考算法；不是执行原始游戏二进制。

原始证据身份验证：

```powershell
python tools/gnmrhi-semantics/ue426-reference/verify_evidence.py path/to/tetris-eboot.elf
```

`evidence.json`保存71个FDE范围的SHA256，ELF身份不符时拒绝。未提供原始ELF，也未自动导入符号。当前Tetris依据x86-64人工重写，未取得Hex-Rays伪码。

## GS/ES 与 LS/HS 扩展

[阶段分析与 KH3 比对](../../../docs/gnm/tetris-ue426-gnmrhi-stages.md)；`GnmStagesCore.h` 为独立 C++14 算法，`stages426_tests.cpp` 有9组验证场景。

GS 使用已解码的 `Shader.export_program/export_reflection`、`geometry_metadata/copy_program` 和原始 header bytes；调用 `pipeline.ConfigureStages()` 得到与工厂对应的 ES stage、mode、LDS/group 与 ring 标志。`context.rings` 提供真实 backing，`ring_seed` 和 extended copy staging 的未知内容由 capture 提供。两块 backing 初始化各4MiB且4MiB对齐，不能沿用KH3的64KiB对齐推断。

backend 另需 `ExportShader(program, patchedBody, modifier)`、`HullShader(program, patchedBody, config, update)`；`Shader(Stage::Geometry/Local,...)` 分别接GS和LS的SDK操作。所有body是临时快照，回调必须在返回前消费/复制。stage-enable、GS mode、ring layout和userData包已由core记录；`BeginPipeline`仅准备backend，不重复发这些包。未知SDK内部emitter仍是明确接入点，不允许以空函数冒充完成。

LS/HS使用 `StageLcue` 的给定状态重放：填入stage6/5的program/reflection/dirty/Update、domain的VS或ES、`tess`元数据及`hull_config_previous`。它没有自动推导未恢复的HS Update条件；该config的高12位由输入保留。`Commit`按GS块→tess块→VS/PS尾段录命令；组合模式产生的ES dirty保留到下次。不等于已实现游戏UE tess入口、TF_MEMORY_BASE或GPU退役。

验证是CPU模型和固定指令字段检查，没有执行原游戏二进制、完整UE4.26 SDK插件或新的设备GS/tess画面。
