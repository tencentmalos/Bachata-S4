# MHW / MHR：分段文件缓冲与系统媒体服务续修

2026-09-22，承接[临时区与内存续修](monster-hunter-followup-20260922.md)。设备 AYN Thor `9c2841a4`，分支 `codex/tetris-runtime-fix`，保留既有 Sports/Tetris 修改；本轮未 commit/push。两款 ZAR、版本和 DLC 内容未改动。

用户随后明确：**直播服务不需要支持，目标是游戏正常运行。** 本文媒体服务改动仅提供游戏可处理的不可用结果，后续不扩展真实直播 provider。完整导入清单用于分析实际阻塞；不以补全可选服务或清零未支持项为目标，优先验证启动、画面、输入、关卡和存读档。

## MHW：Oodle 错误来自索引表读取被错误拒绝

此前观察到的 `compressor not compatible with sliding window!` 不是首个错误。Guest RSP、短时读取探针和真实修复后的读入结果形成以下证据链：

1. `/app0/chunk/chunk0-ps4.bin` 的前 8 字节正确读入，魔数 `CMP\0`，块数 `0x529d`。
2. MHW 随后调用 `sceKernelPread(fd=3, dst=0x29d0030080, size=0x294e8, offset=8)`，要求读取 169,192 字节压缩块索引表。旧 HLE 要求完整缓冲落在单个 Guest 映射中，因此拒绝合法的相邻映射缓冲，返回 `0xffffffff8002000e`（EFAULT）。文件层没有执行这次读取。
3. 游戏未处理失败，继续使用未填充的索引表，产生 6、160、极大值及大量 0 偏移。Oodle 收到的 510 字节输入竟以 CMP 文件头开始，而非应解码的压缩块；错误 compressor 状态触发错误日志，日志格式化随后触及 Guest 栈保护页。这是继发崩溃。
4. 只修正文件字节缓冲的映射准入后，真实 V7 PID11917 成功读入完整索引；首个有效数据读取为 offset215815 / size9077，前 8 字节 `8c0600236f80117f`。后续块偏移正常，越过旧 Oodle 故障，推进至 `sceNetGetMacAddress` 的具名未支持错误。

成功 RSP 会话 PID6530 / startTicks81948786 在 main Pread PLT `0x5633430` 捕获参数，返回地址 `main+0x4a176f0` 捕获 EFAULT；先前 PID26183 / startTicks81828358 在 `main+0xdfbd0` 捕获 Oodle 输入。运行时 main bias 为 `0x400000`。工具的旧 Guest RSP 未提供完整 Build ID/program identity，证据按 PID、启动时间、模块地址和部署库绑定，不宣称拥有该协议没有提供的身份字段。Linux `/proc/maps` 可合并相邻映射，不能用合并后的 VMA 数量替代 Guest 元数据；本轮未抓取该缓冲每个 Guest 内部切分边界。

没有修改 Oodle、扩大栈、修改 FEX、重新打包或更换 ZAR。具体文件链见证据目录中的 `mhw-oodle-input-rsp.json.gz`、`mhw-pread-index-rsp.json.gz`、`mhw-v6-read.log.gz`、`mhw-v7-read.log.gz`。日志含多次启动，修复前后须按 PID 区分。

## 实现与适用范围

`GuestAddressSpace::DataRequest` 增加默认关闭的 `allow_adjacent_mappings`。文件 Read/Write/Pread/Pwrite/Preadv/Pwritev 的数据缓冲显式启用：同一元数据锁下逐段检查连续覆盖、权限及调用方提供的 mapping generation；完整批次检查后才发布 pin。空洞、只读输出尾段、失效 generation 均拒绝，准入失败不先写第一段。整段 pin 保留到真实 I/O 完成，期间不能退休或重映射其中任何部分。

默认标量/ABI 结构记录保持单映射合同；iovec 元数据和 AIO 没有在本轮全面改为跨映射，不能宣称所有 Guest 指针接口均已处理。Read 的输出仅要求写权限，Write 的输入要求读权限。

