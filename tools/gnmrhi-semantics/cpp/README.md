# KH3 GnmRHI：C++ 语义实现

这是基于 KH3 CUSA15072 01.10 / UE4.17 分支反汇编与伪码写成的 **C++20 静态库**。已恢复的算法有函数体，可以编译、执行示例、替换后端；尚未恢复的 Sony SDK 操作通过接口交给适配器。它不是 Epic 原始 GnmRHI 源码，也没有接入 shadPS4 正式渲染路径。

建议先读 [gnm_rhi_semantic.h](gnm_rhi_semantic.h) 的数据模型，再读 [gnm_rhi_semantic.cpp](gnm_rhi_semantic.cpp) 的 `GnmRhi::PrepareCompute`。完整证据见[绑定语义分析](../../../docs/gnm/kh3-gnmrhi-binding-semantics.md)与[生命周期分析](../../../docs/gnm/kh3-gnmrhi-reconstruction.md)。这些 C++ 类型便于阅读和重放，**不能 reinterpret_cast 到 guest 内存**；二进制布局在 [v2 符号包](../../../guest/games/CUSA15072/01.10/symbols/index.json)。

## 已实现的模块

| C++ 入口 | KH3 偏移 / 范围 | 实际覆盖 |
|---|---|---|
| `LooseConstants::Write/Commit` | 368f70 / 36d740 | shadow、dirty、更新末端、旧 extent、16 字节对齐、大小分配路径、buffer descriptor |
| `CommitResourceTables` | 36db40 等 | 8/16/8 packed map、UB 引用展开、texture→SRV→sampler 顺序、清整个 stage dirty mask |
| `StageBindings::Bind` | 36b0f0 / 36db40 / 386680 等 | 未使用 / table / user SGPR 路由；4/8 dword 描述符；实际 `SET_SH_REG` 编码 |
| `StageBindings::CommitCompute` | SDK 4c39b50 主路径 | staging 前缀快照、五个表指针 selector、四个子表位移、清 resource dirty |
| `RefreshBufferSrv` | 36b0f0 buffer 分支 | 用当前 backing 替换地址低32 / 高12，其余描述符位保留 |
| `GnmRhi::DispatchCompute` | 36cfb0 | pre-flush→常量→资源→shader/LCUE→dispatch→可选 post-flush |
| `GnmRhi::DispatchComputeIndirect` | 3772d0 | 同类参数提交；LCUE 之前发 PFP_SYNC_ME / SET_BASE；SDK indirect dispatch |
| `FlushPriorGraphics/FlushAfterCompute` | 36d440 / dispatch 尾部 | 具体 EOP/EOS、WAIT、ACQUIRE 包字；range 清空 |
| `MultiBuffer` | 3a3010 / 39a100 / 3cbeb0 | usage 决定 1/3 槽；每次写 Lock 轮换或 Volatile 新分配；只读不轮换；Unlock 空操作 |
| `DeferredReleaseQueue` | 37d6210 / 37d6b20 | 用当前 ticket 入队；只释放 ≤completed 项；返回旧 ticket 后递增 |
| `ReadOcclusion/ReadTimestamp` | 3955d0 / 3a0d10 | 8/16 对 bit63 就绪与差值求和；时间戳非零、除800/911 |

上表描述的是各函数的已识别语义子集，完整 query 包装的 cache、issueFrame、等待超时不在这两个读取 helper 中。普通 UB 创建时的内存所有权、引用计数由调用者处理。MultiBuffer 接收已分配的槽，不代替原构造器的完整分配/映射过程。

## 构建和运行

在仓库根目录：

```powershell
cmake -S tools/gnmrhi-semantics/cpp -B build/gnmrhi-semantics
cmake --build build/gnmrhi-semantics --config Release
ctest --test-dir build/gnmrhi-semantics -C Release --output-on-failure
```

Visual Studio 等多配置生成器的示例程序位于：

```powershell
& build/gnmrhi-semantics/Release/gnmrhi_demo.exe
```

Ninja / Make 等单配置生成器的程序位于构建目录根部。只依赖 C++20 标准库；当前上传序列化要求小端宿主。

本轮实际验证：Windows x64，VS 18 2026 / MSVC 19.51，Release 编译成功，无编译警告；CTest 1/1，内部 **13/13 场景**通过。证据构建目录为 `build/validation/kh3-gnmrhi-20261016/cpp-build`。这是独立语义库的结果，不代表 shadPS4 桌面版构建或 GPU 渲染验证。

## 最小接入

