# Summary HUD：Foundation 原生电池与 GPU 频率采样

> 本文记录首轮下沉实现与 APK。用户随后要求统一使用原有 JniHelper，已删除下文的独立
> JniContext，改为共享 JniHelper 初始化、ClassLoader 与线程缓存，见[后续实现和最新包](summary-jnihelper-20260929.md)。

## 实现

主仓 `feature/malos/swan_performance`，HEAD `b0778a7f`；Foundation 基于 `b05f80fb` 的本地修改，尚未提交。电池采集已由 Foundation C++ 所有，shadPS4 删除 Kotlin `HostBattery`、service 的 batteryJob、`nativeSetHostBattery`、Core::Diagnostics 中转结构及渲染线程的复制链路。

| 部分 | 当前责任 |
|---|---|
| 既有 `NativePad.nativeInitializeHost` | 额外传一次 application Context；没有新增电池 native 方法或 Java/Kotlin 采集类 |
| Foundation `basic/platform/JniContext` | 获取真正的 application Context 并保留 global ref；采样时取 local ref；native 线程按作用域附着/释放，不 detach 已由 Java 附着的线程 |
| Foundation `perf_metrics/AndroidBattery` | C++ 经 JNI 调用公共 BatteryManager/Context/Intent API，读取、校验及换算原始数据 |
| `DeviceMetricsReader/Sampler` | 默认每秒在工作线程读平台电池与 sysfs；合并缺失字段，计算功率/续航，发布快照 |
| `StatusLayer` / `PerfHud` | 读取已发布快照，形成 Summary 行和历史图；不执行 battery IPC 或 sysfs IO |

现有 `basic/platform/android/JniHelper` 已有 JVM/Context 管理，但依赖 core/log/math 等完整基础层。新增 `spatial::foundation_android_jni` 小目标，仅包含 Context 所有权和 scoped env；旧 `initJniEnviroment` 也接入这份共享 Context。没有把整个基础层、分配器或模块运行时引入 Android host。动态符号核验确认 Context 只有 host DSO 一份，JNI DSO 引用它，没有各存一份导致 sampler 看不到初始化的情况。

这里“C++ 实现”指应用侧采集代码全部为 C++。电池仍通过 JNI 使用系统 Android Framework，不是绕过 Framework 或私有 Binder/HAL。所有使用的类和方法均为公开 API；没有依赖隐藏 ActivityThread，也没有新建 BroadcastReceiver。

## 电池数据与失效行为