扩展回归另发现已有 `WriteData` 在 MemoryObserver 回调前释放 pin。现保留 backing 至回调结束，异常展开也释放；回调始终在元数据锁外。没有 observer 时保留原有一次加锁释放的快路径。此项独立于上述 MHW 根因，不能把它归因为原 Oodle 故障。

## 系统媒体服务：完整导入子族的不可用合同

桌面这些接口多为无参/零返回 stub，不能直接准入宿主函数。本轮核对本地 11.00 固件的普通导出、初始化与冷路径：

- **GameLiveStreaming：21 个导出。** Initialize 的 scalar workspace size 必须为 `0x4000`，错误为 `0x80a00002`。Android 无 ShellCore/IPMI 广播服务，采用固件服务 ENOENT 映射 `0x80a00007`，失败不建立 client。其余 20 个已核对导出在 cold state 返回 `0x80a00004`，初始化检查先于 pointer access，因此保留输出和所有 caller-owned 数据，不建立伪广播、回调或消息队列。
- **SharePlay 4 项 / Remoteplay 5 项。** Initialize 签名 `(void*, size_t)`；非空 workspace 小于 `0x1800` 返回族内 INVALID_ARGUMENT（base|1），空 workspace 表示系统分配。有效 scalar 输入在 Android 明确返回 Orbis ENOSYS `0x8002004e`；这是 HLE 的无 provider 边界，**不是声称从固件观察到的 IPC 缺失错误值**。未初始化其他已准入方法先检查状态，分别返回 `0x810e0004` / `0x80fc0004`，不接触输出。没有实际 client，因而不做固件成功初始化路径的 mspace 分配、workspace 写入或网络连接。
- 准入精确匹配 NID、库、版本和导出类型后缀。SharePlay 的 `+MCXJlWdi+s` 是 `GetCurrentConnectionInfoA`，不将它误标为其他 getter。未知导出继续具名拒绝。

状态分别记录为 `android_bridge_broadcast_no_provider` / `android_bridge_remote_service_no_provider`，表示已接通可用性错误合同，不代表直播/远程游玩功能可用。MHR V5 越过直播后停 SharePlay；V6、V8 已越过三族，转至 SystemGesture。完整导入仍按族保留，VideoRecording、ScreenShot 和其余在线接口不因本轮实现而宣称完成或必定不阻塞。

固件原件及完整反汇编仅留在本机 build 目录，不入仓。保存提取身份和短导出审核：`livestream-analysis.json`、`livestream-cold-path-audit.json`、`shareplay-analysis.json`、`remoteplay-analysis.json`。源 SHA 分别为 `d02e49dd6b03325b8da0e9ea822b07daf1d1b91a75d7a5106eca8711934ff9e5`、`194378a232a14299c12b0e3b3cefa18d85820af07a3b40461adc921364577f84`、`c9362dd36dc443ee7b3c322d07725fda232811147975fcd0dc795ef2767cbc31`。

## 验证与反例

最终 V9 AYN：文件 **737/0**、映射 **169/0**、媒体服务 **623/0**、网络 **299/0**、HMD reprojection **90/0**，共 **1,918 checks / 0 failures**。各测试 executable、host 和 libc++ SHA 与部署记录一同保留。文件检查覆盖真实目录和 ZAR 缓冲、偏移及游标、权限/空洞/旧 generation、退休排斥；映射检查包含 callback 期间保护和异常清理。

正确旧库负对照使用稳定外部 DispatchStorage/GuestStorage 边界的独立 probe，不跨 DSO 传递新 DataRequest：V6 读入返回 EFAULT / 8 字节全零；V8 同输入返回 8 / `ABCDEFGH`。分别见 `buffer-v6-negative` 和 `buffer-v8-fixed` 日志及 SHA manifest。

保留以下失败且不混算：

- 首次把新文件测试 exe 配旧 V6 host 得到 737/25。DataRequest 布局已改变，内联路径跨 DSO 存在 ABI 不匹配，该整套负对照无效，不能把 25 项当作本修复的真实缺陷数；随后用上述稳定边界独立 probe 重做。
- 映射 V8 首次 runner 未切到可写目录，`mkstemp` 失败 exit2；修正 cwd 后 167/1 揭示真实的旧 observer pin 提前释放问题。V9 增加两个生命周期检查后 169/0。
- 早期 RSP 断在 Fios PLT 未命中实际 MHW 自有 Pread 路径；一次未进入游戏即连接导致 remote EOF。两者不是成功捕获，相关现场/清理仍保留。

