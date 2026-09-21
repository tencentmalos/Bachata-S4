# AYN：共享 socket 语义与 Tetris 验证（2026-09-21）

本轮实现常规 TCP/UDP 的真实会话通信，并审计桌面对应代码。分支 `feature/malos/beat_saber_fix`，设备 **AYN Thor `9c2841a4`**，保留已有修改，不做 commit/push。Tetris 的本地 socket 通道已建立，启动越过此前长期等待，随后在 **sceSystemServiceGetDisplaySafeAreaInfo** 的具名拒绝处结束；仍未出帧、未进入菜单。

## 实现

- [GuestSockets](../../../src/core/host_runtime/guest_sockets.cpp) 为 POSIX 和 sceNet 共用同一套实际对象、fd、非阻塞标志及超时。文件与 socket 使用 [GuestDescriptorIds](../../../src/core/host_runtime/guest_descriptor_ids.h) 的同一编号域，分配最小空闲 fd；关闭移除可见描述符，在途操作保留旧对象。guest fd 从不直接作为 native fd。标准 read/write/close 经 runtime 转发到 socket；文件 I/O 原有稳定 lease 保留。
- IPv4/IPv6 地址显式转换，包括 Orbis 的 len/family 字节、IPv6 family=28、port/flow/scope。地址输出按 caller capacity 截断并返回实际长度，不能越界写固定 sockaddr。sendmsg/recvmsg 深拷贝 Orbis 48 字节头和最多 1024 个 iovec，显式填 native msghdr，合并/分散有效 payload 并转换截断标志。
- native socket 始终 NONBLOCK/CLOEXEC，Guest fcntl 与 SO_NBIO 管理 Guest 可见状态。实现 bind/listen/connect/accept、peer/local 地址、shutdown/EOF、send/recv/sendto/recvfrom、消息收发、常用 socket/TCP/IP 选项。非阻塞 connect 返回映射后的 EINPROGRESS，SO_ERROR 返回 Orbis errno。BSD timeval 与 Orbis 整数微秒超时分别转换；send/receive/accept/connect timeout 独立。
- select 的 1024 位 fd_set 查询真实 socket 就绪及已有文件就绪，支持三组位、超时和空集合无限等待；超时清零输出，不改输入 timeval。sceNet epoll 是会话内就绪注册，不与桌面全局表混用；保存 guest ident 和 64 位 userData，支持 ADD/MOD/DEL、abort/destroy、变更唤醒、实际 IN/OUT/ERR/HUP。当前只支持 socket 注册，不伪造 resolver async 事件。
- 等待使用 native poll 和每个 waiter 独立的 wake pipe；会话 Stop、owner stop_token、close 和 socket/epoll abort 能唤醒所有相关 waiter。等待不持 Guest pin、VM gate 或对象 metadata mutex；只在非阻塞 syscall/元数据处理期间短持 socket mutex。Guest 输出在操作前记录映射身份，等待后整批重新准入，再写回；原址 remap 返回 EFAULT，不把旧结果写入新对象。
- close 同时移除 epoll 注册，避免空闲注册项延迟 native close/对端 EOF；active wait 的临时 lease 由取消路径释放。accept 的输出发布失败或异常会回收新 fd。
- [NativeToPosixErrno](../../../src/core/libraries/kernel/kernel.cpp) 增补网络错误映射。POSIX 与 sceNet errno TLS 各自独立，前者 -1，后者 Net 错误域；会话 Stop 保留 sceNet ECANCELED，POSIX 中断返回 EINTR。最多记录 64 条创建/建连日志，不逐包打印 payload。

离线配置下允许 loopback 通信；外部 connect/sendto 明确 ENETDOWN，入站地址同样检查。允许 wildcard bind，但并不赋予外网通信许可；PSN/HTTP/登录策略没有改变。SocketAbort 当前仅 flags=0 的整对象等待取消，方向 flags 合同未完成。普通文件 fcntl 仍明确 ENOSYS；socket fstat/dup/ioctl、AF_UNIX/socketpair、raw/P2P、ancillary/SCM_RIGHTS、正 linger、PEEK+WAITALL 组合和非默认 receive low-water mark 等未完成项保留错误，不声明完整 POSIX/Orbis 网络栈。传输暂有每次 64MiB、最多 1024 个同时打开的 socket 上限。

## 定向检查

