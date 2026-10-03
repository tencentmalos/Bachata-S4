# Madeira / FEX 对 macOS arm64 的可复用性调研

日期：2026-10-03。分支：`feature/mac_fex_arm64`。本轮为源码调研与参考引用，**没有完成 macOS 编译、JIT 执行或游戏实测**。

## 结论

**可以作为 macOS arm64 原生版的移植参考，不能作为现成后端直接替换。** Madeira 已有 Darwin/iOS 构建分支、JIT 可写/可执行别名处理、缓存刷新及 Apple 寄存器约束相关代码。但它的实际 Windows 游戏执行路径是 Wine ARM64EC 下的 FEX DLL；其中相当一部分 iOS 适配只在这条路径启用，不能把 iPhone 游戏运行结果外推为原生 Mach-O FEXCore 已具备同样能力。

建议沿用本仓 `guest_cpu_api`、`guest_cpu_fex`、`GuestRuntime` 和 typed Orbis HLE，选择性迁入 Darwin 平台能力；保留目前已经工作的 Android FEX 依赖作为基线。无需引入 Wine、DXMT、WoW64 或 Windows ARM64EC ABI。FEX 只翻译 PS4 的 x86-64 guest，宿主服务、图形和音频继续以原生 arm64 运行。

优先级最高的是：**JIT 内存与发布、Darwin 异常上下文、Apple ABI/特性检测、16 KiB 宿主页，以及桌面入口接入现有 Session/FEX 运行时**。这些是实际源码缺口，不是简单增加一个 CMake 架构选项。

## 1. 固定来源与引用方式

