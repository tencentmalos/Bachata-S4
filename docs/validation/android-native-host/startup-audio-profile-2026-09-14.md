# Android 启动、loading、音频与桌面对照（2026-09-14）

状态：`4da582b7` 后本地未提交修复。AYN Thor / API33 / ARM64 / 4 KiB，固定私有 Turnip。
Host/JNI 为 RelWithDebInfo，FEXCore Release，普通 playstoreDebug APK。未做完整回归。
目前不能把优化版的 60 FPS 纯色画面当作游戏或 loading 验收。

## 实际瓶颈

普通 APK PID5502，从 Library 启动，190 秒 PROF 文件与 100 Hz cpu-clock simpleperf
采样同时记录；没有附加 LLDB。首次 guest flip 在 77.86 秒。跳过片头后 flip1712
约 54.78 秒没有前进，随后恢复，因此该次停顿不是进程永久死锁。
模块加载、重定位和服务 Prepare 本身约 206 ms，不是这 78 秒的主体。

| 样本栈分类（CPU 样本份额） | 启动窗口 | loading 窗口 |
| --- | ---: | ---: |
| GuestAddressSpace（包括其下锁） | 33.72% | 33.16% |
| GuestMutexDomain/cond 剩余部分 | 12.93% | 13.44% |
| 其他 HLE | 7.37% | 6.49% |
| AudioOut | 1.92% | 1.22% |

按首个匹配栈互斥分类；不是关键路径墙钟占比。未知内核符号/JIT unwind 仍有缺口。
ValidateRangeLocked 自身占 8.44%/8.72%，Query 自身 2.80%/2.55%。锁释放到内核的
样本同时出现在地址空间和 FexCpuContext::RunInternal；不能全部归因于同一把锁。
未观察到大比例已识别的 CompileBlock/GenerateIR 栈，不等于证明所有 JIT 成本为零。

loading 中一秒窗口主 guest owner 大部分时间在 libfmodstudio 的 1 ms 轮询。
返回地址是模块 `+0xebbfa`，外围函数 `+0xebb30` 反复调用 `+0x5cc20` 检查一个
pending counter，再 sleep；不能凭地址给 stripped 函数发明名称。
该秒 mutex lock/unlock 各约 19400 次。AudioOut 的约一秒等待在独立 guest 音频
producer 线程，不在游戏主线程；GPU driver call 仅约 6 ms。

## 音频改动与参考实现

桌面 AudioOutputThread 本就独立，但 Output 持有 port mutex。原 Android 路径虽然
也有 worker，producer 仍需等上一块设备写入结束（`ready || busy`）。Citron 的 Oboe
callback 和 Azahar 的 cubeb callback 均由音频消费者取队列数据，并保留有界反馈。

现在每个 Android port 有两个拥有 PCM 副本的待消费槽，另有最多一个正在消费的块。
worker 名为 `shad:AudioOut`，取走队列块后先发布空位，解锁再写 AAudio。
producer 只在队列满时等待，null Output 排空；Stop/Close 可取消并回收 worker。
错误会清空尚未播放的块并传递真实错误；不持 guest pin/VM gate 跨等待或设备写入。
保留原 period 节奏和 AAudio 的 short write/disconnect 处理，不是完整 Oboe callback 移植。

真机 native 音频测试 68/0：阻塞 consumer、继续入队两块、满队列取消、FIFO、入队后
修改 guest PCM 不污染已接受块、Close 取消。实际 APK ring 可见写设备在 AudioOut
worker；一轮平均 DeviceWrite 0.187 ms，队列等待主要在 guest 音频 producer。
不同线程 ring 窗口不等长，不能把这些 scope 时长相加当 CPU 利用率。尚未做听感验收。

## 桌面对照发现：AvPlayer 并发误报

桌面 AvPlayerState 通过 source mutex 查询真实状态。Android GuestAvPlayer 原本对
整个 API 使用 try_lock；竞争时抛出的失败在 bool API 外层变成 false。因此另一个
owner 正在发布视频/音频时，IsActive 会误报 EOF，GetStreamCount 会返回错误。

新增确定性测试在真实解码帧 Publish 中停住一个 owner，再让另两个 owner 查询：
原实现 1044 checks /4 failures，修复后同一测试 1044/0。普通 owner 现在可取消地等待
API 所有权；回调 worker 仍禁止跨线程阻塞重入，防止 native API 等 callback 的循环。
这是一项已证明的缺陷，但只修这一项后优化 APK 仍白屏，不能宣称它是本次白屏根因。

## 地址查询优化的状态

曾把不重叠映射保持为按地址排序，并以二分定位替代线性查询，native M33 和全部
47/47 契约通过。优化版记录到 16.38 秒首次提交，但之后为白/黑纯色 60 FPS，场景
与基线不等价，所以不能据此宣称启动提速或 loading 修好。已撤下这项优化；线性查询版本恢复传送门，但继续按 Circle 后仍然白屏。因此这次
回退并未关闭白屏，也不能据此认定二分查询实现有错。尚不保留未完成场景验证的性能变更。

## 原始证据与身份

小型日志和精确 artifact identities 在 [记录目录](2026-09-14-startup-audio/)。
本地完整数据在 `build/startup-profile/`，不包含到 Git：

- baseline PID5502 PROF 224701427 bytes，SHA256
  `db4612967f9522cf22cdede1a79fb881d0059f22949f4f5f2754a50cd44bcade`。
  2677 chunks、27297299 events，零 skipped chunks/丢失标记；capture 边界有 open scopes。