- `BatteryManager.getLongProperty`：charge counter（µAh）、current now（µA）、capacity（%）。不支持时的 `Long.MIN_VALUE` 转为缺失；current=0 保持既有“无有效电流读数”的约定，不用于估算功率/续航。[Android API](https://developer.android.com/reference/android/os/BatteryManager#getLongProperty(int))。
- `registerReceiver(null, ACTION_BATTERY_CHANGED filter)` 只取 sticky Intent：level/scale、voltage（mV→µV）、temperature（0.1°C→°C）、status、plugged。null receiver 不注册回调。[Context API](https://developer.android.com/reference/android/content/Context#registerReceiver(android.content.BroadcastReceiver,%20android.content.IntentFilter))。
- 优先使用有效的 sticky level/scale；Intent 缺失或 level 无效时仍可用 manager capacity。manager 和 Intent 独立处理异常；单个字段异常不污染后续 JNI 调用。
- 平台缺项逐字段回退 `/sys/class/power_supply/battery/*`。自定义 `FileSource` 默认不夹带真实平台读数，测试或宿主可显式注入 `BatterySource`；原 `setBatteryOverride` 仍保留为覆盖入口，nullopt 恢复自动采样。
- 不跨线程保存 JNIEnv；只保存 application Context，避免持有 Activity。scope 中先复制 local ref，允许初始化端安全替换 global ref；只清除本次调用产生的异常，不吞调用方已有异常。[JNI 线程与引用规则](https://developer.android.com/ndk/guides/jni-tips)。
- unknown status 不再默认当作已确认放电；字段缺失或充电时清掉旧续航 EMA，避免长期展示旧值。明确 `NOT_CHARGING` 时不因 plugged 字段强行显示 CHG。拒绝 NaN/out-of-range 电量和 `INT64_MIN` 电流，避免 `llabs` 溢出。

## GPU 频率的 C++ 路径

采集直接由 C++ 读取下列节点，不调用 Java/Kotlin：

| 顺序 | 节点 | 输入单位 |
|---|---|---|
| 1 | `/sys/class/kgsl/kgsl-3d0/gpuclk` | Hz |
| 2 | `/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq` | Hz |
| 3 | `/sys/class/kgsl/kgsl-3d0/clock_mhz` | MHz |
| 4 | `/sys/kernel/gpu/gpu_clock` | MHz |
| 5 | `/sys/class/devfreq/<节点>/cur_freq`，节点名含 `gpu`、`kgsl` 或 `mali` | Hz |

此前 discover 只选一次首个可解析节点：首次返回 0 或后来失去读取权限，都会一直困在该节点。本次改为保留候选路径，每次采样依次检查，跳过缺失、不可读、0、负数和转换溢出，恢复后可重新使用高优先级节点。整数解析改为 `from_chars`，避免遇到过大 vendor 文本时有符号溢出。不会根据数值量级猜单位。

这种方法受 app UID/SELinux 权限限制，C++ 不会比 Kotlin 获得更多 sysfs 权限；root/adb 能读不等于 app 能读。全部不可用时频率字段为空；若 GPU usage 可用，Summary 仍只显示 usage。本轮没有真实 Swan 权限/数值验收。

## 验证

原始输出和 APK 在 `build/validation/summary-native-metrics-20260929/`；精简证据在 [evidence](evidence/summary-native-metrics-20260929/validation.json)。

| 检查 | 结果与覆盖 |
|---|---|
| Foundation `[perf]` | 33 cases / 219 assertions 通过；UBSan + no-recover。包括 GPU 0/丢失/恢复/溢出、平台字段合并、缺失后重新采样、unknown 与 sentinel、原 HUD 格式与设置 |
| 实际 host JVM + `-Xcheck:jni` | 239 checks 通过；2000 次重复读、200 次并发 Context 替换；验证 JNI 签名、局部引用、仅 detach 自己附着的线程、manager/Intent/字段异常、单位换算、原有 pending exception 保留、reset 后恢复 |
| Android arm64 host | API33 交叉构建与链接成功，包含原生 AndroidBattery/JniContext |
| Android APK | `assemblePlaystoreDebug` 成功；`:core:runtime:compileDebugAndroidTestKotlin` 通过，原调用者省略 Context 的兼容入口仍可编译 |
| 包内身份/符号 | APK host 与构建产物 SHA 一致；JNI DSO 只导入 `setApplicationContext`，host 导出单份；旧电池发布方法和类不再出现于 APK DEX/host 符号 |
| Swan 实机 | 未运行；`adb devices -l` 无设备。JVM 测试的 Java API fixtures 只在测试目录，未进入 APK，不能替代 Android 电池服务或厂商内核验证 |

测试可复跑：

```sh
cmake -S foundation/modules/imgui_overlay/tests -B build/foundation-overlay-metrics-20260929 -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/opt/homebrew \
  '-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize-recover=all' \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined
cmake --build build/foundation-overlay-metrics-20260929 -j6
build/foundation-overlay-metrics-20260929/foundation_overlay_tests '[perf]'

JAVA_HOME=/Library/Java/JavaVirtualMachines/jdk-17.jdk/Contents/Home \
  cmake -S foundation/modules/perf_metrics/tests/android_jni \
  -B build/foundation-battery-jni-20260929 -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/foundation-battery-jni-20260929 -j6
ctest --test-dir build/foundation-battery-jni-20260929 --output-on-failure
```

APK `shadps4-b0778a7f-native-metrics.apk` SHA256 `ff2cfbb2a00627d8328704adda94cf80ff29827e6dc5a45685d976bb5a01733b`；host `6b0f25db02c5a130bc577bb9c6f393778d9b7b576e477a8923f19b779c62e687`；JNI `13de40824b10efb9806bff2bcbaaee80f6f3c29d947dab7dba46fae2b60d4a00`。[身份记录](evidence/summary-native-metrics-20260929/apk-identity.json)。包内 mainline 驱动仍为正式 86ca / `ea4853bf…`，未将 GPU 实验驱动混入本次包；没有安装或设备操作。

待设备回来后，以 app 进程检查 Summary 的 BAT/PWR/LEFT/温度与充放电切换，以及实际可读的 GPU 频率节点；仍需先完成上一轮 GPU 现场的 cache/property/trace 恢复，见 [CleanupPending](evidence/swan-cp-opcode-20260929/preempt/cleanup-pending.json)。本次未改变该待恢复状态，也不作 GPU 崩溃修复声明。
