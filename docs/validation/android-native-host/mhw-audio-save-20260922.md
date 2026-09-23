# MHW：音频、存档、网络初始化与空对象崩溃

2026-09-22，AYN Thor `9c2841a4`，CUSA09554，现有已核验 ZAR；本地未提交；V10 真机崩溃已取证，设备断开后继续离线修复。

用户要求集中推进 MHW，直到真正进入游戏。以下是已取得证据的阶段记录，不代表关卡可玩或全部兼容完成。工作目录 `build/validation/mhw-audio3d-20260922/`，原始游戏/固件、分析 ELF、APK 和测试存档备份仅留本地，不入 Git。

## 实现

- Audio3d 复用桌面 PCM 混音器与 Android 真实 AudioOut/Oboe，增加会话端口/对象/队列寿命、整批属性和 PCM 快照、阻塞/非阻塞输出及取消。支持单声道/立体声/6/8声道折叠、gain/reset、左右 passthrough；late reverb 仅 dry=0。17 个入口覆盖 MHW 的13个实际导入，未实现 HRTF/位置音效、非零混响及关联 AudioOut 入口。
- Gnm 编码输出、WaitFlipDone 输出、DCB/CCB 提交读取按整批 mapping pin 允许连续相邻映射，保留洞、权限、generation 与退休保护；不将所有 ABI 结构的验证全局放宽。
- SaveData Setup/Get/SetMemory2、SyncMemory、TransferringMount 使用会话 Store 和完整嵌套指针快照。Set 的数据范围整批检查后发布，输出跨文件操作保留映射身份，发布时重新准入。TransferringMount 仍限本用户/本游戏只读挂载，跨游戏访问未实现。
- 存档内存 Sync 在读取前先加载已有文件，避免空缓存覆盖已有数据。Android 会话以临时文件、完整写入、fsync、原子 rename、目录 fsync 持久化；rename 前失败恢复旧缓存，rename 后错误保持缓存与已发布文件一致。参数和 icon 未实现多文件原子事务。Sync 实际同步持久化并备份，完成后才发布 event3，未伪造异步进度。
- Setup option 位按用户固件11.00核对：1参数区、2双缓冲、4放宽大小；FW>=5.50 wrapper 自动加4。单槽上限8/32MiB，双缓冲减半，累计按双份计入32MiB，icon预留上限0x1c800。存取序列化、整批 staging 和发布保证旧/新数据快照，不声称异步双缓冲性能。已有较大文件不会因较小请求截断。
- 目录搜索输出数组支持跨相邻映射。MHW 三次搜索错误实际来自 CUSA02102/CUSA01200/CUSA01222 的跨游戏限制，不能宣称数组修正消除了它们。
- `sceKernelIsProspero` 已有实现，但 Android 漏掉桌面注册的 `libkernel_cpumode_platform` 别名。V10 仅为该 NID、库版本1及 libkernel 模块加准确准入，复用原 PS4 平台查询。

## 证据与反例

| 检查 | 已通过版本 / 结果 |
|---|---|
| Audio3d 混音、输出及取消 | V4 574 / 0 |
| Graphics command admission | V4 83 / 0 |
| AudioOut 定向回归 | V4 123 / 0 |
| SaveData 服务及持久化 | V9 327 / 0 |

V2 Audio3d 567项/513失败源于测试夹具跨两段映射 WriteData 被拒绝，后续改逐段写入、由生产输入路径验证跨段读取。Save V5/V6 编译失败后误跑旧可执行文件的172项结果不算新增测试；V7/V8实际重建292项通过，V9扩展327项通过。构建脚本后续使用 `set -e`。以上是定向测试，不是完整游戏回归。

完整导入、测试摘要和来源 SHA 见 [evidence manifest](mhw-audio-save-20260922/manifest.json)。

