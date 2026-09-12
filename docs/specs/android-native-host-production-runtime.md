# Android 生产 Runtime：从 host 对象闭包推进到真实 PKG APK

日期：2026-09-12。实施基点：`codex/android-fex-round2` / `1a7da92c514f11b9923dd13f6887a1733a076be5`。先读[本轮复核](../validation/android-native-host/pkg-v2-host-closure-review-2026-09-12.md)。本文是下一轮执行任务，细化并接续[两工作包方案](android-native-host-full-link-plan-2026-09-12.md)，保留 [PKG v2](android-native-host-pkg-v2.md) 的功能、正确性和真机验收要求。

## 本轮交付目标

**连续完成一个生产 Android Runtime：普通 APK 从 UI 安装 TMNT 本体＋更新，经主仓 loader、FEX guest、Orbis HLE 和 Turnip，进入可操作游戏场景，运行十分钟，正常 Stop，并在同一进程完成三次启动/停止。**

工程内部依次处理构建、运行时、平台接线和实际游戏缺口；这些是同一任务的施工顺序，允许小提交和定期进度，不在“9符号清零”“首个HLE”“首次present”后交回一个新的规划。实际游戏在此组合上的兼容性尚待验证，不预先声明必然可玩；遇到可修复错误，定位后在本轮继续修复。

Swan / Android16 / ARM64 / 4KiB 是目标。AYN Thor/API33/4KiB 是当前可用辅助设备，先完成它的同一路径。Swan 缺席时可交付准确的 `AYN_VERIFIED / SWAN_NOT_RUN`，不能合并两台设备的验收。16KiB适配、系统Vulkan驱动、VR/Beat Saber、finite Step、广泛游戏兼容不进入本轮。

## 当前可复用基础与固定决策

当前已有388个正式AArch64 host对象，含AAudio；`shadps4_host`链接仍exit1、九个前端符号未解决，无host共享库。此次配置为静态STL、Foundation OFF、没有FEX。当前APK仍是原SessionCore＋FEX计数循环。不能从“能编译”推导guest ABI/异常/窗口语义已迁移。

| 项目 | 本轮决定 |
|---|---|
| 最终库 | 沿用 `libshadps4_fex_session.so` 作为唯一生产session/host/FEX JNI库；`libbachata_pkg.so`保留导入职责。当前 `shadps4_host` 仅作过渡链接诊断，最终不并存两份host/FEX/global registry |
| profile | NDK29.0.14206865、arm64-v8a、native API33、`c++_shared`；target/compileSdk36独立记录。主仓host需要C++23，清理Gradle无条件`-std=c++20`对host的覆盖；依赖和FEX自身标准按target隔离 |
| FEX | 固定owned pin `385a0cc4`，通过既有独立构建和`cmake/fex`接入；核对实际prebuilt的API/STL/options，不无条件复用旧API35缓存；生产`V0_BUILD_TESTS=OFF` |
| 第三方 | 复用已完成的date/glslang/Zydis/miniz/LibreSSL配置、独立FFmpeg `e17ba6e2`、hwinfo `85bbcba3`、adrenotools `60ae5bbc`＋linkernsbypass `aa397589`；不再次逐库调研或迁入Foundation |
| Foundation | 在正式链接中恢复既有DebugBus/Android dumpsys范围；复用其诊断设施。反射/网络按真实需要审计后启用，不扩大为本轮另一个框架项目 |
| 平台 | Kotlin Service/SessionCore掌管会话，ANativeWindow原生呈现，Android controller输入，AAudio普通输出、OpenAL保留Audio3D用途；Turnip为唯一默认Vulkan路径 |
| 内容 | TMNT CUSA50828，gd/01.00本体＋gp/01.08更新。身份和本机路径见[pkg-set.json](../validation/android-native-host/2026-09-12-review/pkg-set.json)；外机通过显式本地路径提供同hash内容 |

保留现有SessionCore的generation、RAII control lease、late-drain所有权、统一Destroy和Service长期观察修复。保持G2事务/poison/sink/pin/译码退休合同。历史COMMON、FFmpeg、hwinfo已修项不再作为待实施工作。

## 实施主体：生产库、session与guest边界一起接通

### 统一构建和真正的 Android frontend

将现有root CMake成果整理为共用host对象target和一份依赖target，再接到最终JNI库。根CORE/COMMON/VIDEO_CORE/SHADER_RECOMPILER/IMGUI等现有源列表继续作为唯一来源，必要时机械抽取为共用cmake模块，并同步census读取方式。不要在app复制一套源文件或手列archive。

