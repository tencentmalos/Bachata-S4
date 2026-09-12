# Android 生产 Runtime：从已链接 host 库推进到真实 PKG APK

更新：2026-09-12。实施分支：`codex/android-fex-round2`。本轮先完成了用户指定的 host `.so` 里程碑，源码基点见[host 库交付记录](../validation/android-native-host/host-library-milestone-2026-09-12.md)；此前[九符号失败复核](../validation/android-native-host/pkg-v2-host-closure-review-2026-09-12.md)保留为历史。本文交给下一位 AI，连续接通生产 Runtime 到真实 PKG APK，保留[两工作包方案](android-native-host-full-link-plan-2026-09-12.md)及[PKG v2](android-native-host-pkg-v2.md)的正确性要求。**手柄迁移由下一位 AI 实施，明确复用 citron Android 输入，不适配当前 SDL 手柄。**

## 本轮交付目标

**连续完成一个生产 Android Runtime：普通 APK 从 UI 安装 TMNT 本体＋更新，经主仓 loader、FEX guest、Orbis HLE 和 Turnip，进入可操作游戏场景，运行十分钟，正常 Stop，并在同一进程完成三次启动/停止。**

工程内部依次处理构建、运行时、平台接线和实际游戏缺口；这些是同一任务的施工顺序，允许小提交和定期进度，不在“9符号清零”“首个HLE”“首次present”后交回一个新的规划。实际游戏在此组合上的兼容性尚待验证，不预先声明必然可玩；遇到可修复错误，定位后在本轮继续修复。

Swan / Android16 / ARM64 / 4KiB 是目标。AYN Thor/API33/4KiB 是当前可用辅助设备，先完成它的同一路径。Swan 缺席时可交付准确的 `AYN_VERIFIED / SWAN_NOT_RUN`，不能合并两台设备的验收。16KiB适配、系统Vulkan驱动、VR/Beat Saber、finite Step、广泛游戏兼容不进入本轮。

## 当前可复用基础与固定决策

已从正式根 CMake 生成 `libshadps4_host.so`：native API33 / arm64-v8a / `c++_shared` / Foundation ON / `--no-undefined`。可复用的 `shadps4_host_objects` 和 `shadps4_host_dependencies`、原生构建机工具、build wrapper 均已实现；不要再新建一套 host 源码闭包。

已完成 `Frontend::Window` / `AndroidWindow`、Presenter 持有窗口、带身份校验的窗口发布/撤回、`ApplicationControl` 重载请求接口、设置界面对桌面启动器的依赖注入。它们尚未绑定生产 SessionCore。ARM64 Ucontext 已确定初始化，缺 guest 快照和写回时显式失败，禁止原生执行 guest signal handler；这不等于 guest 异常支持。epoll 正微秒取整已修。Android 数据目录改为显式初始化，避免库装载前使用 HOME/XDG 目录而崩溃。保留这些修复，继续完整 guest/平台接线。

**当前 APK 仍为 CPU smoke；新 host 库尚不含 FEX/session/JNI，仍静态拉入旧 SDL 实现。host 链接/CLI 验证不能视为 APK/ART/Turnip/游戏验收。**

| 项目 | 本轮决定 |
|---|---|
| 最终库 | 沿用 `libshadps4_fex_session.so` 作为唯一生产session/host/FEX JNI库；`libbachata_pkg.so`保留导入职责。当前 `shadps4_host` 是已能构建和装载的独立 host 库，最终不并存两份host/FEX/global registry |
| profile | NDK29.0.14206865、arm64-v8a、native API33、`c++_shared`；target/compileSdk36独立记录。主仓host需要C++23，清理Gradle无条件`-std=c++20`对host的覆盖；依赖和FEX自身标准按target隔离 |
| FEX | 固定owned pin `385a0cc4`，通过既有独立构建和`cmake/fex`接入；核对实际prebuilt的API/STL/options，不无条件复用旧API35缓存；生产`V0_BUILD_TESTS=OFF` |
| 第三方 | 复用已完成的date/glslang/Zydis/miniz/LibreSSL配置、独立FFmpeg `e17ba6e2`、hwinfo `85bbcba3`、adrenotools `60ae5bbc`＋linkernsbypass `aa397589`；不再次逐库调研或迁入Foundation |
| Foundation | 保持已开启的DebugBus/Android dumpsys链接范围，并完成进程内注册和退出；复用其诊断设施。反射/网络按真实需要审计后启用，不扩大为本轮另一个框架项目 |
| 平台 | Kotlin Service/SessionCore掌管会话，ANativeWindow原生呈现，Android controller输入，AAudio普通输出、OpenAL保留Audio3D用途；Turnip为唯一默认Vulkan路径 |
| 内容 | TMNT CUSA50828，gd/01.00本体＋gp/01.08更新。身份和本机路径见[pkg-set.json](../validation/android-native-host/2026-09-12-review/pkg-set.json)；外机通过显式本地路径提供同hash内容 |

