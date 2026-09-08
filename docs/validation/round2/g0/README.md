# G0 evidence（2026-09-08）

本轮 G0 修正验收口径，不改 guest CPU 行为。

- `runner-accounting.txt`：R2-B02 的 8 类 runner 负例，12/12。用 `scripts/android/test-runner-accounting` 复现。
- `artifact-verification.json`：`scripts/android/verify-native-artifacts` 对本次构建的三个 Android ELF 的检查。
  **`coverage: auxiliary`** —— 这是独立 ELF 证据，不是 B02 要求的整包覆盖。
- `host-contract.txt` / `host-hle.txt` / `host-pages.txt`：host 回归，33/34+1 SKIP、14/14、全通过，与一周目一致。
- `v0-results-host-only.json`：本次 runner 输出。**设备在本次运行期间从 USB 断开**，
  因此设备用例记为 NOT_RUN 且原因为 adb 不可用，不是失败。B02 由 11 PASS 时代的
  "完整通过" 变为 `NOT_RUN` + `auxiliary_status=PASS`。

复现：

```sh
cmake -S cmake/fex -B build/v0-host -DCMAKE_BUILD_TYPE=Release -DV0_ENABLE_FEX=OFF
cmake --build build/v0-host
scripts/android/build-fexcore-android
cmake -S cmake/fex -B build/v0-fex -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
  -DV0_ENABLE_FEX=ON -DFEX_BUILD_DIR=build/fexcore-android
cmake --build build/v0-fex
scripts/android/test-runner-accounting
scripts/android/run-v0-tests --build-dir build/v0-host --fex-build-dir build/v0-fex
```