最终 AYN **1133 checks / 0 failures**（socket/POSIX/Random 326、network control 299、file/ZAR I/O 508）。最终日志、被测 Host SHA 见 [tests-final.txt](tetris-sockets-20260921/tests-final.txt)。覆盖真实 IPv4/IPv6 TCP 双端、UDP 数据报、混合 POSIX/sceNet、地址截断、非阻塞 connect、TCP_NODELAY、PEEK、WAITALL、多 iovec、MSG_TRUNC、fd_set 的 63/64/1023 边界、select/epoll 发送唤醒、超时、Stop、owner 取消、abort、close、fd 复用、EOF、epoll 空闲注册关闭、Guest 输出 remap 与无效指针保护。实际文件/ZAR I/O 同版回归 508 项通过。没有桌面运行测试、全游戏回归、TSan 或性能收益声明。

开发中反例保留：第一版输出被截短后没有同步裁剪 mapping identity，合法 getsockname 返回 EFAULT；修复后再测试。首次 remap 测试误对单一 64KiB 映射做局部 Unmap，改为独立映射后验证实际身份变化。首次 fd 复用断言忽略了更小的空闲 listener fd，补齐占位后验证旧等待不转向新 socket。旧控制测试的 session Stop errno 两项失败已通过 sceNet/POSIX 分层转换修复。各阶段日志归档，不把测试夹具失败写成游戏故障。

## Tetris 实测

使用原 CUSA13427 ZAR、无 guest patch、无 debugger、无 RenderDoc。完整绑定仍为 2603 行：1146 guest_export、1018 runtime_bound、422 refused、17 not_relocated，333 个唯一拒绝项。见 [绑定表](tetris-sockets-20260921/imports.json)及[按库清单](tetris-sockets-20260921/audit.json)。这些拒绝项未因本轮 socket 成功而标成已完成。

V1 PID32719/gen1、run `6ca1a8b7662dea0f77f67d724eac278c`，日志记录实际 socket fd7 → bind0，fd8 → connect0，accept 返回 fd9，后续多组同类通道也成功。V2 PID5487/gen1、run `4c13aa84a3b53850f247d8e32fd31f73` 同样越过旧边界。最终 V3 PID7667/gen1、run `4db1303821b0c015dd0510cb36523267` 再次得到相同建连成功与后续拒绝结果（[日志](tetris-sockets-20260921/host-v3.txt)、[状态](tetris-sockets-20260921/status-v3.txt)、[画面](tetris-sockets-20260921/tetris-v3.png)）。EOS 模块初始化返回 0；之后主线程触发 `1n37q1Bvc5Y#libSceSystemService` 的 Unsupported import，op419，stage Failed/Faulted、guest return419。没有把该 return 当作正常退出或原生 SIGSEGV。对照此前精确 EOS 的 loopback factory 断点与本次真实创建/连接/accept，已确认该通道能力接通；未做这次调用链的完整 RSP unwind。

Commerce 0xa8 仍返回 `0x805a10ff`，不能声称 Toolkit 整体初始化成功。SystemService 当前拒绝族还有 LaunchWebBrowser 与 LoadExec；后续须一并核对显示安全区/显示信息流程与离线应用控制合同，不采用“遇到一个入口就返回 0”。Commerce 仍按此前共享状态、深拷贝参数、取消结果和 ImGui 模态完整族处理。

## 桌面结论

**桌面实现不完备。** 已有真实 socket 基础，但有 Linux msghdr 直接强转、IPv6 family/地址布局、小缓冲输出、accepted socket_type 未初始化、shutdown/abort stub、select 空集合/超时及 epoll 生命周期等明确缺口。[独立审计报告](desktop-sockets-audit-20260921.md)按源码列出证据、影响平台和改良顺序；Android 新实现不等于这些桌面旧路径已经修复。

最终 APK/Host/JNI 与设备全包核对见 [build identity](tetris-sockets-20260921/build-identity.json)；最终实测、清理和配置比对见 [交付状态](tetris-sockets-20260921/delivery.json)。所有 APK、游戏二进制、完整运行产物留在本机 build，仓库只归档有界证据。

最终安装 APK `5bb981ff` / Host `7db2bf5a` / JNI `0cc8d22e`。游戏已经自行 Faulted，确认 session:none 后才重开应用恢复 Library（PID8989，TracerPid0）；未把本轮状态写成正常 UIStop。配置逐字节未变，Turnip/0.5/High/SBS/gyro 保留，调试属性关闭、无 adb forward。
