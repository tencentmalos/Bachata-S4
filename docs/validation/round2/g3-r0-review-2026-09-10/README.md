# G3 R0 独立复核证据

被审 `0484a0b9`；[复核结论](../g3-r0-review-2026-09-10.md)、[下一执行spec](../../../specs/android-fex-round2-g3-r0-exit-next.md)。本次未修改生产代码、正式测试或runner，无commit/push。

- [manifest.json](manifest.json)：实际source/fixture/FEX静态库SHA、Build ID、git refs、设备身份；FEX静态库与H2复核一致，embedder全新构建。生产文件与H2未变，故此前unknown/rejected/FP/exception问题没有修复；本次未重复运行那四个独立探针。
- [device-results.json](device-results.json)、[canonical-runner.txt](canonical-runner.txt)、[suite原始输出](device-results-suites/guest_execution_tests.txt)：本次完整guest 104IDs/202checks全过，约22.11秒；device contract43/43、host42/43+1SKIP、host ABI14、host page probe22、device bionic smoke12通过。V0 10/0/47+3延期、R2正式24NOT_RUN。runner进程exit0。
- `device-results-suites/*.txt` 中DEPLOY行分别记录device/local SHA相等；runner在执行前核验身份并清理其临时设备文件。manifest记录另行构建的device page probe身份，但本次canonical runner运行的是host page probe；设备实际页大小同时由bionic smoke确认。
- [runner-probe.py](runner-probe.py)、[runner/summary.json](runner/summary.json)及每场景JSON/stdout：mock外部执行/设备发现，使用实际parser/聚合/main。七个G30–G32 FAIL+exit0及G31重复冲突令R2失败却main返回0；未知Z99失败返回1；crash/timeout taint正确。合成输入不是guest运行证据。
- [runner-process/run.json](runner-process/run.json)及同目录fixture/stdout/JSON：额外启动真实runner子进程，以host合成fixture打印G31a FAIL；R2-H01为FAIL、runner进程exit0。隔离ADB，只证明实际进程退出码缺口，不证明guest suite来源。
- [历史日志索引](historical-log-index.json)、`historical-logs/*.txt.gz`：导入既有112份日志，包含修复前失败和v1–v60全部通过日志；每份gzip解压后与索引SHA相符。附原执行脚本文本。**没有历史binary身份绑定，不属于本次独立运行，不用现在的binary SHA补填历史。**
- configure/build/host-configure/host-build和`*-elf.txt`保存构建输出及设备二进制Build ID。无ELF、静态库或游戏文件纳入证据目录。

本机重编命令（路径是观察上下文，换机器需替换根目录和NDK路径）：

```sh
cmake -S cmake/fex -B /tmp/shadps4-g3-r0-review-20260910/device -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 -DCMAKE_BUILD_TYPE=Release \
  -DV0_ENABLE_FEX=ON -DFEX_BUILD_DIR="$PWD/build/fexcore-android" \
  -DV0_FIXTURE_DIR=/tmp/shadps4-g3-r0-review-20260910/fixtures
cmake --build /tmp/shadps4-g3-r0-review-20260910/device -j 6
cmake -S cmake/fex -B /tmp/shadps4-g3-r0-review-20260910/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/shadps4-g3-r0-review-20260910/host -j 6
python3 scripts/android/run-v0-tests \
  --build-dir /tmp/shadps4-g3-r0-review-20260910/host \
  --fex-build-dir /tmp/shadps4-g3-r0-review-20260910/device \
  --serial 9c2841a4 --expected-page-size 4096 \
  --run-id g3-r0-independent-20260910-01 \
  --out docs/validation/round2/g3-r0-review-2026-09-10/device-results.json
```

这份证据使用NDK `29.0.14206865`、实际target35，设备AYN Thor/API33/ARM64/4KiB。换机器运行使用新run-id和新输出目录，勿覆盖本历史证据。正式目标Swan/API36/4KiB普通APK仍未验收。
