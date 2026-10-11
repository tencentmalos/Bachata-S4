# One Piece Grand Cruise：GC 信号与手柄端口修复（2026-10-11）

## 范围与身份

从 `feature/malos/swan_performance` 的 `ea99cf224c59b981b597e3157bf191acfb27bce3` 切出 `feature/malos/vr_perf`。本次只改主仓的 Android/FEX 宿主适配层及测试；没有改 FEX 子仓，也没有恢复搁置的 KH3 CRB 试验。

设备为 Thor `9c2841a4`（Android 13 / Adreno 740），游戏为 CUSA11396 v1.00。用户允许自动操作。Odin3/Swan/桌面未做本次实机验证。

本地完整证据在 `build/validation/onepiece-thor-20261011/`，含原始故障、导出模块、反汇编、APK、截图、构建和测试日志；游戏二进制与 APK 不纳入 Git。最终包身份见该目录 `final/identity.json`。

## 初始化崩溃的原因

旧版约在初始化第 4 秒发生 GuestFault，落点并不固定：

- `Il2CppUserAssemblies.prx+0x42b86b`，SimpleCollator 构造读取空对象。
- `+0x7c8fe`，allocator 空闲链指针为 `0x0076006300000009`。
- `+0x44233`，Monitor.Exit 的对象字段被 UTF-16 字符内容覆盖。
- `+0x289b4`，指针为 `0x005f003700300139`。

关闭同步 fastpath、关闭 FEX multiblock 都仍然崩溃。错误形态是对象被提前回收/复用，与单一指令翻译错误不同。

引入这条错误路径的提交是 `2017902450860700f0351c157b1616fb2f3d491b`（2026-09-19，`Integrate Android HLE and Beat Saber SBS VR bring-up`）：它把名称为 `SuspendSemaphore` / `ResumeSemaphore` 的零计数信号量当成前台门控，创建时及等待时补足计数；同时接受 `sceKernelInstallExceptionHandler` 与 `sceKernelRaiseException(SIGUSR1)`，却不执行回调。

One Piece 的模块字节证实这两个信号量实际用于 GC 停止其他线程：

1. IL2CPP `+0x893b0` 建立两个零计数信号量，通过 PS4Util 注册 SIGUSR1（30）回调。
2. `+0x896a0` 枚举 GC 线程并调用 `arch_raise_user`，随后等待每个线程的 suspend 确认。
3. `+0x894d0` 在目标线程上保存寄存器和被中断的 RSP，发出 suspend 确认，等待恢复，再确认 resume。
4. PS4Util `arch_init_gc +0x240` 安装 `+0x290` 处理器；处理器从 `ucontext+0xf8` 取 RSP。

原实现让 GC 在其他线程仍运行、也没有提供寄存器/栈根的情况下继续扫描与回收。修复后同一游戏越过原崩溃点并进入场景。没有声称已捕获所有旧故障的首个错误写入者。

## 实现

- 移除两个名称的特殊放行，恢复普通信号量计数语义。
- 注册实际 guest 异常处理入口；SIGUSR1 按 guest pthread 句柄投递到目标 owner。
- FEX 适配层通过现有中断轮询页退出 JIT，取得已恢复的架构寄存器，再通过 `HleScope::InvokeGuest` 执行回调；不从宿主信号处理器直接跳 guest，也不创建替代线程。
- 等待中的 owner 使用可唤醒的 HLE 等待：释放等待锁后执行回调，再回到原等待队列/期限；Stop 仍可取消。mutex/cond/semaphore/rwlock/event flag 路径接入。
- 同一 owner 的信号回调不嵌套，允许在当前处理期间排队下一次；每线程 TLS 保留上下文暂存区，调用期间不持 guest memory pin。异常退出由 RAII 撤销等待节点。

此轮实现的是显式 SIGUSR1 的正常返回型回调。同步 guest CPU 异常、处理器修改 ucontext 后改变执行流，以及完整 x87/AVX 扩展上下文并未由此验收。

## 第二个问题：注意事项页无法确认

临时 Pad 调用日志和 eboot 反汇编显示 Unity 先打开普通端口（type 0），再探测特殊端口（type 2）。旧 Android 适配层返回：

```text
Open(user=1000, type=0) -> 1
Open(user=1000, type=2) -> 0x80920001
Read(handle=-1)        -> 0x80920003
```

eboot `0xf12d7b` 的第二次 Open 失败后跳过 `0xf12dc9`，因此连已成功打开的普通句柄也没有保存。修复为和桌面普通 DS4 配置一致：特殊端口有独立可关闭的句柄，报告未连接。它不占用 DS4、不产生输入或振动。新日志为普通句柄 1、特殊句柄 2、关闭 2、持续读取 1 成功。

## 验证

- Thor 原生测试：FEX HLE 45/0（含 JIT 中断、阻塞 HLE 唤醒、owner 身份、寄存器恢复和回调不嵌套）；publication 6/0；kernel semaphore 602/0；event flag 1205/0；condition 67/0；guest Pad 105/0。
- Android host 与 FdroidDebug APK 编译成功。
- 第一候选在注意事项页运行约 10 分钟后正常 Stop，返回 Library。
- 同时修复 GC 和特殊端口后，自动确认进入双眼标题、关卡选择、船甲板及娜美所在室内场景。画面约 30 FPS，无 GuestFault / DeviceLost。注意事项页是游戏的单屏输出；进入 VR 标题后才出现 SBS，无需额外修改显示配置。
- 尚未完成整关/战斗、音频听感、存读档、其他游戏和其他设备回归。关卡选择页一张截图中的边缘角色呈红色，未归因；不能据本次启动修复声称全部渲染效果正确。

## 最终包与收尾

- 最终 APK SHA-256：`0a266f8f036e74db4c19c05b7e935063b294a2f87da57029e6e82c81d9f677ac`，设备已安装包逐字节校验一致；host 为 `5a7f4715d07dd305228052eab5fe2dff5935ea9f0bb28804737ad1f628eda915`，FEX JNI 为 `dd78a4cc5a9851448676226247dff64f236e2bc100e08497a65668b95c95d1f0`（完整身份以 `final/identity.json` 为准）。实际使用 Turnip mainline `8266dd17ce65acb25c2ca5dc378a4dc5576ac8d63d74d8ee23b5a096f404bbe6`，源码 `92c96071dc5`。临时 Pad 调用日志已移除。
- 最终冷启动为 20:41:30，PID 3670，generation 1，run UUID `dc7e137b48e3516dcf806ce62f94c2b4`。自动输入越过注意事项、标题和关卡选择，再次进入双眼船甲板；截图 `final/title.png`、`final/select.png`、`final/deck.png`，甲板状态层显示 30.0 FPS。
- 主动请求的 watchdog 在 20:47:14 / 20:48:26 分别记录 11287 / 13450 帧、55 个线程、最新帧年龄 0 ms；它们不是超时转储。此最终会话的 host 日志未出现 GuestFault / DeviceLost / BackendFailed / Fatal。
- 原有 One Piece profile 逐字节未变，仍为 `android.psvr_display=sbs`；同步 fastpath 保留原来的 1，FEX multiblock 未禁用。自动按键已释放，设备保留游戏运行；本任务的 `/data/local/tmp/shad-onepiece-signal` 测试目录已清理，没有遗留调试转发。未人工修改存档；之前保存的 settings tar 只有设置，不能作为完整存档备份。
- 本轮修复位于 `feature/malos/vr_perf`，测试产物保留在本地证据目录。
