# shadPS4 Android / FEX development context

## 最新：R8文字修复与真实RenderDoc回放

先读 [字体根因与复验](docs/validation/android-native-host/font-r8-tiling-fix-2026-09-14.md)。TMNT自带字形在guest内存中完好；Turnip shaderInt8=1但storageBuffer8BitAccess=0，原host R8解平铺错误依赖后者。现改32位打包，真机6/6（逐组1MiB独立CPU比较）通过，普通APK使用条款文字清晰，FPS曲线和全屏保留。实际RDC及Android同Turnip replay已完成，具体hash/SDK配对补丁见图形工具交付和cmake/renderdoc。后面的“无真实RDC/文字破损”是历史阶段；完整工具/其他画面/guest8位SSBO/可玩验收不因此自动关闭。没有新增spec或全回归，FEX/Foundation不变。

## 最新直接修复：图形控制、GPU Reshape、并发分析

先读 [交付和证据边界](docs/validation/android-native-host/graphics-toolkit-repair-2026-09-14.md)。旧6062df92审核的控制/capture/counter问题已经直接修复；Citron有界handoff recorder、分析/Archify投影已迁移，四类队列/同步问题已修正。Cemu式可选GPU Reshape已接普通Turnip APK，独立SDK仍含未发布dirty源码，使用主仓补丁和精确清单，不伪造可fetch新pin。242/0定向C++、Python12；SDK OFF和单shader各120秒Stop通过，正确物理屏截图显示背景/弹窗，但文字破损仍在。全量SDK映射告警尚不能归因为游戏越界。真实RDC/replay、完整PROF/Layer/PM4和可玩验收仍未完成；不改spec、不全回归、不加auto tag、不改FEX/Foundation。后面的“本轮只审核未修代码”已由此交付覆盖。


## 最新复核：调试工具尚未完成（30e3b21f）

见 [进展审核](docs/validation/android-native-host/graphics-toolkit-progress-review-2026-09-14.md)。§3.1 部分完成，§3.2 仍有同边界空捕获、超时遗留 backend、Query 等待 End、跨请求/Session 归属及 Android loader 未调用等问题；queue_submit 实为 guest 入队，host_present 非实时，phase/Stop 未接。原 native173/0 重跑通过，但12个反例也全部复现；原 APK2/0 未验证真实 RDC/replay。本轮只审核与归档，未修生产代码。先整批修控制/捕获，再按原 spec 连续推进 PROF/Layer/PM4；不以常规子仓/ImGui 适配为由改变目标，auto tag仍延期。

## 下一阶段：图形调试工具整包（待实施）

执行 [android-graphics-debugging-toolkit.md](docs/specs/android-graphics-debugging-toolkit.md)，来源见 [审核](docs/validation/android-native-host/graphics-debug-tooling-audit-2026-09-14.md)。用户让另一位Opus4.8实施RenderDoc、litep/PROF、Foundation ImGui Layer、guest command trace、PM4/GPU指令证据与统一关联。**最新明确auto tag先不做；不改FEX、不加guest二进制探针。** 两个连续工作包，不拆微型spec。工具目前尚未补齐，不能将该规划写成实现状态；具体版本/命令/ABI/验收见spec。Citron本机6a86baf4未发布，远端仍2106bcd8；参考Foundation2b2683ff可取得，SDK保留原独立子仓来源。

## 最新：按键后 NP 账号查询故障（2026-09-14）

[本轮修复及证据](docs/validation/android-native-host/np-offline-identity-fix-2026-09-14.md)：op56/GetAccountIdA Unsupported已关闭。桌面/guest共享离线身份规则，11查询＋2原有poll，SDK/signup快照、checked输出、库名/online准入。native339/0、x86 syntax2/2、APK真实FEX13接口同PID三轮PASS。真实TMNT已走到账号/online ID并返回SIGNED_OUT，继续账号存档读写；单轮120秒、主动Stop CANCELLED，但只有161 presents、截图黑屏，画面正确性/可玩仍未关闭。不要重报op56缺失，也不要将存活测试写成游戏验收。host/JNI仍RelWithDebInfo、FEXCore Release、playstoreDebug；保持网络SSL仅桌面离线策略，无全回归、FEX/Foundation不变。

