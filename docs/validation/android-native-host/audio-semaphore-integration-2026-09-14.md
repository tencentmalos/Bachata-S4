# AudioOut、内核信号量与 Json2 桌面兼容

**AudioOut 与内核信号量已接通真实 FEX/普通 APK，Json2 加载失败触发的 guest invalid-free 已消除。完整资源 TMNT 三次同进程运行20秒后可干净取消；随后120秒冷启动观察在进程约51秒时首次compute shader编译触发ARM64 SRT未实现断言。仍0帧、不可玩。** 本轮从 `8762e541` 开始，不扩展网络和 SSL；继续保留桌面的离线/dummy 兼容行为。

## 实现与桌面对照

- [GuestAudio](../../../src/core/host_runtime/guest_audio.cpp) 显式接入9个 AudioOut NID：Init/Open/Close、单端口与多端口Output、SetVolume、LastOutputTime、PortState、SystemState。复用桌面format/端口定义，所有句柄、队列、worker由runtime拥有；不调用desktop static端口表。guest PCM先经checked copy进入host所有存储，多端口全部校验完成才原子入队，无guest pin或VM锁跨等待/驱动写。Close/Stop实际取消并join，先于VM/clock析构；旧句柄不重用。
- [AAudio backend](../../../src/core/libraries/audio/aaudio_audio_out.cpp) 修复原来丢弃短写、忽略音量、打开失败仍返回backend的问题。S16/F32转换处理通道布局、实际音量和非有限值；按已接受帧数推进，断连后只补未接受尾段，一次恢复；每次写最多10ms、整段200ms预算并检查stop_token。打开校验真实rate/channel/format与requestStart结果，失败不发布端口。驱动open/close自身耗时不伪称受写预算保证。
- [GuestKernelSemaphore](../../../src/core/host_runtime/guest_kernel_semaphore.h) 接入Create/Wait/Signal/Poll/Cancel/Delete六入口。沿用desktop `OrbisSem` 的32位SlotId、计数/上限、priority/FIFO和可满足waiter扫描，与已有POSIX sem_t严格区分。Cancel/Delete/Stop分别给出真实状态；等待时释放VM锁/pin，timeout写回重新核对跨分段的完整mapping generation，同VA remap不覆盖新对象。成功取得token后如果输出地址失效会返回EFAULT，不宣称回滚token。非空未建模pOptParam显式EINVAL；不声称实时调度保证。
- [Json2兼容策略](../../../src/core/host_runtime/guest_platform.h) 独立于真实provider表。桌面`sysmodule_internal.cpp`对optional Json2缺LLE且无HLE时返回stub句柄/成功；当前仅为这个明确策略保留load/refcount/handle记账，真实已初始化provider优先，实际未实现Json2函数仍由Bind拒绝。**没有把任意缺失模块标为已实现，没有增加JSON、网络或SSL能力。**

## 崩溃来源

`apk-eighteenth`在越过AudioOut和CreateSema后出现真实guest崩溃（原日志fault address0x39）。同源LLDB复现session `28ec89583bfb462c90363a97c0a763f5` / PID5887 / TID6229 / stopEpoch3：原始sigcontext PC位于FEX JIT，实际本次fault address为0x21d3e262ee1，不能沿用上次地址。按精确FEX版本的JIT block header/tail映射到guest `libc+0x2b527`，是free读取分配头的位置，输入实际落在资源数据中。

继续核对guest调用栈与本地ELF，构造函数在`SysmoduleLoad(0xe7 / libSceJson2)`返回负值后，进入尚未初始化全部成员的析构路径。对照desktop的可选Json2无HLE策略后，仅补齐上述记账兼容。没有patch游戏代码、跳过free或修改FEX。修复后`apk-nineteenth`不再崩溃，45秒仍运行，所以旧“应当Faulted”测试失败；原FAIL保留。随后新观察用例明确要求RUNNING→Stop→CANCELLED，不能把TIMEOUT记作渲染成功。

