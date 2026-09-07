# FEXCore 的 Android NDK/bionic 构建

本文记录 FEXCore 首次成功为 Android NDK/bionic 构建并在实机运行的完整过程：每一处需要的改动、
它解决的具体问题，以及证据的确切边界。

- FEX 分支：`feature/malos/host-page-size`，提交 `385a0cc4d`
- 构建入口：[`scripts/android/build-fexcore-android`](../scripts/android/build-fexcore-android)
- 实机验证：[`scripts/android/run-fexcore-smoke`](../scripts/android/run-fexcore-smoke)
- 验证程序：[`tests/fex_bionic/fexcore_bionic_smoke.cpp`](../tests/fex_bionic/fexcore_bionic_smoke.cpp)
- 前置改动：[host page 适配](fex-host-page-size-adaptation.md)

## 1. 这一步的意义与既有认知的差异

此前的判断（见 [V0 实施报告](validation/v0/implementation-report.md) §4）是：
参考实现 `references/Bachata-S4-android` **从未**用 NDK 构建过 FEXCore，
它用 `-DCMAKE_SYSTEM_NAME=Linux` 加 glibc 交叉工具链，再靠设备上的 Debian rootfs 容器执行。
其 APK 验证器甚至**主动禁止** FEXCore 进入 `jniLibs`。

这个判断本身是准确的。但由此容易推出一个**更强、并不成立**的结论：
"FEXCore 不能为 bionic 构建"。实测表明只需五处适配即可，且 FEXCore 能在 Android 进程内
完成初始化与线程生命周期。glibc 路线是参考实现的**选择**，不是唯一可行路径。

## 2. 硬性前置：必须 NDK r29 或更新

这是唯一无法在 FEX 侧绕过的要求。

FEXCore 的 `SpinWaitLock.h` 与 `WritePriorityMutex.h` 使用 `std::atomic_ref`（C++20）。
NDK 附带的 libc++ 在 **r28c 及所有更早版本中完全没有 `atomic_ref` 头文件**——
不是"未启用"，是不存在，加 `-fexperimental-library` 同样无效。

本机八个 NDK 全部实测：

| NDK | `atomic_ref.h` |
|---|---|
| r25.1 / r26.1 / r26.3 / r27.1 / r27.3 | 无 |
| r28.2（两个安装，含记录中的 r28c） | 无 |
| **r29.0.14206865** | **有** |

r29 的 libc++ 版本 21，`__cpp_lib_atomic_ref = 201806`，已在设备上实测可运行。

这与上一轮的环境锁（r28c）冲突。构建脚本因此**检测 `atomic_ref.h` 是否真实存在**，
而不是比较版本号或目录名——本仓已有"目录名 `28.2.13672827` 自报 `Pkg.Revision 28.2.13676358`"
的先例，标签不可信。

注意 r29 在本机 `meta/platforms.json` 仍只到 API 35，所以 native 目标仍是 35，
[DEC-01](validation/v0/decisions.md) 的结论不变。

## 3. 五处改动

全部由"实际构建 → 读报错 → 定位"得到，不是静态审阅推测的。

### 3.1 平台白名单拒绝 Android

`CMakeLists.txt:65` 原本只接受 Linux 和 Windows，`CMAKE_SYSTEM_NAME=Android` 直接
`FATAL_ERROR`。Android 本质是 Linux 内核 + bionic，加入白名单即可。

同时在该分支关掉 `ENABLE_JEMALLOC_GLIBC_ALLOC`：bionic 没有 glibc 分配器可挂钩，
jemalloc 的 glibc 集成假设了 glibc 内部结构。

**为什么不沿用参考实现的 `SYSTEM_NAME=Linux`**：那样能骗过检查，但 CMake 会去找 glibc
交叉工具链和 sysroot，产物链接 `libc.so.6` 与 `/lib/ld-linux-aarch64.so.1`，
**无法装载进 Android app 进程**。这正是参考实现必须外挂 Debian rootfs 的原因。

### 3.2 `TUNE_CPU=native` 读构建主机的 `/proc/cpuinfo`

默认 `native` 会执行 `Scripts/aarch64_fit_native.py` 与 `NeedDisabledSVE.py`，
两者都读 `/proc/cpuinfo`。macOS 上该文件不存在，`string(STRIP)` 收到空值直接报错。

更重要的是：**即使能读到，交叉编译时读构建主机的 CPU 也是错的**——会针对错误的 CPU 生成
`-mcpu=`。改为 `CMAKE_CROSSCOMPILING` 时默认 `none`。

### 3.3 bionic 没有 `valloc`

`AllocatorHooks.cpp:194` 的 fallback 分配器转发 `::valloc`。bionic 未提供它
（POSIX.1-2001 标记废弃，2008 年移除）。

按其定义"页对齐的 malloc"用 `posix_memalign` 精确实现，而不是退化成普通 `malloc`
丢掉对齐保证。

### 3.4 新增 `BUILD_FEXCORE_ONLY`

`Source/Tools/` 是 **Linux 前端**（`FEXInterpreter`、`LinuxEmulation`），
它拥有进程、信号处理与 syscall 分发——这些恰恰是嵌入方自己提供的部分。
嵌入方需要的只有 FEXCore 本体，加上 `Source/Common/HostFeatures.cpp`（读真实 ID 寄存器）。

该选项同时把 `Common` 裁到必需文件。`FEXServerClient.cpp` 在 libc++ 下编译失败：

```
fextl/functional.h:55: static assertion failed due to requirement
  noexcept(this->internal = std::move(wrapped_lambda)):
  This implementation of std::function does not support implementing fextl::move_only_function
```