@AGENTS.md

`AGENTS.md` 是共享事实与工程约束；本文件只提供 Claude/Opus 入口。

## 当前交接（2026-09-14）

最新先读[本地AvPlayer、存档目录与Stop修复](docs/validation/android-native-host/avplayer-directory-integration-2026-09-14.md)。21个AvPlayer NID已接入桌面解码＋真实FEX回调，目录O_DIRECTORY失败导致的存档崩溃已修复；native1042/0、目录334/0、图形准入9/0。最终普通APK合成AvPlayer三轮PASS；真实TMNT单轮120秒、2525 presents、主动Stop CANCELLED/JUnit PASS。游戏弹窗可见但文字损坏，没有可玩/十分钟/Swan或真实游戏三次长启动验收。GNM等待准入取消和AvPlayer全局Stop后回收映射的失败均已修复，不应再列为待实现。FEX/Foundation未改；保留分阶段失败与dirty产物身份。网络/SSL只按desktop离线dummy处理，不扩在线功能；继续图形正确性和可操作场景，定向测试，不另写spec。以下“AvPlayer未实现”已是历史边界。

最新先读[SRT、BC纹理与WSI修复](docs/validation/android-native-host/srt-bc-integration-2026-09-14.md)。ARM64现代SRT已补，native479/0、x86 syntax4/4、shader8变体PASS；普通APK synthetic六代PASS。真实TMNT143次present后在sceAvPlayerSetLogCallback/op110具名fault，120秒用例仍FAIL、未确认可用游戏画面。Android逐帧swapchain重建已消除。继续desktop本地AvPlayer实例/解码/guest回调/取消整族，尚未实现；网络SSL仅离线兼容，定向测试，无新spec。以下“ARM64 SRT未实现”是历史边界。

优先读[AudioOut、内核信号量与Json2兼容交付](docs/validation/android-native-host/audio-semaphore-integration-2026-09-14.md)。Native55/0＋55/0、Json2 35/0；普通APK13-import三轮。TMNT原invalid-free已按desktop的Json2可选模块记账策略修复，同PID三次运行20秒后正常取消，仍guest_presents=0、不可玩。延长观察后约51秒真正到首次compute shader，ARM64 FlattenExtendedUserdataPass未实现导致SIGABRT；直接补现代SRT实现/缓存，不能照搬仅静态offset的旧reference。只做定向验证；网络/SSL不扩展，不新建spec；下文AudioOutInit边界已过时。

最新先读[生产AJM异步解码与真机证据](docs/validation/android-native-host/ajm-runtime-integration-2026-09-14.md)。Native126/0含真实MP3解码；普通APK三轮真实FEX验证8/10参数batch，完整TMNT三轮到AudioOutInit/op95，仍0帧。继续会话AudioOut和AAudio短写/音量/打开失败，不扩网络SSL，不全回归。下文模块/libc修复仍保留。

先读[零偏移 DT_INIT、模块复用与线程析构修复](docs/validation/android-native-host/module-init-thread-dtors-2026-09-14.md)。FMOD 空指针根因是把存在但值为0的DT_INIT误当缺失，现已修正；旧“六模块初始化完成”日志不可信。普通APK模块矩阵9轮、析构4轮、完整TMNT同PID3轮越过原崩溃，稳定到sceAjmInitialize/op385，Turnip已就绪但0帧。继续AJM整族的checked batch/解码/会话寿命；动态新模块/TLS卸载仍未完成。网络/SSL不扩展，只做定向测试；FEX/Foundation不变。

## 前一交付（2026-09-14）