| 对象 | 本轮固定版本 | 用途 |
|---|---|---|
| 主仓基点 | `397c4055152c63746eb1224afa2baeed30d0ba8b`，另有既存未提交改动 | 调研本仓现状；不能用基点 SHA 代替 dirty 文件的完整身份 |
| [Madeira](https://github.com/willfaust/Madeira/tree/ca3183ea3dfb0fd706aff1bea2abb871b5d27aec) | `ca3183ea3dfb0fd706aff1bea2abb871b5d27aec`，`main` | 新增 `references/Madeira`，只作独立参考 |
| [Madeira 所固定的 FEX](https://github.com/willfaust/FEX/tree/26859e184ad90f0e811d7f8bbd943a4b1573a2c3) | `26859e184ad90f0e811d7f8bbd943a4b1573a2c3`，`ios-port-2607` | `references/Madeira/FEX`，由 Madeira 自己的 gitlink 固定 |
| [现有生产 FEX](https://github.com/tencentmalos/FEX/tree/3f1f30a060b633980ed8e7674eb8d8997457edad) | `3f1f30a060b633980ed8e7674eb8d8997457edad` | `references/FEX`，本轮不改引用、不修改子仓源码 |

本轮 `git ls-remote` 确认 Madeira `main` 与 FEX `ios-port-2607` 当时分别指向上表 SHA。分支将来可能前进；复现应使用 gitlink。身份与审计文件哈希见 [source-lock.json](../data/madeira-fex-macos-arm64-20261003/source-lock.json)。

Madeira 使用 partial clone + sparse checkout，只物化文档、FEX 构建脚本和选定的宿主桥接源码；FEX 源码已检出，其他嵌套依赖未初始化。没有下载整个应用 DLL/资源集合，也没有运行 Madeira 构建脚本。参考子模块不会自动加入本仓 CMake。

## 2. 必须区分的两条 FEX 路径

Madeira 的 [README](https://github.com/willfaust/Madeira/blob/ca3183ea3dfb0fd706aff1bea2abb871b5d27aec/README.md) 描述了 Wine ARM64EC、FEX、Metal 图形层的组合。具体代码比项目简介更能说明复用边界：

| 路径 | 源码证据 | 对本仓的意义 |
|---|---|---|
| iOS 原生静态 FEXCore | [build/fex-ios/build.sh](https://github.com/willfaust/Madeira/blob/ca3183ea3dfb0fd706aff1bea2abb871b5d27aec/build/fex-ios/build.sh)，[FEXBridge.mm](https://github.com/willfaust/Madeira/blob/ca3183ea3dfb0fd706aff1bea2abb871b5d27aec/app/Madeira/FEXBridge.mm) | 构建目标和嵌入流程可参考；桥接包含最小 Linux syscall/ELF 执行逻辑及固定 A15 特性，不能直接当作生产 PS4 宿主 |
| 游戏用 ARM64EC FEX | [build/fex-arm64ec/build.sh](https://github.com/willfaust/Madeira/blob/ca3183ea3dfb0fd706aff1bea2abb871b5d27aec/build/fex-arm64ec/build.sh)，[ARM64EC/Module.cpp](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/Source/Windows/ARM64EC/Module.cpp#L983) | PE DLL 经 Wine 初始化，依赖 ntdll、TEB、Windows 异常及分配接口；不能直接链接进本仓 Mach-O 宿主 |

`Module.cpp` 1007–1041 行明确从 `WINE_IOS_JIT_RW/RX` 设置 `DualMap::WriteOffset`，注释指出实际 guest JIT 池由 StikJITHelper 拥有，而不是 FEXBridge 的池。静态库能构建与生产游戏路径已可靠工作，是不同证据。

目标结构应为：

```text
macOS arm64 桌面入口 / SDL 窗口
  → SessionCore / SessionBackendFex / GuestRuntime
      → guest_cpu_fex → 带 Darwin 适配的 FEXCore → PS4 x86-64 guest
      → 本仓 Orbis HLE、线程/TLS、文件、音频、输入
      → 现有 GCN → Vulkan → macOS 图形实现
```

Madeira 的 D3D→Metal 部分不会解决 PS4 GCN→Vulkan 的问题；图形继续由本仓已有实现承担。CPU 后端落地也不自动证明图形正确或性能更高。

## 3. 值得复用的实现及限制

### 3.1 构建与平台层

Madeira FEX 的 [CMakeLists.txt](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/CMakeLists.txt#L55) 接受 `Darwin`/`iOS`；[FEXCore 构建](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/FEXCore/Source/CMakeLists.txt) 及 [Allocator.cpp](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/FEXCore/Source/Utils/Allocator.cpp) 有 Apple 分支，可参考如何隔离 Linux allocator、头文件与宿主工具。

但仍有只为使路径可用而设置的默认值或占位逻辑：Apple `DetermineVASize()` 返回固定 39；[Syscalls.h](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/FEXHeaderUtils/FEXHeaderUtils/Syscalls.h) 将缺失的 `MAP_FIXED_NOREPLACE` 定义为 0，`tgkill(tgid, tid, sig)` 则发送给 `pthread_self()`，没有遵守目标线程参数。这些不能照搬为 macOS VM/线程语义；是否触及运行路径须由 embedder 决定。

### 3.2 JIT 的 RX/RW 地址、补链与缓存发布

最有价值的参考是 [DualMap.h](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/FEXCore/include/FEXCore/Utils/DualMap.h)、[CodeEmitter/Buffer.h](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/CodeEmitter/CodeEmitter/Buffer.h) 和 [JIT.cpp](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/FEXCore/Source/Interface/Core/JIT/JIT.cpp)：执行地址作为规范地址，写入经别名转换；不仅初次生成代码，运行时分支补链和元数据写入也要覆盖。Apple 原生分支包含 RW 侧清理与 RX 侧 `sys_icache_invalidate`。

宏与地址所有权必须逐处审计，不能给所有指针盲目加同一个偏移，也不能只在编译函数外包一层写保护切换便认为补链、信号内修补和回收已经安全。

Madeira 的 [JITAllocator.c](https://github.com/willfaust/Madeira/blob/ca3183ea3dfb0fd706aff1bea2abb871b5d27aec/app/Madeira/JITAllocator.c) 包含调试器分配、Mach 双映射和 iOS 私有记账接口。macOS 首选按 Apple 官方 [JIT 移植文档](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon) 设计 `MAP_JIT` 与写保护/回调模式；采用 Hardened Runtime 时配置 `allow-jit`，并考虑其单个 JIT 区域要求。具体选择仍需实机验证。无需把 StikDebug 或 iOS Jetsam 处理搬进 Mac 应用。

还应区分 PS4 guest 字节与生成的 AArch64 代码：前者由 FEX 读取译码，后者才由宿主 CPU 执行。不能把所有 guest executable 映射都放进 JIT 池。

### 3.3 Apple ABI：有代码，但普通 Darwin 路径未闭环

Apple [ARM64 ABI 文档](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms) 明确保留 `x18`。Madeira [Arm64Emitter.cpp 82–114 行](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.cpp#L82) 在 `FEX_IOS_HOST` 下从非 ARM64EC 动态寄存器集合中去掉 `r18`，否则仍包含它。

本次审计的 `build/fex-ios/build.sh` 没有传入该宏；`__APPLE__` 本身也不会选择上述分支。因此源码足以证明“允许配置 Darwin”不等于“普通原生构建已经启用全部 Apple 修正”。不能简单全局定义 `FEX_IOS_HOST` 解决：该宏还打开 Wine TEB/TSD、Windows/iOS 分配与异常辅助路径。应把真正的 Darwin ABI 能力与 Wine 集成能力拆开，另查 `x29`、栈、浮点保存、回调及变参边界。

### 3.4 16 KiB 宿主页：本仓已有基础，但仍有生产缺口

本仓 [address_space.cpp](../../src/core/guest_cpu/api/address_space.cpp) 已读取 `_SC_PAGESIZE`，FEX adapter 调用自有 `SetHostPageSize()`；并非所有 VM 代码都假定 4 KiB。Madeira 的 `TypeDefines.h` 仍保留 `FEX_PAGE_SIZE=4096`，不能以此单独认定错误——guest 译码页与宿主页本来就可以不同。

真正需要验证的是宿主保护范围：Madeira [InternalThreadState.h](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/FEXCore/include/FEXCore/Debug/InternalThreadState.h#L158) 的中断页仍按 4 KiB 布局，Apple 下还跳过了与 libc++ 结构大小相关的偏移断言。其静态宿主路径是否正确隔离保护页，没有本轮运行证据。

本仓 [guest_runtime.cpp](../../src/core/host_runtime/guest_runtime.cpp) 的 VM 更新和 `ProtectGpu()` 则确实直接以 `page * 4096`、4 KiB 长度/步进调用 `mprotect`（约 950–1001 行）。16 KiB 宿主上需要按宿主页聚合保护及跟踪所有者，保持子页权限/写关注语义，不能仅向外取整并意外开放邻页。中断页、call-ret guard、代码失效和 GPU 写跟踪都要测；PS4 ABI、guest 索引与宿主页大小不能全局统一替换。

### 3.5 CPU 特性、AVX 与内存顺序

本仓使用 `FEX::FetchHostFeatures()`；Madeira 的 [Source/Common/HostFeatures.cpp](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/Source/Common/HostFeatures.cpp) 仍包含 Linux sysfs 和 ID 寄存器探测。早期 FEXBridge 则硬编码 A15 特性、关闭 AVX/SVE，并直接声明硬件 TSO。两者都不能原样作为通用 M 系列 Mac 特性提供器。

应提供 Darwin 特性检测，并分别验证 x86 AVX/YMM 的翻译能力与 ARM SVE 能力；不能把“没有 SVE”推导为“必须关闭 guest AVX”。TSO 先保留正确性所需屏障，只有确认目标线程已成功进入所需模式后才启用硬件 TSO 优化。Madeira 的生产 [TSOHandlerConfig.h](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/Source/Windows/Common/TSOHandlerConfig.h) 通过 `TryEnableHardwareTSO()` 判断，和早期 bridge 不同；不要仅凭 Apple 芯片型号宣称可免屏障。

## 4. 本仓需要改动的接入点

| 位置 | 本次观察 | 后续适配 |
|---|---|---|
| [根 CMake](../../CMakeLists.txt) | `BUILD_HOST_CORE` 明确只接受 Android arm64-v8a | 抽出 macOS arm64 可用的宿主组合；隔离 Android 包装与平台库 |
| [cmake/fex](../../cmake/fex/CMakeLists.txt) | 生产 runner 要求 Android；FEX 源码目录固定；链接包含 `--start-group/--end-group` 及现有 allocator 档案 | 增加显式 Darwin profile、匹配源码/库/生成头身份，采用 Mach-O 可用的链接方式 |
| [fex_context.cpp](../../src/core/guest_cpu/fex/fex_context.cpp) | 直接使用 Linux `uc_mcontext.pc/regs/sp/pstate` 与 `SYS_gettid`；依赖私有 FEX API/布局及 IR passes | 提供 Darwin 信号读写 adapter、线程身份和 host feature provider；逐项核对 FEX API 差异 |
| [signal_context.cpp](../../src/common/signal_context.cpp) | 已有 Apple ARM64 PC/ESR 读取 | 优先扩展现有抽象，补需要的寄存器写回，避免再造不一致的解释方式 |
| [SessionBackendFex](../../src/core/host_runtime/session_backend_fex.cpp)、[GuestRuntime](../../src/core/host_runtime/guest_runtime.cpp) | 已有真实 PS4 loader、typed HLE、TLS、回调、Stop 与 VM 生命周期 | 由桌面入口接入这些组件；无需移植 Wine 的 syscall/TEB |
| [macOS 构建说明](../../documents/building-macos.md) | 当前明确使用 `CMAKE_OSX_ARCHITECTURES=x86_64` | 原生 arm64 路径验收后再新增正式构建说明，不将架构识别误报为已经支持 |

异常恢复必须保留现有 GPR/flags 与返回栈契约，特别是 Pause/Stop 的 block-boundary spill、HLE 内取消、未对齐原子故障和 GPU 写保护故障的分流。不能因为 Darwin `ucontext_t` 能编译就认定可恢复；需要真实信号、嵌套回调和并发失效验证。

## 5. 建议实施顺序与验收

1. **独立 CPU 最小程序。** 固定 FEX 版本、Darwin allocator/特性提供器和签名配置；arm64 Mach-O 在 Mac 上实际执行 x86-64 算术、SSE/AVX、原子与 syscall→host 返回。确认进程本身为 arm64。
2. **接本仓 guest_cpu_fex。** 跑现有 ABI/执行测试，重点覆盖寄存器及 flags 保存、TLS、嵌套回调、Pause/Resume/Stop、忙循环中断、fault chaining 和代码重新发布。包含会使错误实现失败的对照。
3. **16 KiB VM 与写跟踪。** 用真实 macOS 页大小验证边界/相邻子页、guard、中断页、SMC、保护恢复和并发回收，随后连接 GuestRuntime。
4. **完整桌面会话。** 复用桌面窗口/输入/音频与 renderer；真实游戏完成加载、交互、Stop 及同进程三次重启。先验证正确性，再做与现有 x86_64/Rosetta 路径的同场景比较。

这四步是后续验收顺序，不是已经实施的功能，也不承诺性能收益。本轮 Windows 工作区没有执行 macOS 构建，未连接或操作远端 Mac/Android 设备。

## 6. 许可证与维护边界

Madeira FEX 的 [LICENSE-MADEIRA.md](https://github.com/willfaust/FEX/blob/26859e184ad90f0e811d7f8bbd943a4b1573a2c3/LICENSE-MADEIRA.md) 区分上游 MIT 与 Madeira 新增 GPL-3.0-or-later 内容，另有 Metal Shader Converter 额外许可。不能只看混合文件顶部的 MIT SPDX 就把整个文件当作纯 MIT 复制；本次引用保留完整来源。当前本仓相关文件标为 GPL-2.0-or-later，后续若迁入 GPLv3 内容，应逐文件检查及记录组合分发版本和通知，不能从少数 SPDX 推断整个依赖闭包已经完成审计。

Madeira FEX 的 `AGENTS.md` / `CLAUDE.md` 保留禁止 AI 生成贡献代码的说明。本轮只读源码，不修改其 fork、不向其提交代码。后续依照主仓既有自有 fork 约定处理，并保留上游贡献边界。

## 7. 本轮完成与未完成

- 已从当前 HEAD 创建并切换 `feature/mac_fex_arm64`，保留此前 dirty 文件；本轮引用与调研独立归档，既存未提交改动不纳入本次提交。
- 已增加独立 Madeira 参考 gitlink，并取得其精确 FEX 子模块源码；现有生产 FEX pin 不变。
- 已核对两条远端分支的精确 SHA、参考工作树、宿主接入点及平台差异，并记录来源。
- 只修改引用元数据与文档；不把源码审计视为编译/运行验证。尚无原生 Mac 可执行文件或游戏验收结果。
