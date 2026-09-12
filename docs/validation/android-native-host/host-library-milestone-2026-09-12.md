# Android host 共享库交付记录（2026-09-12）

源码提交：`1149e948080b475ee0c8a61dd891728f085cd610`，分支 `codex/android-fex-round2`。按用户最新要求，本轮完成“可生成 host .so”并验证实际装载，再交接[生产 Runtime 整块 spec](../../specs/android-native-host-production-runtime.md)。此前[九符号失败复核](pkg-v2-host-closure-review-2026-09-12.md)保留历史，不再作为当前链接状态。

## 结果与证据边界

**已生成 `libshadps4_host.so`，最终 `--no-undefined` 链接 exit 0；AYN Thor / API33 / ARM64 / 4KiB / shell UID2000 实际装载并执行 61 checks / 0 failures。** 库、smoke executable、同 NDK 的 `libc++_shared.so` 部署后逐个 SHA256 一致。缺少数据目录参数时到达 main 并正常返回 exit2，未再在 DSO 构造期崩溃。

| 检查 | 本轮结果 | 含义 |
|---|---|---|
| 正式 host target | 388 个 host TU；共享库链接通过 | COMMON/CORE/HLE、loader、video/shader、media、native window/control 的真实链接闭包 |
| profile | NDK29.0.14206865、native API33、arm64-v8a、RelWithDebInfo、c++_shared、Foundation ON | 已替换此前 static-STL/Foundation-OFF 诊断 profile |
| 工具 | 本机源码构建 font embed + protoc `36.0-dev`；与目标 protobuf runtime header 匹配 | 不依赖预置 `/tmp/host-*` 文件；工具参与生成依赖 |
| 设备契约 | 61/0，exit0 | 窗口/控制对象寿命、真实 LoadExec export、异常无效状态拒绝、目录初始化/失败重试、timeout取整 |
| macOS portable 契约 | 42/0，exit0 | 直接编译本轮 window/control 实现，无设备/GPU/guest 声称 |
| 桌面 adapter 编译保护 | `sdl_window.cpp`、`emulator.cpp`、`big_picture.cpp` NDK语法检查3/3 | 不等于完整desktop运行测试 |
| profile负例 | API36 exit2、jobs0 exit2、c++_static CMake exit1 | 实际NDK最大native API35；拒绝静默回退与不匹配STL |
| 尚未验收 | APK/ART、FEX生产入口、Turnip/Surface、游戏、Swan | 原APK仍CPU smoke；本轮无游戏数据操作 |

[原始证据目录](2026-09-12-host-library/README.md)包含源码清单、命令/日志/退出码、Build ID、动态依赖、符号/map摘录及失败→修复→通过链。构建发生于父提交`7157f801`的工作区；其源码hash与提交`1149e948`逐项一致。保留当时dirty/HEAD身份，不把SCM字符串改写成后来的commit。该库尚不含FEX/session生产接线；“host链接完成”不等于“完整模拟器已经可运行”。

## 修复内容

### 前端职责拆分

- `Frontend::Window`提供尺寸、WindowSystemInfo和键盘捕获接口；desktop `WindowSDL`与`AndroidWindow`各自适配。Android持有`ANativeWindow`引用与不可变generation。Presenter保留shared ownership；撤回发布窗口不会释放在用Surface。晚到的旧窗口撤回不能清掉新绑定。
- GNM没有绑定窗口时在初始化renderer前明确失败。mouse的SDL专用读取在没有SDL窗口时报告未初始化。ImGui原生窗口路径使用实际尺寸/时钟，保留Vulkan纹理渲染；完整Android输入事件尚待下一版。
- `ApplicationControl`把Android LoadExec导向真实会话控制接口；无会话返回ENOSYS，异常返回失败，跨回调持有控制对象。下一版仍需实现SessionCore后端、模块/路径验证和带generation的重载，不能将这条接口当作已能启动游戏。
- desktop launcher的三个TU从embedded closure排除；游戏内settings保留。仅desktop通过`LauncherServices`注入profile扫描和SDL纹理服务；不再借此拉入Emulator/SDL主循环。保留desktop编译路径，无WindowSDL布局伪装或同名空函数。

