# KH3 长时间游玩后失声：AJM 实例编号耗尽

## 结论

2026-10-23 夜间，用户在 Odin3 的 KH3 黄昏镇剧情中报告持续无声。当前进程读取到
`next_instance=0x4000`，但仅有 **6 个存活的 AT9 解码实例**。Android guest AJM 将低
14 位编号当成只增不减的累计创建次数；累计创建 16,383 次后，后续 InstanceCreate
永久返回 OUT_OF_RESOURCES。销毁实例只删 map，不恢复编号。

这会阻止新的音乐、对白和音效创建；尚存的循环声源仍可继续。与本次“多数声音消失，
偶有音效”的表现一致。本次是已取得进程状态与旧版负对照的确定缺陷；不能仅凭此倒推
昨晚每一次失声都一定是同一原因。它与 ImGui 重开崩溃、后台恢复的暂停/时钟问题不同。

## 现场证据

- Odin3 `6edb0593`，KH3 CUSA15072 update 01.10；PID 19591 / generation 1，
  run UUID `dbe306f0a42b30714093133759b652b7`。
- 已装 APK `6ad827014af5f1ebd0137d5ce65d73b03a5443c25d51544a2c50dc339c4c44a8`；
  host SHA `141798348bc51e3eab3f9c540d7ab6f8d48de0e86f92329c478cf49a48ca999d`，
  Build ID `f9135dddd8c459b7ac163064977fe1cd672f4678`。
- 失声时保存约 15 秒原始 PCM：8436 块、48,951,296 字节、dropped 0。
  主游戏端口 0x20000004 的 L/R RMS 为 0.002815 / 0.003429，峰值
  0.014593 / 0.019947；其余四个环绕通道也有低幅信号，所有通道 nonfinite 0。
  电影端口 0x20000001 和手柄端口 0x20000005 全静音。Center/LFE 在此前正常
  场景也为零，因此不把它们单独当作坏声道。
- PCM sample 显示主端口八个 gain 均 32768，立体声转换后仍有信号；accepted /
  completed 持续前进。Oboe reconnects / stalls / error / xruns 均 0。
- AudioFlinger 的 PID 19591 MMAP 轨仍在播放，speaker 路由，master mute 0、
  stream mute 0；媒体音量 6/15。没有系统静音、回调停滞证据。
- 23:46 截图显示人物讲话和字幕。23:59:52 用户会话切后台暂停，此前最近一次
  resume 是 23:27:12。诊断没有主动暂停、重启游戏、重开音频或修改进程内存。

### 只读进程检查

从保存的已装 APK 解出相同 Build ID 的完整 DWARF，解析 GuestRuntime::Impl、
GuestAjm::Impl、Context 的字段偏移，经 active patch control 的已知对象关系定位。
读取 `/proc/19591/mem` 保存原始字节，不执行目标函数、不写目标内存。多次读取均为：

- context 数 1；实例数 6；next_instance 16384；next_batch 700450（后续暂停时）。
- 活跃实例 ID：18860、21884、28285、28551、29922、32229，均为 AT9。
- context 地址 `0xb400007252034ff8`，next_instance 地址 `0xb400007252035030`。

Native Debugger 首次因工具链版本不匹配未附加，匹配 NDK 21 后 CodeLLDB 协议附加
失败。会话 `7675488728d94f2eb31102678af57a01` 已导出证据并 cleaned；TracerPid=0、
目标 PID/start time 不变。随后采用上述非停止的只读方式，没有第二个活动调试器。

完整本地证据：`build/validation/kh3-audio-loss-20261023/`，包含 PCM、声道统计、
系统音频状态、host/logcat、截图、精确旧 host、类型布局、读取脚本和原始内存片段。
这些含游戏/进程数据的文件不入仓。

## 修复

`guest_ajm.cpp` 按 14 位槽位循环查找空闲编号，跨 codec 保留现有活跃槽；4096 个
并发实例上限保持。一个编号只有从实例 map 移除后才可复用；在途 batch 仍通过原有
shared_ptr / BUSY 规则阻止销毁。查找最多遍历 16,383 个槽，实际受并发上限约束。

创建顺序也改为先登记实例，再向 guest 发布 ID；guest 写入失败时撤销登记，不留下
无对象的已发布 ID。此分配语义与桌面 AJM 的可回收 SlotArray 一致，不修改解码器、
音量、混音和设备输出实现。

## 验证

在 Odin3 上运行生产 host 导出的 GuestAjm：

- 新回归保持一个 MP3 实例存活，创建/销毁 40,000 个 AT9 实例，跨越编号空间两次；
  每次验证 codec、非零槽位、不得占用保留实例的槽，最后对保留实例提交 batch。
- 同一个新测试程序配已装旧 host：稳定触发编号耗尽；2 个直接生命周期断言失败，
  后续创建/退出检查连带失败，总计 65,688 checks / 5 failures。
- 修复 host：160,167 checks / 0 failures；合成 MP3 解码 input 2925、output 32256、
  samples 8064。AudioOut 原有设备回归 179 / 0。
- 初次测试未提供 MP3 fixture / fixture 推送未完成的两次结果不计通过；最终使用
  `scripts/android/generate-ajm-test-audio --out ...` 合成素材后重跑，以上为最终结果。
- Android host 构建和 assemblePlaystoreDebug 成功，git diff --check 通过。
  直接 cmake 首次缺少预编译 Turnip 环境触发 Mesa/mako 构建错误；带既有
  SHADPS4_TURNIP_PREBUILT 后通过，没有修改驱动源码或设备驱动。

候选 APK：`ac5b5a96e5befd8394d256def4b8dd43cf8e2998d3b393413f596b8ec8b714d1`，
host `2f9c6f6b4ca0cb4c3094e108d365f528ce96a9dcffe3612f193ccc17c64dc673`，
FEX session `5b2eda5b990e0d629b2abb733639f80f2fa3d577c283770906181addebf1f239`。
包含本分支此前尚未发布的慢帧与后台恢复修改。

### 2026-10-24 覆盖安装与用户游玩

用户确认已保存并允许安装后，正常 Stop 旧会话，再覆盖安装上述 `ac5b5a96`。
09:47:01 的设备包时间及 base.apk SHA 与候选完全一致。新 KH3 会话为 PID 24451 /
generation 1，run UUID `b820015f8830eda4a7447bdf16e05d36`；用户随后自行游玩黄昏镇。
用户开始体验后没有再发送手柄输入、暂停、重启或覆盖安装。

保存的约 15 s PCM 仍为 8436 块 / 48,951,296 B / dropped 0。主游戏端口 0x20000004
的 L/R RMS 为 0.044007 / 0.044306，峰值 0.498454 / 0.489068；四个环绕通道 RMS
为 0.010656 / 0.009637 / 0.017795 / 0.009760，所有通道 nonfinite 0。
Center/LFE、电影及手柄端口此场景为零，不据此认定异常。与失声现场不是同一段内容，
不把幅值比值当作修复效果量。09:59 的设备状态 callbacks 349195、consumed_frames
101335264，reconnects / stalls / error 均 0。证明本次有持续多声道数据及设备消费，
不替代扬声器听感确认，也不等于长期运行已通过。

安装、包身份、启动日志、截图及 PCM 保存在本地 `deployment/` 子目录。
同步慢帧分析见 [2026-10-24 续验](kh3-slowframes-20261024.md)。
旧会话虽然在次日早晨仍存活，但安装前未做持续恢复游玩，因此不记为“实际过夜恢复通过”。
