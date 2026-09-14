# DiagnosticsHub reachable from device — service + JNI + dumpsys bind (Work Package A, step 8)

2026-09-14，主仓 `codex/android-fex-round2`,实施 [图形调试工具整包 spec](../../specs/android-graphics-debugging-toolkit.md) §3.1 的可达入口。此前 hub/registry/commands/producer 都在 C++ 层;本步把命令 registry 绑定到 Android dumpsys 桥并加 JNI 入口,使 `adb shell dumpsys` 与 app 都能真正读到 `debug_status`。这是整条链首次从设备可达。

## 交付

**`DiagnosticsService`**(src/core/diagnostics/diagnostics_service.{h,cpp}):进程内唯一的 `DebugCommandRegistry` facade。
- `EnsureDiagnosticsRegistered()` —— `std::call_once` 幂等:建 registry、`RegisterDiagnosticsCommands(registry, DiagnosticsHub::Instance(), clock)`、`SetDumpsysRegistry(&registry)`。registry 是 function-local static,寿命长于进程,故 dumpsys 桥持有的裸指针始终有效。
- `HandleDebugCommand(command)` —— 首次调用时建好 registry,再 `Handle`。空命令返回 help。

**JNI 入口**(fex_session_jni.cpp):`nativeDebugCommand(String): String` —— marshals 拷贝字符串(绝不传 native/guest 指针),调 `HandleDebugCommand`,在边界 catch 所有异常。

**Kotlin 声明**(NativeFexSession.kt):对应 `external fun nativeDebugCommand(command: String): String`,补全两侧绑定(否则 native 符号不可达)。

现在两条路径共用同一类型化后端:`adb shell dumpsys <service>` → Foundation `HandleDumpsysRequest` → 同 registry;app → `nativeDebugCommand` → 同 registry。状态命令读无锁 hub 快照,绝不等 session mutex / VM drain / GPU fence。

## 验证

新增 `tests/host_runtime/diagnostics_service_tests.cpp`(8 checks),注册进 CMake `HOST_BUILD_PROBES`,链接 `shadps4_host` + `spatial::foundation_debugbus`;`diagnostics_service.cpp` 加入 `if(BUILD_HOST_CORE)` 的 `target_sources`。

- 本机 `clang++ -std=c++23 -Wall -Wextra`,链接真实 `DebugCommandRegistry.cpp` + `DumpsysBridge.cpp`:**8 checks / 0 failures**,run exit 0。
- 覆盖:首命令建 registry;`EnsureDiagnosticsRegistered` 幂等两次不重复注册/崩溃;dumpsys 桥 `HandleDumpsysRequest("debug_status")` 路由到同后端(非 "no registry bound");注册 session 后经 service 与桥都能看到 `generation: 88`;空命令返回 help。
- **真实 NDK r29** `aarch64-linux-android33 -std=c++23 -Wall`:`diagnostics_service.cpp` 与**改动的 JNI `fex_session_jni.cpp`**(用 NDK sysroot 的 jni.h)均 exit 0。

## 边界

- 尚未在 app 启动时主动调 `EnsureDiagnosticsRegistered`——目前是首个命令懒注册。若希望 app 一启动 `adb dumpsys` 即可用,后续在 app init 显式调一次(§3.1 限调试构建/adb 可达)。
- Service.dump 的 Android 服务侧(哪个 Service 暴露 dump、`onCommand` 路由)未接;`HandleDumpsysRequest` 已就绪但需 app 侧 Service 调它。JNI `nativeDebugCommand` 已可直接用。
- producer 现状同前:guest_flip/host_present 已接,其余信号与 SessionCore phase/run_uuid 待接。
- 未做完整 host/APK 链接或真机运行(本机不可跑);真实 NDK 逐 TU 编译 + 本机单测。未改 FEX/Foundation/Citron。
