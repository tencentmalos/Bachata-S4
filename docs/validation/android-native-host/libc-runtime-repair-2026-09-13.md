# Android guest libc 启动与服务修复

日期：2026-09-13。被审基点 `d499219e5b7267fbbceb8b8f0be2d07c76974929`；本轮直接实施修复，未修改 FEX/Foundation 子仓。下列二进制在该基点加工作区修改时构建，保留其实际 SHA/Build ID，不倒写成后来的提交身份。

## 结论与原报告修正

真实 TMNT 已越过原报告的空指针崩溃，实际完成六个依赖模块的 DT_INIT，进入主程序并在尚未实现的 `sceSysmoduleLoadModule` 返回具名结构化 Faulted。它仍不是游戏可玩验收：本轮真实内容验证是 AYN/API33/4KiB 的 shell runner，只部署本体加更新选出的 executable/modules/param.sfo，没有完整 assets、UI 安装版本 lease、Turnip 画面或十分钟运行。普通 APK 验证使用自有合成程序。

本轮确认了四个原报告问题：

1. `Module::Start` 原日志在调用 DT_INIT **之前**输出，不能证明初始化完成。本轮添加返回后的完成日志；bootstrap 还须先经过 `_malloc_init` 和可选的 libc 内存互斥初始化。
2. `0-KXaS70xy4` 是 `pthread_getspecific`，原来误绑为 clock_gettime。真正 clock_gettime 是 `lLMT9vJAck0`。因此原来“首个 HLE 是时间调用且返回正确”的判断不能概括所有相似日志。
3. `GetGuestBlockEntry` 的 `0x445c5f` 是块归属，不是精确 faulting RIP；JIT PC 也不足以证明所有故障都是 guest 地址访问。离线检查显示相邻代码是 malloc 后的字符串扩容/终止符写入，libc malloc 又依赖 `_malloc_init` 初始化的状态。生产 Run 原先漏调该函数，这是本次修复所针对的启动机制；补齐后实际游戏越过此处。不能仅因时间上紧接 HLE，就归咎 clock_gettime 或猜测要保留 RCX。
4. 游戏 `libc.prx` 与 `libSceLibcInternal` 不能笼统视为同一库。真实 `Need_sceLibc` 导出大小为 4 字节；原先无依据的 8 字节零对象及 Linker 的全库别名已移除。

有界的模块 SHA、符号大小/NID及解释见 [libc-source-audit](2026-09-13-wp1-review/libc-source-audit/observations.json)。游戏代码字节及重建 ELF 不入仓。

## 本轮实现

- **启动顺序与数据解析**：owner/TLS 就绪后，通过已有 FEX Call 调用 guest `_malloc_init`，检查返回并传播取消/错误；再调用可选 mutex enable，然后按依赖顺序启动模块。仅对明确的 Internal 标准流和 dependency tag 使用真实 guest libc 导出，检查提供方歧义；这是限定兼容策略，不是完整 stdio ABI 或两库等价的证明。无提供方仍拒绝。
- **堆与 VM 接口**：heap callback table 只保存经可执行范围验证的 guest PC，不写入 desktop 原生函数指针表。heap trace 输出明确 guest-owned 结构/掩码/表；接入 proc params、direct/flexible 查询、direct allocation/map，并用 CallCursor 正确取 named direct map 的第七个栈参数。VM 变更继续走原 MemoryManager/VmGuard；SDK判断读取当前 guest 配置，不继承未初始化的 desktop 全局。没有启用另一套内存账本或通用原生 guest 指针调用。
- **时间与错误 ABI**：使用 session 自有时钟，避免 desktop RegisterTime/原生 pthread errno 的隐式前提。POSIX 错误写当前 guest errno，SCE 返回对应错误码；检查 timespec 指针、负数、纳秒范围、总时长溢出，nanosleep 经可取消 scope 分段等待，取消时写实际剩余时长；gettimeofday 使用本地结构再写回 guest。当前时区为 UTC，网络时钟无 provider 时明确拒绝。
- **mutex/cond**：Orbis 句柄指向 guest ABI 前缀，host map 保存实际同步状态；libc 可修改 `+0x20` flags，不暴露 native mutex/pthread/std::string。支持基本类型、递归/错误检查、trylock、ownership、默认 cond signal/broadcast/wait。锁等待与重新加锁均可被 session Stop 取消。通知后仍保留 cond waiter，直到重新加锁/退出完成，防止 CondDestroy 与 RAII 清理形成 use-after-free；异常出口也释放 waiter 计数。
- **TSD**：正确接入 key create/delete/get/set，session key sequence 防止删除复用后读取旧值，owner value 独立。正常线程退出最多执行四轮 guest 析构，调用前先清值、锁外经现有回调边界执行，错误/取消继续传播；主线程最终返回也反映析构结果。终止整个 session 时不模拟完整 POSIX pthread_cancel 析构语义。
- **诊断**：保留原 SIGBUS 非对齐回补，并以真实 x86 非对齐原子 fixture 验证。SIGSEGV 诊断改为有限栈缓冲和 write，移除信号处理器内 Android logging；仍转发原信号，不进行不可靠的中途寄存器 spill。**通用 SIGILL/SIGSEGV 恢复仍未实现。**

