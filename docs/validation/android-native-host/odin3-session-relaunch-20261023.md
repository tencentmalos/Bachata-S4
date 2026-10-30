# Odin3：重开游戏崩溃、后台恢复与 KH3 失声（2026-10-23）

## 范围与状态

设备 Odin3 `6edb0593`，KH3 `CUSA15072`；分支 `feature/malos/swan_performance`，起点 `22bd5a9d7`。
已修图层注销、后台暂停竞态、暂停未扣除进程时间和后台服务被系统终止四个缺陷；实机 APK 回归结果见下文。失声尚未复现或定因，不能宣称修复。
本地证据：`build/validation/crash-odin3-20261023/`（不入仓，含原 APK、匹配符号、原始日志及测试产物）。

## 原始崩溃

10-23 21:26:38，PID 8858 / `shadPS4:Present` SIGSEGV。匹配设备 host Build ID
`dd68b2cec87bb7e42e5bafd64ce3b9d9c5effd88` 的 DWARF / 指令确认：
`ImGui::Core::MustKeepDrawing` → `Presenter::PresentsDirect` → `DrawBlankFrame`。
查询 `Layer::ShouldKeepDrawing` 时读取了无效 vptr；待绘制列表首项仍是已销毁的堆对象。
过去两日还保留四次同 PC、同 Build ID 崩溃。采集的内核日志无同期 DDE / GPU fault。

旧注册表只在 `NewFrame`/`DrawLayers` 应用注销队列；上一局的 RuntimeTooltips 销毁后，
下一局首次 `PresentsDirect` 会先遍历旧裸指针。原注册表的最小状态检查确认销毁后指针仍在列表，
只有处理队列后才删除；原诊断 ASan 超时，没有有效 ASan 结论。

## 图层修复

`LayerRegistry` 将注销改为同步操作，并与绘制、查询共用递归锁。遍历保留注册项快照；注销立即使
该注册项失效，支持回调内关闭自身/后面的图层，且同地址重新注册不会被旧快照调用。
注册去重，增删请求一次重绘以清除旧覆盖图。进程级一次性注册的 guest 对话框跨会话保留。

存档、IME、通知、奖杯等调用者统一先拿注册表锁、后拿自身锁，避免同步注销引入锁反转。
在销毁回调可见状态前注销；RuntimeTooltips 更新也与 producer 的查询同步。

验证：生产注册表测试通过（100 次会话重开、首次绘制前注销、重复注册、回调自销毁、同地址复用、
查询时注销、跨线程 draw/query 注销等待）。两个负对照分别移除快照失效和注销屏障，都按预期失败。
RuntimeTooltips 原有转场/去重测试通过。新 ASan/UBSan 可编译但在此 Mac 执行超时，不能记为通过。

## 后台恢复的第二个缺陷

昨晚 23:37:39 进入后台暂停，23:37:40 恢复标记出现后马上销毁 renderer，最终报告
`BackendFailed guest return=0`；进程一直存活到今晚再次启动，69 ms 后触发上述旧图层崩溃。
旧日志没有记录具体 guest stop reason，不能直接证明当晚首次退出就是以下竞态。

FEX 的 session/debug pause 不创建 InterruptTicket，但会保护 InterruptFaultPage。
若暂停落在最后一次 JIT poll 与 syscall/HLE 边界之间，旧代码只为 ticket/owner callback 解除保护。
随后 HLE 阻塞时 `running=false`，resume 可以解除 debug_pause；回 guest 后残留页保护产生孤立
`PauseRequested`，生产运行器将它当会话结束。修复 HLE 边界与快续跑退让时的页保护退役条件，
保留 Cancel/Shutdown ticket；终止日志同时新增真实 stop reason。

Odin3 确定性测试在真实 FEX 执行中强制该交错：修复前 SP02 返回 PauseRequested（4 项中 1 项失败）；
修复后返回 42 / Returned，4/4 通过，含同时 Cancel 的对照。相关 publication 6/6、debugger 93/93 通过。
测试入口：`guest_execution_tests --focused-session-pause`。

## 实机追加发现：后台时间进入 UE4 渲染线程看门狗

首版修复 APK `fe108022` 在同一 PID 10490 连续正常 Stop→重开三次（generation 1→4）均通过。
随后后台保持约 137 秒，22:25:19 恢复时 KH3 在 `RenderingThread.cpp:1002` 明确报告
`GameThread timed out waiting for RenderThread after 136.77 secs`，随后 libc+0x21c44 的
`int 0x44` 主动终止（GuestFault）；不是图层访问错误，也不是 GPU 挂死。完整记录在
本地 `resume-fault/`。前两次触屏没有产生恢复请求，最后一次产生了正常 release，未据此修改 UI。

根因：线程和设备暂停，但 sceKernelGetProcessTimeCounter（含 guest leaf 快路径）仍按真实经过时间
计数。进程时间现在用独立 RW 页的原点/冻结值和序列号表示；HLE 与 guest 读取相同状态。
GetProcessTime 和 CLOCK_PROCTIME 使用相同的进程时间，相关音视频时间戳也同步。
恢复成功的 CPU 安全点先发布扣除暂停时长后的原点，再释放任何 guest owner；Busy 时不更新时钟。
原始 RDTSC / ReadTsc、系统日期和其他系统时钟保持原含义；未把宿主等待期限改成暂停时钟。

