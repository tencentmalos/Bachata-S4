# Beat Saber 2.04 启动 GuestFault 修复（2026-09-29）

工作区 `feature/malos/swan_performance` / `b0778a7f`，本轮代码未提交。
接续 [源码 Turnip / XR 错误层](xr-error-beatsaber-20260929.md)。

## 结果与版本区别

Swan `PB3110PGL6240001G` 的 CUSA12878 **02.04** 已进入游戏自己的语言选择界面。
本轮保持源码 Turnip `87aeb405…`，没有切系统驱动，也没有修改游戏文件或跳过异常指令。
实机画面见 [头显合成截图](evidence/beatsaber-guest-fault-20260929/candidate-7-headset.png)。

之前非 XR 的 [AYN 01.00 记录](psvr-triage-20260920.md) 确实到过安全提示 / Continue / 光剑，
随后修正了 SBS 与陀螺仪；但没有歌曲游玩验收。[内容导出记录](executable-export-debugbus-20260921.md)
也明确区分 AYN 1.00 与 2.04。不能把两版的启动结果直接当成 XR 回归对照。
这次最初的错误发生在 `DT_INIT`，早于 OpenXR instance 创建。

**当前验收开着本游戏的 Force Disable MSAA。** 默认保留 MSAA 时，2.04 请求 8×，
Swan 当前管线路径选到 4×，触发原有一致性断言；这项没有在本轮实现 8× 模拟。
全局配置未改，仅新建 `files/settings/games/CUSA12878.json`，
`values["gpu.force_disable_msaa"]=true`。为保留可启动状态，验收后保留该单游戏设置。
这是有损画质选项，不能把此结果描述为保留原始 MSAA 的兼容性通过。

## 故障链与修复

### 1. IL2CPP 预绑定导入跳到未映射地址

有效更新里的 `Il2cppUserAssemblies.prx` 已含如下原始指令：

- 加载基址 `0x1027e4000`；`+0x1228` 是 `scePthreadSemInit` 的 PLT 位置。
- 字节 `e9 b3 bc 88 81 90`，实际跳到未映射的 `0x84070ee0`。
- 对应 `R_X86_64_JUMP_SLOT` 仍在 `+0x428a1e0`，其解析结果正确，但直接跳转绕过了 GOT。
- 同模块另外五个 sem 导入也是该形式。原 SELF 与设备导出 ELF 的这些指令字节相同，
  不是 FEX 运行时改坏的指令。RSP 逐个构造函数运行确认先前构造函数及 key/atexit 调用返回，
  到此调用才触发原始故障。

新增 `core/loader/plt_import.h`，加载器根据保留的未定义函数 JUMP_SLOT 记录恢复导入入口。
严格要求：两侧完整 `ff 25 disp32 + 10×cc` 入口夹住一个或多个
`e9 disp32 90 + 10×cc`，两侧 GOT 地址推导出相同的 8 字节步长，
**整组每个槽都有合法的未定义 GLOBAL/WEAK 函数、零 addend JUMP_SLOT**。
检查边界/rel32；普通直接调用、形状不符、缺 relocation 均不改。

只在 Guest backend 加载阶段、JIT 发布之前修改内存。GOT 随后仍由正常符号解析器填写，
没有硬编码游戏 ID、模块地址或 sem 函数地址，没有读写原文件。

### 2. 只修外部跳转仍不足：主程序混用了两种 sem_t 格式

首版保留了 eboot 中六个跳往模块内部的预绑定入口。结果 IL2CPP DT_INIT 成功，
但 Unity worker 在 `eboot+0xa7646e` 调用空函数指针，PC=0，RAX=`0x80020016`。

[主程序 wrapper](evidence/beatsaber-guest-fault-20260929/eboot-sem-wrapper-disassembly.txt)
显示本地 `SemInit` 直接写入内联结构（magic `0xffff736d`、计数等），
`SemWait` 却再调用导入的 POSIX `sem_wait`；后者已被 HLE 接管，期待的是会话句柄。
等待因此返回 EINVAL。[worker 反汇编](evidence/beatsaber-guest-fault-20260929/eboot-worker-disassembly.txt)
显示它忽略 wait 错误，直接读取尚未发布的 callback 并 call 0。

最终同样恢复**有 JUMP_SLOT 证明的本地预绑定入口**，连续入口也作为一组验证。
实机共恢复 eboot 6 项、IL2CPP 6 项，所有 sem 操作统一经过同一符号解析与 HLE 对象域。
这不等于扫描任意本地跳转并强行替换。

### 3. 接入两个后续所需的真实接口

- `libScePosix::getpagesize`：返回 Orbis 16 KiB，而不是 Android host 页大小。
- `sceKernelUuidCreate`：复用仓库现有 UUID 生成器；Guest 输出单独验证、pin、一次复制 16 字节。
  null 返回 EINVAL；不可写/截断返回 EFAULT；不把 Guest 指针交给 native 实现。
  UUID 结构移到 kernel.h，既有生成器语义未改变。

