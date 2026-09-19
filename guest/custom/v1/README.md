# Guest custom SDK v1

跨游戏共用的 freestanding x86-64 SysV C/C++ SDK。入口为
[`include/shad_guest.h`](include/shad_guest.h)，版本号和服务 ABI 由生产
`Core::GuestPatch::Manager` 校验并绑定。

- `runtime.c`：在 FEX 内执行的基本内存函数。
- `payload.ld`：RX/RW 分段、内部指针重定位与静态 ELF 布局。
- host 服务只接受 clock/counter/带声明 tag 的标量日志，无 host 指针或任意符号查找。
- `shad_sdk_log(tag, value)` 只允许 recipe `logs` 中声明的 tag；宿主日志使用固定
  `[PATCH_LOG]` 前缀并附带 package、context、thread、generation、invocation，写入日志目录
  下独立的 `guest-patch.log` sink，便于把 guest patch 的生命周期和 ABI 观测与普通 HLE 日志
  分开；主 `shadps4.log` 与控制台不会收到这些记录。
- 游戏地址、原型、采样频率归 `guest/games`，不得放入此 SDK。

构建、部署、trampoline、运行时切换和限制见
[完整使用指南](../../../docs/guest-function-patches.md)。