以下片段与 [demo.cpp](demo.cpp) 使用同一接口。`backend` 和 `resolver` 可以是 recording adapter，也可以由 trace 分析器实现。

```cpp
kh3::gnm::GnmRhi rhi(backend, resolver);
kh3::gnm::LooseConstants loose(capturedShadowBytes);
kh3::gnm::ComputeContext context;
context.shader = &decodedShader;
context.loose = &loose;
context.shader_dirty = capturedShaderDirty;
context.uniforms = decodedUniformBindings;
context.descriptors.SeedStaging(capturedStagingPrefix);
loose.Write(parameterOffset, parameterBytes);
rhi.DispatchCompute(context, groupsX, groupsY, groupsZ);
```

本独立库的 dirty 与绑定对象仍由捕获或显式输入提供。后续已在 [UE 参考版](../ue4-reference/README.md) 补出 KH3 CS shader setter、UB setter 的引用与 dirty 行为；本库没有自动移入这些 UE 对象方法，全部 context reset / pipeline 路径也仍未闭合。indirect 调用的 backing 必须在 dispatch 时从当前 multibuffer 槽取得。

`Backend` 需要实现：

- `AllocateUpload/WriteUpload`：按种类处理 context 64 KiB 常量块、较大 transient 请求、LCUE 表。返回的地址须在被引用命令完成前保持有效；`extent` 不是底层物理池的实际保留大小。
- `EmitPacket`：按顺序录入这里已经恢复的 PM4 字；描述符直接写包含前导 NOP marker。
- `ReserveZeroedLabel8`：真实后端要在 DCB 的 NOP 载荷中预留对齐8字节，并由 CPU 清零。示例后端用独立合成地址记录这项操作，**没有伪装成真实 DCB 布局**。
- `BindComputeShader/DispatchDirect/DispatchIndirect`：接真实 GNM SDK helper，或记录调用供分析。SDK shader 机器码、modifier、prefetch 和完整 dispatch 包由适配器负责；indirect base 的已知包及其顺序在本库中实现。

`ResourceResolver` 将资源句柄变为 4/8 dword 描述符，负责 texture reference / 虚调用，以及 buffer SRV 的当前 backing。不要把旧 capture 的对象地址直接变成宿主指针。

`recording_backend.h` 给出可直接使用的 CPU 示例后端：保存分配、字节写入、PM4 和 SDK 语义调用。它以 `0xcd` 填充新块，专门暴露错误的“padding 应该为0”假设；这个值不是 KH3 初始化规则。运行 demo 可以看到两次 dispatch 的不同常量和表地址，以及旧表字节保持不变。

## 明确的实现边界

后续 [UE graphics参考](../ue4-reference/GnmGraphics.cpp) 已恢复普通VS/PS LCUE、vertex fetch与direct draw。本目录现在另编译 `gnmrhi_graphics_tests`，测试该UE参考实际包含的C++14共享core；Debug CTest为2/2，含旧13场景与新增6组。下列限制仍描述本目录原 `gnm_rhi_semantic` 库，没有把它的 `CommitCompute` 改成通用graphics入口，也没有完成UE工程构建。

- Python 检查器允许表项为 `None`；C++ emitter 在上传前要求整个 prefix 已知，缺项就拒绝，避免构造不存在的数据。显式 seed 的零只表示输入者提供了零，不表示 LCUE 构造器一定清零。
- reflection +c/+d/+e 的附加写入尚未实现。解析器发现这些 selector 被使用时，必须设置 `has_unmodeled_extra_bindings`；当前库会拒绝这条路径。shader prefetch 留给 SDK adapter。
- graphics LCUE commit、vertex fetch / tessellation、UAV、texture clear / resolve / defrag 未实现。图形 stage 的 `Bind` 可解码描述符路由，但 `CommitCompute` 对非 CS stage 明确拒绝。
- 分配失败、命令块溢出失败和 SDK 错误后的原始恢复流程没有复现。分析输入越界会抛异常；这不是声称 KH3 有同样的检查或事务回滚。
- 上传分配器的 bank / generation / 物理映射、提交 worker、完整 query 等待及 CPU/GPU 并发不在本库中。延迟释放队列是单线程语义核心，不增加未证实的锁规则。
- 测试验证还原算法、包编码和状态关系，没有执行游戏二进制、没有向设备提交这些命令，也没有宣布修复闪黑。

迁移到其他 UE PS4 游戏时应先确认该游戏的实际 profile。当前硬件 stage 表、反射布局、描述符常量和 query divisor 均固定于 KH3 证据，不能仅凭“同为 UE4”复用。
