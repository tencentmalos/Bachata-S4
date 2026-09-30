# Summary 采样统一使用 JniHelper

按用户要求，替换[首轮实现](summary-native-metrics-20260929.md)里的独立 `JniContext`。
当前只有原有 `spatial::platform::JniHelper` 管理 JavaVM、application Context、ClassLoader 和线程环境。
主仓 HEAD `b0778a7f`、Foundation 基础版本 `b05f80fb`，本轮修改未提交。

## 实现与生命周期

- 宿主既有 `nativeInitializeHost` 调用 `JniHelper::initJniEnviroment`。同一 application 再次初始化直接成功，保持原 Context/ClassLoader global ref；其他 application 被拒绝，初始化中途失败不发布部分状态。返回值由原来的 void 改成 bool，旧调用者可继续忽略结果。
- 电池采样使用 `JniHelper::getJniEnv()` / `getActivityContext()`；后者保留旧名字，但返回 application Context，不持有 Activity。JNIEnv 始终按线程保存。
- 原有环境实现移入同一类的 `JniHelperEnvironment.cpp`，由 `spatial::foundation_jni_helper` 编译。完整 `foundation_platform` 与轻量采样目标链接这一份实现；原 `JniHelper.cpp` 保留调用、字符串、路径、DPI 和 asset 方法。路径/DPI 查询延迟到实际使用，初始化环境不再额外查询它们。
- native 工作线程首次使用时 attach，之后复用，线程退出时 detach；由 Java/调用方附着的线程不由 helper detach。每次电池读取仍使用局部引用帧，避免持续附着后积累 JNI local refs。
- 使用 C++ `thread_local` 析构释放环境：初版延用 pthread key 时，嵌入式 HotSpot 先清掉 JVM 自身 TLS，helper 随后的 detach 未真正移除 VM 线程，`DestroyJavaVM` 等待。该失败已复现、保留线程列表；改成 C++ TLS 后最终测试正常退出。此结论来自 host JVM，不冒充 Android ART 实测。
- 显式 `releaseThreadEnv()` 仅供提前结束 native JNI 工作，不在每次采样调用。嵌入式 VM 销毁前先停所有消费者，再 `shutdownJniEnvironment`；还有 helper 附着的线程时拒绝 shutdown。普通 Android app 按进程生命周期持有环境。

已删除首版的 `JniContext.h/.cpp`、`AndroidJni.cmake`、`setApplicationContext` 和 `ScopedJniContext`。GPU 主频读取及电池字段算法沿用首轮实现，本轮未修改。

## 验证

证据索引：[validation.json](evidence/summary-jnihelper-20260929/validation.json)。

| 检查 | 结果 |
|---|---|
| 真实 JVM + `-Xcheck:jni` | **253 checks / 0 failures**，2000 次读取；涵盖初始化失败不发布状态、ClassLoader 可用、200 次重复初始化不重建引用、线程环境复用/退出清理、Java 附着线程不被 detach、显式释放/重新附着、活跃线程期间禁止 shutdown、已有异常保留、原电池字段/异常测试；最终 DestroyJavaVM 成功 |
| 原平台辅助实现 | NDK API33 arm64 对 JniHelper.cpp、AndroidHelper.cpp、JniInvoker.cpp 语法检查通过；不代表完整 Foundation 平台运行验证 |
| Android host | `cmake --build build/android-host-api33/native --target shadps4_host -j6` 成功 |
| APK / Kotlin 调用者 | `:app:assemblePlaystoreDebug :core:runtime:compileDebugAndroidTestKotlin` 成功 |
| DSO 符号 | host 导出唯一的 `JniHelper::initJniEnviroment`，JNI DSO 只导入；两者无旧独立 Context 管理符号 |
| 指标计算/格式 | 沿用首轮 33 cases / 219 assertions（UBSan）的通过记录，本轮未修改这些算法，未重复运行 |
| 实机 | 未安装、未采样、未验收 Android ART 生命周期或 Summary 实际显示 |

JVM 测试使用 `foundation/modules/perf_metrics/tests/android_jni` 中的测试专用 Android API fixtures，未打包进 app。构建与测试原始记录位于 `build/validation/summary-jnihelper-20260929/`；首次等待的线程列表为 `first-test-threads.log`，该失败进程已终止，没有遗留测试进程。

## 产物

`build/validation/summary-jnihelper-20260929/shadps4-b0778a7f-jnihelper.apk`：

- APK SHA256 `2325cf2ba0a1ba5c50f7e1a2027afd33e4ec027587b1645ae683c58300addd88`
- host SHA256 `4d80178f05dba61a29290e5aebd0c9eac43a9f80b0068d58557e0a8712191c7b`
- JNI SHA256 `7b08604de1a91bb732545d36ffc23d7c8332c47bf3982b590161649a6523e572`

[包内身份](evidence/summary-jnihelper-20260929/apk-identity.json)已核对 host 与本机构建一致；mainline 驱动仍为发布的 86ca / `ea4853bf…`。
没有设备操作；前轮 GPU cache/property/trace 的 [CleanupPending](evidence/swan-cp-opcode-20260929/preempt/cleanup-pending.json) 未改变。