逐轮具名拒绝证据保留，没有批量准入 unsupported-import 列表或用恒定成功值绕过这些 API。

### 4. sem_t 累计 4096 次初始化导致 ENOMEM / libc trap

接通上述接口后已能绘制，但约十几秒后进入 `libc+0x267d4` 的 `int 0x44`。
新增的主线程故障记录显示 RAX=`0x8002000c`，调用者 `eboot+0x7d2930`。
反汇编确认其 `+0x7d2878` 调用 SemInit，返回非零即进入该 trap。

`GuestSemaphoreDomain::Init` 的 `allocations>=4096` 是累计计数，Destroy 不减；
Unity 不断回收临时 semaphore 仍会耗尽该伪配额。
现删除累计上限，实际分配失败返回 ENOMEM；失败不发布对象、不覆盖输出。
句柄用独立 `GuestSyncArena` 每 16 KiB 块分配 64 个 256 字节槽，
保持整个会话内唯一、不复用已退休 token，避免旧别名重新指向新 semaphore。
原来每个 sem 分配 16 KiB；等待/通知/销毁语义及 Android 独立实现保持原状。

同时保留首个 GuestFault 的寄存器和有界 RBP 链记录，覆盖初始化、worker 与主 Run。
`stack_top` 单独标记，不能把故障时栈顶任意值当成可靠返回地址。

## 验证

| 检查 | 结果 |
|---|---|
| PLT fixture / 连续入口 / 残缺指令 / 缺槽 / 越界，macOS UBSan | 170/0 |
| 同一 PLT 测试，Swan native | 170/0 |
| Process services（含 UUID 越界、只读、未对齐、canary 与两次生成） | 1753/0，exit 0 |
| 新 semaphore 测试对旧 HEAD 头文件 | 12309 checks / **3 failures**，exit 1 |
| 同测试对修复实现，1 万轮 init/wait/destroy、旧 token 拒绝、分配失败不覆盖 | 30029/0，exit 0 |
| Host / APK 编译与链接、git diff --check | 通过 |

[安装包身份](evidence/beatsaber-guest-fault-20260929/candidate-7-identity.json)：
APK `5ba2b373…`、host `cfb61a4c…`、JNI `0bcf3566…`、Turnip `87aeb405…`。
设备安装 APK 完整 SHA 与本地一致。驱动日志确认源为 Turnip / Mesa `1b588fceef`。

第一轮 PID24978 / generation1 / run UUID `4d5ba496794e622047f9869a59e980e1`，
截取状态时 3412 flips，语言界面真实显示；由本任务发送正常 STOP_EMULATION service action，
确认 Stopped / user_stop 后才关闭进程重启。
第二轮 PID29166 / generation1 / run UUID `151f1f234eaef830fa8aa3331448577a`，
独立冷启动后达到 8810 flips，仍为 Running，日志没有 GUEST_FAULT。
[第二轮状态](evidence/beatsaber-guest-fault-20260929/status-candidate-7-restart.txt)与
[第二轮头显截图](evidence/beatsaber-guest-fault-20260929/candidate-7-restart-headset.png)保存在同目录。
最终保留该运行中的语言选择界面给用户。

未注入语言确认或歌曲操作；未验收控制器点击、歌曲、完整 PSVR 投影，
未声称修复此前 Turnip GPU hang。新截图证明启动界面恢复，不证明全游戏可玩。

## 证据和清理

[摘录与结果目录](evidence/beatsaber-guest-fault-20260929/)；
原日志、三轮 RSP 结构记录、构建日志、原文件 hash 在
`build/validation/beatsaber-guest-fault-20260929/`，
[原始产物索引](evidence/beatsaber-guest-fault-20260929/source-artifacts.json)。
`prebound-plt-scan.json` 是首轮“仅孤立入口”扫描，连续的两项显示无推导结果，
最终修复的 12 项以 candidate-7 日志摘录为准。
设备时钟与 Mac 不一致，以 PID/generation/run UUID 对齐，不跨设备按墙钟拼时序。

所有本任务 RSP session/forward 已关闭，guest_debug_port / guest_debug_wait 恢复空值，
自有 PLT/UUID/semaphore probe 文件已清理。第三轮 RSP 在 continue 时 remote_eof，
其控制未作为成功证据；先存记录再清理，后改用普通启动的首故障日志完成定位。
保留此前 `debug.mesa.tu.debug=kgsl_preempt_rb`、`etfr.subsample=0` 与旧 GPU A/B 缓存待清理事项，
未改变它们。未直接修改游戏内容或存档；游戏自身初始化产生的正常写入不作逐字节不变声明。
