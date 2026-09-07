# V0 实施报告

所属：[V0 总 spec](../../specs/android-fex-v0.md)、[验收矩阵](../../specs/android-fex-v0-acceptance.md)。

- 分支：`codex/android-fex-v0`
- 实现起点：`dccc0d7938827ebc58143b8ea784e14491cf6d03`
- 比较基线：`a712889343ccd2588b712988dae0a76a104e0dce`（未修改）
- 机器可读结果：[results.json](results.json)
- 原始输出：[host-suite-output.txt](host-suite-output.txt)

## 最终状态：`V0_BLOCKED`

**16 PASS / 0 FAIL / 39 NOT_RUN，共 55 项。**

按 spec §9 的定义，`V0_BLOCKED` 用于“不可解决的实际依赖/能力/规则阻断，有最小复现、证据、已完成项和下一步”。
本轮符合该定义，理由见下节。

不能用 `V0_IMPLEMENTED_DEVICE_PENDING`：那个状态适用于“代码及能执行的检查完成，只缺设备”。
本轮不是只缺设备——**即使现在接上一台 16 KiB 的 Android 16 实机，FEX 相关用例仍然无法运行**。
用 DEVICE_PENDING 会掩盖真正的阻断，spec 明确禁止这种遮盖。

也不能用 `V0_ACCEPTED` 或 `V0_IN_PROGRESS`：前者要求全部 MUST 通过；后者暗示只要继续实现即可推进，
而实际阻断点不在实现工作量上。

状态由 `scripts/android/run-v0-tests` 按规则自动推导，不是人工填写。

## 1. 阻断：FEXCore 在 16 KiB host 上无法启动，且修改受贡献规则限制

这是本轮最重要的结论，两部分都是实测而非推断。

### 1.1 技术事实

[页大小审计](page-size-audit.md)在 FEXCore-only 链接范围内定位到四个阻断项，全部有确切行号：

| 编号 | 位置 | 后果 |
|---|---|---|
| PS-01 | `Allocator.cpp:105-135` `GetHostVABits()` | 7 个探测地址都只 4 KiB 对齐 → 16 KiB 内核全部 `EINVAL` → `FEX_UNREACHABLE`。**启动即中止** |
| PS-02 | `SharedCodeBufferManager.cpp:25-27` 等 4 处 | guard page `mprotect` 失败只记一条日志继续 → JIT 溢出静默破坏堆 |
| PS-03 | `InternalThreadState.h:125` | `InterruptFaultPage` 是 jemalloc 堆对象内的 4096 字节数组；16 KiB `mprotect` 会破坏相邻 12 KiB，含同段生成代码要读的 `BaseFrameState` |
| PS-04 | `64BitAllocator.cpp` 32 处 | `static_assert(sizeof(LiveVMARegion) == 4096)` 等，改常量直接编译失败 |

PS-01 已在源码中直接确认：探测地址是 `(1ULL << Bits) - FEXCore::Utils::FEX_PAGE_SIZE`，
而 `FEX_PAGE_SIZE` 是编译期 4096。PS-03 的 `InterruptFaultPage` 是 dispatcher 中断机制本体，
验收 T02“无 HLE 死循环可暂停”正建立其上。

另一项相关实测：FEXCore 链接范围内**没有任何** `sysconf(_SC_PAGESIZE)` 调用。
唯一获取 host 页大小并注入 allocator 的代码在 `Source/Tools/FEXInterpreter/`，
而 FEXCore-only 构建不包含它。嵌入式 FEXCore 目前根本不知道 host 页大小。

这四项都在 FEXCore 内部，**无法从嵌入方绕过**。

### 1.2 规则事实

`references/FEX/AGENTS.md` 与 `references/FEX/CLAUDE.md` 内容相同，各一行：

> AI must not be used to generate code for contributions to this project.

因此本轮**没有**生成任何 FEX 源码修改。任务书对这种情况的指示是“准确说明阻断，
并继续可独立完成的部分，不绕过规则”，本轮照此执行。

明确未采用的规避手段：换目录改写、外置 patch 生成器、对参考树 `sed`、把改动包装成“配置”。
这些都是同一件被禁止的事换个形式。

### 1.3 解除阻断需要什么

