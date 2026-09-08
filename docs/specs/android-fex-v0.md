# V0 首个验证版：Android NDK/bionic + FEXCore + 16 KiB

状态：**执行中，尚未验收**。编写日期：2026-09-07。需求来源：本次用户明确要求第一个验证版，包含 Dynarmic-like API、NDK/bionic，并交由另一个 AI 执行。

> **最新实现进展**：[2026-09-08 复核与缓存修复](../validation/v0/followup-2026-09-08.md)。Swan 4 KiB 执行 harness 36/36；完整发布事务、HLE、runner 与 app 门槛仍未完成，不等于 V0 验收通过。

> **2026-09-08 用户范围调整（优先于下文原始页大小要求）**：当前目标为 Swan / Android 16 / ARM64 / **4 KiB host pages**。16 KiB 适配、16 KiB 实机及同 APK 的双页大小对照后置，不作为本阶段完成阻断；不要求继续修改现有 16 KiB 代码。页大小相关执行用例先按真实 4096-byte host page 验证，专属于 16 KiB 的子页权限用例保留为后续项，不记 PASS。NDK/bionic、真实 guest/HLE/callback、暂停停止、线程/内存安全、有限 Step、普通 app 与原生 Vulkan 等要求保持有效。当前审计与证据见 [2026-09-08 执行审核](../validation/v0/review-2026-09-08.md)。下文保留原始设计供后续恢复 16 KiB 目标时参考。

文档集：

- 本文：目标、范围、架构、构建、阶段与交付。
- [CPU API 契约](android-fex-v0-api.md)：接口语义与并发/内存规则。
- [验收矩阵](android-fex-v0-acceptance.md)：用例、门槛与证据格式。
- [执行任务书](android-fex-v0-handoff.md)：可直接交给执行 AI 的入口。
- [子仓归属](../subrepository-ownership.md)、[Foundation 接入](../foundation-integration.md)：依赖开发分支、已落地的构建基础与待完成的复用要求。

本文的 MUST/必须是 V0 完成条件；SHOULD/建议允许有书面理由的实现选择。示例目录和命令是**待实现接口**，不是对仓库现状的声明。

## 1. 一句话目标

交付一个可安装的 ARM64 Android 验证 APK：**在 Android 16、真实 16 KiB 页的普通 app 进程中，由 NDK/bionic 构建的原生 backend，通过自己的 CPU API 驱动 FEXCore 执行自有 x86-64 fixture，完成 guest/HLE/callback 往返、内存一致性、线程控制和可观察状态；同时跑通最小原生 Vulkan Surface 生命周期。**

它验证后续 shadPS4 Android 原生化最危险的边界。V0 不要求运行商业游戏，也不以完整 PS4 GPU/HLE 兼容为目标。fixture 产生颜色/计数值，由 host Vulkan 绘制的演示不等于已经运行 PS4 GNM/GCN。

### 可演示的完成结果

1. 安装后离线启动验证页面，显示实际 Android API、ARM64 ABI、host page size、backend/FEX/构建版本。
2. 点击或通过 instrumentation 启动测试：真实 x86 fixture 更新 guest buffer，经实际 HLE adapter 返回 host；普通输入影响 fixture 状态，host 用结果更新原生 Surface。
3. 可以暂停、读写停止态寄存器、继续、停止并再次启动；不会靠杀进程完成正常停止。
4. 导出机器可读结果及精确版本证据。所有 MUST 用例成功，才可报告 `V0_ACCEPTED`。

## 2. 起点与范围

固定行为比较基线：[a7128893 状态记录](../baselines/2026-09-07-android-fex-foundation.md)。实现从**包含本 spec 的最新文档提交**建立分支，比较时仍用 `a7128893`；不要 checkout 老基线后丢掉 spec。

默认决策：