- 磁盘索引 `index-cold/a3fd61cb50bd425cadfecd51ff6e111f.litep.sqlite`。
  停顿分析窗口是绝对 CPU 时钟 `[171090000000000,171091000000000)`。
- simpleperf `cold-perf.data`、`cold-samples.txt`，零 lost samples；FP unwind 有限制。
- optimized PID9877 文件 SHA256
  `8c628a135c7a03ed179382525550a973fd34b559a3c23f3a8639227e153ef127`，480348698 bytes。
  首次 MCP 后台拉取因过程中重启 APK 而正确拒绝 PID 变化，保留 `.partial`。
  最终从原 capture 的不变设备路径读出完整文件并保存原 sidecar；不冒充 MCP 身份验收成功。
- PID11921 白屏 ring SHA256
  `b01af15fdaa255c6fbe6e97c277ed62844f073d63f7b4b7d2360360c285ba270`，截图 `manual-now.png`。
- AvPlayer 修复 APK `e48e5822…`、host Build ID `5338abf9d78dc082df1bef1ff02ff848d77249e8`，
  PID15212 截图仍白屏，保留 `avplayer-fixed/`。

FEX 主仓 RunInternal 的每次 HLE 退出/重新准入及 frame 注册锁仍是后续性能方向；
必须保留 pause/drain、取消和嵌套 callback 语义，不能简单去锁。guest C/C++ fast path
仍是独立原型，见 [前序记录](ring-clock-compiled-guest-2026-09-14.md)。

## 后续桌面差异与验证

AvPlayer Close 先设置 `closing` 禁止新事件，随后等待 callback worker barrier；但已经
入队的事件也检查 closing 并直接退出。这与“排空已接受事件”的目的冲突，可能丢掉
游戏等待的状态通知。现仅以 stopped 取消已排队事件，closing 仍阻止新事件入队。

确定性反例使用实际 looping 解码源：阻塞已进入的 Pause 回调，Resume 接受下一条
Play 事件，再从另一 owner Close；释放 Pause 后，Play 必须在 Close 返回前执行。
原实现1204/1 FAIL，修复1204/0。同批还包含 Close/Stop、并发查询等测试。
首次反例误用已到 EOF 的短视频，Pause 没有真正进入，产生4 FAIL；已改为显式手动
Start + looping，不能拿首次失败作为丢事件证明。完整前后日志保留在本地。

线性查询 + 音频队列 + 第一项 AvPlayer 修复的 APK 为 `e422efb8…`，host Build ID
`34df24b653ee50f33cdf4aa9624205e0e0486613`。PID16484 首次 flip87.02秒，片头正常，
按键后白屏；增加有界错误原因日志的 PID18884 同样复现，当前区间没有错误日志，
不能再把纯色画面归因于任何一条未经验证的失败调用。事件排空修复后的设备观察见下一节。

## 最终普通 APK 观察（事件排空修复后）

APK `b65fe36a…`，host `ce4fea22a43b271abda06ee3f2fb8b479ae29082`，JNI
`bee19bd6272163000b14acf9d04683660ccf877f`。PID21732，generation1，run UUID
`71e05025473e45f385cbca7dfb0a4274`，TracerPid0。205秒连续观察：约81秒第一次 guest
提交（2秒轮询在82.63秒看到flip2，age1.56秒）；之后实际传送门画面存在。
93.24秒执行8次Circle，随后约152秒再长按1.5秒；最终仍停在传送门，没有进入后续
可操作场景。此轮没有重现纯白，不代表白屏的所有触发方式已关闭。

Ring `110dcfa4f8d783b724a17ef183f8fd1b304c212ae316b0cba7cbdc2976dd3dc3`
中 host 按钮有9次按下/释放；guest owner21907在最后一次长按期间读取到0x2000后
读到0，证明圈键到达实际 guest。它不是只注入 Android UI 的输入证明。
音频 Port worker21993/23231运行，队列分别有512/1024 frames的待消费量。
此前0.187ms的DeviceWrite均值来自纯色场景，仅证明消费者位置，不能当相同游戏负载
的性能提升。当前音频仍需听感、延迟和欠载测量；本轮不宣称听感验收。

最终 native AvPlayer1204/0、音频68/0、线性查询CPU contract47/47。
新增EOF/Close反例旧库Build ID为`8b9d03411de0ac1fe4208eabb9824d22d2a0aa9c`。
没有更换系统驱动、扩大网络/SSL、补全任意guest libc或进行全回归。

下一步实际调查对象是“片头/传送门→下一场景”的 guest 条件与完成通知，而不是
继续凭高FPS判定渲染完成。应结合已保留的GuestPoll/事件数据，比较桌面相同场景的
状态序列；仍需验证FEX callback返回后guest观察到的状态、实际视频结束条件和后续
加载工作。当前观测还不能把其中任一项命名为最终根因。

最后经真实 APK Stop 按钮停止，Service 报 `session:none / stage:Stopped /
stop_reason:user_stop`，PID21732仍在，两个 AAudio stream 完成 Stop/Close。终态计数
12075次guest flip；该计数包含205秒采样之后继续观察的时间，不是205秒窗口计数。
界面一度保留最后一帧，不能把截图当作仍在执行。完整 Stop 状态保留原始的未解析导入
清单；这是静态准入列表，不是这轮实际触发的 Unsupported fault。
