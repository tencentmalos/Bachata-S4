# MHR 离线崩溃复核与读写锁诊断（2026-09-22）

用户确认 AYN 暂时无法连接，本轮仅做离线分析、针对性测试和构建；未访问设备、安装 APK、启动游戏或修改存档。保留当前分支所有 MHW 等已有修改，不改 FEX 子仓，不提交或推送。**尚未找到首次错误写入，不能宣称 MHR 崩溃修复或可玩。**

## 证据基线

复核上一批 `monster-hunter-network-gesture-20260922` 的三份独立 tombstone，而非将不同 PID 拼成同一次执行。MHR 为 CUSA34119，原始 eboot ELF SHA256 `607073d8c3a9019e39cf396e4d72fd7ae67e5cbe80d14e621f93d836faee6fd6`；分析 ELF 保持段地址，SHA256 `37f764009917f151df72a6b46fe8ed050fc9c305a39913e09102c8402fdc1e03`。以下 `main+` 是该 ELF 相对位置，旧会话实际主模块基址为 `0x400000`。旧 APK 为 `8f44fcc7…`，不是本轮构建版本，也不是 MHW V12。

| 旧会话 | 经 JIT / ELF 对照的 guest 故障点 | 本轮新增结论 |
| --- | --- | --- |
| PID15222 / Guest-27 | main+0x3ef588d：树节点右孩子的标志字节读取 | 节点 `0x2355fb000` 的 parent 字段已是未对齐的 `0x23560e1f3`。按转储原字节从该地址+16读取，确实得到 `0x10000000235`，再+25正好为 fault address `0x1000000024e`。 |
| PID16981 / Guest-26，RSP 轮 | main+0x4a5145a：`this+0x20` 所指对象的引用计数递增 | 静态构造入口 main+0x4a51210 先建立 vtable、引用计数并复制描述符；崩溃时 RBX 指向的地址头部和+0x20均不符合该布局。需跟踪原始 this、复制源及地址复用；不能只把末端引用计数改成忽略坏指针。 |
| PID19593 / Guest-26，sync fast path 关闭 | main+0x4a68e15：缓存树孩子的标志读取 | main+0x4a68b82 调用读锁后**未判断返回值**就读树。锁槽为 main+0xf13a7b8，旧实际地址 `0xf53a7b8`，转储槽值 `0x1c00244000`；树当时 count=1。有非零句柄不等于这次 Lock 成功。 |

三处均靠近 shader/资源对象处理路径，第二处的描述符复制、散列与引用计数流程和第三处缓存查询已做静态核对。这是定位范围，不是某个图形驱动、ZAR、FEX 或锁实现的根因结论。

两条误归因线索已排除：

- 第三份寄存器里的 `0xdeadbeef` 在 main+0x4a68b24 被游戏主动载入，并参与随后两次散列结果组合；它不是本例中已证实的释放填充值。
- 第一份有效 RBP 链的返回地址是 `0x4ec7c2d`，对应 main+0x4ac7c28 对 main+0x3ef5820 的直接调用。RSP+8 中残留的 `0x4397c2b` 对应另一段分配函数，位于当前帧局部区，**不能拼成 allocator→树插入的活动调用链**。

## 本轮代码

增加独立、默认关闭的读写锁 HLE 日志。覆盖实际安装的 `scePthreadRwlock*` / `pthread_rwlock*` 适配器，在 POSIX→Orbis 返回值转换之后观察真实输出：

- Android 启动前设置 `debug.shadps4.rwlock_log=1`；可选 `debug.shadps4.rwlock_slot` 为十六进制 guest 锁槽，空/0 表示不过滤。macOS 对应 `SHADPS4_RWLOCK_LOG` / `SHADPS4_RWLOCK_SLOT`。
- 每个入口最多记录 32 次成功及独立的 64 次非零结果/adapter 错误。前期大量成功不会吃掉后续错误额度；属性本身不改变锁策略、返回码或线程调度设置。
- 包含 context、PID、thread/generation、invocation、operation、输入六寄存器、原 RSP、经 checked ReadData 获取的 caller 及其可读状态、实际结果。日志为 `RWLOCK_TRACE` / Android `GuestRwlockTrace`。只观察返回后的调用，不将未完成等待写成已完成。
- 不直接解引用 guest 参数；实际锁等待期间不保留诊断 pin 或锁。日志 sink 异常和不可读 caller 不改变真实结果。筛选地址格式非法时不启用诊断。
- TryLock 的 EBUSY 等也是正常合同结果，不能仅凭非零日志判为模拟器 bug。错误额度耗尽后也不能用日志缺席证明后续没有错误。默认关闭时不安装 wrapper。