保留现有SessionCore的generation、RAII control lease、late-drain所有权、统一Destroy和Service长期观察修复。保持G2事务/poison/sink/pin/译码退休合同。历史COMMON、FFmpeg、hwinfo已修项不再作为待实施工作。

## 实施主体：生产库、session与guest边界一起接通

### 统一构建和真正的 Android frontend

复用已实现的 `shadps4_host_objects` / `shadps4_host_dependencies`，接到最终JNI库。根CORE/COMMON/VIDEO_CORE/SHADER_RECOMPILER/IMGUI等现有源列表继续作为唯一来源，必要时机械抽取为共用cmake模块，并同步census读取方式。不要在app复制一套源文件或手列archive。

选择一个canonical生产构建入口供Gradle和命令行共用。推荐在根`BUILD_HOST_CORE`模式建立完整JNI target，并让Gradle调用同一入口；若保留app作为CMake入口，则先把根配置抽成可被子目录复用的模块，修正`CMAKE_SOURCE_DIR`/生成资源路径，不能把当前根项目直接嵌入后靠绝对路径补丁维持。单独的FEX配置仍隔离，最后只链接一次。

扩展现有 `scripts/android/build-host-android`，让它与 Gradle 共用生产入口。已支持原生 protoc/font 工具、profile校验、产物/源码hash和真实失败出口；保留 `scripts/android/run-host-library-smoke.py` 的辅助验证，但最终以普通 APK 为准：

- 从显式NDK/profile、固定gitlinks和忽略的输出目录构建；支持从新目录一次得到JNI库及可供Gradle打包的产物，命令失败保留真实非零退出。
- 从同一gitlink的ImGui/protobuf准备**构建机**font embed和protoc，检查可执行身份/版本，与目标libprotobuf匹配。工具路径和内容身份参与generated outputs依赖，替换工具会重新生成；不要求预先存在任何`/tmp/host-*`文件。
- 强制正式profile一致，核对C++异常/RTTI、PIC和STL；隔离Homebrew/desktop pkg-config。检查FEX与主仓fmt/xxhash等实际符号提供者，正常处理冲突和ODR，不打开全局多重定义容忍。
- 最终JNI库显式`--no-undefined`、Build ID、link map；完整DT_NEEDED树可以在APK namespace解析。UND中正常Android动态库导入不算缺失。link map证明实际生产loader/HLE/renderer/media和FEX都在可达链路内。
- 保留正式host、guest和JNI符号，不把构建输出、NDK、游戏、驱动包写入Git；生成可检出源码/pins与产物hash manifest。

**前端符号已通过接口拆分解决，生产启动职责继续迁移。** 从desktop `emulator.cpp`迁移配置、挂载、metadata、HLE注册、模块加载/执行等实质逻辑到`src/core/host_runtime/`的生产backend；systemservice退出/重启/重载转换为带generation的session请求。旧desktop入口仍使用其adapter。生产路径不得借用`quick_exit`、fork、进程级静态`exit_done`或另一个SDL主循环。

复用 `src/frontend/window.*`、`android_window.*`、Presenter 的 shared ownership 和 `src/core/host_runtime/application_control.*`，由当前 generation 的 session 实际绑定。拒绝未绑定窗口启动 GNM 的行为已存在。Surface 替换仍须完整退役旧 renderer/driver 使用者，不能因为引用计数已安全就省略 stop/drain。ApplicationControl 后端必须验证有效安装范围和可重载模块，再接受请求，不在 HLE 回调里直接销毁当前 session。