| 项目 | V0 决策 |
|---|---|
| 分支 | 执行者建立 `codex/android-fex-v0`；如已存在，先检查其状态 |
| 目标平台 | ARM64 Android 16 / API 36 / 16384-byte host page；同一个 APK 在 4096-byte ARM64 Android 上作回归 |
| app 形态 | 主仓新增最小验证 app；NDK backend 与 ART/Java 同进程，guest owner threads 为原生 worker |
| FEX 初始实现版本 | `f2b679f6028ce1c38875233aecfcf5d3f8ebecec`，与参考 bridge 一致 |
| 新 FEX `50e6eee9…` | 比较/必要修复来源，不顺带全量升级；若旧版不可行，先记录证据、迁移差异与新的固定版本 |
| 地址模式 | 受控同址映射 DirectMapped；不做任意 guest VA 到任意 host VA 的全软件地址转换 |
| guest | 可重建的自有 x86-64 汇编/C fixture，无 Linux guest rootfs、无商业游戏资源 |
| CPU 线程 | 一个 guest thread 绑定一个 owner host thread；至少两个 guest threads 同时执行 |
| x86 特征 | 首版最小 SSE2 profile；AVX/AVX2 等显式能力协商，声称支持的特征必须测试 |
| 图形 | Android native Surface + Vulkan；无 X11、Vortek、Canvas 像素拷贝路径 |
| 音频 | V0 不纳入必需验收；可以沿用已有音频测试作对照，AAudio/Oboe 属于下一阶段 |
| guest debugger | V0 必须有安全点状态、有限 Step 与 host LLDB 观测；完整 RSP/GDB server 后续实现 |

首版不包括：完整 Bachata UI 迁移、商业游戏/SELF 兼容性、全量 libSce、PSVR/Move/OpenXR、通用 Linux syscall frontend、32-bit x86、fiber 调度、任意 MMIO 软件慢路径、完整 guest 调用栈展开、保存状态文件格式和游戏帧率优化。

V0 的 CPU/内存/ABI fixture 必须共用将来迁入 shadPS4 的 production library；不得另造一个仅用于“通过演示”的模拟 backend。完整 emulator/renderer 可以暂不链接，避免把 PS4 游戏兼容性混入第一门槛。

## 3. 要交付的代码组织

下面是推荐新增位置；若与执行时仓库布局冲突，可以在 M0 的设计记录中调整，模块职责不变：

| 位置 | 职责 |
|---|---|
| `src/core/guest_cpu/api/` | FEX 无关的公开 CPU、寄存器、memory、callback、stop 类型 |
| `src/core/guest_cpu/fex/` | FEX 私有 adapter、state 转换、dispatcher/cache/异常接入 |
| `src/core/guest_cpu/hle/` | 从参考迁入的 typed ABI adapter、callgate 与 callback dispatcher |
| `src/platform/android/` | JNI、host page/mapping、signals、native window 与事件桥 |
| `cmake/fex/` | 可复现的 FEX 构建接入、版本/配置和必要依赖清单 |
| `android/validation/` | 小型 Activity + SurfaceView + instrumentation runner |
| `tests/guest_cpu/fixtures/` | 自有 x86 fixture 源码、生成规则、输入和独立预期值 |
| `tests/guest_cpu/` | API/ABI/内存、控制、并发测试，共用真实 backend |
| `scripts/android/` | 环境检查、构建、产物检查、安装/运行/收集的一键入口 |
| `docs/validation/v0/` | 环境锁、设计决定、适配审计和验收报告；大文件放被忽略的 artifact 目录 |

`references/` 保持固定参考，不直接在其工作树上做产品开发。若需要另一份 FEX 构建源，使用版本锁控制的独立依赖 checkout；禁止不记录的复制和对参考树的就地 `sed`。FEX 必要源修改应独立可审查，并记录 upstream base、修改范围和复现方法。执行者必须遵守适用的贡献规则；不要用换目录或外置 patch 绕过 FEX 自身的 AI 代码贡献限制。若限制实际阻碍必要修改，明确报告，继续独立的 Android/API/测试工作，不能宣称 FEX 阶段完成。

公开 API 的 include/链接依赖不能出现 FEX 私有头、InternalThreadState、CPUState、Kotlin/JNI 类型。FEX 的编译选项与 allocator 配置不得传播成整个主仓的全局选项。

建议构建目标：`guest_cpu_api`、`guest_cpu_fex`、`guest_cpu_contract_tests`、`shadps4_validation_jni`。默认桌面构建仍能配置；新增 Android/FEX 目标由明确开关启用。

### 3.1 Foundation 复用（新增要求）

