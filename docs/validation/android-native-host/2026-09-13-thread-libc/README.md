# 本轮定向验证证据

结论和范围见[实施报告](../thread-libc-integration-2026-09-13.md)。这些证据保留测试时 `f4ba0623+dirty` 的源码身份，不用未来提交号替换。所有测试均为 AYN Thor/API33/ARM64/4KiB；没有完整回归、Swan、完整 assets 或游戏可玩验收。

## 最终证据

- `apk-thirteenth/`：三个 JUnit 选择器；thread fixture 正常/子线程故障两模式，libc provider 五模式；TMNT 同 PID 三轮均到 op527 / sceSysmoduleLoadModuleInternalWithArg，guest_presents=0。TMNT 的 JUnit PASS 是具名边界观察，不是游戏 PASS。
- `host-build/`：正式 host wrapper 的 result/source/ELF/link 日志。原运行目录为 `build/android-host-api33/runs/1789307990455500000`。
- `attr-gate-final/`：当前 thread attribute header 的 43/0；manifest 说明补录遗漏 header SHA 的原因，源码自构建起未改变。
- `rwlock-gate-final/`：当前 mutex/rwlock 的 60/0，含写回与 VM 发布排他检查。
- `drain-delivery/`：FEX adapter 最终 G49a–d/G48a–b 定向 6/0；后续只改 host runtime/fixture，没有再改 FEX adapter。
- `libc-audit.json`：静态 provider 核对，112 个策略/102 个现有游戏导出/当前 27 个 Internal 导入不再拒绝，不是全函数执行矩阵。
- `selected-content.json`：仅选取的 executable/modules/param.sfo 哈希，无游戏文件。

## 失败和中间结果

`apk-first`–`apk-twelfth`、`drain-before/after/page-fixed/final`、其他 unit/rwlock 目录保留阶段日志与当时源码哈希，不能当作最终源码的验证。`rwlock-desktop` 明确 INVALID_EVIDENCE：错误地在构建结束前运行了旧 41 项 binary；后续 `rwlock-desktop-final` 是新 59 项，最终加入 VM 门闩为 60 项。APK eleventh 的 SIGILL 是失败。APK twelfth 的 host-error-extract 只保留 app 自身 append-only 日志中一条 Busy 异常及原文件 SHA，不冒充按 PID 归档的三轮日志。

原 APK logcat 用秒级开始时间过滤，开头可能带上前一个 selector 的尾部；按 tag/PID/case 判断归属。没有收入其他应用的旧 crash buffer。

## 复现入口

正式构建使用 `scripts/android/build-host-android --ndk <NDK> --jobs 4`，随后 Gradle `:app:assemblePlaystoreDebug :app:assemblePlaystoreDebugAndroidTest`，传匹配 `fexBuildDir` 和 host `shadps4-host-loader.cmake`。定向 CMake target 为 `guest_thread_attribute_tests`、`guest_rwlock_tests`、`guest_execution_tests --focused-publication`。

三个 instrumentation selector：`ThreadAttributeRuntimeInstrumentedTest`、`LibcPolicyRuntimeInstrumentedTest`、`RenderedRuntimeInstrumentedTest#realContentUsesSessionRendererAcrossThreeRestarts`。真实内容以 `contentRelativePath` 传入测试，不提交游戏数据。libc policy 测试在存在用户系统 libc 时跳过以保护文件；没有实际执行不能记成通过。

本目录 `run-*.py` / `stage-content.py` 是当时命令的归档，含机器上的绝对 SDK/NDK/内容路径与设备序列号，运行前需按环境替换。unit runner 原始退出状态在 manifest 中，不能只看外层 Python 的退出码；先等待构建完成，再核对二进制/源文件 SHA 与检查数。
