# WP1 复核、修复与生产运行时接通

日期：2026-09-13。被审基点 `4b2a72db`；基础修复提交 `07ce52ae`，其后的运行时、测试和文档随本次交付提交。证据采集时为 `07ce52ae + dirty`，不能把旧二进制身份改写为最终提交。[原始证据及复现入口](2026-09-13-wp1-review/README.md)。

**结论：生产 Module/Linker、统一 VM、真实 guest pthread/TLS、可取消 HLE 与嵌套 InvokeGuest 已经在本轮组成运行时，并通过普通 APK 的生产 Service 执行。** 后续任务是扩展真实游戏使用的 Orbis 函数族和游戏平台；不再把这套连接交给下一位 AI 从头实现。它仍不是 TMNT 可玩或 Swan 验收。

## 复核发现及本轮处理

| 问题及来源 | 处理与证据 |
|---|---|
| 原 InvokeGuest 没有完整恢复调用者栈；G41b/c 真机失败，225 checks 中2失败 | 调用 frame 的验证、状态补丁与 admission 统一；整数栈参、临时 FS、RIP/RSP/flags 恢复；保留错误 owner/sibling/direct reentry 负例 |
| Adopt 修改可共享 descriptor 的 operation/name，另一个 registry 会改变已有绑定；6项负例失败 | binding 独立且不可变；null、unsupported/raw callback 的拒绝不消耗 operation；registry13/0 |
| “有模板即有生产 ABI”不成立，raw pointer/callback/aggregate/varargs 不能自动放行 | 自动 factory 保持保守；生产按签名和地址策略显式适配，不把 ARM64 函数指针写入 guest GOT；未知函数变为具名失败 operation，未知数据仍拒绝 |
| host 构建缺失 std::span 适配，host/JNI 重复链接 CPU API，prebuilt profile 不一致 | 修复 host 全链接；host 唯一提供 GuestAddressSpace/lease/输入，JNI/FEX 消费生成 SDK；拒绝错误 ABI/API/STL 和来源缺失的旧 prebuilt |
| 原 HLE 在 JIT C++ crossing 中，VM drain 会等待自身 Run lease，也不能安全嵌套 dispatcher | per-owner NON-spill stop 先退出 ExecuteThread；在 host HleScope 中执行 HLE；释放本层 pin/lease，回调使用新一层受控 ExecuteThread；返回时恢复外层 continuation |
| Android ART 与整段低 VA/旧 allocator self-steal 冲突 | guest 只拥有4MiB–256MiB、4GiB–120GiB两个区间，保留 ART 中间段；exact advisory mmap 冲突即回收失败，绝不扫描 maps 后覆盖；FEX 保留 rpmalloc，使用 pinned allocator 的普通 mmap/munmap hooks 初始化，不替换 bionic malloc、不 steal 高 VA |
| 扩大 VA cap 本身不能排除 FEX lookup 别名 | G46 执行相距64GiB、低36位相同的两个代码地址，100轮交替并单独 remap；全地址 tag/失效的当前 pin 行为通过 |
| APK child 热循环绕过 InterruptFaultPage，Quiesce 超时后 join 卡住 | LLDB 看见条件回边跳到 entry poll 之后；主仓 EntryBackedgePass 改 guest-entry 边为 ExitFunction。G47 先由 guest counter 证明≥10000次回边，再取消；生产 child 热循环期间父线程 VM 修改也通过。没有修改 FEX child 或设置 MAXINST=1 |
| 共享库默认2GiB hint 落到 ART 空段，旧 MemoryManager 搜索触发断言 | 非固定 hint 推进到下一 guest VMA，固定请求必须属于实际 owned range；Module 装载错误改为可回收 Result/exception |
| 装载需要真实 RELRO/TLS/模块初始化，不能用旧 prologue harness | 直接改生产 Elf/Module/Linker：段和动态表边界、RELATIVE/PLT/符号/TLS重定位、最终权限、DT_NEEDED图及依赖顺序 DT_INIT、每 owner TLS模板/TCB/DTV。主程序及初始化代码都由 FEX 执行 |
| 真实 SELF 的 e_shoff 指向原 ELF 逻辑节表，不能按容器物理 offset 校验 | SELF 只消费实际嵌入的 program headers 与 segment records；普通 ELF 节表仍严格校验。自编 SELF 和真实 TMNT 均覆盖；不清零 RELRO、不忽略实际段读取失败 |
| 游戏依赖位于 modules/，数据 import 也不能填 host pointer | 增加内容根下 modules/ 搜索并校验 canonical containment；`__stack_chk_guard` 使用 session 所有的 guest 只读8字节随机数据，GLOB_DAT 指向 guest 地址；同一域还提供guest程序名指针及空环境数组，其他未声明数据策略继续拒绝 |
| 真实libc/Fios用未命名STT_SECTION作为当前模块DTPMOD64，严格STT_TLS检查误拒绝 | 仅为该零值local module marker提供明确兼容；offset重定位与named symbol仍严格检查；依赖fixture验证DT_INIT前的DTPMOD64=2，真实样本记录见tmnt-tls-relocations.json |
| 模块初始化期间 Cancel 被转换成 BackendFailed | 同步 Module::Start wrapper 保留完整 guest stop/fault identity；Cancel 不进入主入口；增加初始化取消及恢复场景 |
| Service 错误 START 会 stopSelf，从而干扰既有 session | 无有效新内容时，只在没有活跃 generation 时结束 Service；真实游戏必须传已安装内容路径，无失败后 CPU smoke fallback |

## 当前可复用的实现

