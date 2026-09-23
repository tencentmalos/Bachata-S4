# MHW / MHR：网络查询、SystemGesture 与内存类型续修

日期：2026-09-22。设备：AYN Thor `9c2841a4`，Turnip；本地未提交。

本轮延续用户“游戏正常运行优先、直播服务不需要支持”的范围。两款仍未到菜单或关卡，不能宣称可玩。保留之前 ZAR、临时区及分段 Pread 修复；没有重打包游戏、修改 FEX、格式化存档或提交推送。

证据目录：[manifest](monster-hunter-network-gesture-20260922/manifest.json)。完整本地工作目录为 `build/validation/monster-hunter-network-gesture-20260922/`；里面的分析 ELF、固件和 APK 不加入 Git。下文 V1/V2 是本轮包序号，前轮 V9 仍见 [媒体与读取报告](monster-hunter-media-20260922.md)。

## 本轮实现

### 网络查询

- `sceNetGetMacAddress` 复用桌面 NetUtil 的真实网卡查询，返回精确 6 字节。初始化状态、flags、空指针、权限及取消检查均保留；网卡查询失败返回 ENODEV，保持输出不变，不生成假 MAC。同时修复 NetUtil 查询失败时的 socket 泄漏。
- `sceNetGetSockInfo` 复用桌面的 160 字节记录填充逻辑：真实 IPv4 本地/对端地址、端口、缓冲区、队列长度、TCP 状态、socket 名称；NONBLOCK 取 Guest 设置，不泄露宿主内部强制非阻塞状态。
- 会话只枚举自己的 socket；快照持有稳定 socket lease，释放描述符表锁后查询宿主。输出在查询前记录映射身份、发布时再整批准入；不跨宿主查询持有 Guest 输出 pin。关闭/复用 fd 不会让旧 lease 指向新 socket。
- 固件 `GetSockInfo +0x45d0` 的 `flags & 0x31000` 直接返回 EINVAL 且不改 errno；其它非零特殊枚举 flags 明确 EOPNOTSUPP。此记录限 IPv4，IPv6 用另一接口，未假装覆盖。桌面未实现的 pid/带宽等统计仍为原有零字段，不称完整 TCP 统计。

来源：用户本机 firmware/11.00 的 `libSceNet.sprx`；转换产物及完整反汇编在本轮工作目录 `net-analysis.{elf,json}`、`net-full.asm`。MAC 导出 `+0x37f0`、帮助函数 `+0x53b0` 显示初始化检查、flags/null 检查及成功后 4+2 字节复制。宿主权限不足是可用性限制；实际 MHW 已继续到后续接口，但本轮没有捕获其 MAC 返回字节，不声称游戏拿到了硬件地址。

### SystemGesture 的真实 Guest 模块

桌面允许加载用户提供的系统 SystemGesture。Android 现在也按实际导入，在冻结 TLS/重定位前发现该模块，补入依赖及 DT_INIT 顺序：

- 仅 `libSceSystemGesture`、库版本 1 的窄白名单；不是自动加载所有固件。
- 已有游戏/provider 优先；多个 provider 拒绝；全局文件同时验证 export library 和 module 身份。
- 继续走生产 Module/Linker/FEX，解析实际依赖；不使用 host dlopen、不复制 SDK 算法、不把手势函数改成零返回。
- 模块缺失时保持具名拒绝，不假报成功。生命周期及 sysmodule 状态复用已有路径。

部署的用户文件：

| 项目 | 内容 |
|---|---|
| 来源 | `/Users/bytedance/game/ps4/firmware/11.00_sys_modules/libSceSystemGesture.sprx` |
| 设备位置 | `com.shadps4.android/files/host/sys_modules/libSceSystemGesture.sprx` |
| SHA-256 | `98425066e5c44346fc849931f62e369cad70e358e89ccadfa75eca2c5b1425f8` |
| 安装检查 | 新文件、来源/设备 SHA 相同、传输临时文件已清理；固件不提交 Git |

补齐模块依赖的 `sceKernelGetCompiledSdkVersion`：读取当前会话的游戏 SDK 元数据，验证并 pin 4 字节输出，空指针 EINVAL、非法地址 EFAULT，成功才写回。