`Common::FS::InitializeAndroidUserPaths(filesDir/shadPS4)` 必须在日志/配置/HLE/线程启动前显式调用。成功后布局在进程内不变，同路径重复调用幂等；失败可重试，JNI/Prepare 捕获异常并返回结构化错误。不要通过更改 HOME、cwd 或 SDLActivity 绕过它；现 `SetUserPath` 在 Android 明确拒绝修改。按 Android storage 边界提供 cache/content/save 的子路径策略，不能复用桌面全局构造 I/O。

### Android 生产 target 移除 SDL

**可行性与当前状态：Android原生接口已有替代方向，可以在本次整版实施中完成移除；当前库尚不能直接删掉SDL链接项。** 本项由下一位AI与citron手柄迁移、生产Runtime一起完成，不另拆交接。SDL没有承担FEX执行或PS4 guest ABI语义，需替换的是host平台服务及历史共用代码的耦合。

不迁移 SDLActivity、不为 `org/libsdl/app/*` 补类、不接 SDL 窗口/手柄/音频事件循环。Android最终target不链接SDL3 archive/shared library；desktop继续保留SDL adapter及原子仓。当前host库依然拉入 `SDL_android.c.o:JNI_OnLoad`，必须通过真实职责拆分消除，不能只隐藏/改名JNI符号。

范围包括 controller/input_handler/input_mouse、ImGui SDL平台backend、trophy音效、SDL音频输入/输出、mouse/camera，以及user_manager/emulator_settings/devtools/ipc的SDL消息框/事件。普通音频输出使用AAudio，Audio3D保留有效OpenAL链。Android UI或session control承接平台交互；暂未提供的camera/microphone/mouse能力返回正确的未支持/未初始化状态，保留HLE注册及错误语义，不假成功。仅删除桌面启动器不能称为去SDL。用最终link map确认没有SDL实现对象和SDL JNI初始化，并确认真实游戏必需HLE未被删掉。

| 当前SDL职责 | Android替换与保留边界 |
|---|---|
| 窗口、尺寸、呈现 | 已有AndroidWindow/ANativeWindow与Vulkan Surface接线；继续补齐generation/Stop/重建，保留ImGui的Vulkan renderer |
| 手柄、按键、轴、震动 | 按下文固定版本迁移citron Android输入，接Orbis pad；清除Android路径中的SDL_Gamepad、joystick id和SDL事件队列依赖 |
| 音频输出、trophy提示音 | AAudio承接实际PCM输出及音量/停止；提示音使用同一原生输出服务，避免为其保留整个SDL；Audio3D继续OpenAL |
| UI、设置、键盘捕获、退出事件 | Kotlin UI/原生ImGui输入adapter与ApplicationControl/SessionCore承接；desktop launcher和SDL renderer不编入Android |
| 摄像头、录音、鼠标外围能力 | 实现实际需要的Android backend；本轮未支持的功能明确返回对应Orbis错误，不删除RegisterLib或伪造数据/成功 |
| SDL计时、内存、字符串等工具调用 | 换为已有Common/Foundation或C++标准库；不新增一套同名SDL兼容stub来维持链接 |

构建调整同时覆盖根`CMakeLists.txt`的源列表/`SDL3::SDL3`链接、`externals/CMakeLists.txt`的SDL子目录和系统包查找、共用header及下游target传递依赖。Android配置不再查找、下载或编译SDL；desktop条件下继续使用现有SDL依赖。把SDL专有类型从Android需要编译的接口中隔离，不能只移除几个`.cpp`而依赖其header或旧cache才能编译。

**完成条件必须同时满足：**