四项修改都很局部，已定位到行号，需要**人类工程师**完成：

1. `GetHostVABits()` 的探测偏移改为一个 host page（当前硬编码 `FEX_PAGE_SIZE`）。
2. code buffer / temp buffer 的 guard 改为一个 host page，`UsableSize()` 同步扣减；
   并把 `mprotect` 失败从日志升级为错误——静默失去 guard 比失败更危险。
3. `InterruptFaultPage` 改为独立映射，按 host page 对齐与定尺，
   同时满足 `InternalThreadState.h:130` 的 `<= 65520` 偏移约束。
4. 页常量按三种语义拆分后分别赋值，**不可全局替换**（理由见审计 §3：
   dispatcher 发射的 `lsr #12`/`and #0xFFF` 钉死了内部索引，
   vsyscall 与 `AT_PAGESIZE` 钉死了 guest ABI 的 4096）。

之后还需要一条向 FEXCore 注入 host 页大小的路径，因为它自己不查询。

## 2. 已完成并验证的部分

### 2.1 公共 CPU API（`src/core/guest_cpu/api/`）

后端无关的接口，不含任何 FEX/JNI/Kotlin 类型。验收 B05 已验证：
只用 `-I src` 即可编译一个纯消费者，不需要 FEX include 路径或 compile definitions。

关键设计（对应 spec 的具体要求）：

- **三种页大小分开**（DEC-04）。`HostPageSize()` 运行时发现，`kGuestAbiPageSize` 与
  `kCodeIndexGranularity` 恒 4096。host 页大小绝不编译期常量化——B04 要求同一个 APK 在两种页上都正确。
- **停止原因不折叠**。fault / backend failure 永不被同时到达的 pause 掩盖；
  cancel 优先于 pause；未选中的原因保留在 pending 位集中。
- **快照有 validity mask**。未采集的寄存器读作 unknown 而非 valid zero；
  `AsyncJitStop` 种类明确表示“guest 值还在 host 寄存器里，没有重建”。
- **RSP 只有一个权威槽**，不存在两个可能不一致的成员。
- **未实现的模式在初始化就拒绝**。`TransparentSMC` → `Unsupported`，
  `SoftwareCallbacks` → `UnsupportedMemoryMode`，不静默降级。

### 2.2 GuestAddressSpace（16 KiB 内存模型）

- reservation 用 `PROT_NONE` 整块申请，`MAP_FIXED` **只**用于自己已持有的 reservation 内部。
  不做“读 maps 找空洞再抢占”——ART 或驱动可能在两步之间占住它。
- `Protect()` 对跨 host page 边界的子页权限请求**先拒绝再说**：
  权限、内容、登记表全部保持原状，不做部分修改后报错（DEC-06 / M03）。
- `Quiesce()` 在还有 HLE writer 持 pin 时返回 `Timeout` 且不改任何状态——
  部分停止不能授权代码事务。
- observer 通知按 host page **保守放大**：多报可接受，漏报不可接受（M12）。
- observer 回调在锁外执行，避免 observer 回调进地址空间导致死锁。

### 2.3 HLE typed ABI adapter（`src/core/guest_cpu/hle/`）

从 `references/Bachata-S4-android` 的 `hle_call_adapter.h` 迁入形态，做了三处契约驱动的修改：

| 修改 | 原因 |
|---|---|
| 不支持的签名在**注册时**拒绝 | 参考实现按次调用判断，错误注册的函数要到 guest 真的走到才暴露 |
| guest 指针对地址空间**强制校验** | 参考实现放行任何 >4096 的非空值，等于把未校验的 host 地址交给 native 代码（用例 H04c 专测这条） |
| 错误说明是哪个参数、为什么 | 参考实现统一返回 EFAULT |

### 2.4 测试结果

**32 个 host 子用例全部通过**，映射到 16 个验收项。

`api_contract_tests` 18/18，在**真实 16384 字节页**上运行（构建主机是 macOS ARM64，
`getconf PAGE_SIZE` = 16384）。这不是在 4096 上假装 16 KiB。

