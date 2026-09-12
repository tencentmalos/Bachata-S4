# WP-1 host 对象与首次链接复核

日期：2026-09-12。被审主仓 `1a7da92c514f11b9923dd13f6887a1733a076be5`，分支 `codex/android-fex-round2`；本次 `git ls-remote` 确认远端同提交。审核范围是 `d9df1d74..1a7da92c` 的四次提交及其实际使用路径。附带 AI 执行记录仅作为待核对的陈述。

**结论：完整 host 源集已能生成 Android 对象，第三方链接障碍明显减少；完整 host 共享库、生产 FEX 接线和游戏 APK 仍未完成。下一步应一次完成生产 Android Runtime、APK 和真实 PKG 链路，不能把“实现 9 个同名符号”当作剩余工作量。**

执行入口：[生产 Runtime 整块实施 spec](../../specs/android-native-host-production-runtime.md)。既有 [PKG v2 验收](../../specs/android-native-host-pkg-v2.md)继续有效。

## 已独立核实的进展

| 项目 | 本次观察 | 能证明什么 |
|---|---|---|
| CMake 正式源集 | 根 `BUILD_HOST_CORE=ON`，`shadps4` 为 OBJECT，另有 `shadps4_host` SHARED 诊断目标 | 已从零散 TU 扫描进入真实构建图；目标尚不是完整运行库 |
| 对象构建 | 重新生成已有构建图并执行 `cmake --build /tmp/host-core-cfg --target shadps4 -j 6`，exit 0 | 是增量构建，不是干净 checkout 全量重建 |
| 对象身份 | **388/388** ELF `ET_REL / EM_AARCH64`，逐文件 SHA256 | 387 是添加 AAudio 前的数量；当前包含 `aaudio_audio_out.cpp.o` |
| 最终链接尝试 | 使用实际 Ninja 链接命令，额外设 `--error-limit=0`；刷新对象前后均 exit 1，9 个唯一未定义符号 | NDK 已提供 `--no-undefined`，不存在本轮“放开未定义符号得到成功库”的情况 |
| host 产物 | 构建目录不存在 `libshadps4_host.so` | 未通过 full-host link，不能写成“已生成完整 host 库” |
| 第三方 | 当前链接诊断仅剩 Emulator/WindowSDL/g_window；AAudio vtable 不再缺失 | 对当前源集和配置是积极结果；加上 FEX、Foundation、入口及不同保留路径后仍需重新链接 |
| APK | runtime CMake 仍仅加入 SessionCore、FexSessionBackend 和 `guest_cpu_fex` | 本轮没有把根 host 对象接入 APK，也没有运行真实游戏 |

9 个符号的完整签名、真实失败日志、对象身份和配置见[本次证据](2026-09-12-host-closure-review/README.md)。不以文本摘要替代链接退出状态。

## 需要修正的问题

### P1：ARM64 Ucontext 留下未初始化寄存器，信号分发却按有效上下文使用

位置：[exception.cpp](../../../src/core/libraries/kernel/threads/exception.cpp) 的 ARM64 构造分支（99–107 行）及 `SyncHostFromGuest`（231–234 行）；消费者是 [signals.cpp](../../../src/core/signals.cpp) 的 `SignalHandler`（257–265 行）和 [pthread.cpp](../../../src/core/libraries/kernel/threads/pthread.cpp) 的 `DispatchSignal`。

注释称 mcontext 会默认清零，但 [Ucontext/Mcontext](../../../src/core/libraries/kernel/threads/exception.h) 没有成员默认初始化，构造函数也没有初始化列表。`Ucontext context{info, raw_context}` 调用用户提供的构造函数，不会先清零整对象。新增 ARM64 分支只写 `mc_addr`，紧接着 `SignalHandler` 就读取 `mc_rip` 填入 guest signal 地址。

本次用预填 `0xa5` 的存储 placement-new，**链接已有正式 `exception.cpp.o`**，在 AYN/API33/4KiB shell 运行，得到：

```text
rip_object_bytes=a5a5a5a5a5a5a5a5
rip_storage_untouched=1
mc_addr=12345000
sync_leaves_host_pc_unchanged=1
```

探针只检查对象表示，不求值未初始化整数；没有复制生产构造逻辑。它证明寄存器未被写入，源码另确认 ARM64 Sync 当前为空；host PC未变本身不是guest写回测试。没有触发真实 fault，也不是 guest signal 或 APK 验收。详见[探针与日志](2026-09-12-host-closure-review/README.md)。

修复必须包含有效性和分发边界：未取得可信 guest 状态时，不能读取、投递或恢复一个貌似有效的 mcontext。清零只能消除未初始化数据，不能生成 guest 状态。ARM64 host `ucontext` 的 PC 是 host PC，`si_addr` 也不能无条件标成 guest 地址；异步 JIT stop 不能直接读可能尚未 spill 的 CPUState。需要依据固定 FEX 布局/host-PC 元数据和 owner 安全点构造 guest exception frame，并通过正式回调/恢复路径消费。做不到时有明确不支持/故障归属，不能用空 Sync 假装恢复成功。

现有 `CallbackWrapper` 还会直接调用 guest handler 地址，因此这项应与生产 guest callback/signal 路径一起完成，不能另补一个 host 函数占位。

### P2：本轮首链配置与生产 APK 不一致，缺少可复现的完整构建入口

