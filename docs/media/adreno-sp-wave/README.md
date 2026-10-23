# SP、wave 与 eviction buffer（视频）

约 11 分钟、1920×1080、60 fps，中文旁白（Windows 离线 TTS「慧慧」）加内嵌中文字幕。
《[Turnip 架构剖析](../turnip-architecture/README.md)》《[KGSL 架构剖析](../kgsl-architecture/README.md)》的续集，
往下讲到着色器真正执行的地方：Adreno 840 的 SP、wave、指令缓存和 eviction buffer。讲解以 2026-10-13
Swan 上 XR 合成器的一次 GPU 挂死为线索，最后给出这次挂死的根因分析和证据边界。
八章：SP 的位置 → wave → 取指 → 乱序完成与 eviction buffer → 现场 → 解码 384 个 wave → 卡在取指 → 结论与边界。

| 文件 | 内容 |
|---|---|
| `adreno-sp-wave.mp4` | 成片（H.264 crf 12 + AAC 192k，字幕已烧录） |
| `adreno-sp-wave.srt` | 同一份字幕的外挂版本 |
| `adreno-sp-wave.md` | **文字版**：每章的表格、寄存器、源码行号与快照数据出处 |
| `narration.json` | 旁白与字幕文本；`sub` 是屏幕字幕，`say` 是朗读用写法 |
| `make_video.py` | 本片的全部场景、时间轴与封装 |
| `analysis/sp_snapshot.py` | 从 KGSL 快照里读 wave slot 与指令缓存的脚本，画面里的数据都由它导出 |
| `analysis/snapshot_data.json` | 上面脚本导出的数据（devcd4 的 384 个 slot、两个 SP 的指令缓存、20 份快照的序号关系） |

## 重新生成

环境同前两集：Windows、Python 3.10+（Pillow、numpy）、带 libx264 的 ffmpeg（`FFMPEG` 环境变量可指定路径）、
中文语音 `Microsoft Huihui Desktop`、字体 Noto Sans SC 与 Cascadia Mono。绘图、字幕、TTS 与时间轴工具复用
`../turnip-architecture/make_video.py` 和 `tts.ps1`（以模块 `turnip_kit` 载入）。

```bash
python make_video.py all
```

中间文件写到仓库的 `build/media/adreno-sp-wave/`。常用子命令：`tts`、`timeline`、`still s6 50`（导出单帧检查布局）、
`render`；`render --only s4,s8` 只重渲指定场景。场景里用 `sc.c(i)` 指向第 i 句旁白开始，增删 `narration.json`
的句子时要同步修改对应场景。

画面数据来自 `analysis/snapshot_data.json`，重新导出（只读快照与解析库）：

```bash
python -I analysis/sp_snapshot.py --snap <快照目录> --db <sqlite 目录> export analysis/snapshot_data.json
python -I analysis/sp_snapshot.py --snap <快照目录> --db <sqlite 目录> series
```

`<快照目录>` 放 `kgsl-*-devcd4.bin` … `devcd23.bin`；`<sqlite 目录>` 放 gpu-snapshot MCP（`gpu_snapshot_analyze`）
生成的 `devcd*.sqlite`，脚本只用其中 `shader_states` 表给出的各 SP 状态块偏移。本次的两个目录分别是
`C:\Users\Admin\Downloads\gpu_snapshot` 和 bug_reports 仓库的 `analysis/shader_eviction_hang_20261013/snapshot/`。

## 内容来源

| 章节 | 内容 | 依据 |
|---|---|---|
| 01–03 | SP / uSPTP 层级、wave 宽度、占用率公式、寄存器 | Mesa `references/mesa-turnip`（父仓引用 `65b63f66bf5`）：`freedreno_devices.py`、`ir3.c`、`a6xx.xml`、`tu_shader.cc`；快照的 `shader_states` 分组 |
| 04 | eviction buffer | 高通专利 [WO2022235402A1](https://patents.google.com/patent/WO2022235402A1/en)（Wave slot retirement procedures，2022-11-10） |
| 04 | 驱动如何选 EVICT_BUF_MODE | bug_reports `qualcomm_gpu_driver/docs/umd_minidebug/settings/README.md` 第 9 节（0855.0.9 静态还原；仓库已提交的版本还没有这一节，本片用的是 2026-10-13 的本地版本） |
| 04 | KGSL 的 SP chicken bit | KMD `adreno-gpulist.h`（`workbench-hub:/data00/swan_source/.../graphics-kernel`，提交 `a0a588dca`）第 3015–3032、3440–3452、4013–4015 行 |
| 05 | 挂死时间、Fault id、SELinux 拒绝 | 设备日志 `aplog.029`（klog）、`aplog.034:3915`（avc） |
| 05–07 | wave、指令缓存、寄存器 | 20 份 KGSL 快照 devcd4–devcd23，经 `analysis/sp_snapshot.py` 解码 |

## 已证实与未证实的边界

- **快照可证**：全部 384 个 wave slot 被占用，像素 wave 绝大多数停在 PS `end`（PC 973）；每份快照恰好一个 SP 有 1–3 个
  wave 停在 PC 512、1 个停在 PC 0，PC 0 的序号 = 某个 PC 512 的序号 + 136（20/20）；卡住的 SP 里 B 变体缺第 32–40 行，
  另有 3 个条目 tag 是第 32–34 行、flag `0x1a`、数据却是别的着色器的旧代码；`SP_PS_CNTL_1 = 0`；挂死那次开机合成器进程读
  `esx_config.txt` 被 SELinux 拒绝。
- **推断**：序号是 wave 的分配顺序；tag 的 bit 1 表示填充未完成；每个 SP 有宽 136 的按序窗口；卡住的 wave 跑的是 B 变体；
  其他 SP 在等同一个输出顺序点。
- **未证实**：取指为什么不返回；EVICT_BUF_MODE 0 / 3 的硬件含义。另一条线索：Mesa `f7ca1f551510` 把 A750 的
  `SP_CHICKEN_BITS_2` bit 30 描述为“取指缺失时不再让所有 wave 一起等”，Swan 读到该位为 0；A840 上是否同义未知。
- **写视频时更正的说法**：序号并非严格不重复（120 组里 5 组各有一对相同）；每个 uSPTP 的 VS wave 是 2–4 个而不总是 2 个；
  KGSL gen8_9_0 写的是 `0xc21800` 而不是 `0xc20800`。