`hle_abi_tests` 14/14，覆盖 H01 点名的全部摆放：8 整数（6 GPR + 2 栈溢出）、
9 double（8 XMM + 1 栈）、混合类别独立序列、float 返回用 xmm0 低半、
以及 syscall gate 下第 4 参数从 r10 取（rcx 被破坏）。

一个过程中的发现：初版 M07 用 RWX 映射，macOS W^X 直接 `EACCES` 拒绝。
改为 RW→RX 发布后通过——而 RW→RX 本来就是 spec §5.1 要求的方式，
所以宿主限制和规范要求在这里恰好一致。顺带修正了错误分类：`EACCES` 曾被报成 `OutOfMemory`，
会把读者引向找内存泄漏而不是权限策略。

### 2.5 Android NDK/bionic 交叉编译

公共 API 与两个测试二进制都能为 `arm64-v8a` 交叉编译，产物经 B02 验证：

```
Class: ELF64          Machine: AArch64
NEEDED: libm.so, libdl.so, libc.so        （bionic，无 glibc）
interpreter: /system/bin/linker64
min PT_LOAD align: 0x4000                 （16 KiB）
Build ID: 23fac3907c3c4d8e5d17301c10dd0f89cf0613b1
GNU symbol versions: none
```

**这是真实的 bionic 产物，但它不含 FEX，也没有在设备上跑过。**

### 2.6 工具链

四个入口，均可运行：

| 脚本 | 作用 |
|---|---|
| `scripts/android/check-v0-environment` | 读 `Pkg.Revision` 与 `meta/platforms.json` 而非目录名；确认编译器 wrapper 真实存在；deviation 退出码 2 |
| `scripts/android/verify-v0-artifacts` | B02/B03；zip 对齐独立于 zipalign 检查一遍，工具缺失时记 NOT_RUN 而非默默不验 |
| `scripts/android/run-v0-tests` | 全 55 项默认 NOT_RUN，只由实际执行的 suite 提升；状态机械推导 |
| `cmake/fex/CMakeLists.txt` | Android 构建入口；`-DV0_ENABLE_FEX=ON` 明确失败并给出原因 |

## 3. 工具链偏离：不存在原生 API 36 的 NDK

spec §4.1 要求 native API 36。实测本机 r27d / r28c / r29 的真实 `meta/platforms.json`
与 sysroot **均只到 35**，且 `bin/` 中不存在 `aarch64-linux-android36-clang`；
API 35 目标编译通过，API 36 无编译器。

上游确认：NDK r28/r28b/r28c/r29 各版 changelog 都没有 sysroot max API 提升，
唯一的提升声明在**预发布**的 r30 RC 1（“Upgraded the max API to 37”）。

决定：native `ANDROID_PLATFORM=35`，Java 侧 `compileSdk`/`targetSdk`/`minSdk` = 36。
Android 16 设备可以运行以 API 35 sysroot 构建的 native 代码——native API level 是**最低**
兼容目标而非运行上限，所以这不阻断实机验收，只是让“native API 36”一条无法按字面 PASS。
按 DEVIATION 记录，不计 PASS。

顺带一个实例：本机 NDK 目录 `28.2.13672827` 自报 `Pkg.Revision 28.2.13676358`，
目录名与元数据不一致。这正是 spec 警告的“不要只信目录/包标签”，
`check-v0-environment` 因此一律以 `source.properties` 为准并在不一致时提示。

## 4. 关于参考实现的重要更正

`references/Bachata-S4-android` 有可用的 FEXCore-only 构建，很容易被当作“Android FEXCore 已可行”的证据。
**它不是。** 五项独立证据（详见 DEC-02）：

1. 构建脚本设 `-DCMAKE_SYSTEM_NAME=Linux`，target `aarch64-linux-gnu`，sysroot `/usr/aarch64-linux-gnu`，
   **完全没有 `CMAKE_TOOLCHAIN_FILE`**。
2. 全仓 grep：NDK 标记只出现在 Box64 构建脚本里，FEX 构建路径零个。
3. 两个验证器硬断言 glibc soname 与 `/lib/ld-linux-aarch64.so.1`。
4. strip 用 GNU 交叉 binutils。
5. APK 验证器**主动禁止** FEXCore 进入 jniLibs，改由 Debian rootfs 在设备上以 glibc 容器执行。