MHR 实际导入的 Close、CreateTouchRecognizer、FinalizePrimitiveTouchRecognizer、GetPrimitiveTouchEvents、GetTouchEvents、InitializePrimitiveTouchRecognizer、Open、UpdatePrimitiveTouchRecognizer、UpdateTouchRecognizer **9 项全部为 guest_export**。实测日志有模块 DT_INIT 完成、Sysmodule `0xce` 加载成功，并越过原 op637。定向测试验收的是初始化/重复初始化/Open/Close/Finalize/错误及 SDK 查询；未覆盖完整触摸事件算法或实体手柄交互。

### 内存类型与 GPI

- `sceKernelMtypeprotect` 桥接桌面真实 MemoryManager；仍按桌面 16 KiB 页面取整，新增加法/取整溢出及 owned range 检查。非法地址不再触发 MemoryManager 断言。
- 内存访问权限与 Direct/Pooled 物理类型元数据在同一个 mapping writer/metadata 临界区发布，避免原来 Protect 与 SetDirectMemoryType 之间的重映射窗口。I/O 引用先退休、后取 metadata 锁；GPU invalidate 在这些锁外。类型 0–10 的范围沿用当前分配合同，未声称模拟所有硬件 cache 属性。
- `sceKernelGetGPI` 按 64 位返回 ABI 连接共享实现。固件 11.00 `libkernel +0x216a0` 查询硬件信息失败时清零返回；本机无 devkit GPI provider，采用该不可用回退。未顺手准入参数不同的 SetGPO/GetGPO。

## 定向验证及反例

最终 V2 对应 host 库的 Android 真机结果：

| 测试 | 结果 | 覆盖 |
|---|---:|---|
| GuestNetwork | 368 / 0 fail | MAC 提供/不可用、输出保护、初始化/取消、真实 SockInfo、logical NONBLOCK、flags/errno、枚举/截短/关闭 |
| POSIX socket | 326 / 0 fail | 原 TCP/UDP、IPv6、WAITALL、epoll/select、取消等定向回归 |
| MemoryManager | 168 / 0 fail | typed protect 前后权限/物理类型、非法范围、不阻塞无关查询、退休、既有池/高地址场景 |
| 生产 FEX 模块测试 | 4 组 × 3 轮通过 | 缺模块拒绝、错误身份拒绝、固件+真实游戏 libc 生命周期、游戏 provider 优先于错误全局文件 |

模块测试还执行 SDK 空/非法/合法输出与 GPI 返回检查。脚本和 SHA 见证据目录，合成入口源码为 `tests/guest_cpu/fixtures/runtime_system_gesture.S`。

失败试验保留，没有更改生产语义去迎合夹具：

1. 首次没有 libc provider，因 `Need_sceLibcInternal` 对象无法解析而拒绝。
2. 使用真实游戏 libc，但旧合成 ProcParam 只有 `0x20`，使其 `_malloc_init` 失败并调用 DebugRaiseException；这是夹具不完整，不是本轮真实 MHR 的失败。
3. 改试系统 LibcInternal 又遇到它额外的 `_sigintr` 对象依赖，未为此扩展无关 signal 模型。
4. 按真实 libc 的参数检查修正合成入口：ProcParam `0x40`、重定位指向 `0x40` 的默认 libc 参数块。真实游戏 libc 与 SystemGesture 随即通过所有正向/反向组。没有忽略 DebugRaiseException 或替换算法。

两次无效构建目标名称、第一次把 SELF 当 ELF 的分析失败仅留工作日志，不作为测试结论。最终 native/APK 构建通过，`git diff --check` 通过；没有进行全游戏回归。

## 最终包与实测边界

最终 V2 APK：`8f44fcc7a0b9fb59073ff30a796d0b354e2e27e2328ec20d2d60dde7ef53fc7c`。

- host：`ce0c7ff83c1859096a0b7f60d73efd78647e93c40b242cffa5665e6a45433965`
- JNI：`55e9bbccda2e2f8a82a26d80bef95e556ba5bc089aeea18d8299bd26bb727a40`
- 安装后读取设备 base.apk 完整 SHA 对比相同，测试记录里的 host SHA 与 APK 一致。

