# 2026-09-13 Runtime / Input 证据

[复核与修复报告](../runtime-input-review-2026-09-13.md)解释结论和证据边界。主仓源码身份为 `768636aa + dirty patch`；Foundation `5388ef4` 已提交/push。随后主仓提交交付本次源码，不倒改构建时 SCM。

- `artifact-manifest.json`：最终源码文件hash、parent/child、device、APK、源/打包host与JNI Build ID/DT_NEEDED、SDL/TLS/唯一pad符号检查。
- `build/`：末次正式host result/source manifest/dirty patch/ELF/build，以及Gradle原始日志。host source manifest不包含所有Android文件，后者由总manifest补充。
- `host-cli-final/`：当前host产物，真实dlopen与85项契约，部署hash。
- `load-audit-final/`：同host产物、API33 audit、11项合成ELF正负例和真实eboot审计。guest始终NOT_RUN。
- `input/`：Foundation portable49、Orbis adapter45、Foundation Android5。
- `apk/input-final.log`：普通APK input6；实际scePad/source、序号/epoch/Stop负例。
- `apk/service-final.log` / `service-final-logcat.log`：实际app Service，PID26026/UID10157/4KiB，三轮真实CPU Running/Cancelled及pad按下/松开。
- `host-cli/`、`load-audit/`、`apk/historical-*` 和 `*before-*` 保留早先产物、TLS/motor失败与较弱验收结果，不与final身份混用。

所有 `.log` 是有意提交的受限验证证据。这里没有游戏、APK、DSO、驱动或内存dump。游戏hash仅用于标识，不能据此还原内容。

## 重现

从仓库根目录，显式提供本机NDK。`NDK_DIR` 是示例变量，设置为实际安装路径；不依赖原机器的build cache。

```sh
scripts/android/build-host-android --ndk "$NDK_DIR" --api 33 --out build/android-host-api33
python3 scripts/android/run-host-library-smoke.py --build build/android-host-api33 --serial "$DEVICE_SERIAL" --out build/host-device-new
cmake -S cmake/fex -B build/eboot-audit-api33 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK_DIR/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DV0_ENABLE_FEX=OFF -DV0_BUILD_EBOOT_HARNESS=ON \
  -DV0_HOST_LOADER_CONFIG="$PWD/build/android-host-api33/native/shadps4-host-loader.cmake"
cmake --build build/eboot-audit-api33 --target eboot_prologue_harness
python3 scripts/android/check-eboot-load-audit.py --serial "$DEVICE_SERIAL" \
  --build build/eboot-audit-api33 --host-build build/android-host-api33 --out build/load-audit-new
```

可选 `--eboot-device-path` 指向已合法提供的设备文件；不开启guest执行。runner输出目录必须为新目录以免覆盖证据。APK沿用现有FEX prebuilt/profile设置；先完成上述host build，Gradle默认使用生成config，也支持 `-PhostLoaderConfig=<absolute config>`。

```sh
android/shadps4-app/gradlew -p android/shadps4-app \
  :core:runtime:assembleDebugAndroidTest :app:assembleFdroidDebug :app:assembleFdroidDebugAndroidTest
adb -s "$DEVICE_SERIAL" install -r android/shadps4-app/core/runtime/build/outputs/apk/androidTest/debug/runtime-debug-androidTest.apk
adb -s "$DEVICE_SERIAL" shell am instrument -w -r -e class \
  com.shadps4.android.runtime.input.NativePadInstrumentedTest \
  com.shadps4.android.runtime.test/androidx.test.runner.AndroidJUnitRunner
adb -s "$DEVICE_SERIAL" install -r android/shadps4-app/app/build/outputs/apk/fdroid/debug/app-fdroid-debug.apk
adb -s "$DEVICE_SERIAL" install -r android/shadps4-app/app/build/outputs/apk/androidTest/fdroid/debug/app-fdroid-debug-androidTest.apk
adb -s "$DEVICE_SERIAL" shell am instrument -w -r -e class \
  com.shadps4.android.HostInputServiceInstrumentedTest \
  com.shadps4.android.test/androidx.test.runner.AndroidJUnitRunner
```

`am instrument` 的 shell退出码不能单独证明通过；检查完整终态 `OK (6 tests)` / `OK (1 test)`、每项状态及无ignored/assumption（设备枚举case缺硬件会明确skip），并核对打包产物hash/Build ID。生产游戏启动尚不在这些测试内。
