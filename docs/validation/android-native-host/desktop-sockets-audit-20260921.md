# 桌面 socket 完整性审计（2026-09-21）

结论：**已有真实通信后端，但不完备，不能直接作为 Android/Orbis 语义正确性的基准。** 本报告审计当前工作树，不代表桌面实机回归。源码 SHA 记录于 [清单](tetris-sockets-20260921/desktop-source-identity.json)。本轮对桌面共享代码只补充 `NativeToPosixErrno` 的网络错误映射；下列旧桌面缺口没有被 Android 新实现自动修复。

## 可复用的基础

`sys_net.cpp` 的 POSIX 与 `net.cpp` 的 sceNet 包装共用桌面 HandleTable/Socket，确实使用系统 socket、bind/connect/listen/accept 和 send/recv。sceNet 将失败转换为 Net 错误域，POSIX 返回 -1。文件系统 read/write/close 与 socket 描述符在同一桌面表中。这是正确的总体分层方向。

Android 已有会话独立文件表、GuestAddressSpace 和 Stop 生命周期，无法直接把 guest 指针及 descriptor 交给桌面全局 HandleTable。此次新增会话 socket 对象同时供 POSIX/sceNet 使用，与文件共用编号分配，避免另一张独立编号表和宿主 fd 泄漏。

## 已确认缺口

| 优先级 | 源码 / 位置入口 | 当前行为与后果 |
|---|---|---|
| 高 | `posix_sockets.cpp` / SendMessage、ReceiveMessage 的非 Windows 分支 | 将 OrbisNetMsghdr 直接 reinterpret_cast 为 native msghdr。Orbis 结构 48 字节、iovlen/controllen 为 32 位；Android/Linux LP64 msghdr 为 56 字节，controllen 为 64 位、flags 在 +48。会读错长度、越过 guest 结构写 flags；msg_name 也没有转换。Darwin 的布局不同，不能把 Linux 结论无条件扩展到所有平台。 |
| 高 | `sys_net.cpp` / sys_socketex；`posix_sockets.cpp` / sockaddr 转换、Bind、Connect | socketex 将 Orbis family 直接传 native，IPv6 的 28 在 Linux 不等于 AF_INET6。地址转换仅按 IPv4，bind/connect 固定 sockaddr_in 大小，未按 guest length 检查。IPv6 不是完整实现。 |
| 高 | `posix_sockets.cpp` / Accept、GetSocketAddress、GetPeerName、ReceivePacket | 输出转换固定写 16 字节，没有完整尊重调用者长度；sockaddr sa_len 没设置。小输出缓冲存在越界风险。recvfrom 零长度 UDP 返回 0 时不转换来源地址；getsockname/getpeername 也有不必要的输出缓冲输入读取。 |
| 高 | `sockets.h` / PosixSocket(net_socket)、重复 socket_type 成员 | accepted 构造只初始化基类 type=0，没有初始化派生类同名 `socket_type`；后续 flags/WAITALL/linger 判断读取未初始化值。 |
| 高 | `sys_net.cpp` / sys_shutdown、sys_netabort | shutdown 直接返回 -1；netabort stub 返回 -1，均没有正确设置当前调用的 errno，可能暴露旧 errno。 |
| 高 | `net.cpp` / sceNetEpollAbort；`net_epoll.h/.cpp` / EpollTable | Abort stub 返回成功而不唤醒。GetEpoll 返回 vector 元素裸指针，锁外扩容/销毁存在失效风险，负 id 未单独检查。Destroy/Wait/Control 对 events 和 async_resolutions 也缺少完整同步与稳定 owner。若现行调用走文件表中的 shared epoll，不能据此断言它必然经过旧 EpollTable 的裸指针路径；此处按实现分别记录。 |
| 中 | `kernel/file_system.cpp` / posix_select | 非 Windows 分支 max_fd==-1 直接返回 0：空集合的超时/无限等待不正确。只有 ret>0 才清空输出，超时会保留输入位；直接传 Guest timeval 给 native，Linux 会修改它。缺少 nfds 与 native FD_SETSIZE 的完整边界检查。Windows 分支也在无 socket 时直接返回；混合已就绪文件时仍可能阻塞等待 socket。 |
| 中 | `posix_sockets.cpp` / SendMessage、SendPacket、Connect、Accept | 持 m_mutex 执行阻塞 syscall；Close/setsockopt 等可被阻塞。receive_mutex 虽单独，但仍无会话 Stop/线程取消协议，不能把 Android 生命周期套在这些调用之外就算完成。 |
| 中 | `posix_sockets.cpp` / SO_ACCEPTTIMEO、CONNECTTIMEO、REUSEPORT 等 | 部分选项仅缓存或直接返回成功，未落实对应内核/等待语义；未知选项会 UNREACHABLE。Linux EOPNOTSUPP 没纳入该文件自己的转换分支，可能变成 EINTERNAL。 |
| 中 | `sys_net.cpp` / sys_socketpair；`net.cpp` / sceNetTerm | socketpair native 失败缺少完整 errno 转换；Term 仍是成功 stub，不代表对象清理完成。 |

此外，P2P、resolver、NetCtl/在线策略是各自功能族；本次没有给这些整库作“完备”结论。源码发现不等于已在某款游戏复现崩溃，也不等于所有平台都有相同 ABI。

## 推进建议

1. 将桌面 `msghdr` 与 sockaddr 输入/输出改为显式转换并测长度截断、IPv6 和 accepted type；先消除越界与错误 ABI。
2. 提取稳定 socket owner、native errno/flag/option 转换与可取消 readiness 后端供桌面/Android 共用。Guest 指针检查仍由各自适配层承担，不能共用 raw pointer 接口。
3. 用实际双端 socket 检查 shutdown/EOF、非阻塞 connect/SO_ERROR、select 空集合与超时、epoll abort/destroy/close 并发和 fd 复用，再替换现有 stub/缓存成功分支。

本轮 Android 实现及真机证据见 [socket 验证报告](tetris-sockets-20260921.md)。其常规 TCP/UDP 语义已经测试，不代表本表中的旧桌面实现一起修复。

ABI 对照来源：[FreeBSD 9 socket 定义](https://raw.githubusercontent.com/freebsd/freebsd-src/releng/9.0/sys/sys/socket.h)、[Linux recvmsg 手册](https://man7.org/linux/man-pages/man2/recvmsg.2.html)，并核对本仓 Orbis 类型与实际 Android NDK 头文件。Orbis 扩展选项以本仓定义和已有使用合同为准，不把通用 FreeBSD 常量直接当作所有 Orbis 扩展的证据。