- 从新Android输出目录完成host及最终JNI库的`--no-undefined`链接；配置/compile commands/依赖图没有SDL包、源码或include依赖。
- link command/map中没有SDL archive或实现对象，未剥离库的符号检查无SDL函数定义/未解析导入；`DT_NEEDED`及APK/splits中没有SDL动态库。仅检查`DT_NEEDED`不足以排除目前这种静态SDL。
- APK普通UID真实加载，只执行本项目自己的JNI初始化；不要求`org/libsdl/app/*`类或SDL Java setup。现有61项host库契约及被修改的desktop adapter编译检查保持通过。
- 实际游戏呈现、citron手柄/震动、音频、UI控制、焦点/Surface变化、Stop和同进程重启通过下文整版验收；未支持外围能力的错误返回有负例。不能用“去SDL后能链接”替代这些行为验证。

### 一个 session 拥有完整执行资源

沿用`SessionCore → ISessionBackend`接口建立生产backend，smoke backend保留为显式测试模式。扩展目前只有content_id/iterations的`SessionParams`和JNI/Service协议，传递不可变的launch描述，至少涵盖：

| 输入/资源 | 合同 |
|---|---|
| 已安装内容 | 受控安装根/effective entry、title/content/version、base/update/effective模块身份；准备阶段锁定安装版本 |
| guest环境 | argv/env、模块/保存/配置路径、用户设置快照；guest可见字符串/指针放入受控VM，不能传host栈对象或TCB |
| graphics/input/audio | 选定Turnip包身份、Surface及独立surface generation、controller快照、音频设置；运行时更新走版本化通道 |
| 结果 | 保留现有结构化phase/terminal/stop回执，增加真实stage/module/RIP/NID等诊断；用户界面不暴露内部ABI细节 |

生产SessionRuntime统一持有VM、CpuContext、长期owner线程、linker/modules、HLE registry、renderer/driver/window、音频和工作队列。跨线程控制继续获取control lease，JNI线程不直接访问裸context。用户Stop、guest exit、guest fault、Prepare/Run异常、Surface退出最后都汇合到同一退役流程。

Start/Finalize/WaitTerminal绑定具体generation；旧waiter或surface事件不能清理新会话。退出后才清除旧module函数地址、TLS、注册项和会话状态；进程级第三方设施另有明确寿命，不能每场重复初始化/销毁破坏下一场。保存回收前后的thread/fd/VA/GPU/audio资源数据，允许有说明且有界的进程缓存。

### guest执行、HLE、VM、回调和异常作为一个迁移整体

按主仓生产路径改造下表，不仅替换`RunMainEntry`。ARM64参考分支可用于接口结构比较，实际执行走当前固定FEX和公共`guest_cpu` adapter。

| 现有路径 | 必须落地的实现 |
|---|---|
| `linker.cpp` / `module.cpp` | 真实SELF/ELF、procparam、依赖、relocation、BSS/RELRO、libc和module init，主入口通过FEX；module init/fini、malloc初始化和heap callback通过Run/InvokeGuest |
| `libraries/libs.h` / resolver | 区分guest export、host HLE和guest ABI对象。`LIB_FUNCTION`生成guest veneer并注册typed descriptor；NID/library/module/version正确匹配，不把ARM64地址放进guest导入表 |
| `guest_cpu/hle` / `fex_context.cpp` | 实际typed gate、输入/输出长度和方向pin、guest/host ABI、结果mask、errno/TLS、FP/fenv和异常边界；非法gate和两owner并发错误正确归属 |
| `pthread.cpp` / TLS / callbacks | guest pthread start/once/destructors、atexit、signal、fiber和实际媒体callback经正式guest入口；长期owner与独立guest栈/FS/GS/TLS，不跨JIT帧随意longjmp或native调用guest地址 |
| HleScope / InvokeGuest / WaitingHle | 完成原H3的两层嵌套、返回现场/栈恢复、可取消等待、多owner隔离；真实callback不能另留下一版 |
| `address_space.cpp` / `memory.cpp` | Orbis policy与GuestAddressSpace共同管理reservation/backing/alias/pin/权限/失效和GPU观察者；映射HLE通过安全协调边界执行 |
| `signals.cpp` / `exception.cpp` | 明确host fault、JIT fault、guest exception和异步控制的分类及状态来源，保持已修的确定初始化与无效状态拒绝，完成真实guest快照/写回、handler调用与恢复 |