先读[离线兼容、条件变量/VM与生产Pad交付](docs/validation/android-native-host/network-condition-pad-integration-2026-09-14.md)。最终TMNT同PID三轮已越过NetInit/SSL/Pad，现为LoadStartModule/op44，仍0帧。定向native network199/0、cond66/0、Pad85/0；APK52-import取消恢复五轮、clock/VM三轮、Pad真实guest三轮均过。**用户最新明确网络和SSL不展开实现，按桌面兼容处理；SSL Init/Term复用已有dummy，不宣称TLS。** 后续直接确认并处理LoadStartModule路径与guest初始化/TLS，不再写spec，不跑全量回归。FEX/Foundation未改；输入从JNI到同一host InputHub再到guest已经过新用例。

## 前一交付（2026-09-13）

先读[Sysmodule／文件／RTC／AppContent 增量与验收限制](docs/validation/android-native-host/sysmodule-rtc-content-integration-2026-09-13.md)，它覆盖下文旧 WithArg 卡点。设备已重连：sysmodule30/0、文件/DLC/临时目录144/0、最终RTC397/0；22-import真实guest三轮返回，完整PFS资源TMNT同PID三轮到 **sceNetInit/op312**，仍零帧。AppContent八入口、实际DLC/临时挂载、空间查询和Stop清理已真机验证；RTC格式化/解析由desktop与guest共享纯helper。临时目录每会话新建，不能称持久缓存；Precise保留原桌面两位小数输出。旧“设备断开未测”和APK/host不一致保留为历史，最新库SHA已核对一致。继续直接处理网络会话/init/池/状态/errno/回调与取消等依赖，不用另写spec或重跑全量回归。所有源码/产物/内容身份、测试目录与明确缺失项在该报告。

下文是之前已交付的线程/libc与更早阶段记录：

优先读[桌面 libc 策略与线程族整批实现](docs/validation/android-native-host/thread-libc-integration-2026-09-13.md)。本轮已接系统 `libSceLibcInternal.sprx` 优先加载、初始化失败不混用回退；无系统库时用显式 112-NID 策略解析真实 guest libc（当前游戏102导出、原27个Internal缺口归零，仅静态导入覆盖）。attr/create实际消费、rwlock、desktop mutex protocol/ceiling 与 VM 写回排他已接；后台fault保留原owner并取消主线程。最终AYN：attr43/0、locks60/0、G49/G48定向6/0；普通APK三个JUnit选择器通过，其中TMNT仅边界观察，三轮均到 `sceSysmoduleLoadModuleInternalWithArg` / op527 / Unsupported，guest_presents=0。没有真实系统固件libc/完整游戏/全量回归/Swan验收；最终源码与产物见manifest，FEX/Foundation未改。

**用户要求：每个缺口先查桌面版注册、实现及依赖，按函数族连同provider/init/生命周期一起处理，不再单个NID交微型spec。** desktop protocol同样没有实时优先级捐赠，不能把其现有模拟语义说成Android实时调度保证，也不应未经对照就返回更严格的ENOTSUP。继续当前整版目标，默认只测改动相关部分；下一实际边界是动态sysmodule加载族，必须真实处理初始化参数/provider就绪，不能假成功。以下旧pthread_attr_init、libc逐个补洞等状态由本段取代。

最新读[图形／存档实际集成与设备证据](docs/validation/android-native-host/graphics-storage-integration-2026-09-13.md)，它覆盖下方旧状态：GNM→VideoOut→Turnip→Surface 已由 FEX synthetic guest 三轮各呈现四帧；会话 GPU 页保护/FEX fault 分发、命令所有权/VM drain 已接。存档跨进程及 APP_VER 更新读回、生产 Compose Cancel 已过；信号量已接。host511/0、FEX244/0、Session840/0。**真实 TMNT 仍未出画面，当前 op280 pthread_attr_init，不能说所有非图形启动已完成**。下一实际工作是 attr 域和 pthread_create 消费属性，继而完整内容/实际游戏负载；不要重做初始图形接线，不另拆 spec。以下旧“RegisterBuffers 边界／保存未接”的说法作为历史保留，以此为准。

