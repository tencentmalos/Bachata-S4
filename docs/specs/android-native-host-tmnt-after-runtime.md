# Android：在已接通生产运行时上完成 TMNT 整版 APK

日期：2026-09-13；执行者：用户指定 Opus4.8。仓库 `https://github.com/tencentmalos/Bachata-S4`，分支 `codex/android-fex-round2`，从包含本文的最新远端提交开始。先读[本轮复核](../validation/android-native-host/wp1-mechanism-review-2026-09-13.md)和[可复现证据](../validation/android-native-host/2026-09-13-wp1-review/README.md)，再实施。本文承接[上一整版目标](android-native-host-runtime-after-input.md)，不重新分配本轮已完成的运行时接线。

交付目标：普通 APK 经 UI 导入 TMNT CUSA50828 本体01.00和更新01.08，生产 loader/FEX/Orbis/Turnip/原生 Surface 到实际可操作场景，画面、输入和音频工作，运行十分钟，正常 Stop，同一进程完成三轮游戏启动/停止。两个工作包连续实施，可按内部依赖分提交；不得到首个 HLE、首帧或某个子库编译通过就停下另写微型 spec。

**2026-09-13 直接修复增量：** 先读[libc/runtime修复结果](../validation/android-native-host/libc-runtime-repair-2026-09-13.md)。guest libc bootstrap、session clock/errno、基本mutex/cond/TSD析构、heap/VM启动族现已实现；合成验证更新为CLI10组×3和普通APK16代。真实选取模块六个DT_INIT完成，当前确证失败为`g8cM39EUZ6o#libSceSysmodule#1#libSceSysmodule#Function`。不重做已完成基础接线，不把基础族的通过扩大成完整pthread/动态TLS/游戏兼容。本spec整版目标不变。

## 已有基线，直接复用

生产 `GuestRuntime` 已拥有 Linker/MemoryManager、模块依赖图、guest backing/栈/线程/TLS/errno/stack guard；`FexSessionBackend`、JNI、Service 已接真实 executable path。G45的受控 HleScope/两层InvokeGuest/WaitingHle取消，G46的高VA tag，G47的热回边中断，以及跨owner VM token/pin/失效机制已存在。普通APK12代及CLI7组×3轮是合成程序证据。后续扩覆盖、修新发现，不能恢复旧 CPU loop 当游戏，也不另造 CallGuest、ELF runner 或第二个内存账本。

host DSO 唯一提供 GuestAddressSpace与Foundation InputHub/OrbisPadAdapter，JNI/FEX导入它。保留两DSO架构、SDL-free Android、桌面SDL和明确无麦克风错误；不再因“必须单库”重排构建。NDK29/API33/c++_shared的FEX profile检查、生成host SDK和APK Build ID一致性必须保留。

Foundation owned分支 `codex/shadps4-android-fex-v0` / `5388ef45313d6c32cb5f4bb5b07f1246ee381370`；FEX `385a0cc4`不改。FEX私有IR pass与allocator初始化依赖该pin，变更版本需独立重新审计，不能顺手升级。修改依赖先读[子仓归属](../subrepository-ownership.md)及实际子仓指令，确认owned remote/branch、先push子仓再更新gitlink。保留`references/Bachata-S4`和`externals/dear_imgui/`的无关本地工作。

## 工作包一：真实内容需要的 Orbis 服务和 guest libc 启动

从真实 base+update 的完整有效树启动。参考 `tmnt-content-selection.json` 与最新 `tmnt-*` 边界证据；仅 eboot 缺 FMOD，不能作为装载器失败的最终诊断。`modules/`、`sce_module/`、内容根的正常依赖图已经支持，guest库导出优先参与解析；不要把所有guest库强行换成host HLE或跳过DT_INIT。synthetic SELF的原始节表偏移不在容器内是正常测试输入，不可恢复旧物理offset误判。真实libc/Fios的STT_SECTION当前模块DTPMOD64标记已适配并有fixture；不再当成STT_TLS变量硬拒绝，也不能把此例外扩到所有TLS符号。

历史入口 `tmnt-tls-marker` 已越过：限定Internal标准流/tag兼容策略复用真实guest libc导出，移除了假8字节对象和全库alias；`_malloc_init`通过既有Call执行且可取消，六模块初始化完成。当前入口见`tmnt-libc-close`：sceSysmoduleLoadModule尚未实现。沿当前session图实现provider readiness、引用/加载事务和guest start/cancel，不能直接复用desktop全局模块表里“缺provider也返回成功”的路径；动态TLS/卸载仍须完成live-owner协议。模块ID须由实际调用参数确认。若需要额外系统模块，使用已有/用户合法提供的文件并记录身份，不擅自获取固件或把不可用提供方伪装成成功。

先保存真实模块/import分类清单：模块及Build ID/hash、NID、library/version/module、函数或对象/TLS、重定位位置、guest export/HLE/data-policy/未实现分类。结合具体 crossing 的context/thread/generation/invocation/operation/guest RIP确定实现顺序；映射过不等于调用过，所有UNSUPPORTED_IMPORT清单不等于全部运行失败。先做成族的基础服务，再沿最早确证失败推进，避免每个NID一份交接。

扩展现有生产适配层，建议把函数族从 `guest_runtime.cpp` 按明确依赖拆成实现文件，保持统一 runtime/registry/VM。当前默认pthread、once、timed wait、TLS、errno和flexible VM可用；真实guest libc及模块初始化还会要求线程属性、mutex/cond/rwlock、key析构、时间/调度、direct memory查询/映射、文件与sysmodule/user等。复用主仓实际Orbis算法与返回约定；修正需要host线程对象的部分为guest opaque handle域，不能把bionic pthread结构地址返回guest。

