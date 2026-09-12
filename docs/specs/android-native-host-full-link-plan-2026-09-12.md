# Android host bionic 迁移：整包执行方案

更新：2026-09-12。实现基点 `2d01ce4b`，分支 `codex/android-fex-round2`。本次已直接完成下面的依赖前置，具体源码身份与验证见 [前置交付记录](../validation/android-native-host/bionic-prerequisites-2026-09-12.md)。本文替代原来的七步 full-link 设计；与 [PKG v2 整版任务书](android-native-host-pkg-v2.md) 配合执行，原 CPU/VM/生命周期合同不变。

**只保留两个连续工作包：完整 bionic host/APK，真实 PKG 集成与真机收口。两个包属于同一 PKG v2 版本；内部可以按依赖顺序编译、小提交和定位，不能在一个 option、一个库、一次 first-link 后再次交接规划或等待许可。**

## 当前事实与本次已做的前置

| 项目 | 状态及意义 |
|---|---|
| COMMON 两处修复 | `4740f053` 已完成、已回归，不再进入待办 |
| date Android gate | `2d01ce4b` 已加入；本次进一步验证真实 tzdata/current_zone 运行 |
| 五组共用依赖构建配置 | 本次抽取 [HostPortableDependencies.cmake](../../cmake/HostPortableDependencies.cmake)，主仓 externals 和 [Android 依赖入口](../../cmake/android-host-deps/CMakeLists.txt) 共用；date、glslang、Zydis、miniz、LibreSSL 的 upstream 源码和 gitlink 不变 |
| 可重复构建入口 | [build-host-deps](../../scripts/android/build-host-deps) 校验实际 NDK metadata、API、ABI、STL 和缓存身份，记录命令、退出码、源码/库 SHA、ELF 和 link map；不是第二份 emulator 源列表 |
| 实际验证 | 五组依赖的实际 ARM64 编译＋`--no-undefined` probe `.so`；真机验证时区、SHA256、TLS context、CRC、x86 解码、GLSL→SPIR-V；结果与限制见前置记录 |
| FFmpeg与驱动依赖 | 独立owned子仓：FFmpeg7.1.5 `e17ba6e2` 已提供六库NDK源码provider，生产媒体TU7/7＋AYN媒体probe9/9；libadrenotools锁azahar成套版本并完成NDK五个DSO链接，尚未加载Turnip |
| 扩展依赖验证 | 生产externals图完成NDK shared-link；AYN14/14，hwinfo的x86 glibc路径误判已在owned fork `85bbcba`修复，原失败断言通过；旧13/14证据保留。此处报告Android native process ABI |
| 三方库同批处理 | 见[完整归属与配置核查](../validation/android-native-host/bionic-third-party-audit-2026-09-12.md)；修复spdlog绕过固定fmt源码、OpenAL Android后端选择；健康gitlink保留，不把独立库搬进Foundation |
| 尚不存在 | 完整 host target、真实游戏 backend、native Turnip 接线和真实 PKG 游戏验收。五组依赖 probe 不能安装为游戏 host，也不证明其他依赖已通过 |

参考 host 是 ARM64/glibc，**这是早已确认的迁移前提**，不应再当成改变架构的新发现。只能排除那份 glibc 库直接装入 bionic 进程，不能推导“参考工程完全没有可复用的 native 库/代码”。同样，glslang 出现 ANDROID 分支、date 单库编过，都不证明所有 CMake 库自然可用；本次各probe及其验证范围分开记录，不把依赖结果合并成游戏完成率。