固件来源为本机 `firmware/11.00_sys_modules/libSceSaveData.sprx`。wrapper +0x2bc30 → helper +0x23b80；+0x23bfe 添加 bit4；+0x5ae0 验证 flags/大小/总预留/icon/reserved。分析 ELF 保持 PT_LOAD；转换 SHA 和反汇编保存在工作目录。Reverse Study 首次未形成函数，改用 LLVM 静态反汇编核对；未把反编译失败当作不存在实现。

## 实机进展

| 版本 / 会话 | 已观察结果 |
|---|---|
| V3 PID20258 | 越过 Audio3dInitialize，在 GnmInsertWaitFlipDone op479 因命令缓冲跨映射拒绝，0 flips |
| V4 PID25183 | 跨映射修正后1281 presents，后因 SetupSaveDataMemory2 op398 未接入终止 |
| V8 PID3916 | option3 被旧限制拒绝，最终出现“保存资料受破损”提示；未接受初始化覆盖，正常 UI Stop/user_stop/return0 |
| V9 PID8528 | 8MiB option3（有效7）Setup/Get2 均0；实际进入新建存档、语言、隐私与声音设置。后半设置由用户操作，不记为自动验证 |
| V9 同会话 10:58:51.021 | 因 IsProspero 的 cpumode_platform 别名拒绝退出，Failed/Faulted/op33；全999导入、199拒绝行/197唯一。这个长列表不是197项均已调用失败 |
| V10 PID27992 | 别名变为 runtime_bound/android_bridge；出现 NP SignedOut 错误弹窗，用户继续操作后 Guest-1 / VA0x8 崩溃；没有关卡验收 |

V9 APK `48e7556fe86b8be13c4a9a98245996c0872666eef2f8f23403d7e57205703fe8`；V10 APK `2ac026619018ce42fd933911de3732c7bc0252821b427aac5f1c336ca79f8b7b`，host `850ff84930b8ba6573988fd2c140ca8e793322c34bd4d2d755e8cd01c98eb94f`，JNI `ab66f8968a2ad1314629372c28011b309887c7ced9bdf7c96994417215611cfe`。安装后完整 base.apk SHA 与本地相同，host 与构建输出一致。

截至 V9 创建测试存档后，开工前37个其他存档文件 SHA 全部不变。MHW 新增6个文件（memory.dat、参数、icon及对应备份），已单独备份；未做存档格式化。全局设置未修改，仍为 Turnip/0.5/High/SBS/gyro。V10 崩溃后用户说明设备暂时不能连接；尚不能复核断线后的存档、当前进程和界面。没有安装 V11/V12，也没有关卡成功结论。

## V10 崩溃证据和因果边界

MHW 原始导出 ELF SHA `e066f746f5e98ac1c217cd5d5e89941edad562230c82ce456b66f45a7bdef36e`，保留 PT_LOAD 的分析 ELF SHA `cd4e3620be7182f1d35d7a67626c3d543bd481257bf764271663e054a8c2ec9b`；下面 main 偏移仅针对该模块。

会话 PID27992 / Guest tid29093 / generation1 / run UUID `fcf10f95b52a9d1e16ce0d1f63f7c7b5`，安装身份为上述 V10。2026-09-22 11:34:05.035，SIGSEGV / SEGV_MAPERR / VA `0x8`。此前弹窗内容为 `0X80550006`（NP SIGNED_OUT），不是本次终止的 Unsupported import。用户继续操作后的实际 tombstone、logcat、运行快照均留本地工作目录，不用历史其他 PID 的崩溃代替。