每个启用的接口审查标量及地址语义、输出/可空/长度/方向/溢出、bounded string、结构布局、retained buffer、callback与opaque handle。自动typed factory是候选，禁止全面放行raw pointer、varargs、aggregate、任意整数转地址。guest数据独立声明大小/权限/寿命；本轮stack guard已是只读guest对象，__progname/environ也有明确guest对象与内容，其他data未知应保持拒绝，不可用host全局指针或零地址消错。

guest libc的_malloc_init、相关内存互斥初始化、后续module start/stop与pthread/once/key析构等都走已有guest call边界。需要动态module/TLS时扩展现有图和owner协议，验证live owners、DTV generation、析构顺序和失败回收；不要另建bionic TCB替代guest TLS。生产owner当前保存完成线程的栈/TLS直到generation销毁，持续create/join负载需实现可证明安全的回收/上界，回收必须在owner退出且无callback/pin后走同一VM事务。

HLE等待使用scope cancellation，VM由VmGuard/token协调；不可持pin跨任意InvokeGuest，也不可在仍执行JIT时标记owner已drain。保留fault粘性、errno/fenv、outer continuation与用户Cancel优先级。必要新接口增加针对真实风险的反例/并发/重启测试，而非镜像实现的几十项小断言。

通用guest指令异常需在实际触发时补正式识别和清理：区分host/JIT/guest访问、异步寄存器来源、FEX布局/PC元数据和ART信号链。HLE未知operation的安全fault已经存在；它不能代替SIGILL/SIGSEGV闭环。禁止全局吞host fault、恢复旧host SIGILL或调MAXINST=1掩盖中断问题。

本包出口是正常APK内真实TMNT模块和guest libc初始化/主程序推进的可归属证据，基础服务的可取消停止与失败恢复齐备；遇到下一图形/音频接口继续工作包二。最早未实现HLE是内部定位点，不是整版成功。

## 工作包二：同一 APK 的游戏画面、音频、输入及内容寿命

**图形固定先用Android/bionic Turnip。** 钉住可复用驱动包/hash和ELF ABI，集成adrenotools匹配四hook、实际loader handle/vkGetInstanceProcAddr、namespace与library寿命；实际查询shaderInt64和必要扩展，记录driverUUID/version/identity。禁止静默系统驱动fallback、旧glibc EMULATOR zip和只改环境变量就报通过。

把Session generation的ANativeWindow/AndroidWindow/Presenter与guest renderer绑定；生产输入/用户/平台服务应在放行首次guest调用前就绪，不能依赖nativeStart返回后Kotlin初始化的时间差。有限acquire、suboptimal信号量消费、Surface换代、Stop先于join、fence/waitIdle和driver错误回执需覆盖；借用surface/driver/shared GPU backing在in-flight工作退出前不得回收。GPU映射/alias/page tracking消费现有GuestAddressSpace事务，不新增平行VM或跨越ART保留hole。

AAudio实现真实输出与短写、gain、open failure、Stop，保留OpenAL Audio3D；媒体/FFmpeg使用独立owned子仓现有provider。输入复用Foundation模块→app采集/JNI→host唯一pad，实现FEX guest-origin scePad和实际游戏消费，补持久mapping/profile与热插拔/真实震动。普通手柄机制不进OpenXR全局模块，不引回SDL，不在Foundation中新增应用JNI_OnLoad或C++ Java worker。

UI本体/更新安装沿现有importer/overlay完成title/category/version身份验证和失败恢复。运行时取得不可变有效内容版本的lease，更新写入新版本后原子发布；Stop/drain后释放旧版本，禁止原地覆盖正在执行的guest模块或被读取的文件。普通APK只能用正常私有目录/SAF授权，不把shell文件路径硬编码进生产；保存数据/user/session初始化也必须随generation正确绑定。

最终在同一普通APK完成：UI装本体+更新、真实游戏交互场景、可观察音画输入、持续十分钟、正常Stop和同PID三轮游戏。加Surface销毁/重建、等待期间Stop、失败后再次启动、更新中断恢复；记录线程/FD/VM/GPU/audio资源及缓存上界。合成程序12代、CPU smoke三轮或壳进程长驻都不能代替三轮游戏。

## 验证与交付

先运行本轮canonical脚本建立基线；其ELF/SELF+双模块+TLS+VM+取消/错误恢复矩阵必须保留。使用 `scripts/android/build-host-android`、匹配profile的FEX prebuilt和Gradle生成生产APK；测试hooks OFF，源DSO与APK中strip后Build ID相等，原始日志、退出值、唯一终止记录和case归属一致。根据实际改动加必要测试，不为追求总数反复跑无关矩阵。

目标Swan/API36/4KiB；AYN/API33/4KiB是辅助。Swan不在线明确SWAN_NOT_RUN；16KiB、系统Vulkan兼容、PSVR/Beat Saber后置。所有历史失败/NOT_RUN保留，规范要求与实现证据分开，不改旧二进制源码身份。

交付代码、构建/复现命令、准确PASS/FAIL/NOT_RUN、真实最早未闭合边界和必要视频/截图。主仓及必要子仓commit/push并验证可从远端检出；不提交PKG/游戏文件、驱动包、APK/DSO、debugger原始私有内存或凭据。不因一个内部编译/链接/导入问题再次停下给用户微型规划。