用户补充：本版必须正确支持存档。缺失的 libSceSaveDataDialog 是交互层，libSceSaveData 是持久化服务；二者现已接入，范围与未覆盖模式见最新记录。缺失错误不是完成或延期理由。随真实游戏推进接入会话隔离、guest 指针/文件挂载、稳定用户/标题存档目录与真实 dialog 状态；验证保存→退出→重启读回、更新保留存档及失败/取消，不能假成功或把存档放进临时内容目录。详见当前复核记录的存档补充，不另拆 spec。

最新先读[WP1复核与原生Turnip／Session图形接入](docs/validation/android-native-host/wp2-native-turnip-runtime-review-2026-09-13.md)。本轮直接修复并推进，没有新增 spec：普通 APK 已加载固定 bionic Turnip（实测 shaderInt64=1），四个 hook 仅打包、不静态变成 JNI DT_NEEDED；Session generation 持有窗口/Presenter/VideoOut/IRQ，输入桥接完成后才 PlatformReady。VideoOutOpen/Resolution/SetBufferAttribute/Vblank 已过实际 guest；SetBufferAttribute 正确解第七个栈参数。真实选取 TMNT 的新边界是 sceVideoOutRegisterBuffers（w3BY+tAEiQY, op153），并非游戏画面或可玩验收。

已修 provider readiness、每代用户/系统队列、库准入绕过、PageManager覆盖FEX信号、VM发布与继续执行的准入竞态、线程销毁与发布冲突、内部Pause污染HLE取消源。G48强制33次交错和token下Cancel；FEX子仓保持385a0cc4。passive SignalDispatch只解决OS信号归属，GPU tracking与guest VM/FEX fault delivery仍需真正组合；不得仅扩大allow-set透传桌面缓冲/命令指针。精确测试计数和产物身份见交付manifest。旧“所有非图形启动已完成／尚未绑定Turnip窗口”两种描述都不再作为当前事实。用户要求继续在当前实现推进，不另拆微型spec。

先读[libc 启动与服务直接修复](docs/validation/android-native-host/libc-runtime-repair-2026-09-13.md)：已修 `_malloc_init` 顺序、错误 getspecific NID、session clock/errno、mutex/cond/TSD、heap/VM 启动族及通知后 cond 销毁竞态。最终 AYN services43/0、guest242/0、CLI10组×3、普通APK16代同PID；保留d499219e+dirty身份。真实选取内容已完成六模块DT_INIT，原停在sceSysmoduleLoadModule（现已被上面 WP1 族扩展越过），仍非可玩/Swan验收。不能重复原先“clock_gettime已正确、0x445c5f是精确fault RIP、两libc是同一库”的结论。继续下列整版spec，session sysmodule/provider与平台服务仍需实接；不要再从基础运行时或libc bootstrap重做。

先读[本轮运行时复核与修复](docs/validation/android-native-host/wp1-mechanism-review-2026-09-13.md)，然后连续执行[TMNT整版spec](docs/specs/android-native-host-tmnt-after-runtime.md)。用户明确要求生产 Linker、VM、线程/TLS、可取消HLE回调由当前复核轮完成，**这部分已经实现，不再交给下一位从头接线**。

生产 `GuestRuntime` 通过 `FexSessionBackend`、JNI 和普通 Service 执行 ELF/SELF；Module/Linker/VM/backing、guest pthread/TLS/errno/stack guard、HleScope/两层InvokeGuest/WaitingHle取消都已组成运行时。AYN/API33/4KiB：guest242/0、contract46/46、ABI14/0、registry13/0、veneer19/0；普通APK12个同PID generation覆盖双模块DT_INIT/TLS、子线程运行期间VM、正常/取消/坏指针/未知import/初始化取消及恢复。host dlopen+85/0、macOS现代LLVM contract45+1SKIP/46、Session807/0。原始证据保留07ce52ae+dirty身份；不能把它们说成TMNT已可玩或Swan已验。