选择一个canonical生产构建入口供Gradle和命令行共用。推荐在根`BUILD_HOST_CORE`模式建立完整JNI target，并让Gradle调用同一入口；若保留app作为CMake入口，则先把根配置抽成可被子目录复用的模块，修正`CMAKE_SOURCE_DIR`/生成资源路径，不能把当前根项目直接嵌入后靠绝对路径补丁维持。单独的FEX配置仍隔离，最后只链接一次。

同时交付一个仓内可重复执行的构建脚本（建议`scripts/android/build-host-android`，当前尚不存在）：

- 从显式NDK/profile、固定gitlinks和忽略的输出目录构建；支持从新目录一次得到JNI库及可供Gradle打包的产物，命令失败保留真实非零退出。
- 从同一gitlink的ImGui/protobuf准备**构建机**font embed和protoc，检查可执行身份/版本，与目标libprotobuf匹配。工具路径和内容身份参与generated outputs依赖，替换工具会重新生成；不要求预先存在任何`/tmp/host-*`文件。
- 强制正式profile一致，核对C++异常/RTTI、PIC和STL；隔离Homebrew/desktop pkg-config。检查FEX与主仓fmt/xxhash等实际符号提供者，正常处理冲突和ODR，不打开全局多重定义容忍。
- 最终JNI库显式`--no-undefined`、Build ID、link map；完整DT_NEEDED树可以在APK namespace解析。UND中正常Android动态库导入不算缺失。link map证明实际生产loader/HLE/renderer/media和FEX都在可达链路内。
- 保留正式host、guest和JNI符号，不把构建输出、NDK、游戏、驱动包写入Git；生成可检出源码/pins与产物hash manifest。

**九个符号通过职责迁移解决。** 从desktop `emulator.cpp`迁移配置、挂载、metadata、HLE注册、模块加载/执行等实质逻辑到`src/core/host_runtime/`的生产backend；systemservice退出/重启/重载转换为带generation的session请求。旧desktop入口仍使用其adapter。生产路径不得借用`quick_exit`、fork、进程级静态`exit_done`或另一个SDL主循环。

`g_window`/WindowSDL关联点一起改：为Presenter/Instance/GNM提供真实window信息和生命周期接口，desktop和Android各有adapter。Android尺寸/Surface来自当前generation，不创建假WindowSDL对象或重解释内存布局。desktop big-picture/icon/file-dialog/keyboard路径按产品边界禁用或迁移；仍启用的功能必须有实际实现。

SDL仅保留生产实际依赖。当前静态SDL会强制拉入`JNI_OnLoad`并注册`org/libsdl/app/*`；现app未包含这些类。最终库必须明确JNI初始化归属和SDL子系统使用范围，避免缺类、重复JNI_OnLoad或未setup的Android SDL调用。允许保留通用SDL函数和必要ImGui renderer，不要求全面重写SDL，但现Kotlin app不改成SDLActivity，也不运行desktop big-picture窗口。

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
| `signals.cpp` / `exception.cpp` | 明确host fault、JIT fault、guest exception和异步控制的分类及状态来源，消除本轮未初始化/空Sync问题，并接通受支持guest handler的调用与恢复 |

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

**输入和音频。** 将现Android controller snapshot交给真实pad HLE，验证断连/焦点丢失释放键；gameplay实际响应。AAudio补短写、gain、格式/通道、open失败不发布假成功、断连/停止；Audio3D仍走有效OpenAL输出链，媒体保留FFmpeg真实解码/seek/EOF/退出。以真实guest PCM和实际听感验收，静音/音量/重启有效。

**bionic与ART。** 最终实际链接rpmalloc/provider为准核对SetupHooks和VA所有权，在普通UID、ART/图形并存时验证冷启动、首次失败后重试、同进程重建和Java churn。preflight的可重试性不代表SetupHooks fatal可恢复；固定版本public接口若真正限制方案，保存具体调用/布局证据并提出最小接口需求，同时完成独立工作。遵守子仓适用规则，不能暗改FEX或切到未审计旧runtime。

**真实APK装载。** 默认游戏Launch注入生产backend；smoke模式必须可区分且不会被游戏失败触发。检查最终安装splits、JNI/依赖/libc++、四hooks、Build IDs与构建结果一致；service保持非导出和普通app UID。JNI_OnLoad/资源生成/driver namespace等必须实测，shell库加载不能替代。

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
| 三项本轮发现 | guest上下文无未初始化读、无空Sync成功；生产共享STL及完整依赖配置；epoll边界值/负值/EINTR，正的亚毫秒值不变0 |
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