FFmpeg 源码供给已补齐，完整host媒体接线仍未完成。旧 citron 包不能直接替换：真实消费者 5/7 可编译，两个 VideoDec TU 因缺 COPY_OPAQUE/INTERLACED API 失败，不能直接替换；细节见[前置记录的 citron 核查](../validation/android-native-host/bionic-prerequisites-2026-09-12.md#citron-的-ffmpeg有现成-bionic-包但不能直接替换)。AJM MP3 也使用它；不能为第一次链接删除媒体/HLE 注册后把缺口交给下一版。

## 工作包一：完整 bionic host 与普通 APK

这个包把构建、依赖、平台边界一起做完，产出普通 app UID 能加载、初始化、停止和重建的完整 host。此时可用局部 fixture 定位，但接着直接执行工作包二。

### 构建结构与固定选择

| 决策 | 执行方式 |
|---|---|
| ABI/API/STL | ARM64、host 4KiB。辅助 AYN 统一 native API33，Swan/API36 可先运行同一 API33 原生包；只有实际 API 需求才提高整套 native profile。保留既有 `c++_shared`，APK 只打包对应 NDK 的一份运行库。compileSdk/targetSdk 与 native API 分开记录 |
| FEX | 固定 `385a0cc4`，仍由既有脚本单独构建、`cmake/fex` 提供 guest_cpu_fex。原有 prebuilt35 不因文件存在而自动用于 API33；检查或重建一致版本。FEX options/allocator 不泄漏到 host；生产 `V0_BUILD_TESTS=OFF` |
| 源列表 | 根 CMake 现有 CORE/COMMON/VIDEO_CORE/SHADER_RECOMPILER 等是唯一源。允许机械提取到共用 CMake 文件，并同步 census 读取入口；不复制一份大列表，也不因 regex 工具而禁止必要重构 |
| targets | 首先做真实对象集合和最终 SHARED，不要求先证明一条虚假的无环 headless 层级。可用 `shadps4_host_objects` OBJECT＋依赖 INTERFACE target；保留必要静态分组，但同一 TU 只编入一次 |
| APK DSO | 默认保留现有 `libshadps4_fex_session.so` 的 JNI 装载名，将完整 host 对象接入它，降低 Kotlin/JNI 改名成本。`libbachata_pkg.so` 保留独立导入职责。不能在两个 DSO 各嵌一套 FEX/SessionCore/全局 runtime；无需为了名字再拆第三个 host DSO |
| Android 入口 | 从现有 SessionCore/ISessionBackend 接入生产 HostRuntime，代替 smoke backend；保留 generation/control lease/terminal/owner 的既有修复。不能直接调用含 quick_exit、fork/restart 和 SDL 主循环的 Emulator::Run |
| 桌面支持 | Android 选择 host target，桌面保留有效入口和平台依赖；host 配置不能先执行 desktop 下载、打包和可执行文件链接步骤。新增定义、include、语言标准尽量绑定 target，注意 app 当前 `-std=c++20` 与 host 已用 C++23 的需求不能相互覆盖 |

根 COMMON/CORE 中仍有 frontend、NP、input、renderer 引用；`AUDIO_CORE` 空变量只说明真实音频在 AUDIO_LIB/CORE，绝不表示音频依赖可以删除。不要把头文件未引用 Vulkan 或 STATIC archive 创建成功当作完整 CPU/HLE 链接。

### 同批解决依赖供给，尽早链接最终库

本次五组配置与其余依赖的生产 externals 配置继续共用，直接在同一 host 构建图接入，不逐库新写 spec。默认优先使用固定源码＋NDK；统一 PIC、异常、RTTI、STL 和生成头身份，输出依赖清单，隔离 Homebrew/桌面 pkg-config/sysroot。FEX 与主仓自带的 fmt/xxhash 等同时入链接时检查版本、重复符号/ODR 和实际取用的定义，不全局打开 allow-multiple-definition。

| 依赖组 | 同一工作包内的处理 |
|---|---|
| 已落地的五组 | 调用共用配置；产物目录和身份可用于复现/定位，正式 host 仍由其自身 CMake target 正确传递依赖，不无条件 glob `.a` 或把诊断目录当作通用 SDK |
| 压缩/图像/字体/基础 | zlib-ng、zstd/ZArchive、libpng、freetype、fmt/spdlog、pugixml、sirit、Vulkan headers/VMA、Tracy 等按真实 target 构建；header-only/代码生成/需链接库分开记录 |
| FFmpeg/媒体 | 直接复用新增 `externals/ffmpeg/cmake/android` 的 `FFmpeg::ffmpeg`，固定 owned `e17ba6e2`（官方n7.1.5基点），无需再次比较旧citron包或从零编写provider。NDK六库/同prefix头已验证；正式host还需验证完整媒体消费者链接和游戏触及的解码/seek/EOF/资源退出。保留软件decoder/parser/demuxer/filter/scale/resample，不补宏、删HLE或混新头旧库；默认不启用网络/硬解。provider已修复PIC汇编符号绑定和安装archive的增量重链接 |
| FFmpeg 版本身份 | Android以新独立源码gitlink、公开av*_version和同prefix头为准。桌面原ffmpeg-core `94dde08c` 的ffversion.h虽写5.1.2，实际major61/61/59，不据旧字符串选包。源码构建version string可能为owned短SHA，不能误报版本缺失 |
| 音频 | LibAtrac9/fdk-aac 与 OpenAL 的实际引用保留。普通 AudioOut 默认 AAudio；Audio3D 仍依赖 OpenAL，不能因换 AAudio 就全删 OpenAL。本轮已加入Android OpenSL/REQUIRE_OPENSL配置，关闭桌面后端发现；若用 OpenAL loopback 向 AAudio 输出，验证实际 PCM 去向，不能用 NULL backend 报有声音 |
| SDL/ImGui/input | 首版不追求把所有 SDL helper 重写。允许 NDK SDL3 为仍在用的通用函数服务，但不让 SDLActivity、SDL 窗口/主循环接管现有 Kotlin app。抽出 window info/Surface provider 和 controller adapter，按实际调用裁剪 desktop big-picture/file-dialog/input backend；保留 renderer 真正需要的 ImGui 核心/后端，不能靠空函数消符号 |
| SHADNET/protobuf | NP handler/matching2 直接引用 ShadNetClient，不能只从源列表删 SHADNET。默认构建真实 client/runtime，默认离线不连网；protoc 单独为构建机准备，版本匹配目标 libprotobuf，不能执行 ARM64 Android protoc。若选择明确禁用在线能力，须同时定义所有调用边界和正确的不支持结果，不能伪造 guest 成功 |
| host 平台差异 | uuid、epoll-shim、Discord、updater、hwinfo、USB 等按实际用途处理。Android 不继承桌面专有 provider；USB 权限/设备不可用必须明确，不把 null 设备当成功 |
| Foundation | FFmpeg、FDK-AAC、OpenAL、libusb等已有独立子仓，不搬入Foundation；本轮Foundation pin不变。复用已开启 DebugBus/Android dumpsys。反射/网络没有完成其依赖和生命周期审计前不扩大启用范围；guest ABI 留主仓 |

FFmpeg provider已落地；**完整 host/APK 包保留并接入全部现有消费者**。一开始就建立最终共享库 target，反复驱动其真实链接错误；对象库/STATIC 库只是内部组织方式，不作为交付点。

最终共享库使用 `--no-undefined`、Build ID、link map；检查实际 loader/HLE/renderer/media 对象是否被保留。`nm -u` 中由已声明 Android 动态库提供的导入是正常现象，要求的是依赖可解析，而非机械要求所有 UND 消失。禁止 unresolved-symbols 忽略、空 RegisterLib 和没有被入口引用的空壳库。再验证整个 DT_NEEDED 树在 APK namespace 可加载，无 glibc/桌面运行时混入。

### 平台接线与 APK 初始化一起完成

- **Turnip 作为唯一默认驱动路径接线。** 复用新增libadrenotools `60ae5bbc`＋linkernsbypass `aa397589`，主仓CMake已关联配套四个hook target；完成APK提取/打包、nativeLibraryDir与app私有driver目录，不能只打包静态loader。 受控 bionic 包的 SHA/ELF/ABI校验、实际 native loader handle、vkGetInstanceProcAddr、dispatcher、instance/device 共用同一归属；直到最后一个 GPU worker 退出才卸载。缺失/不兼容明确失败，不回退系统，不用环境字典代替加载。记录实际 driverName/driverInfo/shaderInt64/扩展；系统 vkjson 不算 Turnip 验证。
- **窗口、输入、音频作为 session 资源。** Kotlin/JNI 传入真正的 ANativeWindow 和 generation，替换 WindowSDL 硬依赖；处理尺寸、detach/recreate、Stop 前通知、已 acquire semaphore/image 的退休、fence/queue 有界退出。Controller snapshot 接入 pad；AAudio 补短写、初始化失败、gain、断连和停止。细节合同沿用 PKG v2，不另立小阶段。
- **bionic/ART 共存属于迁移包。** 实际 rpmalloc/SetupHooks 的地址所有权、重入初始化、信号处理顺序和 teardown 与 ART/图形同进程验证。保持 guest/host TLS 分离，不随意改 SIGILL 或 mmap 覆盖 ART。FEX 子仓受限制的必要接口更改给出精确证据和接口提案，继续其他可做工作。
- **APK 真加载。** 从既有非导出 service/普通 app UID 进入生产库的初始化和停止，确认打包的 JNI/依赖/libc++ 与源码一致。验证多次同进程 create/stop、Surface变化、早停/失败恢复、时区/媒体基本调用与真实 Turnip 的资源创建。通过仅表示 HOST_READY，随后直接做真实游戏。

## 工作包二：生产执行链与真实 PKG 真机收口

不再拆成 first entry、first HLE、first frame 三个交付版本。复用 [PKG v2 §3、§5、§6](android-native-host-pkg-v2.md)，连续完成以下一条主线：

```text
UI 本体＋更新安装 → effective SELF/ELF → 主仓 loader/relocation/module init
→ FEX guest → typed Orbis HLE/线程/TLS/回调 → GNM/VideoOut
→ Turnip present＋pad＋真实音频 → 可操作场景 → 十分钟 → Stop/同进程三轮重启
```

核心实现归入同一包：VM/backing/alias/GPU tracking 统一所有权，修复 64GiB 几何冲突；module init/fini、malloc、pthread、destructor、callback 等所有 guest 函数都走正式 Run/InvokeGuest，禁止 cast 成 host ARM64 函数。typed resolver/veneer、HleScope、两层 InvokeGuest、可取消 WaitingHle、独立 guest 栈/TLS 和多 owner 错误归属同时补齐；保留 publication/poison/drain 合同，不能先把安全边界关掉。

真实内容使用已确认的 TMNT CUSA50828 **1.00 本体＋1.08 更新**，按 [pkg-set.json](../validation/android-native-host/2026-09-12-review/pkg-set.json) 获取路径/身份。通过已有 UI importer 做可恢复安装/overlay 和 launch 描述，不把补丁单独登记为本体，也不改数据库伪造安装。游戏缺失实际所需模块时记录准确名称/导入/失败证据，不下载不明 runtime 或广泛给未知 HLE 返回成功。

AYN/API33/4KiB 先验证；Swan/API36/4KiB 独立记结果。Swan 不在线不妨碍完成 AYN，但不得写 Swan PASS。16KiB、系统驱动、VR、finite Step 和广泛兼容继续后置。

## 执行方式、完成条件与交付

**内部检查持续进行，但只报告整包状态。** 编译、链接、加载、guest 前进和游戏可玩分别有证据；HOST_READY 不等于 PKG v2 完成。不要再提交“只改一个 date gate / BUILD_HOST_CORE option，下一轮继续”的阶段报告，不再等待是否开始 CMake 的确认。

| 整包 | 必须交付的结果 |
|---|---|
| bionic host/APK | 干净输出目录从固定依赖完成 full-host `.so` 和 APK；无未解决链接缺口；普通 UID 实际加载；默认 Turnip、Surface、输入、音频、allocator/生命周期接线与故障恢复证据。附 toolchain/API/STL/pin/依赖清单、命令、库/符号身份和 link map |
| 真实 PKG | 主仓生产链加载匹配内容，进入可操作游戏场景，持续十分钟、实际声画/输入；有界 Stop 和同进程三轮重启。安装取消/回滚、surface/early-stop/guest fault，以及实际触及的 G2/G3 合同有回归。附 APK、符号、内容身份、设备日志/场景证据和准确复现步骤 |

准备代码可小批提交，最终交付必须能在另一机器按同一脚本重建，子仓先 push 才推进 gitlink。游戏、驱动大包、NDK、build 输出不入主仓。依赖确需改源码时先检查 owned fork/分支和适用规则；本次FFmpeg子仓新增可复用adapter，hwinfo在新owned fork修复Android位数判断；libadrenotools是无源码修改的匹配pin，其他健康gitlink不变。

长任务可跨上下文继续，保存当前失败命令和下一处具体错误，不重做调查/规划。若真正外部阻塞无法消除，报告最后一个实际失败的阶段、RIP/module/NID 或链接符号、已尝试的修复及需要的具体外部条件；同时完成不依赖该条件的工作。不能把“工程大/上下文不足/第一次链接成功”当成整版完成。