下一阶段扩真实游戏的Orbis函数族与session sysmodule/provider，集成Turnip/Surface/AAudio/FEX-origin pad/内容版本事务，最后UI本体+更新、交互场景、十分钟、Stop、同进程三轮游戏。已完成的guest libc启动直接复用，连续推进两个工作包，不在每个NID/库/首帧处重新交回微型规划。

## 必须保留

- 目标Swan/Android16/API36/ARM64/**4KiB**；AYN为辅助，Swan不在线标NOT_RUN；16KiB和PSVR后置。FEX只跑PS4 x86 guest，host是NDK/bionic原生ARM64。
- host DSO唯一提供GuestAddressSpace和Foundation InputHub/OrbisPadAdapter；JNI/FEX导入生成SDK。无需重做单库。Android已无SDL和冲突JNI_OnLoad，桌面SDL保留。
- Foundation owned `codex/shadps4-android-fex-v0` / `5388ef45313d6c32cb5f4bb5b07f1246ee381370` 已push；FEX维持 `385a0cc4`，不绕过子仓指令改动。依赖修改先查owned ref和[归属](docs/subrepository-ownership.md)，子仓先push再pin。
- FEX使用实际rpmalloc+普通mmap/munmap hooks；不steal ART高VA、不替换bionic malloc、不用Windows HookPtrs。guest owns4MiB–256MiB、4GiB–120GiB，ART hole不在账本。exact reserve碰撞可回收失败，不扫描maps后MAP_FIXED。
- 保留主仓EntryBackedgePass：pinned FEX局部条件回边会跳过entry interrupt poll，G47以真实热循环复现。pass只改guest EntryPoint边；不改REP/原子内部循环、不用MAXINST=1掩盖。
- NON-spill退出JIT后执行HLE，scope控制回调；native fenv/errno、outer continuation、fault/cancel/exit归属保留。VM内部park必须真实退出JIT、释放资格；不能清用户Cancel或带pin跨任意callback。
- SELF原ELF节表offset不是容器offset；模块依赖支持modules/与sce_module/。guard是显式guest数据；未知对象拒绝，未知函数具名fault，不写native函数或global地址到guest GOT。
- 通用guest指令异常、非默认pthread/动态TLS/API覆盖仍有实际边界；新调用沿已接好的runtime完善，不把合成check数当全部HLE语义已经完成。
- 默认Android/bionic Turnip，无静默系统driver fallback；实际loader handle/shaderInt64/namespace/Surface寿命必须验。FFmpeg是独立owned子仓，不塞Foundation。
- TMNT01.08是更新；[本体/更新身份](docs/validation/android-native-host/2026-09-12-review/pkg-set.json)。本轮选择了base后overlay的eboot/模块/param.sfo，不含完整游戏assets。不能拿旧01.00 eboot-only冒充完整1.08启动。
- 不提交游戏/PKG/driver/APK/DSO或凭据；保留无关`references/Bachata-S4`和`externals/dear_imgui/`本地工作。所有失败和旧NOT_RUN保留，不能倒写历史Build ID或源码身份。

## 复现与历史资料

[本轮命令与证据](docs/validation/android-native-host/2026-09-13-wp1-review/README.md)；构建入口`scripts/android/build-host-android`、匹配profile FEX、Gradle的`fexBuildDir`/`hostLoaderConfig`；验证入口`scripts/android/validate-production-runtime-android`。源host与APK剥离符号后的Build ID必须相同。

[输入接通复核](docs/validation/android-native-host/runtime-input-review-2026-09-13.md)保留Foundation49/0+Android5、pad45/0、APK input6等证据。旧Stage0“crt到首个HLE已过”已撤回；prologue harness仅LOAD_AUDIT，不重新用作执行门槛。

[完整资料索引](docs/README.md)、[基础版本](docs/baselines/2026-09-07-android-fex-foundation.md)、[V0](docs/specs/android-fex-v0.md)、[Foundation](docs/foundation-integration.md)、[host→guest LLDB](docs/fex-lldb-host-guest-workflow.md)。历史allocator BLOCKED/无HleScope/未接runtime等描述是旧状态，当前实现以本轮复核和AGENTS为准。
