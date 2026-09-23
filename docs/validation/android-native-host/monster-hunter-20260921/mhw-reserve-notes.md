# MHW ReserveVirtualRange / 非固定提示

本地 11.00 固件和 live RSP 数据保留于本任务 analysis / JSON。PID18075/gen1：Format 输入 `/temp0`、返回0。main+0x4a31082 调用 ReserveVirtualRange，地址提示0xdad0000000、size0x6400000、flags0x80（NoOverwrite，无Fixed）、alignment0x200000；返回0x8002000c。随后 main+0x4a2ac49 向空分配指针写入，匹配 PID16932 的 FEX fault。

[FreeBSD mmap 官方手册](https://man.freebsd.org/cgi/man.cgi?query=mmap&sektion=2)说明非固定 addr 是提示；Fixed 才要求原址。这是通用 VM 兼容依据，不是已取得 Sony ReserveVirtualRange 的完整规范。Guest helper 使用实际返回地址，未要求保留原提示。

实现仅对 Guest backend 的非固定搜索回绕一次，遍历已保留且空闲的 VMA，保持长度、对齐、Fixed 拒绝和宿主空洞保护。未扩张288GiB guest envelope、未修改 FEX 或其512GiB策略。
