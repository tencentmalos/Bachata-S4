# 1a7da92c host 复核证据

对应[复核报告](../pkg-v2-host-closure-review-2026-09-12.md)。主仓源码 `1a7da92c514f11b9923dd13f6887a1733a076be5`；本轮仅添加审核资料，不改生产源码。绝对路径是当时观察值，不能作为另一机器的构建前置。

- [summary.json](summary.json)：源身份、配置、对象数量和失败链接的全部唯一符号。
- [object-build-dry-run.log](object-build-dry-run.log)：首次 dry-run 要求重新生成 CMake，因此随后实际构建；不据它声称缓存已是最新。
- [object-build.log](object-build.log)：已有目录重新生成并增量构建 `shadps4` 的完整输出与 exit 0；不是 clean checkout 证明。`scm_rev.o` 包含审核期间的 dirty 元数据。
- [objects.json](objects.json)：刷新后388个主仓OBJECT产物的ELF头和SHA256；[objects-before-refresh.json](objects-before-refresh.json)保留第一次观察。
- [link-command.txt](link-command.txt)、[link.log](link.log)：来自实际 Ninja 图的链接命令，仅增加 `--error-limit=0`，刷新后 exit 1、9个唯一未定义符号、没有 host DSO。[link-before-refresh.log](link-before-refresh.log)是刷新前同样失败的记录。
- [ucontext_probe.cpp](ucontext_probe.cpp)：链接实际 `exception.cpp.o` 的构造/Sync负例，无复制实现、无真实信号。预填对象存储后检查RIP的字节表示，不求值未初始化整数。
- [ucontext-command.txt](ucontext-command.txt)、[ucontext-build.log](ucontext-build.log)、[ucontext-device.log](ucontext-device.log)、[ucontext-identity.json](ucontext-identity.json)：实际编译、AYN设备输出、object/binary hash和设备环境。**probe exit 0 表示成功复现缺陷，绝不表示功能通过。** 静态STL仅用于这一CLI反例，不能当生产APK配置。

设备 `9c2841a4`，API33/4096-byte pages，adb shell UID。没有ART/普通APK、真实guest callback/signal、Turnip、游戏或Swan结果。源码修复后应将相应负例转为“正确构造/拒绝无效状态/真实安全点恢复”的正式测试，不能继续以复现bug的exit 0计为PASS。