| 游戏/轮次 | 实际结果 |
|---|---|
| MHW V1 PID7513/gen1 | 越过 NetGetMacAddress；拒绝 Mtypeprotect op67，Failed/Faulted |
| MHR V1 PID8981/gen1 | 真实 SystemGesture DT_INIT/加载成功；拒绝 GetGPI op567，Failed/Faulted，detail return0 不是成功 |
| MHW V2 PID14187/gen1 | 越过 Mtypeprotect；Guest-42 调用 Audio3dInitialize op632 被具名拒绝，Failed/Faulted |
| MHR V2 PID15222 | 继续加载后 Guest-27 非法访存，进程 SIGSEGV；没有菜单验收 |
| MHR V2 + RSP PID16981 | 同包再现 Guest-26 非法访存；RSP 在启动暂停后恢复，崩溃时 EOF，没有取得 crash-stop 寄存器快照 |
| MHR V2 同步快路径关闭 PID19593 | 绑定清单 guest_fastpath 从19变0，仍 Guest-26 非法访存；不能只归因于快路径，也不能据此排除所有同步问题 |

MHR 三轮 tombstone 从 DropBox 按精确 PID 过滤保存。直接访问 `/data/tombstones` 失败；没有把报错文本当 tombstone。前两轮代码对应关系由 tombstone 的 JIT 指令/内嵌地址与当前游戏 ELF 交叉核对：

- PID15222：native `ldaprb [x4+0x19]`，`x4=0x10000000235`，fault `0x1000000024e`；与 `main+0x3ef588d` 的 `cmp byte [rax+0x19],0` 匹配，前一指令从树节点 `+0x10` 取指针。
- PID16981：native `ldaddal` 对 `x4+8`，`x4=0xbf30171b9ffbf98f`；与 `main+0x4a5145a` 的 `lock inc dword [rax+8]` 匹配，是对象引用计数路径。
- 这说明读取/引用计数之前指针已异常；并未取得第一个错误 writer。原始类型和分配对象尚未恢复，不能直接归因于 FEX、Turnip、手势模块、ZAR 或某个锁。

RSP 的协议没有完整 program/build 身份，提供了精确 host PID/start ticks 和模块基址；本次误省略 byteOrder 后元数据默认为 big，但没有读取或采用寄存器解释值。后续须显式 little。会话关闭/forward 回收均成功，证据为 `mhr-rsp.json.gz`。

## 剩余族与下一批

完整清单：[remaining-families.json](monster-hunter-network-gesture-20260922/remaining-families.json)。最终 MHW 999 导入、216 refused 行/214 唯一；MHR 因真实手势模块增加依赖变为1215导入、321 refused 行/317唯一。不能只看 UI 截断输出，也不以全部变为 bound 作为可玩验收。

1. **MHR 首个错误 writer**：优先追树节点/对象引用指针的创建、发布和释放及输入缓冲。两个崩溃路径都属于 Guest 数据结构，需在有效节点阶段下断点/观察写入者；不能用扩大栈、忽略非法读写或强制返回掩盖。
2. **Audio3d 整族**：MHW13、MHR16 个导入，合并17项。桌面已有实际 PCM 入队/混音/AudioOut 路由，不适合照搬 Initialize 的零返回。需要提取共享混音逻辑、会话状态/端口寿命、嵌套 Guest PCM 与属性结构快照、可取消的 Advance/Push/Flush、AudioOut 关联句柄及统一 Stop/Terminate。MHR 的 AudioOutOpen 有第七参数，须明确 Guest 栈 ABI。验证必须含混音内容及队列/取消，不能仅确认初始化成功。
3. Json2、其它对话框、部分网络/在线族等仍在清单内；尚未证明均不阻塞。直播服务仍不建设，已可处理的不可用语义保持。

## 终态与数据保留

Library PID21001，`session:none`、TracerPid0，无服务/forward。Guest debug port/wait=0，read_probe=0，sync_fastpath 恢复空值；本轮没有录屏或 GPU capture。全局设置和 host config 与开工前逐字节一致，仍保留用户 Turnip/0.5/High/SBS/gyro 配置。

37 个已有存档文件，无新增/删除、全部 SHA 相同。SystemGesture 用户模块按上表留在设备供后续使用；两款 ZAR 未改动。存档字节未变不等于怪猎实际存读档已验收。