`foundation/` 已固定自有分支，`cmake/SpatialFoundation.cmake` 提供最小 `shadps4::foundation`
目标。V0 MUST 复用它的 DebugBus 注册诊断命令，写操作投递 owner thread；新建诊断
payload 的通用反射与编码 MUST 优先使用 foundation reflection/packing，完成真实 NDK
依赖闭包和 round-trip 验证，不另造框架。若因依赖问题暂不能启用，记录阻断，不能把该项标 PASS。
网络并非 V0 必需功能；若启用 TCP 控制，MUST 复用 NetSystemModule/DebugBus TCP，
补生命周期、线程归属、断线/超时和清理测试。CPU API 不暴露 foundation 元对象或单例。
本次只验证了最小 DebugBus；不要把反射/网络看成已经接入。具体闭包问题及 TLS 约束见接入记录。

## 4. NDK / bionic 构建要求

### 4.1 锁定工具链

M0 固定一个可取得的 NDK r28c 或更新的稳定版本，写入 `environment.lock.json` 的**完整 package revision、Clang 版本、sysroot API**。建议从 r28c 开始验证，遇到真实编译器缺陷再升级；不用不同机器各自的“latest”。

必须验证实际 `meta/platforms.json`、sysroot 与编译器支持所选 native API，而不只读
`source.properties` 或目录名。本次本地自报 r28c/r29 的安装实际都只到 API 35，API 36
配置失败，详见 Foundation 接入记录。API 35 库探针不能代替 V0 API 36 构建；应取得与
所选配置匹配的完整工具链，不修改 NDK 元数据伪造支持。

最小 app 的 minSdk/targetSdk/compileSdk 初始均为 36；V0 不承诺旧 Android 支持。选择一个官方兼容的 AGP/Gradle/JDK/CMake 组合，锁定版本及 Gradle wrapper 校验值；不要把历史前端的整套预览工具链当作必须条件。

