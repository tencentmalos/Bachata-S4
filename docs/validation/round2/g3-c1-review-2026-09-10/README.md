# N12 修补 / N3 C1 复核证据

被审 `f3b93d98`；[复核结论与下一步](../g3-c1-review-2026-09-10.md)。本目录的 probe 是审核诊断，不是正式修复。
正式 adapter、runner、测试、FEX/Foundation 均未改动，没有 commit/push。

- [manifest.json](manifest.json)：源码与固定 FEX 库 SHA、fresh embedder Build ID、remote、设备、tests ON/OFF 排除结果。
- [probe-runs.json](probe-runs.json)：独立原始/增强/非 spill 副本的进程退出码、部署 SHA 和耗时。
- [state-probe.cpp](state-probe.cpp)、[review.S](review.S)：真实 FEX unknown 后继 store 与 GPR/XMM 标记反例。
  [当前实现](device-state_probe.txt)10 GPR mismatch；[诊断副本](nonspill-state_probe.txt)0 mismatch。
  [nonspill-diagnostic.patch](nonspill-diagnostic.patch)只改 syscall stop 地址，未写回主仓 adapter。
- [timeout-probe.cpp](timeout-probe.cpp)与[日志](device-timeout_probe.txt)：实际 Run 超时后连续 WaitStopped Timeout。
- [gate-race-probe.cpp](gate-race-probe.cpp)与[日志](gate-race.txt)：直接使用当前 gate header 的公共 API，旧 Release 污染新 Arm。
  未修改私有状态；真实 deadline 有界，最多100次，本次第1次复现。
- [runner-probe.py](runner-probe.py)与[结果](runner/summary.json)：从上一复核原样重用的 A 反例，现已返回正确失败。
  B02/B05/B06 通过实际 main 注入 checker；corrupt-artifact 通过实际 OS/runner 子进程执行。
- [device-results.json](device-results.json)、[canonical-runner.txt](canonical-runner.txt)、`device-results-suites/`：
  当前正式实现的完整回归，guest104 IDs/202checks、device contract43/43；V0 10/0/47+3deferred，R2全部NOT_RUN。
- `host-*.txt`、`runner-unit.txt`、`runner-accounting.txt`：host contract42/43+1SKIP、ABI14、gate测试通过、runner21/21与21/21。
- [syscall-wrapper-disassembly.txt](syscall-wrapper-disassembly.txt)：当前实际 probe 的 wrapper/naked exit。C++ dispatch 已 inline，缺独立符号的 objdump warning 是该次观测，不是运行失败。
- `configure-*.txt`/`build-*.txt`：完整构建命令与退出码；`tests_off-hooks.txt` 匹配符号为0。

`state_probe-initial.txt`保留初次诊断：当时同时要求 optional `RunResult.guest_pc` 必须有值；复核发现 `fault.guest_rip` 已正确设置，
后续改为明确校验 fault 信息。初次和修正后的 probe 均发现10个 GPR 错误；主结论使用后者。
`probe-runs.json`中初次 probe SHA 对应当时探针版本，不能与后来的二进制混用。

## 复现

从仓库根运行。准备与 [构建记录](configure-device.txt)等价的 NDK 和固定 FEX 库；本机 NDK target35、真机 API33/4KiB。
下面变量由执行者设置为本机路径：`NDK_ROOT`、`REVIEW_BUILD`。fixture assembly 随本目录归档，生成器与正式 fixture 使用仓库版本。

```sh
cmake -S docs/validation/round2/g3-c1-review-2026-09-10 \
  -B "$REVIEW_BUILD/device" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFEX_BUILD_DIR="$PWD/build/fexcore-android" \
  -DV0_FIXTURE_DIR="$REVIEW_BUILD/fixtures"
cmake --build "$REVIEW_BUILD/device" --target state_probe timeout_probe -j 8
```

`state_probe`预期当前被审实现 exit2（10处寄存器错误），`timeout_probe`预期 exit2（receipt 未收尾反例），不是基础设施失败。
通过 adb 部署、chmod、读回 sha256 后，用 `adb -s SERIAL shell timeout 30 DEVICE_BINARY` 执行；原始日志保留实际命令和返回码。
重新配置到独立 `nonspill` 目录，并加 `-DREVIEW_NONSPILL_VARIANT=ON` 可编译一行修改的 adapter 副本；当前状态 probe 预期 exit0。
默认值为 OFF，不会修改主仓文件。不要把诊断副本的 PASS 当作生产修复。

原 `original_probe` 目标按原文件原样编译，仅用于此次对照，仍受原 source 绝对 include 限制；上述两个新 probe 不依赖那个 include。
构建最初从上一审核的同字节 `review.S` 生成；随后把同 SHA assembly 放入本目录方便复现。source fixture 字节与生成 header 均未改变。

Host gate 反例：

```sh
clang++ -std=c++20 -O2 -pthread -Isrc \
  docs/validation/round2/g3-c1-review-2026-09-10/gate-race-probe.cpp \
  -o /tmp/g3-c1-gate-race
/tmp/g3-c1-gate-race
```

退出2表示复现交错错误；退出0只表示100次内未复现，不是并发正确性证明。正式回归仍需确定的协议顺序测试。

全部二进制/静态库留在构建目录，本证据目录不包含游戏、APK 或 build 产物。Swan Android16 普通 APK 未在此验收。