并且 `runtime/probes/fexcore-smoke.cpp:428` 在 `sysconf(_SC_PAGESIZE) != 4096` 时**硬失败**——
参考实现从未在 16 KiB 页上运行过 FEXCore。

可迁移的是：CMake flag 集合、`fex-fexcore-only.patch`、12 个静态库依赖清单、ABI adapter。
不可迁移的是：工具链层、sysroot、glibc soname 断言、rootfs 打包路径。

## 5. 未完成项

| 项目 | 状态 | 原因 |
|---|---|---|
| FEX backend 接入 | 未开始 | DEC-03 阻断 |
| CpuContext / ThreadHandle / Run / Step | 未实现 | 需要后端才有意义，否则只是空壳 |
| 验证 APK、Activity、SurfaceView | 未实现 | 依赖上一项 |
| Vulkan Surface 生命周期 | 未实现 | 同上 |
| x86 fixture 与生成规则 | 未实现 | 需要后端执行才能验证 |
| foundation reflection/packing | 未启用 | 依赖闭包缺 `fmt`，实测见 §5.1；F02 记 NOT_RUN 而非 PASS |
| segment mapping helper（M06） | 未实现 | 本轮范围外 |

这些**没有**写成占位实现。spec 明确要求“不要仅返回一份计划或接口空壳”，
一个返回固定值的假 backend 会让验收矩阵看起来在推进，实际什么都没验证。

### 5.1 F02 的具体阻断点（实测，非推断）

F02 不依赖 FEX，因此本轮实际尝试为 Android 构建 reflection/packing。
CMake **配置成功**，编译失败于：

```
foundation/basic/underlying/core/public/spatial/core/utils/StringTool.hpp:14:10:
  fatal error: 'fmt/format.h' file not found
```

`foundation/third_party/` 下没有 `fmt`，也没有任何 `find_package(fmt)`。
`basic/underlying/core` 是 reflection 的传递依赖，reflection 又是 packing 的依赖。

排除掉两个误判（记在 [DEC-08](decisions.md)，避免后续重复排查）：
`zstd` 缺失只影响不相关的 `modules/zar`；
“`basic/` 拉进 zar”是我第一次探测脚本破坏相对路径解析造成的假象，不是 foundation 的结构问题。

另需注意：packing 传递依赖 mimalloc。即使补上 `fmt`，把 mimalloc 带进 ART 进程
仍需单独的 allocator/TLS 审计——不要因为“能编过”就启用。

## 6. 建议的下一步

按依赖顺序：

1. **由人类工程师修复 FEX 的四个 16 KiB 阻断项**（§1.3，已定位到行号）。这是唯一的关键路径。
   在自有 fork `tencentmalos/FEX` 上做，记录 upstream base 与修改范围。
2. 修复后先跑一个最小验证：在 16 KiB host 上让 FEXCore `InitCore()` 成功返回。
   在此之前不要投入 backend adapter 工作。
3. 取得可用的 API 36 NDK 后重新评估 DEC-01；在此之前 API 35 是正确选择，不要用预发布 r30。
4. 准备 A16-16K 实机。注意验收要求的是 **app 内** `sysconf(_SC_PAGESIZE)` = 16384，
   `adb shell getconf PAGE_SIZE` 只是必要条件。
5. backend 可用后，按 M2 → M3 → M4 顺序推进，每阶段单独提交。

## 7. 交付清单

| 项 | 位置 |
|---|---|
| 实现提交 | 分支 `codex/android-fex-v0`，4 个提交 |
| 环境锁 | [environment.lock.json](environment.lock.json) |
| 工具链记录 | [environment-and-toolchain.md](environment-and-toolchain.md) |
| 页大小审计 | [page-size-audit.md](page-size-audit.md) |
| 技术决定 | [decisions.md](decisions.md) |
| 逐项结果 JSON | [results.json](results.json) |
| 原始测试输出 | [host-suite-output.txt](host-suite-output.txt) |
| 构建/检查/测试脚本 | `scripts/android/` |

未交付：APK、native Build IDs（无 APK）、设备测试证据。原因见 §1 与 §5。
未包含商业游戏、下载的 runtime 或无关本地改动。既有基础状态记录未修改。