## 定向验证和产物

[原始结果目录](2026-09-14-audio-semaphore/)、[精确源码/二进制身份](2026-09-14-audio-semaphore/latest-build.json)。AYN Thor `9c2841a4` / API33 / ARM64 /4KiB；普通APK uid10157。FEX `385a0cc4`、Foundation `5388ef45`不变。

| 项目 | 实际结果 |
|---|---|
| native AudioOut/transfer | **55/0**：短写、尾段恢复、cancel/timeout、驱动非法计数、音量/布局、非法输入、批次原子性、反序多端口并发、Close/Stop |
| native kernel semaphore | **55/0**：32位ABI、FIFO/priority、可满足waiter、Cancel/Delete/Stop、timeout、非法地址、same-VA remap |
| native Json2/sysmodule | **35/0**：未opt-in缺provider仍fail、限定兼容、refcount/handle、真实provider优先 |
| ordinary APK synthetic | **3轮**，13-import真实FEX→AudioOut/AAudio＋kernel semaphore；零PCM实际提交，不是听感测试 |
| TMNT apk-eighteenth | **FAIL**，上述invalid-free；保留崩溃日志 |
| TMNT apk-nineteenth | **FAIL**，Json2修复后45秒未Faulted，不再崩溃；保留旧断言失败 |
| TMNT apk-twentieth | **3轮同PID8587，20秒运行→Stop→Cancelled**；graphics=ready，guest_presents=0 |

真实TMNT打开48kHz/8ch AAudio，只证明设备端口可打开；不等于游戏音频质量验收。当前host Build ID `c0f9eb84eaba9178ad5085139349b00675f2df07`，SHA256 `3e772eb4ce97d2562155b7ae6a80de2f147b0d3efab62f99d9cd1922a8dea2d1`；各APK/JNI身份以manifest为准。native55/55用此前AudioOut同源DSO（SHA `6c2f9197…`）；Json2随后单独修改，不把前一DSO结果改记到新产物。构建和测试源码保留`8762e541+dirty`原身份。

## 冷启动观察确认真正的图形边界

`apk-twentyfirst`（PID12992、普通APK、未挂调试器）将观察窗扩到120秒，实际约51秒由GPU command worker触发SIGABRT。精确host栈为`Liverpool::ProcessGraphics → Rasterizer::DispatchDirect → PipelineCache::CompileModule → FlattenExtendedUserdataPass`，该函数ARM64分支在`flatten_extended_userdata_pass.cpp:876`无条件UNREACHABLE。**20秒零帧不是死锁证据，程序已继续推进到真实shader编译。** 原始FAIL、精确Build ID和日志已归档；下一步直接补SRT的ARM64实现并核对现代desktop动态offset/ReadConstBuffer及缓存语义，不能照搬旧reference只支持静态offset的分支。

## 零帧排查与边界

另做两次有界host LLDB快照：`e8c9d8a2a36b4a7a977a7ddace15490d`与`70a3c70dcd09468290a284b0182f7752`。观察到mutex/condition、POSIX/kernel semaphore及AudioOut等待；第二次还有guest owner执行FEX CompileCode，guestRIP `0x1015bf797`。**单次VM锁等待不证明死锁，当前未证明完整锁循环。** 不能据此重写同步实现，也不能把慢启动直接判为GPU问题。

两个观察session均已cleaned，target继续运行、TracerPid=0，未紧急恢复；cleanup proofs和journal hashes已归档。第一次抽象transport失败session也已清理；崩溃复现session曾报告cleanup_incomplete，后续确认目标已退出并清理其独占目录，保留原状态不涂改。原始guest内存和反汇编仅存本机debugger私有导出目录，不进Git。被调试器暂停的JUnit结果不作正常耗时/性能证据。

继续已确认的ARM64 SRT/shader缺口；不新增微型spec，不跑完整回归。没有UI PKG安装、可操作游戏场景、十分钟游戏或Swan验收。
