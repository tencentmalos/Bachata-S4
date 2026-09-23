# MHW / MHR：临时区、FSST 和 Guest 内存续修

2026-09-22，承接[完整 ZAR 与 V2 启动验证](monster-hunter-20260921.md)。分支 `codex/tetris-runtime-fix`，保留原有 Sports/Tetris 修改，未 commit/push。两款 ZAR、版本及 DLC 内容未改动。

## 本次实现

- **AppContent 临时区**：补 TemporaryDataMount / Mount2 / Format / Unmount / GetAvailableSpace 的完整本地生命周期。Format / Unmount 首参数为 16 字节 mount-point，11.00 固件 IPC 分别复制到 0x2000d / 0x2000c 请求；桌面的无参零返回 stub 不能作为 ABI。合法 mount 仅为本会话拥有的 `/temp0`。Format 保留挂载、清空内容；不操作 `/savedata*`、游戏资产或外部目录。打开文件和已关闭但仍被 positioned I/O / mmap 持有的 lease 会返回 Busy。根目录与挂载身份不匹配、非法/未终止 guest 参数、未初始化、未挂载、I/O 错误和 Stop 均有失败结果。目录内 symlink 仅删除链接本身，不跟随目标。原有会话退出清理仍有效。
- **FSST 无提供者合同**：共用桌面/Android `sceKernelSetFsstParam(s32,u64)`，返回 Orbis ENOTSUP，不再因未绑定导入终止整个会话；审计状态为 `android_bridge_fsst_no_provider`。这不等于实现 PS4 内核服务调度。11.00 libkernel 实际构造 16 字节 mask/priority 记录，写 `kern.fsst_param`；MHR RSP 观测 `(255,0x40)`，main+0x3fae24a 调用后不使用返回值，继续 FIOS 初始化。没有改当前 Guest/native 线程的优先级或亲和性。
- **栈查询**：真实 HostRuntime 自建 Guest 栈使用 `VMAType::Stack`；`sceKernelIsStack` 输出通过同批 guest pin 写回，允许任一输出为空，非法输出不造成部分写回。共用 MemoryManager 对未映射地址返回 EACCES，替换会因 Guest 地址而触发的宿主断言。普通映射输出 0/0；查询依据真实 VMA，不按名称猜测，也未把任意 caller-provided 堆映射强标为栈。
- **非固定地址提示**：Guest backend 的空闲搜索先尝试提示地址之后，再回绕一次搜索真正拥有的空闲 VMA；以真实 VMA 范围为准，不沿用旧 AddressSpace 120 GiB 上界。保留长度/对齐，不能占用 Android/ART 空洞或覆盖已用映射；Fixed 请求继续按原址检查。未扩张 288 GiB Guest envelope，未修改 FEX 或其 512 GiB 策略。采用非固定地址为提示的通用 VM 语义，参考 [FreeBSD 官方 mmap 文档](https://man.freebsd.org/cgi/man.cgi?query=mmap&sektion=2)；这不是已取得 Sony ReserveVirtualRange 的完整 SDK 规范。

这里的 Format 是游戏临时缓存区清理，**不是格式化设备，也不是清除存档**。存档 API 的新建、载入、写入仍应有真实持久化语义，不能统一假成功。

## 实测根因和推进结果

MHW V3 PID16932 在 `main+0x4a2ac49` 对空分配结果执行 `vmovups`。独立 RSP PID18075/gen1 先确认 Format 输入 `/temp0`、返回 0；随后在 main+0x4a31082 观察 ReserveVirtualRange：hint `0xdad0000000`（875.25 GiB），size `0x6400000`（100 MiB），flags `0x80`（NoOverwrite，无 Fixed），alignment `0x200000`；返回 `0x8002000c` ENOMEM。Guest 随后未判空就使用结果。这与临时区及存档格式无关。[参数、返回、断点清理](monster-hunter-20260921/mhw-temp-reserve-rsp.json)。

最终 V4 APK `9a887952` / host `390f4019` / session JNI `67d9dad3`，设备安装 APK 完整 SHA 与本机匹配；[完整身份](monster-hunter-20260921/apk-v4.json)。最终 APK 的两款结果：

| 游戏 | 实际推进 | 当前边界 |
| --- | --- | --- |
| MHW CUSA09554，PID24330/gen1 | 临时区成功，原失败预留及后续大量 direct mapping 完成 | Guest-2 在 `main+0x12030b` 的日志格式化调用向栈保护页写入，进程 SIGSEGV；尚无菜单/场景 |
| MHR CUSA34119，PID25050/gen1 | FSST 返回明确不支持后继续，栈查询通过 | `sceGameLiveStreamingInitialize` op96 具名拒绝，Failed/Faulted；尚无菜单/场景 |

MHW 同一 V4 的 RSP PID26415/gen1 抓到保护页错误之前的真实日志：`OODLE ERROR : compressor not compatible with sliding window!`，源码字符串 `v:\devel\projects\oodle2\core\oodlelzcompressors.cpp`、line966，日志函数 `main+0x120280`，返回到 decoder `main+0xe0137`。该线程栈相邻 `/proc/maps`：`0x1c00224000–0x1c00228000` 无权限、`0x1c00228000–0x1c00234000` rw；旧 crash 的 fault `0x1c002273c8` 落在前者。日志先报解压问题，日志格式化再越过栈下界；**未证明 Oodle 错误的源头，未归因于 ZAR 损坏、FEX 或某个 ABI**。输入数据、解码窗口/调用参数及其建立过程仍需下一轮追踪；未扩大栈或屏蔽错误来掩盖问题。[原始寄存器/字符串/decoder state/清理](monster-hunter-20260921/mhw-oodle-rsp.json)。

## 验证与反例

- AYN 最终 V4 文件/存储测试 **659 checks / 0 failures**：生命周期、非法结构/路径、Busy、关闭后的 I/O lease、清空后的可用空间、legacy Mount、重复卸载、Stop、真实存档文件和外部目录/游戏资产保留。见 [日志](monster-hunter-20260921/temp-lifecycle-v4-tests.log)、[测试/host SHA](monster-hunter-20260921/temp-lifecycle-v4-tests.json)。
- AYN 实际 MemoryManager / GuestAddressSpace 测试 **143 checks / 0 failures**：真实栈/普通/未映射地址、可选输出及原子拒绝、超高 hint、尾部空间不足、完全不足、对齐、固定地址拒绝、分段 Guest 空洞，以及既有映射退休/内存池测试。见 [日志](monster-hunter-20260921/memory-v4-final-tests.log)、[测试/host SHA](monster-hunter-20260921/memory-v4-final-tests.json)。
- 首轮 memory 测试 136 项 / 6 失败：暴露了旧 120 GiB 常量不适合真实分段/测试地址布局的问题；使用真实 VMA 边界后通过。原反例 [日志](monster-hunter-20260921/memory-v4-tests.log)保留，不冒充“全部首轮通过”。
- 两次编译问题（VMA helper 名、头文件依赖）已修正并重新构建；V4 APK 构建通过。V2 的 7×3 生产 libc 测试属于此前提交点，本轮不冒充重跑或全回归。
- FSST 第一次 RSP session 默认端序错误地显示为 big；使用 x86 的 rawHex 按 little-endian 解释并与静态调用指令交叉核验，未使用错误 numericHex。一次 60 秒 stop lease 自动恢复后重新命中同一 owned breakpoint，事实保留；后续 session 显式 little-endian、300 秒租约。[原证据](monster-hunter-20260921/fsst-rsp.json)。

完整导入仍保留：[统计](monster-hunter-20260921/import-v4-summary.json)及各 run `imports.json.gz`。MHW 999 行 / 249 refused 行 / 247 唯一；MHR 1201 / 344 / 341。绑定成功、API 返回、初始化继续和真正画面验收分别记录，**两款仍未验收可玩**。

## 剩余功能族与后续定位

直播/远程游玩/录屏/截图族已整体盘点：MHR 导入 3/4/7/3 个，MHW 21/5/7/4 个。桌面 GameLiveStreaming 大部分为空参数 stub；不能直接准入这些宿主函数。新核对的本地 11.00 固件明确 `Initialize(size)` 必须 size=0x4000，错误 0x80a00002；重复初始化 03、未初始化 04、内存不足 06；EnableLiveStreaming 首参数按 bool 发送 IPC，并非无参。此轮仅完成合同核对，[来源及 SHA](monster-hunter-20260921/livestream-analysis.json)，没有加入假直播成功。下一批需做同一会话的离线生命周期、状态输出/许可、无广播服务错误与清理，并核对 Remoteplay / VideoRecording / ScreenShot 相关指针和类型；不宣称未接入项都不会阻塞。

SaveDataMemory2 / 同步 / transferring-mount、AppContent DLC 卸载/下载及其他已拒绝族也保留在全量清单，不把本次临时区支持等同整个 SaveData 或 AppContent 已完整。Oodle 输入/窗口链是 MHW 当前优先定位项。

## 同包回归、存档核查与终态

同一最终 V4 APK 的 TMNT（AYN 既有散文件安装，界面版本 1.11.0）PID28503/gen1 到中文主菜单；停止前快照 16,512 host presents，正常界面 Stop 后 `Stopped / user_stop`，Guest return `18446744073709551615`（非 0）。仅主菜单和停止回归，不宣称关卡、性能或其他游戏全回归。[运行快照](monster-hunter-20260921/runs/tmnt-v4-pre-stop/status.txt)、[主菜单截图](monster-hunter-20260921/runs/tmnt-v4-start/screen.png)、[停止结果](monster-hunter-20260921/runs/tmnt-v4-stop/status.txt)。

最早 `savedata-before-v3.txt` 因 shell 引号错误没有生成有效清单，原失败证据保留，不能据它声称整个 V3 前后存档不变。有效 checkpoint 是 V3 后的 37 个文件 SHA；V4 两款怪猎测试及 TMNT 回归之后仍为 37 个，无新增/删除，35 个 SHA 相同，`CUSA50828/main/User.ini.json` 及其 `sce_backup` 副本 SHA 变化。未保存对应旧内容，不做字段级变化归因；不把该结果写成“所有存档逐字节未变”。临时 Format 不操作存档的边界由实现、命名空间身份检查及定向测试独立验证。两款怪猎尚未进入实际保存、退出、读档，SaveDataMemory2 等缺口仍需验收。[最终存档 SHA](monster-hunter-20260921/savedata-after-v4-sha.txt)。

确认 TMNT session:none 后重开 Library：PID32300、TracerPid0、无游戏服务和 ADB forward，Guest RSP port/wait 均为 0。所有本轮 owned breakpoint/session 已撤销释放；未遗留自动输入或本轮录屏。全局设置与 host/config 对比原基线逐字节相同，保留 Turnip / Render0.5 / TextureHigh / SBS / gyro / 默认 MSAA。[终态与比较结果](monster-hunter-20260921/cleanup-v4-final.json)、[Library 实景](monster-hunter-20260921/library-v4-final.png)、[源码 SHA 与基线](monster-hunter-20260921/source-v4.json)。未 commit/push。
