# Oboe 音频交付证据

对应[交付说明](../oboe-audio-integration-2026-09-15.md)。所有设备操作只针对 AYN Thor `9c2841a4` / API33 / 4KiB；未操作 Swan。

- `source-manifest.json`：主仓 dirty source、Foundation新增模块、Oboe固定提交与FEX未改状态。Foundation模块尚未提交，旧gitlink不能重建本轮代码。
- `oboe-artifacts.json`：host/JNI/APK SHA256及Build ID；打包host与实际构建host完全同SHA。安装后读取设备base.apk的SHA也等于 `e6f92f070434d97f2bf27082903eded8fb640dde78228355edf8550c145ad59c`。
- `host-build-result.json` / `canonical-host.txt`：canonical host构建与源码清单位置。manifest增加Foundation audio实际文件，避免只记录旧child gitlink。
- `final-native-tests.txt`：最终host AudioOut123/0、Foundation40/0。手动回调消费测试不是物理音频测试。
- `asan-final.txt` / `tsan-final.txt`：macOS现代LLVM的同一40项检查，回调分配计数0。ASan同时启用UBSan。
- `oboe-device-lifecycle.txt`：批次epoch修复前的静音设备观察，保留为历史；`oboe-device-final.txt`是在最终队列版本上重做的三轮设备生命周期检查。动态check总数取决于该轮实际接受的提交数，不是固定用例数。
- `apk-audio-test.txt` / `apk-audio-logcat.txt`：普通APK PID20098，真实FEX/13 imports，三轮各32块PCM。callback ON仍降级perf12→10；不是低延迟验收。
- `tmnt-audio-test.txt` / `tmnt-logcat.txt`：普通APK PID20416，120秒真实内容定向观察，3909 presents、Stop CANCELLED。`UNSUPPORTED_IMPORT`是静态导入盘点，不是本轮已经触发的fault。
- `tmnt-oboe.log` / `tmnt-audio-counters.json`：最新游戏endpoint的117条observer记录、原始host日志本地路径/哈希/行号。原始android-host.log带既有历史且很大，只在本地build保留；前三轮synthetic endpoint已从该摘录排除。源消费量是多源合计，源starved帧不是设备xrun。

复现核心命令：

```sh
scripts/android/build-host-android \
  --ndk /Users/bytedance/Library/Android/sdk/ndk/29.0.14206865 \
  --gpu-reshape-sdk-root /Users/bytedance/workspace/gpu_reshape/GPU-Reshape --jobs 6
cmake -S . -B build/android-host-api33/native -DFOUNDATION_AUDIO_BUILD_TESTS=ON
cmake --build build/android-host-api33/native \
  --target guest_audio_tests foundation_audio_tests foundation_audio_device_tests -j6
```

APK由 `android/shadps4-app/gradlew` 的 `:app:assemblePlaystoreDebug` 和
`:app:assemblePlaystoreDebugAndroidTest` 构建，`fexBuildDir` 指向已存在的
`build/fexcore-android-api33`；旧本地路径被STL校验拒绝的失败保留在
`build/audio-design-audit/apk-build.txt`，成功构建记录为`apk-build-api33.txt`。

```sh
adb -s 9c2841a4 shell am instrument -w -r \
  -e class com.shadps4.android.AudioRuntimeInstrumentedTest \
  com.shadps4.android.test/androidx.test.runner.AndroidJUnitRunner
adb -s 9c2841a4 shell am instrument -w -r \
  -e class 'com.shadps4.android.RenderedRuntimeInstrumentedTest#realContentRunsAndStopsAfterLongStartup' \
  -e contentRelativePath validation/tmnt-graphics-debug -e observationMs 120000 \
  com.shadps4.android.test/androidx.test.runner.AndroidJUnitRunner
```

设备静音生命周期程序通过shell执行；与普通APK分开描述。它验证受控流重建，
没有物理拔插、音频focus或听感验收。本轮不包含完整回归、FEX mutex快路径迁移、
游戏画面/可玩性或Swan验收。
