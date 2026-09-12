# 2026-09-12 完整 PKG 链路独立复核证据

关联：[复核报告](../full-pkg-review-2026-09-12.md) / [PKG v2 整版任务书](../../../specs/android-native-host-pkg-v2.md)。基点 `0f4fd74b1ef7b704fe6250805f7a6365c538e1a6`，origin `0e10defc`，领先43提交。源码、gitlinks、已有 so 和内容身份见 [manifest.json](manifest.json)。

本次没有修改生产代码/子仓、安装 APK、执行设备 session 或游戏，也没有解包 PKG。设备操作仅 adb 枚举（AYN Thor 在线）。所有新 native 运行探针均在构建机子进程，用 fake backend 验证未修改的生产 SessionCore。

## 实际检查

| 检查 | 结果 | 文件 |
|---|---|---|
| cmake build + host lifecycle suite | 767 checks / 0 failures | [host-session-tests.txt](host-session-tests.txt) |
| runner unit | 23/23 | [runner-unit.txt](runner-unit.txt) |
| Gradle JVM tests，强制 rerun | runtime92、session11、library6，109/0 | [gradle-unit.txt](gradle-unit.txt)、[jvm-results.json](jvm-results.json) |
| Session 边界 probes | early cancel 重叠；Prepare/Run 各自 abort；控制异常 lease 泄漏；迟到 drain 永久未退出 | [源码](session_edge_probe.cpp)、[结果](session-edge-results.json)、[构建](probe-build.txt) |
| Darwin NDK link | 真实 AArch64 Android API33 shared library，`--no-undefined` 成功；只证明本机有交叉链接能力 | [ndk-darwin-link-probe.txt](ndk-darwin-link-probe.txt) |
| 现存 JNI so 的 allocator | InitializeAllocator 实际调用 rpmalloc_initialize_config；SetupAllocatorHooks 非 no-op | [allocator-linked-implementation.txt](allocator-linked-implementation.txt) |
| 本地三份 PKG 的头/SFO/hash | 一个本体，两个字节相同的更新；未执行/解包 | [pkg-set.json](pkg-set.json)、[指定更新的元数据](pkg-metadata.json) |

## 复现命令

在主仓根目录运行：

```sh
cmake --build build/host-hn0 --target session_lifecycle_tests -j 4
build/host-hn0/session_lifecycle_tests
python3 tests/guest_cpu/test_v0_runner.py
```

在 `android/shadps4-app` 中，使用 JDK17：

```sh
JAVA_HOME=/Library/Java/JavaVirtualMachines/jdk-17.jdk/Contents/Home ./gradlew --offline --rerun-tasks :core:runtime:testDebugUnitTest :feature:session:testDebugUnitTest :feature:library:testDebugUnitTest
```

独立 probe 的构建源列表：

```sh
clang++ -std=c++20 -pthread -DSESSION_TEST_HOOKS=1 -Isrc docs/validation/android-native-host/2026-09-12-review/session_edge_probe.cpp src/core/host_runtime/session_core.cpp src/core/host_runtime/session_backend.cpp src/core/guest_cpu/api/status.cpp -o /tmp/shad-session-edge-probe
```

分别在具有 10 秒外部 timeout 的子进程中运行 `early_cancel`、`prepare_throw`、`run_throw`、`stop_throw`、`late_drain` 参数。probe exit1 表示该缺陷仍然出现；Prepare/Run throw 在本次为 SIGABRT（Python returncode -6）。修复后 probe 可能需要调整清理/预期检查以匹配正式新状态合同，不能直接改 exit 值掩盖失败。正式回归应并入 tests/host_runtime，当前文件只是本次可审阅证据。

NDK link probe 使用同三个生产 `.cpp`，不定义 test hooks，参数 `-std=c++20 -fPIC -shared -Wl,--no-undefined -Wl,--build-id=sha1 -Isrc`，编译器为 NDK Darwin 的 `aarch64-linux-android33-clang++`。使用 llvm-readelf 验证 AArch64 ELF 和依赖；临时 binary 已删除。它不是完整 host link，更不是设备执行证据。

## PKG 元数据方法和边界

按当前 `pkg_extractor.cpp::PKGHeader/PKGEntry` 的布局只读 header、table entry 0x1000 的原始 SFO，解析 CATEGORY/APP_VER/TITLE_ID/CONTENT_ID 等字段；全文件 SHA-256 流式计算。未读取游戏保存数据、未修改包，也未依据文件名推断引擎/可运行性。顶层1.08与嵌套1.08哈希相同；1.00本体和1.08更新有匹配的 content ID。下一版必须继续验证实际解包树、依赖和 UI 安装。

已有 JNI so 只建立其自身 hash/反汇编身份，没有在本次把当前 HEAD 干净构建为完整 APK；allocator 的发现不能替代运行/冷启动矩阵。历史 HN-U01 的 UI 观察保留在原文件，未被本次 backfill 为新的设备 PASS。
