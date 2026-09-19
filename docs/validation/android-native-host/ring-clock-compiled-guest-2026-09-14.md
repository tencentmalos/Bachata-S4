# Ring、guest 时钟与编译 guest 函数（2026-09-14）

这是 `4da582b7` 之后的本地改动记录，不是已发布基线。FEX 子仓仍为 `385a0cc4`。
Foundation 当前本地 `8a564bf`（前序 `3f30145`）及未跟踪 `modules/profiler_ring/`
参与构建；这些 Foundation 提交尚未确认发布，不能当作另一台机器可拉取的 pin。

## 已验证的修复

- 主仓 FEX 配置在 Reload 后关闭 SMALLTSCSCALE，使 guest RDTSC/RDTSCP 与 Orbis
  直接使用的 host CNTVCT 同域。AYN 的 19.2 MHz counter 原先被 FEX 放大 64 倍，
  与未放大的 host 时间接口混用。实际 FEX G50 三项从 3 FAIL 变为 3 PASS。
- GuestMutexDomain 按锁对象唤醒，避免每次 unlock 对全 domain 广播；实际 bionic
  condition 66/0、services 70/0。仍是 host-owned Orbis mutex，不是 guest fast path。
- 新增独立 x86-64 C/C++ payload 构建器、严格 ELF 准入、FEX InvokeGuest 执行与竞争
  wait/wake 原型。真机 13/0，包含双 owner 100000 次递增、已进入 HLE 的取消及恢复。
  细节与限制见 [编译 guest 函数](../../guest-compiled-functions.md)。
- Foundation 真实 PROF ring 接入 host，增加手写的 HLE、GNM/Presenter、Pad 和
  AvPlayer 标签及稀疏 GuestPoll 调用栈地址。没有启用 auto tag。多 owner GNM 帧标记
  不能直接当作游戏 FPS；不同线程的字节 ring 保留窗口不同。
- 修复虚拟触摸与物理输入共存时未发送触摸快照；StatusLayer 在停帧时继续显示停顿，
  不用 Compose 的无效零 FPS 覆盖 native 统计。

## 证据与范围

本地原始证据位于 `build/graphics-toolkit-review/`：`clock-before.txt`、
`clock-after.txt`、`condition-after.txt`、`services-after.txt`、
`compiled-device-final.txt`、`compiled-admission.txt`。构建产物不入库。

10000 次 guest 内加锁/递增/RAII 解锁实测 0.407 ms、0 次 HLE；增加每轮两次空 HLE
后为 34.940 ms、20000 次 HLE。这是机制微基准，不是 Orbis mutex 全成本或游戏提速。
不支持任意 C++ STL/异常/动态 TLS，也没有替换游戏函数。

后续真实启动、loading 和音频分析见 [启动与音频记录](startup-audio-profile-2026-09-14.md)。
此前录制的设备视频为 `build/graphics-toolkit-review/tmnt-device-15s-20260914.mp4`，
15.03 秒、1920×1080 H264，无音轨；不能据此验收音频质量。