`src/core/host_runtime/guest_runtime.*` 是 generation 所有者：生产 Linker/MemoryManager、mount/ElfInfo、guest backing、owner线程、栈和静态 TLS、registry/veneer 均在同一资源域。借用旧 Singleton 的 Binding 仅提供迁移入口，不把运行时对象改回进程永久所有；清理先取消和 join，再回收 guest 地址空间。移除了 font/save 路径缓存的旧 mount 指针。

`HleScope` 位于 `src/core/guest_cpu/hle/scope.*`，同 native owner 的合法回调经 scope 进入，最大深度8；普通 HLE 原函数不能直接重入 Run 或 sibling owner。两层回调、最深层 thread exit、nested exception/fault 粘性、native errno/fenv 恢复、WaitingHle 取消均有 G45 真机证据。没有使用 SleepThread、host longjmp 跨 C++ 栈或 FEX HandleCallback 停放外层 dispatcher。

生产 VM 复用 GuestAddressSpace 的 token/lease/pin 和 shared/live/decoder retirement。其他 owner 的内部 Pause 只有在退出 JIT、释放执行资格后才算 parked；token 结束后重新取得 lease 并恢复原 Run。用户 Cancel 保留优先级，不由内部 drain 清掉。保护/映射失败仍 poison，不能恢复旧译码执行。M30/M31 验证事务，M32 验证 hole 不被当作 guest-owned。

当前显式生产 HLE 包括 timed usleep/guest errno、pthread self/create/join/detach/exit/once、TLS address、flexible map/protect/unmap。pthread entry 与 once callback 真正在 FEX 执行；guest FS/TCB/errno 独立，不借用 bionic TCB。整数 veneer ABI 的可靠性不表示所有宿主 HLE 签名已可直接运行。

`FexSessionBackend::Prepare/Run` 已按 executable path 选择生产 GuestRuntime；`nativeStartExecutable` 与普通 FexSessionService 消费同一路径。自编 ELF/SELF 通过安装清单、Service、SessionCore、生产 loader 运行；测试 APK 的 assets 由源码生成，无游戏数据。

## 验证口径

最新结果以 `canonical-runtime-close/manifest.json` 为准；早先 canonical-final、canonical-init-stop、canonical-delivery 是各自时刻的有效阶段证据。主目标仍 Swan/API36/4KiB，本轮在线的是 AYN Thor/API33/4KiB，普通 APK UID10157。shell 证据与 app 证据分列。

- guest_execution_tests：242 checks / 0 failures，包括 G45嵌套/取消/异常/VM、G46高VA、G47热回边。
- device contract：46/46；ABI14/0、registry13/0、veneer19/0。macOS现代LLVM contract45 PASS+1 SKIP/46，ABI14/0、registry13/0、veneer19/0，SessionCore807/0。
- host DSO `--no-undefined`、host dlopen及85/0；依然无 Android SDL/JNI_OnLoad 重复定义。
- canonical CLI：正常ELF、SELF、等待取消、坏指针、未知导入、初始化取消、截断ELF拒绝共7组，每组3轮；坏内容的预期拒绝不是游戏成功。
- 普通 APK：1个 instrumentation test，12个不同 generation，同一 PID/UID；正常返回0xcafe、取消、坏指针/未知导入 fault、初始化取消，随后均能重启。真实双模块、模块TLS与DT_INIT、主/子线程TLS及errno、child热循环期间VM、once回调。本次PID11906/UID10157；源 host 与 APK strip 后 Build ID均为 `4a0b9d2c5a5e803a92c3005a2a468d08ca6d0cee`。

自编程序几秒内完成；不是十分钟游戏、物理手柄震动或 GPU 验收。正式 R2/V0 的历史 NOT_RUN 不因新增 check 数而改写。一般 guest SIGILL/SIGSEGV 的恢复仍未闭环（旧 UD2 进程退出证据保留）；operation-zero 空槽和 HLE 结构化失败不能冒充通用指令异常处理。

## 真实 TMNT 的边界与后续方向

从已核验本体01.00后覆盖更新01.08选择 eboot、guest模块与param.sfo，未把游戏文件提交。旧 eboot-only 仅为01.00且欠FMOD依赖，不能用它代表完整1.08游戏。新选择清单见 `tmnt-content-selection.json`，最新运行为 `tmnt-tls-marker/run.log`：七个模块读取并推进重定位后，Prepare明确拒绝 `ZT4ODD2Ts9o#libSceLibcInternal#1#libSceLibcInternal#Object`，AeroLib名称为 `Need_sceLibcInternal`。退出1，没有执行该游戏DT_INIT/主入口，没有host崩溃或错误零值继续运行。这批选择仍缺其余游戏assets，不是UI完整安装。

真实内容已推动本轮修复 SELF 节表解释、modules/ 搜索，以及 guest stack guard、__progname、environ 数据。后续沿最后一个确证失败继续补 **Orbis函数族语义/参数策略和平台依赖**，不要删除 guest libc/模块初始化以跳过问题。动态模块增卸、非默认 pthread attributes、mutex/cond/key析构、复杂ABI、信号和媒体回调需要随真实调用补覆盖；已有 owner/TLS/HleScope/VM 机制直接复用。

下一位连续执行[TMNT整版 spec](../../specs/android-native-host-tmnt-after-runtime.md)：游戏必需HLE与guest libc启动 → Turnip/Surface/audio/FEX-origin pad/内容版本事务 → TMNT交互十分钟及三次同进程游戏重启。不得把“先重新组装生产运行时”作为新工作包。