没有屏蔽坏指针、清低位、扩大栈、强行单线程或加入假成功。上一批共享修复静态盘点：MHR 的 16 项 Audio3d 导入中当前桥接覆盖 13 项；关联 AudioOut 的 Open/Close/Output 三项仍未接入，不能说 Audio3d 全完成。未取得它们导致本次树损坏的证据，故本轮没有按猜测扩展该族。新增 Json2 按需 LLE 策略对应的用户11.00模块静态包含 MHR 全部63项所需导出（含库/版本/module身份）；设备部署、实际绑定和 DT_INIT 未验。

## 验证

本机 macOS ARM64、LLVM、真实 GuestAddressSpace 和 GuestRwlockDomain：

- 既有读写锁 / libc 合同：60 项，0 失败。
- 新诊断：1233 项，0 失败；覆盖原输入保留、实际错误返回、adapter失败、不可读栈、sink抛错、筛选、独立预算。
- 同一测试中6个线程完成6000轮实际写锁→解锁→读锁→解锁（24000次锁操作），以宿主原子值作为见证，检查 writer 排他及受保护数据一致，0错误。没有运行 FEX 或 Android，不将这些结果当作真机并发问题已排除。

LLVM TSan 使用相同新测试再次通过1233项和6000轮并发检查，无 sanitizer 报告。Android RelWithDebInfo 宿主库及两个配套测试可执行文件已构建成功，尚未在设备执行。最终 APK 身份见下方及[manifest](mhr-offline-20260922/manifest.json)。原始结果、反汇编、字节重算脚本和 APK 保存在本地 `build/validation/mhr-offline-20260922/`；游戏/固件字节不入 Git。

## 恢复连接后的定点验证

1. 核对最终 APK/host/JNI 身份，跑配套 Android 读写锁测试，再进行 MHR 冷启动。先用普通默认设置确认崩溃是否仍存在；不要把 MHW 的共用修复当成 MHR 已验收。
2. 用新会话实际模块基址重新计算 main+0xf13a7b8。启用按槽日志，观察 main+0x4a68b82 对应 caller 的真实读锁返回；另一次不过滤可捕获其他锁的 Init/Destroy/返回错误。保存完整日志与 PID/context，结束后恢复原属性。旧绝对堆地址不可直接复用。
3. 只要仍在对象损坏处崩溃，优先在 main+0x4a51210 保存原始 this/描述符；在 main+0x4a51276（描述符复制后）、+0x4a5128c（下一调用后）、+0x4a51421（散列调用后）比对 this、vtable、refcount 和 this+0x20，缩小首次变化区间。静态入口/调用证据不能替代断点命中。
4. 从当前实际对象地址选择最多三个8字节写观察范围（头、refword、+0x20），在稳定初始化后短时启用现有 Guest RSP 写观察点。现有实现会影响时序，仅覆盖支持的 guest IR；不覆盖 native HLE/GPU 写入，遇到 unsupported 必须保留并清理，不能视为全写入覆盖。仍需区分合法复制、释放复用、错误 writer 或 this 寄存器本身被破坏。

## 工具边界

Reverse Study 本轮打开完整 MHR ELF 超时，随后 status 和 owned workspace_close 也各超时；未取得可用数据库或调用者查询结果。收尾重试 close 返回 ok=false，随后 status 明确返回 WORKSPACE_NOT_FOUND，确认该 workspace 最终不存在。没有创建第二个 workspace 或终止其他分析进程。本轮静态结论来自身份固定的本地 LLVM 指令和转储字节，不声称取得了 MCP 反编译结果。

## 最终离线产物

- APK：`build/validation/mhr-offline-20260922/shadps4-mhr-offline-rwlock.apk`，205,101,542B；SHA256 `3330298e794755f7d1751f35d64f4f67e5e1a7fd47b73ce0a920eb64c1b166a4`。
- 包内 host：`144ba72326d9f3a6b719ccb1f7f9e4720f59a96a025da407e8fed29b8ea19090`，与最终 native 构建输出相同。
- JNI：`fdeeb724da01084d04ab8ce5ed9ed370c59fe1a5733b98d351ab3a98f9d4fee9`。
- 包含本工作区之前 MHW 等共用改动及本轮默认关闭的诊断；不是仅含 MHR 的独立补丁。保留上一轮 MHW V12 APK 副本。
- 配套 Android 测试：本地 evidence 目录内 `guest_rwlock_tests-android`、`guest_rwlock_diagnostics_tests-android`。仅构建，未运行。
- **未安装、未进行 MHR 新版实机验证、未确认崩溃消失。**
