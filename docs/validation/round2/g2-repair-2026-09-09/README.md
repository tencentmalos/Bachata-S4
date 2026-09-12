# G2 第一轮修复验证证据

对应 [修复报告](../g2-repair-2026-09-09.md)。源码为 `5304e5a9` 加 [source-fix.patch](source-fix.patch)；[manifest](manifest.json)记录准确源码 hash、独立目录构建产物、Build ID、部署 SHA 和所复用 FEX 静态库。未修改 FEX/Foundation 子仓，没有 APK/ART/Swan 验收。

最终结果：

- [V0 runner](v0-results.json)及[摘要](v0-runner.txt)：10 PASS / 0 FAIL / 47 NOT_RUN，3 deferred。
- [device guest](v0-results-suites/guest_execution_tests.txt)：90/90，包含 G23 cache 与 G24 timeout/Cancel 恢复。
- [device contract](v0-results-suites/guest_cpu_contract_tests.txt)：40/40，包含 M24–M26。
- [host contract](v0-results-suites/api_contract_tests.txt)：39 PASS、1 SKIP；[typed ABI](v0-results-suites/hle_abi_tests.txt) 14/14；[page probe](v0-results-suites/host_page_size_probe.txt) 22/22。
- [bionic](v0-results-suites/fexcore_bionic_smoke.txt)：12/12。
- Python：[runner 7 tests](test_v0_runner.txt)、[accounting 9 checks](test-runner-accounting.txt)、[G1 evidence 2 checks](test-g1-evidence.txt)。
- [Android configure](configure-android.txt)、[Android build](build-android.txt)、[host configure](configure-host.txt)、[host build](build-host.txt)、[设备 SHA](deployed-sha256.txt)。

日志和 runner JSON 中的 `/tmp` 路径是采集时观察值，suite 日志已随本目录归档，不要求临时目录永久存在。runner 按实际 suite 退出状态汇总；最终部署校验 SHA 与其记录的相同构建产物 hash 一致。设备文件已清理。

`contract-initial.*`、`guest-initial.*` 和 `intermediate-v0/` 保留修复迭代中的成功运行，不能替代最终 manifest 身份。另一次 G1 evidence Python 检查曾因 T03 新纳入 host M26 而错误要求 guest ownership，报 `AssertionError: ('T03', {'M26'})`；修改为只检查 G 前缀后通过。该中间失败从本轮执行输出记录，最终日志没有删改为“首次全通过”。

构建方法（从仓库根目录，按本机 NDK/FEX 构建路径配置）：

```sh
cmake -S cmake/fex -B /tmp/g2-android -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
  -DV0_ENABLE_FEX=ON -DFEX_BUILD_DIR="$PWD/build/fexcore-android" \
  -DV0_FIXTURE_DIR=/tmp/g2-android/fixtures
cmake --build /tmp/g2-android -j 6
cmake -S cmake/fex -B /tmp/g2-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/g2-host -j 6
python3 scripts/android/run-v0-tests --build-dir /tmp/g2-host \
  --fex-build-dir /tmp/g2-android --serial DEVICE_SERIAL \
  --expected-page-size 4096 --out /tmp/g2-results.json
```

复现使用实际设备序列号。assertions-enabled FEX、完整系统调用故障注入、持久双 owner 100 epochs 和 APK 环境仍待下一阶段；不要把脚本能运行当成这些条件已经满足。
