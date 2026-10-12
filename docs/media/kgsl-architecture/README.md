# KGSL 架构剖析（视频）

约 12 分钟、1920×1080、60 fps，中文旁白（Windows 离线 TTS「慧慧」）加内嵌中文字幕。
《[Turnip 架构剖析](../turnip-architecture/README.md)》的续集：读 Swan ROM 里的高通 GPU 内核驱动（KMD）源码，
讲 KGSL 在整个架构中的位置和它的能力。八章：全景 → 对象与 ioctl → 提交与时间戳 → 显存与 MMU → 调度与抢占 →
故障与恢复 → 功耗与可观测性 → 能力清单与边界。

| 文件 | 内容 |
|---|---|
| `kgsl-architecture.mp4` | 成片（H.264 crf 12 + AAC 192k，字幕已烧录） |
| `kgsl-architecture.srt` | 同一份字幕的外挂版本 |
| `kgsl-architecture.md` | **文字版**：每章的表格、代码、源码行号与实测出处 |
| `narration.json` | 旁白与字幕文本；`sub` 是屏幕字幕，`say` 是朗读用写法 |
| `make_video.py` | 本片的全部场景、时间轴与封装 |

## 重新生成

环境同上一集：Windows、Python 3.10+（Pillow、numpy）、带 libx264 的 ffmpeg（`FFMPEG` 环境变量可指定路径）、
中文语音 `Microsoft Huihui Desktop`、字体 Noto Sans SC 与 Cascadia Mono。绘图、字幕、TTS 与时间轴工具直接复用
`../turnip-architecture/make_video.py` 和 `tts.ps1`（以模块 `turnip_kit` 载入），改那边的公共函数会同时影响两支视频。

```bash
python make_video.py all
```

中间文件写到仓库的 `build/media/kgsl-architecture/`。常用子命令：`tts`、`timeline`、`still s4 60`（导出单帧检查布局）、
`render`；`render --only s3,s6` 只重渲指定场景，其余沿用上次的分段。
每句旁白的时长决定画面时间点：场景里用 `sc.c(i)` 指向第 i 句开始，增删 `narration.json` 的句子时要同步修改对应场景。

## 内容来源

KMD 源码在 Ubuntu 主机 `fangfang@10.37.108.94`（ssh 别名 `workbench-hub`）的
`/data00/swan_source/vendor/qcom/opensource/graphics-kernel`，单提交快照 `a0a588dca`；设备树在同目录的
`graphics-devicetree/gpu/`。工作区里 `adreno_drawctxt.c` 有一处 +54 行的未提交补丁，引用的行号都按提交版本（HEAD）。
Turnip 的行号按父仓引用 `c5a0d6fd990`。每条结论的文件与行号见 [文字版](kgsl-architecture.md)。

| 章节 | 例子 | 依据 |
|---|---|---|
| 02 上下文标志 | 请求 `0x0200_0013` → 返回 `0x0200_8052` | [swan-cp-opcode-20260929](../../validation/android-native-host/swan-cp-opcode-20260929.md)（`kgsl_preempt_rb` 实验日志）；`adreno_drawctxt_create` |
| 03 超时 0 | Thor 66.8 ms、Swan 195.9 ms | [frame-tail-wait-2026-09-16](../../validation/android-native-host/frame-tail-wait-2026-09-16.md)、[swan-turnip-20260920](../../validation/android-native-host/swan-turnip-20260920.md) |
| 04 页故障 | Mapper5 修好之前的 CCU 写越界 | [swan-turnip-20260920](../../validation/android-native-host/swan-turnip-20260920.md) |
| 04 兼容区 | 上一集 IB32 实验的 IB 地址 | [odin3-kh3-compute-state-20261011](../../validation/android-native-host/odin3-kh3-compute-state-20261011.md) 第 8 节 |
| 05 抢占 | devcd13/14/15、trace 计数 | [swan-cp-opcode-firmware-20260930](../../validation/android-native-host/swan-cp-opcode-firmware-20260930.md) |
| 06 故障 | fault53 的 dmesg 与 EPROTO | [odin3-kh3-performance-20261011](../../validation/android-native-host/odin3-kh3-performance-20261011.md)、[bugs/odin3-kh3-cp-dde-opcode-20261011](../../../bugs/odin3-kh3-cp-dde-opcode-20261011/README.md) |
| 07 Litep | Thor 血源诊所 GPU 忙 85.7%、p90 80 ms | [tier-b-litep-bottleneck-20260920](../../validation/android-native-host/tier-b-litep-bottleneck-20260920.md) |

## 已证实与未验证的边界

- **源码可证**：gen8 上 KGSL 把提交写进每上下文的 GMU 队列而不写 RB；超时 0 = 永远等待；`FORCE_32BIT` 在 64 位进程里
  无法经 `ALLOC_ID` 生效；EPROTO 表示上下文挺过故障、命令已入队；gen8 致命故障都走硬复位；trace 里的抢占 “level”
  在 hwsched 下取自 GMU 的 `ctx_switch_cntl`。
- **推断**：RB 与 IB 包由 GMU 固件写入；快照里冻结 buffer 的内容取自 work 运行时。
- **未验证**：GMU 固件内部策略与 policy 位含义；`ctx_switch_cntl` 的含义；Odin3 / Thor 的实际内核；Swan 实际使用的频率 bin；
  设备上 `.ko` 的构建配置及是否带 XR 抢占补丁；fault53 缺 draw state 载荷具体落在哪条规则。