位置：[根共享库目标](../../../CMakeLists.txt) 1522–1551 行，以及 [host-objectlib 配方](pkg-v2/host-objectlib-2026-09-12.txt)。

本轮命令未指定 `ANDROID_STL`，实际链接含 `-static-libstdc++`，Foundation 明确 OFF；APK 的 [Gradle](../../../android/shadps4-app/core/runtime/build.gradle.kts) 明确指定 `c++_shared`，还需连接 FEX。两者不是同一生产配置。不能把当前依赖 archive/DSO 直接拼入 APK，并据九符号结果宣布整个生产链接闭合。所有消费者应在一致 NDK/API/STL 下重建，最终进程只使用对应的一份共享 libc++。

此外，font embed/protoc 目前依赖 `/tmp/host-fontembed`、`/tmp/host-protoc/protoc` 的人工准备，仓库没有将它们的匹配源码、构建和身份检查接成正式 host 构建入口。外部工具 override 的 DEPENDS 为空，替换工具本身不触发重新生成。根目标又复制了一份依赖列表，容易与 OBJECT、FEX 和 app 分叉。整块实施时应统一源/依赖 target、构建机工具和最终 JNI target；验证无 `/tmp` 前置的干净构建。

根目标注释所称“已链接 guest CPU、JNI 将其作为现有库装载”尚未发生；`--no-undefined` 则已经由 NDK 启用。这些陈述应按真实接线更新。

### P2：Android epoll 回退把正的亚毫秒等待变成轮询

位置：[net.cpp](../../../src/core/libraries/network/net.cpp) 1013–1022 行。

切换到 `epoll_wait` 对 API33 有必要：本机 NDK 的 `sys/epoll.h` 将 `epoll_pwait2` 标为 API35。但新增 Android 路径直接 `timeout / 1000`，使 1–999μs 变成 0ms，1001μs 变成 1ms。无事件时会提前返回，guest 循环可能变成忙轮询；这是超时语义变化，不只是编译兼容。

至少对正值使用不会整数溢出的向上取整，保留 0 的轮询和负值的无限等待语义；若需要更高精度，再实现有预算的 Android 路径。验证 0、1、999、1000、1001、INT_MAX、负值和 EINTR，不能用不支持的 API35 函数直接提高 API33 产物的最低运行要求。本项是代码审查发现，本轮未做计时实测。

## 九个符号背后的生产集成缺口

以下主要是既有未完成项，本轮没有将它们修复；它们不应误报为新增回归。

| 现有调用路径 | 为什么补符号不够 |
|---|---|
| `Linker::RunMainEntry` | ARM64 分支仍 `UNREACHABLE`，没有 FEX 主入口 |
| `Module::Start`、`_malloc_init`、heap callbacks、pthread/once/destructors | 仍含将 guest 地址作为 native 函数调用的路径，ARM64 能编译不等于能执行 x86 |
| `LIB_FUNCTION`/resolver | 需要 guest veneer＋typed ABI registry，给链接器加入 `guest_cpu_fex` 不会自动把 host 函数地址变成 guest gate |
| `ISessionBackend` / `SessionParams` | 当前输入只有 diagnostic content_id 与 smoke iterations；没有正式内容/模块/Surface/driver 启动描述 |
| `g_window` / `WindowSDL` | GNM 创建 Presenter 时解引用，renderer 还内联读取尺寸/WindowInfo。仅补键盘/icon 空函数不能解决窗口对象及 generation/lifetime |
| SDL 的 JNI 初始化 | 当前链接命令带 `-u JNI_OnLoad`，静态 SDL 的 `JNI_OnLoad` 会查找/注册 `org/libsdl/app/*`；当前 app 没有这些 Java 实现。尚未实际加载此库，不能断言发生过崩溃，但直接装入现有 APK 会遇到未处理的 JNI 集成条件。仅保留 SDL utilities 也需明确其构建和初始化边界 |
| VM | 桌面 USER_MIN 从64GiB起，而现后端 V0 policy limit 为64GiB；需要实际布局/映射/高低VA验证，不能当作 FEX 不可突破的硬上限 |
| 异常/退出 | 旧 signal 注册覆盖 host handlers、回调 native 调用、desktop singleton 和退出路径需与 ART/FEX/SessionCore 协调 |
| Turnip | `CreateInstance` 仍从默认 DynamicLoader 取入口；没有持有所选 bionic Turnip 的 loader/namespace/dispatch 身份 |

不能由“旧 signal/entry/VM 对象链接成功”推导其 ARM64 语义已经迁移。下一轮应把这些路径接入同一个生产 session，再用实际 PKG 驱动必要 HLE，而非另建测试专用 Run driver。

## 验证范围与交付建议

本次没有修改生产代码，没有安装新 APK或运行游戏；保留原 `references/Bachata-S4` 脏文件和未跟踪 `externals/dear_imgui/`。只新增复核证据、报告与执行 spec，更新导航。没有把本轮负例重写成旧版本的 PASS。

后续按照[生产 Runtime spec](../../specs/android-native-host-production-runtime.md)连续做完：统一构建/JNI → 同一个 session 的生产 guest/VM/HLE/异常/回调 → Turnip/Surface/输入/音频 → UI 本体＋更新和真实场景验证。内部可小提交和用 fixture 定位，最终仍以真实 TMNT 可操作场景、十分钟、有界 Stop 与同进程三次重启为准。没有可执行库或 APK 时，状态仍为 `IN_PROGRESS`。
