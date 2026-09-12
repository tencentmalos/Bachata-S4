# N4 独立审核证据

基点 `e0693faa`；[报告](../g3-n4-review-2026-09-11.md)、[到APK执行spec](../../../specs/android-fex-round2-n4-to-apk.md)。
审核未修改生产代码/正式测试/runner/子仓，没有commit/push。probe只存在于本证据目录，未接入正式验收。

- [manifest.json](manifest.json)：源码、固定FEX库、fixture、Build IDs、远端ref、设备、tests=OFF符号及实际自测结果。
- [probe-runs.json](probe-runs.json)：每个设备probe的hash读回、exit、耗时。隔离throw进程exit134，其余失败probe以exit2报告观察到的反例。
- [hle_probe.cpp](hle_probe.cpp)：unknown标记状态、正常HLE RCX、native舍入模式、native throw四种模式。
  `hle_probe-{unknown,valid,fp,throw}.txt`保留实际输出。Run前验证GPR/XMM初始化，unknown作为旧缺陷的修复正例。
- [gate_timeout_probe.cpp](gate_timeout_probe.cpp)：pending Pause与无pending两种实际Run；后一种在6.5秒后由probe追加Cancel清理，
  不是测试通过。对应`gate_timeout_probe-{pending,no-pending}.txt`。
- [gate_early_probe.cpp](gate_early_probe.cpp)、[gate-early.txt](gate-early.txt)：直接调用正式共享header，无线程/无私有状态修改，复现错误终态翻转。
- [gate-old-race.txt](gate-old-race.txt)：上一C1 review的原公共API竞争probe本轮100次未复现；不能代替上述确定协议负例。
- [runner-unit.txt](runner-unit.txt)、[runner-accounting.txt](runner-accounting.txt)：22项中2项失败，与21/21通过；两份证据须一起看。
- [device-results.json](device-results.json)、[canonical-runner.txt](canonical-runner.txt)、`device-results-suites/`：当前正式代码完整回归，
  114个guest ID/212checks、device contract43/43。canonical不自动包含review probe和Python自测，所以exit0不与上述失败矛盾。
- `configure-*.txt`/`build-*.txt`记录fresh构建；`host-contract.txt`/`host-gate.txt`记录host检查；
  [nohooks-symbols.txt](nohooks-symbols.txt)证明生产wrapper保留、测试gate/trace排除。

本次设备AYN Thor/API33/4KiB，NDK实际target35；这是辅助运行，不是API33打包支持或Swan/API36普通APK验收。
所有ELF/静态库留在构建目录，证据目录无二进制、游戏或APK。

## 复现入口

从仓库根运行，执行者设置`NDK_ROOT`、`REVIEW_BUILD`到本机路径；固定FEX库先按仓库构建入口准备。

```sh
cmake -S docs/validation/round2/g3-n4-review-2026-09-11 \
  -B "$REVIEW_BUILD/device" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFEX_BUILD_DIR="$PWD/build/fexcore-android" \
  -DV0_FIXTURE_DIR="$REVIEW_BUILD/fixtures"
cmake --build "$REVIEW_BUILD/device" --target hle_probe gate_timeout_probe -j 8
python3 tests/guest_cpu/test_v0_runner.py
python3 scripts/android/test-runner-accounting
```

fixture由canonical CMake从仓内汇编自动生成；两个probe还引用已提交的[setup.inc](../g3-n3-probe-2026-09-10/setup.inc)，需保留相对目录。
设备probe本次部署在`/data/local/tmp/shadps4-g3-n4-review-20260911`；SHA、实际exit和耗时见`probe-runs.json`，原始stdout/stderr分别归档。
下列是等价复现命令，先将`REVIEW_SERIAL`设为目标设备，将`REVIEW_DEVICE_DIR`设为本次专用临时目录：

```sh
adb -s "$REVIEW_SERIAL" shell mkdir -p "$REVIEW_DEVICE_DIR"
adb -s "$REVIEW_SERIAL" push "$REVIEW_BUILD/device/hle_probe" "$REVIEW_DEVICE_DIR/hle_probe"
adb -s "$REVIEW_SERIAL" push "$REVIEW_BUILD/device/gate_timeout_probe" "$REVIEW_DEVICE_DIR/gate_timeout_probe"
adb -s "$REVIEW_SERIAL" shell chmod 755 "$REVIEW_DEVICE_DIR/hle_probe" "$REVIEW_DEVICE_DIR/gate_timeout_probe"
adb -s "$REVIEW_SERIAL" shell sha256sum "$REVIEW_DEVICE_DIR/hle_probe" "$REVIEW_DEVICE_DIR/gate_timeout_probe"
adb -s "$REVIEW_SERIAL" shell timeout 20 "$REVIEW_DEVICE_DIR/hle_probe" unknown
adb -s "$REVIEW_SERIAL" shell timeout 20 "$REVIEW_DEVICE_DIR/hle_probe" valid
adb -s "$REVIEW_SERIAL" shell timeout 20 "$REVIEW_DEVICE_DIR/hle_probe" fp
adb -s "$REVIEW_SERIAL" shell timeout 20 "$REVIEW_DEVICE_DIR/hle_probe" throw
adb -s "$REVIEW_SERIAL" shell timeout 20 "$REVIEW_DEVICE_DIR/gate_timeout_probe" pending
adb -s "$REVIEW_SERIAL" shell timeout 20 "$REVIEW_DEVICE_DIR/gate_timeout_probe" no-pending
```

逐条记录exit，不用`set -e`因预期反例失败而漏跑后续。throw只在自己的隔离CLI进程执行，当前基点exit134；外层`timeout 20`只负责失败清理，不作为内部停止成功证据。
Host early probe用`clang++ -std=c++20 -pthread -Isrc`编译本目录`gate_early_probe.cpp`；
旧race复用[此前的公共API probe](../g3-c1-review-2026-09-10/gate-race-probe.cpp)，同样编译后执行，原始结果为本次`gate-old-race.txt`。
最初gate probe有printf/string_view类型警告，执行前已修正并重编；原build输出保留，实际运行的修正后身份在manifest中。