临时 StorageReadProbe 已从生产源码完全移除；最终 V9 APK 无此读取日志探针。未进行 GPU 性能或全游戏回归。

## 最终包、实景与剩余工作

最终 V9 APK `36d17b18` / host `b810606c` / session JNI `ae1a430e`，安装后完整 APK SHA 与本机一致，host SHA 与上述五组测试一致。[安装身份](monster-hunter-20260921/apk-v9.json)、[源码身份](monster-hunter-20260921/source-v9.json)、[归档 manifest](monster-hunter-20260921/media-evidence-manifest.json)。最终 APK 冷启动结果：

| 游戏 | PID / generation | 实际新边界 |
| --- | --- | --- |
| MHW CUSA09554 | 19927 / 1 | 越过原 Oodle 故障；`sceNetGetMacAddress` op89 具名拒绝，Failed/Faulted，尚无 Guest flip 或菜单 |
| MHR CUSA34119 | 21755 / 1 | 越过直播/SharePlay/Remoteplay；`sceSystemGestureInitializePrimitiveTouchRecognizer` op637 具名拒绝，Failed/Faulted，尚无 Guest flip 或菜单 |

两款仍未验收可玩。完整绑定分别 **999 行 / 219 refused 行 / 217 唯一**，**1201 / 333 / 330**；[按库统计](monster-hunter-20260921/import-v9-summary.json)、[完整剩余功能族](monster-hunter-20260921/refused-v9-families.json)，及两个最终 run 的完整 `imports.json.gz` 均已保存。不能把桥接数量当作可用 API 完成率。

下一批需要按以下整族推进：

1. MHW 网络设备身份/套接字查询：当前未接入 `sceNetGetMacAddress` 和 `sceNetGetSockInfo`，需对齐本地网络控制域、真实设备/离线可用性、6 字节 MAC 输出及 guest 检查。桌面 GetMacAddress 的硬件查询与错误返回需要核对，不能搬零 MAC 或伪成功。
2. MHR SystemGesture：9 个实际导入包含 Initialize/FinalizePrimitiveTouchRecognizer、Open/Close、CreateTouchRecognizer、两类 Update 和两类 GetEvents。固件 Initialize 是无参的真实初始化，包含 SDK 查询和 mutex 状态；桌面仍为 stub。应核对结构、句柄、事件队列、输入与清理整链，或评估在既有加载图/TLS 冻结前使用用户提供的 Guest 模块；本轮没有开启通用固件自动加载。
3. 其余离线/本地依赖仍按完整清单推进：Json2、录屏/截图、NP 离线/对话框、SaveDataMemory2 等。当前失败点不能证明其他未接入族不会阻塞。

## 存档及终态

本轮没有调用存档格式化，也没有更改此前仅清理 session-owned `/temp0` 的约束。有效 V4 后 checkpoint 与最终 V9 对比：**37 个文件，0 新增、0 删除、37 个 SHA 全相同**。该结论仅覆盖本轮有效 checkpoint；不能替代尚未进入菜单的两款怪猎保存/退出/读档验收。[最终 SHA](monster-hunter-20260921/savedata-after-v9-sha.txt)。

两次最终运行都是 Failed/Faulted 后确认 `session:none`，再关闭故障 UI 并重开 Library，**不是正常游戏 UI Stop 成功**。终态 Library PID23165 / TracerPid0，无游戏服务、ADB forward 或 MCP debug session；Guest RSP port/wait 和读取探针属性均为 0。本轮自有断点/session 已清理，没有遗留输入或录屏。全局设置及 host/config 与原基线逐字节相同，保留 Turnip / Render0.5 / TextureHigh / SBS / gyro / 默认 MSAA。[终态核查](monster-hunter-20260921/cleanup-v9-final.json)、[Library 截图](monster-hunter-20260921/library-v9-final.png)。无性能、全部游戏回归或存读档完成声明；未 commit/push。