NDK r28 默认启用 16 KiB 兼容对齐，且不再默认提供旧 PAGE_SIZE 宏；这不代替运行时页大小适配。[NDK r28 说明](https://github.com/android/ndk/wiki/Changelog-r28)

### 4.2 真正的 Android native 产物

- 必须使用 Android NDK toolchain、`CMAKE_SYSTEM_NAME=Android`、`arm64-v8a` 和 bionic/libc++。不能通过把 system name 伪装成 Linux 绕过 FEX 平台拒绝检查。
- FEX 当前根 CMake 只接受 Linux/Windows。要为 Android 接入提供真实的构建适配；已有 `BUILD_FEXCORE_ONLY` 是参考项目 patch 提供的开关，不是未经修改的上游能力。
- FEXCore 可静态链接进 JNI shared library；JNI 外层使用窄的 C/opaque-handle 边界，不跨 JNI 抛 C++ 异常。
- 不得依赖 `ld-linux-aarch64.so.1`、`libc.so.6`、`libstdc++.so.6`、GNU libc 符号版本、FEXLoader、Box64、rootfs、binfmt_misc、Termux/proot 或 adb shell 的特权环境。
- 只选择必要的 Core/host helpers，不整包带入 LinuxEmulation、ThunkLibs、GUI/config 工具。host 代码生成器在构建机运行，目标 ARM64 程序不得被 CMake 当成 host 工具执行。
- `ENABLE_JEMALLOC_GLIBC_ALLOC=OFF` 是必要起点；默认也关闭 `ENABLE_FEX_ALLOCATOR`，再检查最终依赖和分配入口。若保留 Core 私有 allocator，必须证明不 hook ART/bionic 堆且满足 16 KiB。这些开关不是“bionic 已兼容”的验收凭据。
- V0 不需要 Linux thunks 和 x86-32；因此不能为恢复这些未纳入的功能而重新引入 glibc hooks。
- 所有 APK native libraries，包括 libc++ 和第三方 prebuilt，都需验证 ELF PT_LOAD 的 16 KiB 对齐、APK ZIP 对齐；保留 unstripped symbols、Build ID 和部署 SHA-256。
- 一套最终 native library 运行 4 KiB/16 KiB 测试；不要用两个编译常量不同的 APK 冒充 flexible page size。

`mmap/mprotect/futex/signal/ucontext/TLS` 按 bionic/Android 实际 ABI 和 app sandbox 测试，不把 glibc 同名结构直接复制成兼容实现。[bionic 状态文档](https://android.googlesource.com/platform/bionic/+/master/docs/status.md)

## 5. 16 KiB 内存设计要求

必须交付 `page-size-audit.md`：列出实际链接范围内每个页常量/页掩码/分配/保护/释放/文件 offset 的用途、旧行为、修正位置和覆盖用例。查找包括 `FEX_PAGE_SIZE`、4096/0x1000、shift 12、PAGE_MASK、JIT/return/interrupt pages、allocator、VA 探测及 tracker。

区分三种量：运行时 HostPageSize、PS4 ABI 的映射粒度、FEX/GPU 的内部索引粒度。内部 4 KiB 代码索引可以保留；传给 host VM 的操作必须按真实 host page 对齐。不得全局替换常量，也不能把系统返回值伪装为 4096。

### 5.1 地址、权限和一致性

- reservation 不覆盖现有 ART/driver/host mappings；不依赖先读 maps 再 MAP_FIXED 的无锁空洞判断。MAP_FIXED 仅用于已由本模块持有的 reservation。
- V0 接受 host-page 对齐的 guest mapping；4 KiB 子区若要求同一 16 KiB host page 内不同 guest 权限，必须在修改前返回明确的 Unsupported，且映射原状不变。将来可增加软件权限支持，V0 不虚称任意 4 KiB guest mmap。
- host protection reasons 统一记账：guest 权限、guard、code write tracking、GPU dirty tracking。实际权限不得被某个 observer 的临时写入操作单独放宽。
- 对临时写入 trap 允许采用“停止相关 guest writers → 正常上下文记录 dirty/失效 → 在仍满足 guest 权限的条件下放开监控 → 恢复”的保守策略。放开整个 host page 时，必须处理其全部逻辑子页 observer。
- code publication、修改、卸载/同址重映射与 alias 更新都经过统一内存事务。跨线程代码失效必须等到旧 JIT block 不再执行后才返回成功。
- JIT 可采用 RW→RX 发布或分离 RW/RX alias；写入的 ARM64 指令需进行正确的 instruction-cache 同步。禁止以长期 RWX 及取消所有保护作为验收实现。
- V0 接入可复用的 dirty tracker 组件，并用 GPU observer test double 验证粒度与别名。该 observer 不代表现有整个 shadPS4 GPU page manager 已移植。
- ELF 多 segment/BSS 的共享 host page 处理先在 segment-mapping helper 上测试，不要求 V0 完整加载 SELF。
- 未对齐与跨页 x86 原子访问必须纳入实际执行测试。若 host 不原生支持，走受控正确性慢路径；不能通过丢弃 SIGBUS 或禁用原子案例过关。
- 保守保持 x86 内存顺序，固定实际 FEX 配置；记录并发 litmus 结果，但不把有限用例当作完整 TSO 正确性证明。

## 6. CPU API、HLE 与控制要求

完整规范见 [API 契约](android-fex-v0-api.md)。核心原则：

- 借鉴 Dynarmic 的 Config/Callbacks、Run/Step/Halt、寄存器和 cache 操作组织，**不复制 ARM ISA API，不保证 FEX 所有访存都经过 ReadMemory 回调**。
- Context、Thread、Invocation、GuestAddressSpace 和 Android Session 所有权分开。线程 affinity、可重入点和允许跨线程的方法必须可查。
- Run 的普通返回、暂停、取消、guest fault、单步结束、进入 HLE 等原因不能都折叠成 HLT。
- guest→HLE 使用参考的实际 typed ABI adapter 或有等价用例覆盖的迁入版本。至少验证整数/GPR 溢出栈、混合浮点参数、返回值、FS/GS、callee-saved 和嵌套 guest callback。
- 未覆盖 aggregate/varargs 签名在注册阶段报 Unsupported；不得把 host function pointer 或 bionic va_list 当作 guest ABI。
- Runaway guest loop 没有 HLE 调用时仍可暂停/停止；停止请求不能只在 HLE 入口轮询。
- 停止态寄存器快照必须完整定义有效字段。异步 host fault 至少提供真实 host PC/信号、线程关联和快照质量，禁止把旧 RIP 标成 fault RIP。

## 7. Android 生命周期和最小显示

验证 APK 使用 app 自身进程加载 JNI library。必须在 ART 存在、Java 活动与 JNI 回调同时发生的场景下通过，而不只是 adb shell 中启动 ELF。

最小页面有 Start / Pause / Resume / Stop / Run suite / Export，以及 SurfaceView 和状态信息。复用参考的 session/input 概念即可，首版不要求搬入游戏库、驱动下载和 runtime installer。

- Java 主线程只提交请求，不执行长时间 Run 或等待所有 guest threads。
- 通过 JNI 将 Java Surface 转成拥有明确引用生命周期的 ANativeWindow；Surface generation 变化触发渲染线程上的 drain/recreate。
- 使用 Android Vulkan Surface 和 swapchain，验证 resize、失效、后台/前台及重新绑定。不能依靠 X11/Vortek 才有画面。[Android Vulkan 官方说明](https://developer.android.com/ndk/guides/graphics/getting-started)
- Surface 丢失不代表 CPU session 必须销毁；默认让 CPU pause，在重新绑定后由明确 Resume 恢复。Stop 不等待一个永远不会出现的新 Surface。
- 原生 fixture 的 HLE 输出驱动 host 清屏颜色/计数；普通输入通过 session→guest buffer 或 HLE poll 进入 fixture，有可断言结果。
- 如果使用多个进程，必须先单独修改设计记录和 Surface/FD 传递契约；V0 默认不走独立 service 进程捷径。

## 8. 阶段与依赖

| 阶段 | 必交产物 | 进入下一阶段的门槛 |
|---|---|---|
| M0 环境/闭包 | 环境锁、FEX 源码/依赖锁、页大小审计初表、分支与修改范围 | 确认工具链、目标设备类型、适用贡献规则；所有不确定项有明确记录 |
| M1 NDK 可加载 | JNI shared library、最小 APK、ELF/依赖报告 | app 内加载 FEX 初始化成功；不依赖 glibc；这是阶段通过，不是 V0 完成 |
| M2 API + VM | 公共 API、memory/stop/state/registry 与最小 fixture | GPR/SSE2/HLE/回调、16 KiB mapping 与基本控制通过 |
| M3 正确性 | 失效、别名、跨页原子、多线程、有限 Step/LLDB | CPU/VM/debug MUST 用例通过；真实无 HLE loop 可中断 |
| M4 Android 演示 | native Surface、input、session 状态与生命周期 | app 内 guest→HLE→host frame/input 闭环，重建 Surface 与反复运行通过 |
| M5 发布候选 | 可重建 APK、符号、测试 runner、结果报告 | 真正 16 KiB ARM64 实机及 4 KiB 对照门槛满足；主仓相关回归通过 |

M0/M1 期间可以推进独立的 API 与 app 骨架；不能因图形耗时而删掉 CPU/VM 门槛。每阶段单独提交；不创建一份“全 PASS”的预填报告。除实际阻断外，执行者自行决定常规实现细节，持续推进。

## 9. 验收、失败与交付状态

以 [验收矩阵](android-fex-v0-acceptance.md) 为唯一完成检查表。每条 MUST 有结果与证据；核心项未跑不能写成 SKIP 后仍宣告 V0 通过。

允许的最终阶段状态：

- `V0_ACCEPTED`：所有 MUST 与指定硬件矩阵通过。
- `V0_IMPLEMENTED_DEVICE_PENDING`：代码及能执行的检查完成，但缺少指定设备/环境；列明 NOT_RUN，不宣称 16 KiB 通过。
- `V0_BLOCKED`：不可解决的实际依赖/能力/规则阻断，有最小复现、证据、已完成项和下一步。
- `V0_IN_PROGRESS`：仍有实现或已知失败待处理。

如发现需改变 FEX 的通用地址翻译架构、无法保证中断安全、必须依赖 glibc 或无法实现 16 KiB host 保护语义，先给出失败用例和决策记录；不得修改测试目标、吞 fault、虚报页面大小或禁用关键用例来过关。

交付至少包括：源码提交、依赖锁、debug APK + SHA-256、unstripped symbols + Build IDs、可重建 guest fixture、构建/设备测试脚本、逐项结果 JSON、简短实施报告。未含商业游戏。大二进制产物按仓库 artifact 约定保存，报告必须给出可取位置及 hash。

完成后新增里程碑状态文档，保留 [初始基础记录](../baselines/2026-09-07-android-fex-foundation.md)不变。
