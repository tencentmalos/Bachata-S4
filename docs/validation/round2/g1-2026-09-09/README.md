# G1 2026-09-09 证据

读 [修复报告](../g1-repair-2026-09-09.md) 与 [manifest](manifest.json)。源码身份是 `b3bfabce + implementation.patch + manifest 所列新增测试文件`；未包含无关 dirty 文档或子仓改动。当前文件本身也保留在主仓。

## 最终运行

| 日志 | binary / Build ID | exit / 结果 | 100 次 Pause p50 / p95 / max |
|---|---|---|---|
| [canonical 日志](g1-complete.txt) | c19d024ae4ba6c7ce63e9a8ff38b38b11d3b57d5 | 0 / 81 PASS | 68 / 120 / 490 µs |
| [runner canonical 日志](v0-results-complete-suites/guest_execution_tests.txt) | c19d024ae4ba6c7ce63e9a8ff38b38b11d3b57d5 | 0 / 81 PASS | 68 / 219 / 648 µs |
| [clean embedder 日志](g1-clean-complete.txt) | 49e2272fabb2cc5492d02a76286bbb9fb7512e2e | 0 / 81 PASS | 47 / 90 / 208 µs |

两份 binary 都在执行后核对 device sha256 与本地产物一致。canonical 与 clean 的构建路径不同，Build ID 不相同，不能互换符号。manifest 的绝对路径只记录本次位置。

[round2-results.json](round2-results.json) 保持全部正式项 NOT_RUN，其中 C01–C04 记录 CLI auxiliary PASS。[v0-results-complete.json](v0-results-complete.json) 是未经改写的 runner 输出，10 PASS / 0 FAIL / 47 NOT_RUN（57 in scope；另外3 deferred）。其 `native_build_ids` 尚为空，由本目录 manifest 补充明确产物身份；不可仅看该旧 schema 字段认定已追溯。

## 复现

先按 [构建入口说明](../../../../scripts/android/README.md) 准备固定 FEX `385a0cc4d…` 静态库。此次是干净 **embedder** 构建，未重新编译 FEX。

```sh
cmake -S cmake/fex -B build/g1-check -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
  -DV0_ENABLE_FEX=ON -DFEX_BUILD_DIR="$PWD/build/fexcore-android" \
  -DV0_FIXTURE_DIR="$PWD/build/g1-check/fixtures"
cmake --build build/g1-check
adb -s "$ANDROID_SERIAL" push build/g1-check/guest_execution_tests /data/local/tmp/g1-check
adb -s "$ANDROID_SERIAL" shell chmod 755 /data/local/tmp/g1-check
adb -s "$ANDROID_SERIAL" shell 'timeout 55 /data/local/tmp/g1-check'
adb -s "$ANDROID_SERIAL" shell sha256sum /data/local/tmp/g1-check
shasum -a 256 build/g1-check/guest_execution_tests
python3 tests/runner/test-g1-evidence.py
python3 scripts/android/test-runner-accounting
```

CMake 自动从 `.S` 生成 header，不依赖先前目录里已有的 fixture。传入 NDK 实际路径；本次 NDK 所选编译目标为 API 35，运行设备 API 33，不是 Swan。保存 host/device 日志、退出码、sha256、llvm-readelf Build ID，随后清理本次设备路径。

## 失败及历史结果

- `baseline.txt`：修复前原 suite 70/70，只是旧测试通过。`api_epoch.txt` / `api_cancel_parked.txt` 才暴露 epoch/Cancel 缺陷。探针自身为诊断工具，exit 0 不代表其观察到的契约通过。
- `api_probe.cpp` 是旧基点探针，include 路径为当时 checkout；在另一目录复现时需指向旧基点测试源，不能拿修改后的 harness 编译来声称复现旧实现。
- `zero_gap.txt`：旧实现仅一次取消 settle 的试跑通过，不证明无竞态，也没有复现 SIGILL。
- `g1-run-01.txt`：exit 139，新增 fixture 未解析的全局跳转；已由 local label 和 relocation 拒绝修复。
- `g1-run-03.txt`：exit 132，真实重建 context 触发重复 allocator 初始化断言；已改进程生命周期 setup-once。
- `g1-fp-sentinel.txt`：exit 1，强化负数输入后证明 MXCSR 写入未改变执行态 FPCR。此前正数向下/向零舍入结果相同，未能暴露错误；现已修复，覆盖四种模式及 host 异常标志恢复。
- `g1-run-02/04`、`g1-final-01/02/03`、`g1-final-fp`、`g1-clean`、`v0-results-current*`：中间实现的日志，不是最终源码全套验收。
- `progress-before.md`：修复前原文，含错误和彼此矛盾的结论，仅保留审计历史。

其余 suite 日志、runner 负例与空目录编译日志已保留。没有 APK、游戏、二进制或完整符号进入本目录。
