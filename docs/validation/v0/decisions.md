# V0 技术决定记录

所属：[V0 总 spec](../../specs/android-fex-v0.md)。按 spec §9 与任务书要求，
记录实现过程中的判断、证据与被拒绝的替代方案。每条决定标注它是**实测结论**还是**设计选择**。

---

## DEC-01　native API 取 35，Java 侧取 36

- 类型：受客观限制的偏离
- 证据：[TC-01](environment-and-toolchain.md#tc-01阻断性偏离不存在原生-api-36-的-ndk)

spec §4.1 要求 native API 36。实测本机 r27d / r28c / r29 的真实 `meta/platforms.json`
与 sysroot 均只到 35，且 `bin/` 中不存在 `aarch64-linux-android36-clang`。
上游 changelog 显示 max API 提升只出现在**预发布**的 r30 RC 1。

决定：native `ANDROID_PLATFORM=35`，`compileSdk`/`targetSdk`/`minSdk` = 36。
选 r28c 而非 r29，因为 spec 建议以 r28c 起步，且 r29 在该指标上无优势。

被拒绝的替代：改 NDK 元数据伪造 API 36（spec 明令禁止）；锁定预发布 r30 RC（不可作为锁定工具链）。

影响：验收报告中该项记 DEVIATION，不记 PASS。不阻断实机验收——native API level 是最低兼容目标而非运行上限。

---

## DEC-02　参考实现的 FEXCore 构建**不能**直接复用为 Android 证据

- 类型：实测结论，推翻了一个可能的乐观假设
- 证据：审计 `references/Bachata-S4-android` 的实际构建脚本与验证器

`references/Bachata-S4-android` 有可用的 FEXCore-only 构建，但它**不是 Android/NDK 构建**。五项独立证据：

1. `runtime/scripts/build-fexcore-smoke-aarch64.sh:77` 设 `-DCMAKE_SYSTEM_NAME=Linux`（不是 `Android`），
   `:81-83` 设 `aarch64-linux-gnu` target 与 `/usr/aarch64-linux-gnu` sysroot。**完全没有 `CMAKE_TOOLCHAIN_FILE`。**
2. 全仓 grep：`ANDROID_ABI` / `ANDROID_PLATFORM` / `android.toolchain.cmake` 只出现在
   `build-box64.sh`（Box64，不是 FEX）。**FEX 构建路径里零个 NDK 标记。**
3. 两个验证器都硬断言 glibc：`EXPECTED_NEEDED = ["libc.so.6","libgcc_s.so.1","libm.so.6","libstdc++.so.6"]`，
   且要求 interpreter 恰为 `/lib/ld-linux-aarch64.so.1`。这些在 bionic 上都不存在。
4. strip 用 GNU 交叉 binutils `aarch64-linux-gnu-strip`。
5. `runtime/tests/verify-apk-runtime.mjs:79` **主动禁止** FEXCore 进入 APK 的 jniLibs，
   改由 `stage-debian-runtime.mjs` 放进 Debian rootfs，在设备上以 glibc 容器执行。

这与 AGENTS.md 既有事实 #4 一致，并给出了具体机制。

**另一项关键实测**：`runtime/probes/fexcore-smoke.cpp:428` 在
`sysconf(_SC_PAGESIZE) != 4096` 时**硬失败**。参考实现从未在 16 KiB 页上运行过 FEXCore。

决定：把该参考视为**两个可分离的部分**——
- **可迁移**：CMake flag 集合、`fex-fexcore-only.patch`、12 个静态库依赖清单、`src/core/guest_cpu/*` 的 ABI adapter。
- **不可迁移**：工具链层、sysroot、glibc soname 断言、rootfs 打包路径。

不得把该参考的 FEXCore 构建成功当作“Android FEXCore 可行”的证据。

---

## DEC-03　FEX 必须改源码，而贡献规则禁止 AI 生成代码 —— 记为阻断

- 类型：规则阻断
- 证据：[页大小审计](page-size-audit.md) PS-01 ~ PS-04；`references/FEX/AGENTS.md` 与 `CLAUDE.md`

审计确认 FEXCore 在 16 KiB host 上**启动即中止**，且无法从外部绕过：

> **复核更正（重要）**：初版列出四项并称"启动即中止"。追踪调用图后，
> PS-01 与 PS-04 **可由嵌入方规避**，"启动即中止"不成立。必须改的只剩两项。
> 详见[页大小审计 §2](page-size-audit.md)。此处保留更正后的结论。

| 编号 | 位置 | 复核判定 |
|---|---|---|
| PS-01 | `Allocator.cpp:105-135` `GetHostVABits()` | **可规避**。只经 `Setup48BitAllocatorIfExists` 到达，而它只由 FEXInterpreter 调用；`grep FEXCore/Source/Interface/` 无任何引用 |
| PS-04 | `64BitAllocator.cpp`（32 处） | **当前不触发**。其 `OSAllocator_64Bit` 只在上述路径构造；`static_assert` 以现有 4096 常量编译通过 |
| PS-02 | `SharedCodeBufferManager.cpp:25-27` 等 | **必须改**。JIT code buffer 分配时无条件执行；`mprotect` 失败仅记日志继续 → JIT 溢出静默破坏堆 |
| PS-03 | `InternalThreadState.h:125` `InterruptFaultPage` | **必须改**。`Core.cpp:447` 位于 `ContextImpl::DestroyThread`，嵌入方必经；且它是 dispatcher 中断机制本体（验收 T02 建立其上） |

PS-02 与 PS-03 都在 FEXCore 内部，**无法通过嵌入方代码规避**。

`references/FEX/AGENTS.md` 与 `CLAUDE.md` 内容均为：
> AI must not be used to generate code for contributions to this project.

决定：**不生成 FEX 源码修改。** 按任务书“若适用贡献规则阻碍必须修改的 FEX 代码，准确说明阻断，
并继续可独立完成的部分，不绕过规则”执行。

明确不做的规避手段：不换目录改写、不写外置 patch 生成器、不用 `sed` 就地改参考树、
不把改动伪装成“配置”。这些都只是同一件被禁止的事换个形式。

**因此 M1（FEX Android 可加载）与依赖它的 M2/M3 FEX 执行项无法由本轮完成。**
交付状态不能是 V0_ACCEPTED。

需要人类工程师完成的最小工作已定位到行号，**两项**且都很局部：
guard 改为一个 host page 且 `UsableSize` 同步扣减（并把 `mprotect` 失败从日志升级为错误）；
`InterruptFaultPage` 改为独立映射、按 host page 对齐定尺，同时满足 `:130` 的 `<= 65520` 偏移约束。

复核还带来一个可先做的低成本实验：既然 PS-01 不在路径上，
可以先只解决 PS-02/PS-03，试一次"FEXCore 在 16 KiB host 上 `InitCore()` 能否成功返回"，
再决定后续投入。这比初版"四项全改"的估计乐观。

---

## DEC-04　页大小常量拆成三个，不做全局替换

- 类型：设计选择，由审计证据支撑
- 证据：[页大小审计](page-size-audit.md) §3

审计给出四条不能全局替换 `FEX_PAGE_SIZE` 的硬理由：
生成的 ARM64 机器码钉死 `lsr #12`/`and #0xFFF`（`Dispatcher.cpp:195,204`）；
guest vsyscall 页与 `AT_PAGESIZE` 架构上就是 4096；
`static_assert(sizeof(LiveVMARegion) == 4096)` 会编译失败；
`MAX_FORWARD_BRANCH_DIST = FEX_PAGE_SIZE * 4` 这类启发式会被静默放大 4 倍。

决定：本仓库自有代码一律区分三个量，且**不复用同一个符号**：

| 量 | 来源 | 取值 |
|---|---|---|
| `HostPageSize` | 运行时 `sysconf(_SC_PAGESIZE)` 发现 | 16384 或 4096 |
| `kGuestAbiPageSize` | x86-64 PS4 ABI 常量 | 恒 4096 |
| 内部索引粒度 | 与后端译码索引一致 | 恒 4096 |

实现见 `src/core/guest_cpu/api/memory.h`。host page size **绝不**编译期常量化——
验收 B04 要求同一个 APK 在 16 KiB 与 4 KiB 上都得出正确值。

---

## DEC-05　SMC 采用 ExplicitPublication，并拒绝 TransparentSMC caller

- 类型：设计选择，由 16 KiB 约束推导
- 证据：[页大小审计](page-size-audit.md) §4

TransparentSMC 需要按 guest 4 KiB 页做写保护。审计 §4 显示
`Core.cpp:904-905` 的 `MarkGuestExecutableRange(…, FEX_PAGE_SIZE)` 最终要 `mprotect` guest 映射，
而 16 KiB host **无法对 4 KiB 子范围单独设权限**。

决定：V0 采用 spec §7.2 允许的 **ExplicitPublication**，在 capabilities 中声明，
并在初始化时**拒绝**要求 `TransparentSMC` 的 caller（返回 `UnsupportedMemoryMode`）。

这不等于支持任意游戏的透明 SMC，报告中不得如此表述。

---

## DEC-06　同一 16 KiB host page 内的冲突子页权限返回 Unsupported

- 类型：设计选择，spec §5.1 明确允许
- 相关验收：M03

DirectMapped 模式下，若请求在同一个 host page 内给不同 4 KiB 子区不同权限，
返回 `Unsupported`，且**映射保持原状**——权限、内容、登记表都不变，不做部分修改后再报错。

逻辑分段（M02）与权限分段（M03）是两回事：前者支持，后者拒绝。

---

## DEC-07　构建主机的 16 KiB 页用于真实页语义测试，但不冒充 Android

- 类型：范围声明
- 证据：[TC-02](environment-and-toolchain.md#tc-02构建主机自身是-16-kib-页)

构建主机是 macOS ARM64，`getconf PAGE_SIZE` = 16384。因此页对齐、子页分段、
权限冲突、跨页原子等**纯 POSIX 映射语义**可在真实 16384 字节页上获得覆盖，而非在 4096 上假装。

但它不是 bionic、没有 ART、没有 app 沙箱、不是 FEX 的 ARM64 Linux JIT 目标。
HOST 结果单独列出，不折算成任何 A16-16K 项。

---

## DEC-08　foundation reflection/packing 的 Android 依赖闭包缺 `fmt`，F02 记 NOT_RUN

- 类型：实测阻断（非规则阻断，可解除）
- 相关验收：F02

F02 不依赖 FEX，因此本轮实际尝试了为 Android 构建 `foundation_meta_reflection`
与 `foundation_meta_packing`。CMake **配置成功**，编译失败于：

```
foundation/basic/underlying/core/public/spatial/core/utils/StringTool.hpp:14:10:
  fatal error: 'fmt/format.h' file not found
```

连带 `MemoryStatistics.h:80,84` 因 `fmt::format_string` 未声明而报错。

核实：`foundation/third_party/` 下**没有** `fmt` 目录，`third_party/CMakeLists.txt`
中也没有任何 `fmt` 条目或 `find_package(fmt)`。也就是说 `fmt` 既未 vendored 也无发现路径，
而 `basic/underlying/core` 是 reflection 的传递依赖，reflection 又是 packing 的依赖。

顺带确认了两件事，避免后续重复排查：

- `zstd` 缺失只影响 `modules/zar`，与 reflection/packing 无关；
  第一次探测报 `zstd` 是因为整树 `add_subdirectory` 把 zar 也带了进来。
- 早先“`basic/` 拉进 `modules/zar`”的现象是我的探测脚本重定义
  `foundation_add_subdirectory` 破坏了相对路径解析所致，**不是** foundation 的结构问题。
  `basic/modules` 与 `foundation/modules` 是不同目录。

另一个需要留意的点：`foundation_meta_packing` 传递依赖
`spatial::foundation_allocator` → `spatial::third_party_mimalloc`。
即使解决了 `fmt`，把 mimalloc 带进 ART 进程也需要单独的 allocator/TLS 审计，
[Foundation 接入记录](../foundation-integration.md)已就此提出警告。不要因为“能编过”就直接启用。

决定：F02 记 **NOT_RUN**，原因写明为依赖闭包缺 `fmt`，而不是笼统的“未启用”。
这不是 spec 意义上的阻断（可以通过 vendoring 或 `find_package` 解决），
但本轮未解决，因此不能记 PASS。

下一步：为 foundation 提供 `fmt`（vendored 或系统包），重跑上述构建，
再实现 round-trip 与非法/截断输入测试。

---

## DEC-09　ASan 在本构建环境不可用；已用 UBSan 与目标用例替代

- 类型：验证手段限制 + 对先前提交信息的更正
- 发现方式：先前提交 `73be4099` 的信息里写了“clean under -fsanitize=address,undefined”。
  复核时发现该结论来自一次被 `tail` 掩盖了退出码的运行，**不成立**。

实测：

```
$ printf '#include <cstdio>\nint main(){std::printf("hello asan\n");return 0;}\n' > triv.cpp
$ clang++ -fsanitize=address triv.cpp -o triv && timeout 60 ./triv
rc=124            # 超时，无输出
```

**一个什么都不做的 ASan 二进制在本机同样挂死**，因此这是环境问题
（Apple clang 17 + Darwin 25.5 沙箱），不是被测代码的问题。

有效的替代验证，全部实测通过：

| 手段 | 结果 |
|---|---|
| UBSan（`-fsanitize=undefined -fno-sanitize-recover=all`） | 21/21 通过，rc=0 |
| 普通构建 | 21/21 + 14/14 通过 |
| 针对性生命周期用例（L01a/L01b/L01c） | 通过；修复前该场景以 `mutex lock failed: Invalid argument` 中止 |

结论：DEC-08 之前那条“ASan clean”的表述应读作 **“UBSan clean，ASan 未能运行”**。
生命周期修复本身有独立证据——修复前可复现崩溃、修复后不再崩溃，且有三个回归用例守住。

下一步：在 ASan 可用的机器或 Linux CI 上补跑一次，再把结论升级为“ASan clean”。