`fextl::move_only_function` 依赖 libstdc++ `std::function` 的内部实现细节，libc++ 不满足。
而 FEXServer 是与守护进程通信的，嵌入方不需要，裁掉即可，无需改动这个断言。

### 3.5 共享库 target 无法独立链接

`FEXCore_shared` 只链接 object 文件，**除 MinGW 外不链接 `FEXCore_Base`**
（`FEXCore/Source/CMakeLists.txt:279-288`），指望 Linux 前端在最终链接时补齐符号。
单独构建它会得到几十个 undefined symbol（`FEXCore::Config::*`、`XXH3_*`、`fmt::*`）。

嵌入方的正确形态是把**静态库**链进自己的 `.so`，在那里满足这些依赖。
所以 `BUILD_FEXCORE_ONLY` 下跳过共享 target。

## 4. 两个未文档化的嵌入方义务

这两条是"崩了才发现"的，值得单独记录，因为 FEX 文档没有说明，
而任何嵌入实现都必然遇到。

### 4.1 `InitCore()` 之前必须设置 `SignalDelegator`

`Core.cpp:359` 无条件执行 `SignalDelegation->SetConfig(...)`。未设置则空指针崩溃：

```
signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x38
#00 FEXCore::SignalDelegator::SetConfig at SignalDelegator.h:63
    (inlined by) ContextImpl::InitCore() at Core.cpp:359
```

好消息是 `FEXCore::SignalDelegator` 是**具体类**，唯一的虚函数
`GetThunkCallbackRET()` 有默认实现。嵌入方可以先给一个空壳，把真实信号处理留到后面。

### 4.2 `CreateThread()` 之前必须设置 `SyscallHandler`

`LookupCache.cpp:49` 调用 `CTX->SyscallHandler->MarkOvercommitRange(...)`：

```
#00 FEXCore::LookupCache::LookupCache at LookupCache.cpp:49
#01 ContextImpl::InitializeCompiler at Core.cpp:401
```

`SyscallHandler` 有**三个纯虚函数**必须实现，即使嵌入方自己分发 syscall：

| 方法 | 说明 |
|---|---|
| `HandleSyscall` | guest syscall 入口 |
| `QueryGuestExecutableRange` | 查询地址所属可执行范围 |
| `LookupExecutableFileSection` | 查询地址对应的文件段，可返回 `nullopt` |

其余方法（`MarkOvercommitRange`、`InvalidateGuestCodeRange` 等）都有可用默认实现。

## 5. 验证证据与边界

[`fexcore_bionic_smoke.cpp`](../tests/fex_bionic/fexcore_bionic_smoke.cpp) 14 项检查，
在实机 **全部通过，退出码 0**：

```
device: Pico swan, Android 16 (API 36), arm64-v8a, 内核页 4096
[F01a] FEX_PAGE_SIZE 仍是 guest ABI 页                    PASS
[F01b] host 页大小已注入 FEXCore                          PASS  reported=4096 accepted=1
[F02a] FEXCore config layer 初始化                        PASS
[F02b] allocator hooks 接受 host 页大小                    PASS
[F03a] Context::CreateNewContext 返回 context             PASS
[F03b] signal delegator 与 syscall handler 已装配          PASS
[F03c] InitCore 成功                                      PASS
[F04a] CreateThread 返回线程状态                           PASS
[F04b] 活动 InterruptFaultPage 是 host page 对齐            PASS  addr%4096=0 size=16384
[F04c] DestroyThread 恢复权限并释放                        PASS
[F05a] context 销毁完成                                    PASS
[F05b] allocator hooks 与 config 关闭                      PASS
```

`F04b` 同时确认了 [host page 适配](fex-host-page-size-adaptation.md) 的改动在活动对象上生效：
fault page 尺寸 16384，对齐正确。

**这些证据不包含的内容**：

- **未执行任何 guest 代码**。通过的是链接、初始化与线程生命周期，
  **不证明**翻译、JIT 正确性或 guest ABI。
- 手头设备内核页 **4096**，所以这次运行证明的是"16 KiB 改动没有破坏 4 KiB 行为"。
  16 KiB 路径的正确性由构建主机（真实 16384 页）的
  [host_page_size_probe](../tests/host_page_size/host_page_size_probe.cpp) 验证。
- 这是**命令行可执行文件**，不是 APK 内的 `.so`，也未在 ART 进程中运行。
  ART 的 allocator/TLS/信号交互仍需单独验证。
- 静态链接了 libc++（`-static-libstdc++`）。设备上缺 `std::pmr` 符号，
  真实 APK 需要决定是打包 `libc++_shared.so` 还是继续静态链接。

## 6. 下一步

按依赖顺序，前两项已完成：

1. ~~修复两处 16 KiB 阻断~~ → [完成](fex-host-page-size-adaptation.md)
2. ~~验证 FEXCore 能为 bionic 构建并初始化~~ → 本文
3. **实现 backend adapter**：把 `src/core/guest_cpu/api/` 的公共接口接到 FEXCore。
   参考 `references/Bachata-S4-android/src/core/fex/fex_guest_engine.cpp`（1590 行，
   已有可用的 FEXCore 集成形态），但要按 V0 的 API 契约重写，
   并补上本文 §4 的两个义务。
4. 让 guest 真正执行一段 x86 代码——这才是 §5 边界之外的第一个实质里程碑。
5. 取得 16 KiB 页的 Android 16 实机，复跑本文与 host page 的两个探针。
6. 打成 APK 在 ART 进程内验证，处理 libc++ 打包与 allocator/TLS 交互。