VM先列出真实模块、direct/flexible memory、栈、TLS、veneer/gate和GPU资源的布局需求。当前64GiB是V0后端**策略**上限，不是已证实FEX能力硬限制。选择能满足实际内容且不冲突的布局；如需提高策略上限，做同一fixture的低/高VA及lookup/tag/decoder失效比较，并校正capability/allocator/查询边界。不能只改一个常量，不能整体平移含固定地址的游戏，也不能用maps空洞扫描后MAP_FIXED覆盖未拥有内存。

运行中guest mmap/protect/remap/unmap要在可暂停的HLE边界协调其他owner，避免drain自己仍持有的Run lease。继续保留发布前旧翻译退休、失败poison、重试epoch、pin/sink排他和真实alias。GPU映射观察/写入不越过相同backing所有权。

**异常边界必须具有显式有效性。** ARM64 host ucontext不直接视为Orbis mcontext。guest exception frame按固定FEX版本的可信spill/安全点或已验证的JIT状态恢复元数据构造，带context/thread/generation/invocation身份；仅把guest ABI字段序列化给guest，不暴露内部host_context等指针。host siginfo逐字段转换，地址经归属检查。handler修改的RIP/RSP/flags等经校验后写回guest执行状态；未支持的上下文拒绝分发/恢复并正确归因，不能清零后报成功。

真实异步host signal处理保持异步信号安全；不得在任意host信号栈调用native C++ HLE或嵌套FEX。明确FEX、GPU保护页、ART/原有handler的处理顺序与链式转交，未知host fault不当成guest正常返回；不用恢复host SIGILL、SleepThread parking或settle sleep修补控制流。

将本轮epoll正微秒值向下截断问题在同一bionic迁移中修正，保留poll/infinite/EINTR语义。把[Ucontext反例](../validation/android-native-host/2026-09-12-host-closure-review/ucontext_probe.cpp)转成正式有效性/恢复测试；反例exit0表示旧缺陷存在，不能直接抄成通过断言。

实现范围以真实TMNT静态imports＋运行时访问为依据，优先完成启动/互动需要的kernel、libc/sysmodule、filesystem、userservice/systemservice、pad、GNM/VideoOut、audio、保存及实际媒体功能。未使用能力可以明确Unsupported；raw pointer、varargs、aggregate/sret或callback签名不能由模板猜成支持。没有要求无差别迁移所有数千条注册，但实际必需导入不能靠统一返回成功跳过。

## 平台与 APK 在同一生产库上闭环

将以下接线纳入上述Runtime所有权，使用同一APK验证，避免另建只画triangle的最终backend。

**Turnip。** 使用成套adrenotools和四个hook库，明确APK打包/提取策略、nativeLibraryDir与app私有driver目录。选择受控的bionic ARM64驱动包并固定来源/SHA/ELF身份；校验实际DT_NEEDED，而非按zip名称判断。native loader handle、namespace、`vkGetInstanceProcAddr`、Vulkan-Hpp dispatcher、instance/device同源；关闭默认DynamicLoader误取系统驱动的路径。缺包、不支持扩展/格式/`shaderInt64`明确失败，不自动回退系统。驱动及namespace至少活到最后一个使用它的GPU对象/worker退休，记录实际加载映射和driverName/driverInfo。

**Surface与GPU。** JNI取得ANativeWindow引用，尺寸/attach/detach独立generation；渲染线程验证并使用当前引用。Stop/detach在join前通知Presenter和相关scheduler。保留有限acquire及success/suboptimal被Stop打断仍退休semaphore/image的修复；处理fence/queue/present和device-lost，不能用无界waitIdle卡死销毁，也不能超时后释放仍被GPU使用的对象。需要保留资源到迟到完成时，状态明确且拒绝不安全重启。

**输入由 citron 迁移。** 使用下节固定来源，把 Android KeyEvent/MotionEvent、InputDevice 注册和震动派发接到本仓 pad HLE 的平台无关状态，不复用当前 SDLGamepad/SDL_PushEvent 链。既有虚拟按键/overlay与实体手柄汇合到同一输入状态，用户/端口归属、时间戳和 session generation 明确；焦点丢失、Stop、断连清除按键/轴和震动，旧设备回调不污染下一局。以真实游戏操作及 guest pad read 观察值验收。