- native PC `0x595650f400` 位于 JIT block `0x595650f3e0`，元数据 guest base `0x1800014200` 指向随应用发布的 guest mutex 实现。故障指令从 x11=8 读取；没有修改 FEX 或吞掉非法 mutex。
- 对照本次 MHW 主模块（runtime base `0x400000`）：`main+0x4a521b0` 读取 `main+0x7c047d8` 的对象指针，`main+0x4a521d6` 形成对象+8，`main+0x4a521e3` 调用 mutex lock。保存栈中的返回地址 `0x4e521e8` 与此调用一致。对象为空使锁地址成为8。异步 CPUState 的旧 PC 仅作辅助，未当作精确 fault RIP。
- 静态分析中该对象的已观察写入是 `main+0x4a37160`，位于初始化函数 `main+0x4a35d00`。其上游链为 `main+0x3dcd600 → main+0x3e0e280 → main+0x4e1d070 → main+0x4a35d00`。动态/间接路径仍可能超出静态覆盖。
- 初始化顺序包含 NetCtl callback、NP title、`sceNetPoolCreate("http_net",65536,0)`、SSL/HTTP、内容限制、StateCallbackA、Reachability callback、Json2 构造，随后创建并发布对象。多个负返回分支提前退出；不能把静态次序当作 V10 已逐项执行的日志。

固件 `libSceNet` 11.00 SHA `43f8e3411acdcf5fe21f51aa770586a05e76471c9fc8200dd4422017e110a8ae`：DT_INIT `+0x0` 遍历构造表，表 `+0x3c0d8` 的 RELATIVE 项指向 `+0x47f0`，后者进入 `sceNetInit` 共用实现 `+0x7b0`（公开导出 `+0x7a0`）。`PoolCreate +0x1b90` 检查初始化字段 `+0x3c100`，冷态返回 `0x804101c8`。旧 Android 会话发布 libSceNet 时没有执行这一步，属于已确认的生命周期缺口。

旧 V3 日志出现 NP title 设置后没有后续 HttpInit / 内容限制记录，与网络池提前失败相符；**V10 的 PoolCreate 原始返回值尚未捕获，不能宣称已证明唯一根因或已修复这次崩溃。** 不能据此归因 ZAR、Turnip 或 FEX。

## V11 / V12 离线改动

1. Error/Message 模态窗口按显示尺寸放大，顶部标题和可滚动说明，底部固定横向大按钮。保留 CommonDialog 租约、请求 generation、按键释放后确认和模态恢复。Error 标题独立，未改成实际 IME 文本输入；用户所说的 IME 样式落在现有错误/消息渲染器上。
2. 网络 provider 发布前调用本地 Initialize，复用显式 sceNetInit 的状态转移。Term 后重新变冷、活跃资源拒绝 Term、Stop 后禁止复活；不签入 PSN，不伪造网络请求成功。
3. 补 StateCallbackA 注册/注销及 Reachability 注册/注销四入口。沿用桌面 NP 事件队列和离线 SignedOut/Unavailable；8个 StateA trampoline 保留独立 guest 函数/参数及原生 id，注销和复用以 revision 过滤旧事件，Reachability 仍由桌面按状态变化产生。经现有 InvokeGuest 执行三参数回调，legacy 保持四参数；guest 地址不传给宿主作 native 函数。新增容量、重复注册、跨会话注销拒绝、旧事件失效、边沿去重、析构清理用例。Android 可执行文件编译不等于 callback 真机运行通过。
4. Json2 复用已有 SystemGesture 的按导入发现、TLS/重定位前依赖图、游戏 provider 优先和身份校验。文件名/库名 `libSceJson2`，导出 module 实际是 `libSceJson`，不可按同名假设验证。用户本机11.00模块 SHA `6a9d3894497cdfbe8720a5720cc0ab06f4aefbc73dff6e5c86885770b58097c5`，127784B，222导出、25导入，MHW需要的41项静态全部匹配；依赖 libkernel / LibcInternal。固件不打入 APK、不入 Git。尚未复制到 AYN，真实绑定、构造和后续路径待验。
5. 增加默认关闭的 `debug.shadps4.network_log`（非 Android 为 `SHADPS4_NETWORK_LOG`）。每入口最多16对输入/输出，覆盖已安装的 sceNet/sceNp/sceHttp/sceSsl/sceErrorDialog 适配器，记录 context、PID、sequence、六个整数参数和真实返回值/adapter状态。只记录数值，不解引用 title secret、账户、URL或请求体。日志输出到 host `NETWORK_TRACE` 和 Android `GuestNetworkTrace`；上限之外仍执行原调用。拒绝导入继续由现有错误链记录。

