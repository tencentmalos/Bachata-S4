# Turnip 架构剖析（视频）

约 8 分钟、1920×1080、60 fps，中文旁白（Windows 离线 TTS「慧慧」）加内嵌中文字幕。
按顺序讲七部分：全景 → Vulkan 层 → 着色器 → 渲染模式 → CP 内部 → 内核接口 → 已证实与推测。

| 文件 | 内容 |
|---|---|
| `turnip-architecture.mp4` | 成片（H.264 crf 12 + AAC 192k，字幕已烧录） |
| `turnip-architecture.srt` | 同一份字幕的外挂版本 |
| `narration.json` | 旁白与字幕文本；`sub` 是屏幕字幕，`say` 是朗读用写法（去掉下划线、哈希、十六进制） |
| `make_video.py` | 全部动画、时间轴、混音与封装 |
| `tts.ps1` | 调用 Windows SAPI 合成每一句旁白 |
| `ib2-decode-fault53.png` | 视频里 fault53 的 IB2 解码截图（由 `make_video.py` 生成） |

## 重新生成

需要 Windows、Python 3.10+（Pillow、numpy）、带 libx264 的 ffmpeg（`FFMPEG` 环境变量可指定路径）、
已安装的中文语音 `Microsoft Huihui Desktop`，以及系统字体 Noto Sans SC 与 Cascadia Mono。

```bash
python make_video.py all
```

中间文件写到仓库的 `build/media/turnip-architecture/`。常用子命令：
`tts`（只合成旁白，按文本哈希缓存）、`timeline`（打印各章时长）、
`still s4 60`（导出 s4 场景第 60 秒的单帧检查布局）、`render`（渲染并合成）。

每句旁白的时长决定画面时间点：`make_video.py` 里各场景用 `sc.c(i)` 指向第 i 句开始。
增删 `narration.json` 里的句子时，要同步修改对应场景的序号。

## 内容来源

| 章节 | 例子 | 依据 |
|---|---|---|
| 02 Vulkan 层 | KH3 loading 挂死、`2d96f4bc287` | [odin3-kh3-compute-state-20261011](../../validation/android-native-host/odin3-kh3-compute-state-20261011.md)；IB1 解码与 MVC 寄存器表；`references/mesa-turnip` 提交 `2d96f4bc287` |
| 02 位域示例 | `CP_SET_DRAW_STATE` 第 2 组 `0x0161002C` | fault53 snapshot 中 IB2 第 1301 dword 的原始字；字段位置见 `src/freedreno/registers/adreno/adreno_pm4.xml` |
| 03 着色器 | wave64/128、Int64 | `tu_device.cc`（`threadsize_base`、double threadsize）、`ir3_compiler.c`（`nir_lower_int64`）、shadPS4 `vk_compute_pipeline.cpp` / `vk_graphics_pipeline.cpp`（`requiredSubgroupSize = 64`） |
| 04 渲染模式 | ASTRO BOT VSC 溢出 | [astro-htile-firmware-20261008](../../validation/android-native-host/astro-htile-firmware-20261008.md)「GPU 挂死定位」；`tu_device.cc` 的初始 VSC 大小与 `gmem_warmup` |
| 05 CP 内部 | fault53 `CP DDE BR opcode error` | [odin3-kh3-performance-20261011](../../validation/android-native-host/odin3-kh3-performance-20261011.md)「会话在 21:00:21 闪退」；本地 `build/validation/odin3-kh3-20261011b/perf-pool/crash-arena/fault53/`（dmesg、snapshot、IB2 解码，不入库） |
| 05 抢占线索 | Swan devcd13/14/15 | [swan-cp-opcode-firmware-20260930](../../validation/android-native-host/swan-cp-opcode-firmware-20260930.md) |
| 06 内核接口 | `bind submit failed` → DeviceLost、`92c96071dc5` | `tu_knl_kgsl.cc`（提交、fence、稀疏绑定、分配）；fault53 宿主日志；[odin3-kh3-compute-state-20261011](../../validation/android-native-host/odin3-kh3-compute-state-20261011.md) 第 8 节 |

## 已证实与推测的边界

视频结尾按下面三类收尾，正文中的措辞与此一致：

- **已证实**：ASTRO 挂死时 VSC 流仍是初始大小，`gmem_warmup` 规避后 150 s 无故障；KH3 挂死时 SP 的 CS 配置寄存器为 0，
  派发前重发状态后 loading 通过（`bind_skip off` 对照一致，`TU_DEBUG=nocsreemit` 负对照尚未做）；
  fault53 的 `bind submit failed`（EPROTO）与 DeviceLost 是 GPU 故障之后的后果；
  `KGSL_MEMFLAGS_FORCE_32BIT`（第 32 位）被 32 位 `flags` 截断，从未生效。
- **推测**：KH3 寄存器丢失来自 gen8.0 固件的上下文切换；DDE 读到零包头与 level=1 抢占恢复有关。
- **未定位**：`CP DDE BR opcode error` 的根因；低 4 GiB 为何让字体复制着色器读错。

DDE、BR、BV 没有公开全称，视频只描述它们的功能。VSC 溢出条形图中的溢出量是示意，实际需求量未测。