验证新增：模拟 8 小时暂停、重复 pause/resume、多次累计；实际 FEX guest 与 HLE 的冻结值逐位一致，
恢复后排除暂停的 20 ms；恢复钩子在 Busy 时不调用，成功时先于 owner 运行。services 82/0、
session pause 8/0。sync suite 的旧 SF13 先后两次抢在 waiter 入队前检查 PendingWaits，修正测试等待
条件为 HLE 已进入且 waiter 已入队；修正后全套 80/0，新增 clock 两项在三轮中均通过。

## 实机追加发现：普通后台服务被 Android 停止

计时修正 APK `04caef64` 的 PID 17383 在 22:37:51 进入后台，22:38:51 被系统记录为
`Stopping service due to app idle`，随后 native 正常收到取消；不是 LMK，也没有新的崩溃。
此前 FexSessionService 是普通 started service，无法在后台保留游戏会话。

改为从 RESUMED activity 启动前台服务，准备固件/ROM/Surface 之前立即登记通知。
`specialUse` subtype 明确说明保留暂停游戏的未保存内存，通知提供返回游戏与停止入口；
会话结束、准备失败、主动停止或划掉任务时释放前台服务。重复 START / STOP 使用最新 startId
结束服务，避免原 observer 的旧 startId 留下空服务。后台仍暂停 guest，不申请唤醒锁。
API 和声明依据：[前台服务启动](https://developer.android.com/develop/background-work/services/fgs/launch)、
[specialUse 类型](https://developer.android.com/develop/background-work/services/fgs/service-types#special-use)。
这是本地安装验证，未宣称 Play 已审核该用途；发布 Play 时需要提交对应用途声明。

## KH3 游戏中部分失声

用户确认 10-22 晚出现一两次，表现为大部分声音消失、偶尔保留打击音。
旧日志共 4242 个 Oboe 样本：无 reconnect/stall/device error，启动外每秒消费约 3 路 PCM，
最差短窗也约 2.978 路；没有记录到 AT9/AJM 解码错误。这排除了已观察到的设备整体停摆签名，
不能证明 PCM 内容、每声道增益或系统最终扬声器输出正常。

新增每端口每约 1 秒采一个短块的 `PCM sample` 日志：guest 逻辑声道的 peak/RMS/nonfinite、
每声道 gain、转换后的 stereo peak/RMS 与 accepted/completed。只测量、不修改 PCM、不自动重置
正常静音端口；在释放 guest pin 后、取得批次发布锁前写日志。现有 `audio_capture start N | save`
仍可采集完整原始 PCM。声道映射、非有限值、非对齐 S16 与原有 downmix 测试共 7/7 通过。

## APK / 实机验证

最终 APK `6ad827014af5f1ebd0137d5ce65d73b03a5443c25d51544a2c50dc339c4c44a8`，
host `141798348bc51e3eab3f9c540d7ab6f8d48de0e86f92329c478cf49a48ca999d`，
FEX session `c17007ac4a49bc5098e6c6ec598a23ff8cfa7a18350bcf86a590c2f250d3e7b2`。
Gradle assemblePlaystoreDebug 和 feature/session 18 个单测通过；安装后核对设备 base.apk SHA。
host/FEX 与上一版计时修正包逐字节相同；Turnip 和设备配置未改。

该包 PID 19591 / generation 1：Home 保持约 165 秒后回前台，实际 native 暂停从 22:47:31.973
到 22:50:25.312（173.339 秒）。同会话恢复 60 FPS 片头并继续到标题，Continue 读档进入黄昏镇，
没有出现原来的 UE4 watchdog；系统 `isForeground=true` / notification 43。
黄昏镇再熄屏约 189.5 秒，中途 `mWakefulness=Dozing`、无 suspend blocker，服务和 PID 保留。
唤醒后同会话回到可操作场景，未出现服务取消或 UE4 watchdog。熄屏前后各约 10 秒 PCM
样本中，游戏端口 0x20000004 的 L/R/LS/RS/LE/RE 均有信号，非有限值和采集丢块为 0；
该场景 center/LFE 为静音，不据此判断故障。L/R RMS 从 0.03281/0.04019 变为
0.06641/0.07128，仅证明捕获到 PCM，不替代扬声器验收。
随后按用户请求保留游戏采集慢帧，见 [分析与优化](kh3-slowframes-20261023.md)。
用户表示失声再次出现时会通知；本轮暂不继续声音复现。最终包的正常 Stop/通知移除
尚未重新操作，以保留当前会话；此前三次 Stop→重开通过的是首版修复包。

完整 app lint 未通过：已有 MainActivity.kt:147–148 的 dispatchKeyEvent 报 3 个 RestrictedApi，
该文件本轮未改。未把这次 lint 记为通过；未发现本轮新增前台服务声明错误。

短时后台/熄屏验证不等于真实过夜验证；8 小时仅做过进程时钟的模拟测试。
失声未复现前不记修复。

## 后续失声现场（10-23 夜）

用户随后报告剧情持续无声，已定位 guest AJM 实例编号耗尽，见 [现场与修复](kh3-audio-loss-20261023.md)。以上“未复现”保留为此前暂停回归阶段的历史结论。
