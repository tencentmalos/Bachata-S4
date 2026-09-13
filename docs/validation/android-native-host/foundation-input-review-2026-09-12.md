# SDL 回退报告与 Foundation 手柄接入复核

日期：2026-09-12。被审主仓 `261775607e3d9680046590e81966a6e7948a8e2a`，分支 `codex/android-fex-round2`。本次为源码、Git 和已有 ELF 的只读复核，并编写后续规划；没有实施输入迁移、重建 host 或重新运行真机测试。

**结论：先接通手柄，再清理 Android SDL，顺序成立且已由用户明确指定。通用输入设备、事件、状态及震动机制下沉 Foundation；Orbis pad、用户/端口策略、应用 JNI 和会话接线留在主仓。** 下一位实施者连续执行[输入优先实施补充](../../specs/android-foundation-input-first.md)，随后接回[生产 Runtime 整版](../../specs/android-native-host-production-runtime.md)，无需再次询问施工顺序。

## 报告中的事实与修正

| 报告说法 | 复核结果与影响 |
|---|---|
| 回退到 `26177560`，tree clean | 主仓受版本控制文件确实没有该轮 SDL 改动。但 `references/Bachata-S4` 仍有子仓修改，`externals/dear_imgui/` 仍未跟踪；“整个工作区干净”不准确，必须保留它们。没有留下 `NullAudioIn` 实现。 |
| host 库还含 SDL/JNI | 成立。对既有 ELF 使用 NDK `llvm-nm -D --defined-only`，按符号名精确前缀计数：`SDL_` 1877 个、`Java_org_libsdl_app_` 67 个，含 `JNI_OnLoad`。报告的 2312 可能使用不同符号表/匹配口径，不能直接比较。已有 link map 确认 SDL Android 对象提供 JNI 初始化。 |
| `OpenInGameSettingsDialog` 定义在 `settings_dialog_imgui.cpp` | 实际定义在 [`settings_dialog_layer.cpp`](../../../src/imgui/big_picture/settings_dialog_layer.cpp) 的 36–42 行；layer 再调用 settings window。调用点在 [`input_handler.cpp`](../../../src/input/input_handler.cpp) 的 806–808 行。删除整个 big_picture 分组会切掉仍需使用的游戏内设置层。 |
| 设置调用导致无法排除/拆分，所以 SDL 移除与 citron 迁移等价 | 推论过强。这是输入热键到 UI 的直接依赖，可改成宿主命令回调并保留 Vulkan ImGui 设置层，不是不可拆的循环依赖。先补真实输入可以避免功能空洞；但它完成后仍须清理音频、camera/mouse、消息框和 ImGui SDL backend，二者并非等价。 |
| 没有麦克风就用静音 `NullAudioIn`，保留 HLE 语义 | 尚无依据。现 [`audioin.cpp`](../../../src/core/libraries/audio/audioin.cpp) 有未打开/打开失败错误路径。未实现 Android 录音时须在正确阶段返回对应 Orbis 错误，不能未经定义就报告打开成功并伪造持续采样。 |

被检查 ELF 是原[host 库交付记录](host-library-milestone-2026-09-12.md)中的产物：Build ID `ec1808ab363fce5df51c481e90221369a5e82082`，SHA256 `d978d319a1faaa82797b848eb386081a266885cc7477514b6a316fb71c696217`。原始构建期 SCM 为 `7157f801` dirty，相关源码 hash 后匹配 `1149e948`；本次没有把它重新标成 `26177560` 构建。历史 61/0 装载/契约成绩仍是 AYN shell 辅助证据。

## 实际未接通的输入链

现 [`GamepadInputManager.kt`](../../../android/shadps4-app/core/runtime/src/main/kotlin/com/shadps4/android/runtime/input/GamepadInputManager.kt) 已包含 KeyEvent/MotionEvent、设备状态、profile、HAT 等处理，值得复用已有 UI 和测试；它是直接依赖 `ManagedSession` 的单例，提交最终仍走默认 slot 0。逐设备保存状态不等于多人输入已经隔离。

[`ManagedSession.kt`](../../../android/shadps4-app/core/runtime/src/main/kotlin/com/shadps4/android/runtime/session/ManagedSession.kt) 91–99 行提供 controller sink，但仓内未找到安装这些 sink 的调用；[`NativeFexSession.kt`](../../../android/shadps4-app/core/runtime/src/main/kotlin/com/shadps4/android/runtime/session/NativeFexSession.kt) 和 [`fex_session_jni.cpp`](../../../android/shadps4-app/core/runtime/src/main/cpp/fex_session_jni.cpp) 没有对应手柄提交入口。因此此次工作必须包含 Kotlin → JNI → native pad 的真实连接，单独替换 `controller.cpp` 不会使输入可用。