### 异常、timeout与库装载

- ARM64 `Ucontext`的guest寄存器确定初始化，并标明缺少有效guest快照；`SyncHostFromGuest()`返回false。ARM64自定义guest signal handler不再作为native函数调用。x86原有转换保留。真正FEX snapshot/InvokeGuest/handler改寄存器后恢复仍必须由下一版完成。
- Android epoll微秒转毫秒采用向上取整，保留0与负值，避免正的亚毫秒timeout变忙轮询；边界值含INT_MAX不溢出。本轮未新增设备EINTR或真实阻塞时长矩阵。
- 首个已链接库在AYN装载时复现`path_util.cpp`全局构造创建`/.local/share/shadPS4`失败，exit134；它发生在main/JNI前。已移除Android的desktop HOME/XDG推断与构造期I/O。`InitializeAndroidUserPaths(absolute_root)`显式创建并一次发布布局；I/O失败可重试，同root幂等，不同root或旧`SetUserPath`修改拒绝。未来APK用filesDir下的受控路径，在启动日志/配置/host worker前调用并捕获错误。没有给测试设置HOME或准备伪造desktop目录掩盖失败。

## 可重复构建与运行

```sh
scripts/android/build-host-android \
  --ndk "$ANDROID_NDK_HOME" --api 33 \
  --out build/android-host-api33 --jobs 8

python3 scripts/android/run-host-library-smoke.py \
  --build build/android-host-api33 --serial 9c2841a4 \
  --out build/host-library-validation/device-new
```

输出目录必须匹配同一profile；device-new必须尚不存在，失败/旧结果不覆盖。其他设备更换serial，普通APK验收不能用该脚本替代。第一次从新build目录准备host tools再构建NDK依赖；失败修复后允许增量继续，同一轮完整命令/源码身份保留在`runs/`。本次经历初始构建及增量修复，未额外重做第二套全新目录验收。

本机产物：`build/android-host-api33/native/libshadps4_host.so`；匹配`host_library_smoke`与`shadps4_host.map`同目录。最终库大小 **414493032 bytes**（含调试信息，非APK安装大小），Build ID **ec1808ab363fce5df51c481e90221369a5e82082**，SHA256 **d978d319a1faaa82797b848eb386081a266885cc7477514b6a316fb71c696217**。NDK的`libc++_shared.so`需匹配一起装载。构建输出未加入Git；后续Gradle复用`shadps4_host_objects`/`shadps4_host_dependencies`，只组装一份host/FEX/session。

## 下一位 AI 的实施边界

继续[生产 Runtime spec](../../specs/android-native-host-production-runtime.md)，集中完成最终JNI/SessionCore、生产loader/Orbis/VM/HLE/线程与callback、Turnip、Surface、输入音频和TMNT本体+更新APK。目标是可交互场景、10分钟、Stop和同一PID三轮重启，AYN/Swan分别记录。

**手柄留给下一位AI，按用户要求从citron的Android输入迁移，不适配现SDL手柄。** 本轮未更改controller/输入映射实现。spec已列出固定参考commit、Kotlin/JNI/native driver/震动路径，以及同型号双设备、热插拔、PS4按钮映射和session寿命要求。

当前host库仍静态拉入SDL（map证实`SDL_android.c.o:JNI_OnLoad`），不能直接当作最终APK JNI库加载。下一版从Android生产target移除SDL实现，保留desktop SDL adapter；不补SDLActivity/Java类。按实际路径替换或正确拒绝不支持的外围能力，不能只隐藏JNI符号。Foundation当前为既有DebugBus/dumpsys构建链接范围，尚不代表APK已注册诊断服务。Turnip未加载；不以系统驱动做替代验收。

健康依赖gitlink保持不变；`references/Bachata-S4`两处既有dirty文件和未跟踪`externals/dear_imgui/`均保留，未纳入本提交。FEX pin及子仓规则不变。
