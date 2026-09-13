# Spatial Foundation 接入记录与 V0 复用要求

日期：2026-09-07。这是初始基线 `a7128893` 之后的依赖准备里程碑；不改写初始基线记录。
主仓原先只有桌面核心，本次增加基础设施的构建入口，尚未提供 Android app 或 guest backend。

## 来源与已经落地的范围

与 azahar 使用同一个 [tencentmalos/foundation](https://github.com/tencentmalos/foundation)，
从 azahar 干净 checkout 的 `b753063273a73ff6664c567f62cfa5070f5d60a5` 建立
`codex/shadps4-android-fex-v0`。在自有分支为 DebugBus 添加两个默认 ON 的 CMake 开关，
让小型宿主可关闭 TCP/profiler adapters；公共 C++ API 和 azahar 默认构建行为不变。
本次固定提交为 [`1f7008848736b7c6c0779220480344fd5b5fc5e3`](https://github.com/tencentmalos/foundation/commit/1f7008848736b7c6c0779220480344fd5b5fc5e3)。

- `foundation/` 是正式 **build dependency submodule**，不放在只读研究用 `references/`。
- 主仓 `ENABLE_SPATIAL_FOUNDATION=ON` 默认启用，通过
  [cmake/SpatialFoundation.cmake](../cmake/SpatialFoundation.cmake) 提供 `shadps4::foundation`。
- 当前目标实际链接 `spatial::foundation_debugbus`，Android 时再链接
  `spatial::foundation_debugbus_dumpsys`；静态库按 PIC 构建。
- 最小 profile 只加入 `foundation/modules/debugbus`，关闭 TCP/profiler target。
  没有引入全局 allocator、JobSystem、第二套 ImGui、XR 或 libevent。
- 主程序已有 link 接入；没有注册运行时命令、启动网络端口或添加 Kotlin Service。
  这一步提供可用构建基础，不代表应用侧功能已经上线。

选择这个入口的依据是 foundation 自己的
[宿主集成指南](../foundation/docs/guides/integrating-emulator-host.md) 和
[API 稳定性说明](../foundation/docs/guides/api-stability.md)。仅将完整 foundation
`EXCLUDE_FROM_ALL` 仍会解析全部 CMake，无法避免缺失依赖和全局选项副作用。

## 必须优先复用的能力

**2026-09-13 输入已接通：** owned `codex/shadps4-android-fex-v0`已push `5388ef45313d6c32cb5f4bb5b07f1246ee381370`。[Runtime/input复核](validation/android-native-host/runtime-input-review-2026-09-13.md)记录Foundation portable49/0、Android5 tests及主仓真实scePad/普通APK证据。根host单独加入 `modules/input` / `spatial::foundation_input`，Gradle单独引用其Android Kotlin library；通用设备/值事件/状态/反馈留Foundation，PS4用户/端口/ABI/历史、应用JNI/Session归属留主仓。无Foundation JNI_OnLoad/Java-calling C++ worker或OpenXR耦合；原[输入设计](specs/android-foundation-input-first.md)保留完整合同，自定义profile/物理震感/guest-origin验收尚未完成。本节以下2026-09-07成绩保持历史，不覆盖当前input证据。

下表后两列是 **待执行要求**，不能把目标名称存在当作本仓已完成集成。

| 能力 | Foundation 实现 | V0 的使用边界和后续工作 |
|---|---|---|
| 命令注册/诊断入口 | `spatial::foundation_debugbus`、`_dumpsys` | 已编译接入；V0 app 注册 status/capabilities/stop，写操作投递给 guest owner |
| 反射 | `spatial::foundation_meta_reflection`，`Builder.hpp` / `Objects.hpp` | 新的诊断 payload 优先复用；完成 allocator/core 依赖闭包后链接 |
| 结构化序列化 | `spatial::foundation_meta_packing` | 复用数据编码，定义版本/schema/非法输入行为；不另写通用反射或序列化框架 |
| 网络 | `spatial::foundation_module_network`、`spatial::foundation_debugbus_tcp` | 若 V0 启用远程控制必须复用；先验证 NetSystemModule 启停、libevent 和线程归属 |
| 数学、后续 XR | `spatial::foundation_math`、`spatial::foundation_xr` | 基础数学按需复用，PS4/PSVR 语义和渲染策略留在宿主；XR 不扩入 V0 |

Guest CPU 公共 API 继续使用自己的稳定值类型/opaque handle；不向它暴露 foundation
module 单例、反射元对象、FEX 类型或序列化布局。JSON 只是证据输出格式，不能因此要求
在 JIT 热路径反射 CPUState。PS4 `sceNet` 等 guest API 的语义也不由 host DebugBus 网络代替。

## 启用反射与网络之前必须解决的闭包

1. 反射链接 `foundation_core_minimal` 与 `foundation_allocator`；core 又依赖 `fmt`、
   `cryptopp`，allocator 依赖 mimalloc/property。当前主仓并没有提供完整 Crypto++ 接入。
   不能添加同名空 target 让配置表面通过。
2. 完整 foundation 还假设宿主存在 `vulkan-headers`、zlib 链接名 `z`，JobSystem 从
   `${CMAKE_SOURCE_DIR}/externals/boost` 取头文件。本仓使用不同的 target/目录，需要适配真实依赖；
   跨编译不能拾取 macOS/Homebrew 库。
3. `third_party/CMakeLists.txt` 对 Android mimalloc 定义 `MI_TLS_SLOT=2`、
   `MI_TLS_SLOT_FALLBACK=1`。V0 必须核对 ART、bionic、FEX 的 TLS 所有权，不直接照搬这个配置；
   当前最小 profile 完全不编译 mimalloc。这不是已经确认存在冲突，而是尚未验证的假设。
4. libevent 使用 `azahar_config/posix` 固定配置，包含 epoll 等假设，不等于所有 POSIX 平台通用。
   NetSystemModule/module system 的闭包要在 Android NDK 下真实链接并运行。
5. `TcpServer` 依赖已启动的 NetSystemModule，必须检查 `IsListening()`；网络线程不执行 guest
   控制逻辑。需要限长、取消/超时、断线、端口冲突及退出后的线程/FD 清理证据。
6. `DebugCommandRegistry` 注册表无并发修改保护；启动服务前完成注册。
   DumpsysBridge 解锁后使用非 owning registry 指针，`SetDumpsysRegistry(nullptr)` 本身
   不等待已进入的请求。宿主需要停止接收、解绑、等待 in-flight 请求退出，再销毁 registry。
7. 独立的 LLDB 稳定快照仍按 V0 API 契约实现；不能在信号处理器内调用 reflection、packing、
   DebugBus、JNI 或网络。Foundation 不解决 JIT 中 CPUState 的精确性问题。

一般机制的修正写回自有 foundation 分支。宿主只保留轻量 adapter；如果能力被真实依赖问题
阻断，记录具体问题和后续步骤，不在主仓再造一套通用网络/反射框架。V0 不要求远程 TCP，
先用进程内命令与 dumpsys 验证即可；启用 TCP 时必须补验收。

## 本次验证

| 检查 | 结果 | 边界 |
|---|---|---|
| macOS ARM64 / AppleClang 17 / Release | shared library 编译、链接，CTest **1/1 PASS** | 实际调用 foundation registry；不包含 FEX |
| Android NDK / arm64-v8a / API 35 / libc++ shared | registry + dumpsys + smoke shared library 编译、链接通过 | **未运行**，仅交叉构建 |
| ELF `PT_LOAD` | smoke `.so` 与该 NDK 的 `libc++_shared.so` 均为 `0x4000` 对齐 | 不是 APK ZIP 对齐或 16 KiB 实机测试 |
| API 36 配置 | 本地标记 r28c、r29 的包均失败：实际 max API 为 35 | M0 必须核验并取得真实支持所选 API 的工具链 |
| 完整 shadPS4 / reflection / network / Android app | **NOT_RUN / 尚未接入** | 不借用 azahar 的验证作为本仓结果 |

本地 NDK 的 `source.properties` 自报 `28.2.13676358` / `29.0.14206865`，但两者
`meta/platforms.json` 和 sysroot 都只到 35。因此记录的是这些本地安装的实际状态，
**不推断官方 r29 的能力**，也不修改安装目录来绕过检查。
[官方 r28 对齐说明](https://github.com/android/ndk/wiki/Changelog-r28) 和
[16 KiB 指南](https://developer.android.com/guide/practices/page-sizes) 仍需与真实产物核对。
原始产物摘要在 [构建记录](data/foundation-bootstrap-20260907.json)。

复现宿主检查：

```sh
git submodule update --init foundation
cmake -S tests/foundation -B /tmp/shadps4-foundation-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/shadps4-foundation-host
ctest --test-dir /tmp/shadps4-foundation-host --output-on-failure
```

复现本次受限的 Android 交叉构建（`ANDROID_NDK_ROOT` 指向实际安装路径）：

```sh
cmake -S tests/foundation -B /tmp/shadps4-foundation-android-api35 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
  -DANDROID_STL=c++_shared -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON
cmake --build /tmp/shadps4-foundation-android-api35
```

API 35 仅用于本次库构建探针；V0 的 Android 16 目标与验收要求没有降低。
后续 APK 仍需验证完整 native 依赖闭包、ZIP 对齐、真实页大小以及加载和退出。