mutex/attribute/cond guest 分配目前每 session 累计上限 4096 个、每个 16KiB，销毁后至 generation teardown 才回收。非默认 cond attributes、timed/rwlock/protocol 等未因本轮基本接口通过而自动开放。保存的 guest heap API 尚不代表动态 TLS/模块及全部 host 分配都已迁移至它；硬件内存配置/持续创建线程资源回收仍按整版 spec 处理。

## 验证与身份

最终结果见下列 manifest 与原始日志。AYN Thor `9c2841a4`，Android API33/ARM64/4KiB；Swan/API36 **NOT_RUN**。

最终普通 APK 的 PID 为 `26349`、UID 为 `10157`；16 代均保持一致。源 host 和 APK 内 host 的 Build ID 均为 `446ec4a0d98923fd62bd03d2796c74a484885cb5`，host SHA256 为 `2a166595fe09d904751d6823349fd2bd751fca90acba09539fb2be2153f4b048`；最后的 TMNT 运行也使用该 SHA。canonical manifest 的 14 个修改源码摘要已与交付工作区逐项核对。

| 验证 | 结果与范围 |
|---|---|
| [libc-lifetime-apk](2026-09-13-wp1-review/libc-lifetime-apk/manifest.json) | 10 组 CLI 各 3 轮；普通 APK 16 个同 PID/UID generation，包含 libc bootstrap、bootstrap 中取消、真实 FEX 服务 fixture、旧 ELF/SELF/线程/TLS/VM 与失败恢复；host/APK Build ID 一致 |
| [libc-lifetime-cpu](2026-09-13-wp1-review/libc-lifetime-cpu/device-result.json) | guest_services_tests **43 checks / 0 failures**；新增“通知后等待重新加锁期间销毁必须 Busy”的确定性并发检查 |
| [libc-final-cpu](2026-09-13-wp1-review/libc-final-cpu/device-result.json) | 实际 FEX guest_execution_tests **242 / 0**；本轮稍早 services **41 / 0**，之后增加上述两项，并非同一旧结果改总数 |
| [libc-host-close](2026-09-13-wp1-review/libc-host-close/result.json) | 正式脚本生成 NDK arm64/API33/c++_shared host，HOST_LINK_PASS；源文件摘要与构建期间不变检查保留 |
| [tmnt-libc-close](2026-09-13-wp1-review/tmnt-libc-close/manifest.json) | 真实模块启动后的具名 sysmodule 边界，runner 非零退出，**不是 PASS 游戏** |

新增 `runtime_services.S` 会实际调用启用的服务：检查 POSIX/SCE 坏时钟、guest errno、非法时间、mutex ABI 前缀/递归/销毁、main/child TSD 隔离与四轮析构，以及非对齐原子。新增独立 libc fixture 强制验证 `_malloc_init → mutex enable → DT_INIT → main` 顺序，并在 `_malloc_init` 内等待以验证 Stop 和后续重启。测试不是仅检查注册表有一个 NID。

复现仍使用 [canonical 命令](2026-09-13-wp1-review/README.md#构建和复现)，无需临时 include/archive 清单。最终构建与 APK 日志在 `libc-host-close/`。`libc-apk-close/` 是修复 cond 生命周期之前已通过的 16 代证据，保留作过程记录；最终以 `libc-lifetime-apk/` 为准。更早 `services-first/` 没有运行 APK，不能混称同一次 app 验证。全部临时设备部署目录已清理，manifest 保存退出值。

## 下一实际边界

继续同一份 [TMNT 整版 spec](../../specs/android-native-host-tmnt-after-runtime.md)，不重做生产运行时接线或再按单个 NID 拆交接。当前运行记录为 `g8cM39EUZ6o#libSceSysmodule#1#libSceSysmodule#Function` 未实现；模块 ID 必须从实际调用参数确认，离线 callsite 的候选常量不代表实际执行顺序。

不能直接把 desktop `sceSysmoduleLoadModule` 当无状态函数绑定：现实现使用全局模块引用/handle，部分缺失 provider 会报成功，动态加载还会开始模块并影响 TLS。应沿当前 session 模块图补 provider readiness、加载/卸载引用、事务与 guest 调用/取消，再同时接通真实依赖的文件、用户、平台、Turnip/Surface/音频/pad；未知或缺失 provider 明确报错。后续目标仍是普通 APK 的真实内容完整链路、可操作场景、十分钟和三次同进程游戏重启。