## 断线后的验证与待办

V11 在断线前完成 AYN Message189/0、Commerce178/0，身份及日志见同名 evidence 文件；弹窗新布局尚未安装到实际游戏观察。早期 V11 因 ImGui FontSize API 编译失败，改为 GetFontSize 后构建成功，失败日志保留。

macOS 辅助 harness 使用实际 GuestAddressSpace、GuestNetwork、GuestSockets、net_util、原 tests；NativeToPosixErrno 和 FillNativeSockInfo 及其助手从生产源逐字抽取，日志 sink 置空，链接仓库 spdlog/fmt。结果380项/1失败，新增初始化12项均无失败。唯一失败为既有 Listen SockInfo 的 state：地址和端口正确，状态为 CLOSED；macOS 独立原生 `getsockopt(SO_ACCEPTCONN)` 返回 ENOPROTOOPT（errno42）。保留反例，不为获得全绿修改测试或声称 Android 网络已回归；未修 macOS 查询能力。harness 初期缺 fmt 路径/链接依赖的构建失败也保留，不作为运行结果。

NP 回调离线验证为55项/0失败：运行生产 GuestNpControl 和逐字抽取的桌面注册/注销/分发代码，只用注入 SignedOut 事件替代 UserService/在线事件源，任何不相关 NP 调用直接 abort。覆盖此次四入口的容量、身份、注销和回调事件生命周期；不等于整套 NP、真实 UserService→FEX 回调或 Android 实测。harness 初次读取旧源注释编码失败，修正读取方式后编译运行，未改生产源编码。

V12 最终 Android host、Network/NP 测试可执行文件和 `assembleFdroidDebug` 构建成功。APK SHA `cd4502718ffe7fad4281ae4c9f31705727e6b4a699a91889b1bc8b10755b8dc5`；host SHA `473482c3e6058cd088e85d88fa7906cff114d3e4723cd02f7ed1621f694c49a6`；JNI SHA `ec798b8de8d3869e946823b2daab6a5a5814e56d52ec2cb28e8a4f784f908572`。包内 host 与最终构建输出逐字节 SHA 一致，版本副本保存于本地 `build/validation/mhw-audio3d-20260922/shadps4-mhw-v12.apk`。**未安装、未执行新版 Android Network/NP 测试、未验证实际 Json2 DT_INIT 或 MHW 重启**。

设备恢复后按以下次序继续，当前没有后台等待或重复连接操作：

- 先记录设备/安装包/当前会话；如有运行会话，遵守既有安装保护。核对存档 checkpoint，保留新建的 MHW 存档，不接受格式化覆盖提示。
- 部署并逐字节核对用户已有 `libSceJson2.sprx` 至 app-private `files/host/sys_modules/`，保持 LibcInternal 及其它配置；用 DebugBus 全量绑定确认41项实际 guest_export，不能用静态匹配替代。
- 运行新包配套 Network/NP 定向测试，再安装并核 APK/host/JNI SHA。启动前置 `debug.shadps4.network_log=1`，按目标 PID 保存 `GuestNetworkTrace`，记录 Pool/SSL/HTTP/回调与 Json2 init 的真实结果，结束后恢复原属性。
- 验证先前空对象现在是否完成构造；若仍为空，沿第一处真实负返回追踪。对象发布后紧邻 WebAPI、Party、PresenceA、Matching2 仍有未支持接口；对在线服务保持诚实的离线失败，不能跳过上游对象建立或统一成功。清单沿用V10的999项/198拒绝行/196唯一拒绝快照，V12运行后重新盘点。
- 继续确认主菜单、进入地图、画面和输入，以及存读档；目前只到首次设置和错误弹窗，不声称 MHW 可以正常进入游戏。
