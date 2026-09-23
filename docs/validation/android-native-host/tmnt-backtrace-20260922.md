# TMNT 堆错误与 Guest backtrace（2026-09-22，本地未提交）

用户通过 Android OCR 任务要求修复战斗后的 GuestFault；该任务正常 Stop 后交还 AYN。本轮保留此前 MHW/MHR 改动，不 commit/push。

## 故障证据

原 APK `14158e65`，TMNT CUSA50828，PID15581/gen1/run `bff4e6d55f6dba95e8011d84e0c77da2`。16:57:57.209终止为 `Faulted / GuestFault / operation=467 / Wl2o5hOVZdw#libkernel#1#libkernel#Function`。NID 是 `sceKernelPrintBacktraceWithModuleInfo`。

从设备累计 `android-host.log` 提取的 [上游原文](tmnt-backtrace-20260922/heap-failure-context.txt) 已先报告 `A heap error is detected`、`Garlic work heap (2)`，free/delete地址 `0x25b740000`，segment `0x238400000–0x327000000`；打印的112字节为零。随后才因调用栈打印入口未接入退出。首个破坏堆的写入尚未捕获，不能因数据为零就断言GPU、FEX或存档是根因。OCR记录的死亡/加载与故障有时序关联，尚未完成相同路径的修复后复现。

## 实现与相关族

本地用户固件11.00 `libkernel-original.elf` SHA256 `b3e13917da667d01a353ccf49025d6b1308a32cf86f3f351f96b3a13cd5c7c2f`，导出偏移 `0x2b860`：RDI是文本前缀，函数沿x86 RBP链取返回地址，查询映射名称/地址并打印；不接收待展开的外部栈数组。未引用未取得的Sony SDK文档。

- 新增 `guest_backtrace.h`，从HLE边界真实Guest RSP读取直接caller，再沿RBP链读取返回地址；两槽跨相邻映射时一起pin，权限/退休检查保留。Guest指针不传原生展开器。
- 前缀最多1024字节、栈最多256帧，null/不可读/不递增/不对齐/超长明确记录状态；损坏栈不会继续野指针遍历。日志 `GUEST_BACKTRACE` 包含原始PC、模块名/base/offset及结束原因。此为帧指针展开，不宣称任意DWARF或无帧指针代码完整展开；void诊断入口不改变游戏后续错误路径。
- `RegistersFromCpuState` 原来复制了寄存器但未设置validity，真实FEX测试因此得到空栈；已在本仓适配层标记实际捕获的GPR/RIP/XMM/MXCSR/段基址，不伪造RFLAGS/YMM/X87。未改FEX子仓。
- [完整导入和诊断族](tmnt-backtrace-20260922/family-audit.json)：1449行，最终128条拒绝/101个唯一NID。既有GetModuleInfoForUnwind/GetModuleInfoFromAddr/sysmodule别名及signal-return分类已桥接；此次backtrace实际绑定android_bridge。InternalMemoryGetModuleSegmentInfo仍拒绝：固件实际查询libkernel代码范围，当前采用分散HLE veneer，不能伪造内核模块区间；一般signal处理、在线服务等也未声明完成。游戏libc abort保留真实guest执行，不吞掉堆损坏。

## 验证与部署

- AYN `guest_process_services_tests` **375/0**，含跨映射帧、坏地址、循环、长度限制；首版跨映射夹具/实现374/4保留反例。
- 真实生产FEX三轮，每轮正常前缀/非法前缀两次，都输出3层正确模块PC并返回fixture成功值。未设置validity的中间版虽然Returned但无栈，被判诊断无效并保留日志。
- 同fixture+旧V4 host三轮仍稳定具名拒绝该NID（fixture编号6，与游戏编号467无关），保留负对照。
- [安装身份](tmnt-backtrace-20260922/installed.json)：APK `ade81d86fc1bcdd4a5f8c1a71c0ac2ff11296d9ee0c936c439df5f542d5f4ad0`，host `75b7a709d1459db85e2971f40b233b3111feba5405b4e32d5592603599cf2bb5`，JNI `a58e6d461810d73fd0d770961442d4714d02e427d992f3fb5d78ef2ab6ba48f6`。已核包内host和设备全包SHA。
- 最终普通APK PID19069/gen1/run `a9f0fa03a0c3f80fd131b6a4902f2509`，Turnip5ac41be677/0.5/High/SBS，实际TMNT中文标题菜单（画面v1.11.0），2960guest flip/2958present时Running。绑定表已确认backtrace接入；未在本轮手动进入战斗或宣称堆错误已修。
- 同包包含 [MHW subgroup改动](mhw-swizzle-20260922.md)：Qualcomm/Turnip各808199/0、263SPV通过；MHW场景仍待复测。

## 数据、设备与交接

安装前确认session:none；原home/config/custom_configs已备份到本地tar。44文件无新增/删除，42SHA相同，仅TMNT两份User.ini.json在普通启动后变化；[逐文件记录](tmnt-backtrace-20260922/userdata-check.json)。没有外部写存档、格式化、改ZAR或固件。不能把这份启动后清单说成全程所有存档字节不变。

shader_dump/network_log/rwlock_log/rwlock_slot恢复前轮原始空值。TracerPid0、无ADBforward、无我方held input。用户既有scrcpy会话 `1b9a8e7a4b1e44b9944a2b2f1ed471bf` 保留原2104×1184与TTL，未停止或调整。

设备已明确交回 Android OCR 任务独占，保留TMNT主菜单Running便于继续路线；该任务已确认接收。下一步由其复现并先保存GUEST_BACKTRACE/host-tail/logcat/status；本任务在明确再次交还前只做本地分析，不操作设备。缺失打印接口已修，原始堆错误未闭环，无完整可玩/全回归/性能结论。