**音频。** AAudio补短写、gain、格式/通道、open失败不发布假成功、断连/停止；Audio3D仍走有效OpenAL输出链，媒体保留FFmpeg真实解码/seek/EOF/退出。以真实guest PCM和实际听感验收，静音/音量/重启有效。

**bionic与ART。** 最终实际链接rpmalloc/provider为准核对SetupHooks和VA所有权，在普通UID、ART/图形并存时验证冷启动、首次失败后重试、同进程重建和Java churn。preflight的可重试性不代表SetupHooks fatal可恢复；固定版本public接口若真正限制方案，保存具体调用/布局证据并提出最小接口需求，同时完成独立工作。遵守子仓适用规则，不能暗改FEX或切到未审计旧runtime。

**真实APK装载。** 默认游戏Launch注入生产backend；smoke模式必须可区分且不会被游戏失败触发。检查最终安装splits、JNI/依赖/libc++、四hooks、Build IDs与构建结果一致；service保持非导出和普通app UID。JNI_OnLoad/资源生成/driver namespace等必须实测，shell库加载不能替代。

### citron 手柄迁移的固定参考与适配边界

本次只做源码核对，尚未复制或实施手柄。参考 `tencentmalos/citron_shadow` 的 [远端可检出的 `2106bcd83844e05dfbb5251a902143233a7ec43b`](https://github.com/tencentmalos/citron_shadow/tree/2106bcd83844e05dfbb5251a902143233a7ec43b)；本机 checkout 位于 `workspace/emulations/switch/citron`，本机HEAD `b79cae0a`在下列引用路径与该远端提交无差异，也无工作区修改。远端提交已通过GitHub API核实。外机按准确提交检出，不能把本机绝对路径写入 CMake。

| 参考路径（均相对 citron 根） | 迁移用途 |
|---|---|
| `src/android/app/src/main/java/org/citron/citron_emu/utils/InputHandler.kt` | KeyEvent/MotionEvent 分发、设备发现、注册与 overlay 入口 |
| `src/android/app/src/main/java/org/citron/citron_emu/features/input/NativeInput.kt`、`src/android/app/src/main/jni/native_input.cpp` | 按键/轴/运动的 JNI 数据协议；替换 Citron singleton 和包名为本仓有 generation 的 session 接口 |
| `src/android/app/src/main/java/org/citron/citron_emu/features/input/citronInputDevice.kt`、`citronVibrator.kt` | 实体/overlay 设备、VibratorManager 和震动能力判定 |
| `src/input_common/drivers/android.{h,cpp}` | Android 驱动映射、控制器登记、震动 worker 与 JNI 引用寿命 |

迁移保留来源和许可证；不把整个 Switch InputSubsystem/EmulationSession 搬入 shadPS4。保留本仓Orbis pad ABI/按钮位/数值范围，平台层剥离现 `SDL_Gamepad*`、joystick id 和桌面配置依赖。可共用诊断/线程等优先走已有Foundation，guest pad语义由本仓负责。

参考实现也需适配而非照抄：其GUID由PID/VID生成，同型号设备会相同；port依枚举顺序，且使用 `controllerNumber` 去重。要求在本仓处理同型号双设备、热插拔/端口复用、InputDevice为null、端口数量上限、AXIS_HAT/扳机范围及dead zone；不能照搬Switch的A/B翻转表当PS4按钮映射。震动worker停止和GlobalRef释放须在设备撤回/session退役后闭环，不跨线程保存JNIEnv。

迁移验收包含：实体按键/两摇杆/扳机/方向键、overlay、guest实际消费、震动、两设备隔离、断连与焦点丢失释放、同进程重启无陈旧输入。与下面完整游戏验收一起交付，不另切一个手柄微型spec。

## 真实内容持续驱动实现与验收

沿用现ContentImporter、PkgExtractor、Library与overlay UI。校验CATEGORY、title/content、版本和完整hash；匹配本体和更新产生可追溯effective tree。更新使用可恢复事务，取消/空间不足/进程终止后保留完整版本；运行中不更换其backing。Launch实际消费已提交安装描述，不直接修改Room DB或用adb推入树代替最终安装验收。

从同一生产路径记录：

```text
UI base/update committed → effective SELF opened → mapped/relocated/modules initialized
→ FEX guest entry → guest-origin typed Orbis calls/threads/callbacks
→ GNM/VideoOut → selected Turnip present → pad consumed / real PCM
→ stop receipt → owners/workers retired → next generation executes
```

fixture只用于隔离具体错误，修好立即回到真实内容。每次首个未支持点记录准确stage/module offset/RIP/NID/所需模块、修复并继续。若缺少用户提供的合法系统模块/内容或目标设备，完成所有独立可做的代码与产物，报告确切外部条件；不把工程复杂、上下文换轮或first-link当成阻塞。

以下是一份整版验收表，不是逐项交接的gate。报告中分别给代码/host测试/CLI/普通APK结果，不将旧成绩累计为本版完成比例。

| 验收面 | 本轮必须具备的证据 |
|---|---|
| 可重建产物 | 新输出目录、一条正式入口、匹配host工具和profile；完整JNI链接exit0；APK安装及普通UID装载；source/pins/依赖/Build ID/link map |
| host里程碑回归 | 保持共享STL/工具/profile和既有epoll取整；显式目录初始化失败可重试；真正guest异常写回另验，不能用本轮无效状态拒绝代替 |
| Android原生平台 | 最终库无SDL实现/JNI初始化；citron输入经本仓pad HLE，含设备/焦点/断连/震动/跨generation矩阵 |
| guest/VM/ABI | 生产模块init/entry/必需HLE、真实guest origin；两层InvokeGuest、可取消WaitingHle、两owner错误隔离；低/高VA及alias/publication回归按改动覆盖 |
| 异常与allocator | 真实支持的guest handler修改现场并恢复、无效/异步状态拒绝；host/JIT/ART归属测试；实际rpmalloc/provider冷启动/retry/churn |
| 内容 | UI本体＋1.08更新、effective module身份；缺本体/错title/取消和中断恢复不产生半安装可运行状态 |
| 声画交互 | 实际Turnip身份与shaderInt64；持续真实游戏帧、可操作场景、pad响应、真实音频与音量控制 |
| 寿命与恢复 | 至少10分钟游戏运行；有界Stop及明确完成回执；同一PID三轮启动/停止均执行新generation；Surface重建/早停/guest fault后正确清理和恢复 |
| 历史合同 | 触及的SessionCore生命周期、COMMON、G2发布/poison/drain、typed HLE/FP/errno/pin、runner失败出口回归；保持生产测试hook关闭 |
| 设备和交付 | AYN与Swan独立状态、APK/符号位置、准确复现命令和已知限制；仅设备缺席时可明确SWAN_NOT_RUN |

对于10分钟和三轮重启记录单调时钟、PID、session/surface generation、guest帧/flip/音频进展与资源回收。测试时出现fail/crash/timeout/重复case/无终态时runner非零；第一帧截图或进程存活不能代替游戏前进和Stop成功。

## 修改范围和最终交接

主要修改集中于根CMake/构建入口、`src/core/host_runtime`、loader/linker/module、guest_cpu/HLE、kernel线程/VM/异常、renderer平台接口和Android runtime/service/driver/importer。健康依赖pin保持；必须改子仓时先核对[owned分支](../subrepository-ownership.md)和适用指令，子仓push成功后再推进gitlink。保留既有脏研究子仓及未跟踪目录。

当前[证据目录](../validation/android-native-host/2026-09-12-host-closure-review/README.md)是失败基线，不覆盖。新结果放`docs/validation/android-native-host/pkg-v2/`下独立目录；更新progress/AGENTS/CLAUDE与索引，以可运行程度和真实失败点描述状态。原始命令、退出码、source dirty状态和对象/APK身份都要可追溯。

最终交付一份整版报告和可安装APK/匹配符号：写明实际内容/设备/场景、验收表、已修问题、剩余准确阻塞与复现方法，提交并推送可检出的代码。若游戏目标尚未达到，标`IN_PROGRESS`，保留当前失败现场和下一处实际修复入口；继续同一任务，不另交一串微型spec或只剩“下一步接入口”的建议。