[`controller.h`](../../../src/input/controller.h) / [`controller.cpp`](../../../src/input/controller.cpp) 混合 SDL 设备、Orbis 按钮/轴格式、guest 时间戳、用户与状态历史。不能将它们整体移进共用库。应保留 [`pad.cpp`](../../../src/core/libraries/pad/pad.cpp) 的生产读状态、句柄和反馈语义，抽离设备后端，再将 Android 通用机制迁入 Foundation。

## Foundation 放置位置与复用边界

Foundation 当前 clean，主仓 gitlink、本地 HEAD 与 owned 远端分支均为 `1f7008848736b7c6c0779220480344fd5b5fc5e3`；开发分支 `codex/shadps4-android-fex-v0`，仓库 `tencentmalos/foundation`。不需要另建重复 fork。

| 现有位置 | 适配判断 |
|---|---|
| `basic/modules/imodules/.../IInputSystemModule.h`、`IDeviceJoystickLayer.hpp` | 有旧模块接口，但前者绑定 IModule/JobSystem，并覆盖键鼠/触摸等大范围；检索到接口和模块入口不等于已有可直接接入的 Android 手柄 backend。 |
| `modules/xr/include/spatial/xr/ControllerSnapshot.h` | 使用 OpenXR 类型、双手、pose/XrTime；不应成为普通 Android gamepad 的必选依赖。 |
| 主仓 [`SpatialFoundation.cmake`](../../../cmake/SpatialFoundation.cmake) | 当前仅独立接入 debugbus/dumpsys。新输入应仿照独立 target 接入，避免为了手柄解析完整 Foundation CMake 和引入 allocator/XR/network。 |
| 拟建 `modules/input` | 合适：纯 C++ 核心与可单独引用的 Android Kotlin library 同属输入模块，构建和依赖分离。**当前尚不存在，属于执行计划。** |

Android 官方支持从 View/Activity 传入 KeyEvent/MotionEvent；因此基于现有 Kotlin frontend 注入事件即可，不必引入另一套 Activity 或 native 主循环。[Android 控制器事件](https://developer.android.com/games/sdk/game-controller/controller-input)

设备持久 descriptor 可能被多个逻辑设备共享，`controllerNumber` 也会变化；它们不适合作唯一运行期键。用 Android deviceId 配合连接代次区分实例，持久配置另行匹配。[InputDevice 身份说明](https://developer.android.com/reference/android/view/InputDevice)

## citron 可借鉴内容与不能照搬的部分

固定参考 [citron `2106bcd8`](https://github.com/tencentmalos/citron_shadow/tree/2106bcd83844e05dfbb5251a902143233a7ec43b)；七个实际引用文件的 blob 对应关系已在[参考身份记录](2026-09-12-host-library/citron-input-reference.json)归档。本机后续 HEAD 不是外机检出的依据。

- [InputHandler.kt](https://github.com/tencentmalos/citron_shadow/blob/2106bcd83844e05dfbb5251a902143233a7ec43b/src/android/app/src/main/java/org/citron/citron_emu/utils/InputHandler.kt)：参考事件分发、设备能力和轴处理；其 PID/VID GUID、controllerNumber 去重、枚举端口需修正同型号多设备语义。
- [native_input.cpp](https://github.com/tencentmalos/citron_shadow/blob/2106bcd83844e05dfbb5251a902143233a7ec43b/src/android/app/src/main/jni/native_input.cpp)：实际依赖 `EmulationSession` / InputSubsystem，适合作为调用链参考，不是 Foundation 接口模板。
- [android.cpp](https://github.com/tencentmalos/citron_shadow/blob/2106bcd83844e05dfbb5251a902143233a7ec43b/src/input_common/drivers/android.cpp)：混合 InputEngine、设置、JNI、Java global ref、震动线程和队列。新设计应改成平台无关反馈命令，交由 Kotlin executor 消费，避免在 Foundation C++ 重建这套 JNI 所有权。
- [citronVibrator.kt](https://github.com/tencentmalos/citron_shadow/blob/2106bcd83844e05dfbb5251a902143233a7ec43b/src/android/app/src/main/java/org/citron/citron_emu/features/input/citronVibrator.kt)：零强度产生空 effect 并返回，不等于立即取消现有震动。新实现必须在 zero/Stop/disconnect 执行对应设备的 cancel；Android 已提供取消能力。[VibratorManager.cancel](https://developer.android.com/reference/android/os/VibratorManager)

逐文件保留实际 SPDX/来源，不把整套 Switch 设置、NFC、布局翻转或全局单例迁入 Foundation。低耦合是这次迁移的设计要求，不能把旧耦合挪一个目录后宣布完成共用。
