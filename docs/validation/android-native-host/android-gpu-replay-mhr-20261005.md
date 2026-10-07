# Android GPU 回放录制 / 桌面回放 / MHR 选存档与标题画面对比（2026-10-05～06，本地未提交）

依据：[GPU 回放设计](../../specs/gpu-replay-20261004.md)、[桌面实现与验证](gpu-replay-20261004.md)。原生参考帧：xrgame 在 AYN Thor 上运行 PC 版 MHR 时录的选存档界面 D3D12 RDC（`D:\workspace\xrgame-native-evidence\api-replay\run-20261005\rdoc\mhr_save_select_frame2.rdc`，仓库外）。

## 1. Android 录制（新）

DebugBus `gpu_replay_capture [frames] [name]` / `gpu_replay_status` / `gpu_replay_cancel` 在 Android 上也已注册（`adb shell dumpsys activity service com.shadps4.android gpu_replay_capture 2 name`），trace 写到 `files/host/captures/gpu_replay/<name>.sgpurply`，用 `adb exec-out run-as com.shadps4.android cat ...` 拉回。

改动：

- 驱动对象（16 个 VideoOut label、3 个内嵌 shader、init 序列）在 Android 上改到与桌面相同的固定地址 `0xFE0000000` 和相同布局（`GnmDriver::MapSessionDriverObjects`；被占用时退回旧的 `VideoOutLabelsAndShaders` 分配）。
- 写跟踪：录制器的写保护与 GPU 缓存的写关注走同一条 `ProtectGpu` / FEX access fault 路径；新增 `GpuWatchableRuns`，只对后端接受的页（已映射、不可执行）加保护，其余计入 `untracked_bytes`。
- 快照读取：Android 的物理后备是 memfd，读空洞也会分配页。初次录制在开始时被 LMK 杀掉（PSS 7.8 GB / RSS 9.1 GB）。现在 `ReadForReplay` 用 `SEEK_DATA/SEEK_HOLE` 跳过空洞（新接口 `BackingDataRuns`），写线程排队上限在 Android 上为 96 MiB。修复后 PSS 峰值约 8.0 GB，未再被杀。
- 宿主文件读：录制期间 `read`/`preadv`/`pread` 先读到中转缓冲再拷贝（内核写写保护页会返回 EFAULT）。ZAR 内容本来就走用户态拷贝。
- 命令缓冲：Android 的 GNM HLE 先把命令缓冲复制到宿主 vector 再提交，Submit 记录里原来存的是宿主指针，桌面回放读 `0xb4000…` 时崩溃。现在当提交来源不是数据指针时，把 DCB/CCB 内容附在 Submit 记录后面（`NoteSubmitContents`），回放有附带内容就用它。
- 游戏自带线程栈会通知录制器（`NoteGuestStack`）。
- 录制时同时导出设备原帧：`<name>_device/frame_<n>.png` 与 `frames.txt`，编号与桌面回放的 `frame_<n>.png` 相同。第一版目录建得晚，PNG 没写出来，只有哈希；已修，新 APK 已构建但尚未安装验证。
- 调试属性 `debug.shadps4.executable_base`（十六进制）：把 eboot 从 `0x400000` 移出。桌面低地址被 Windows 进程占用（可用区从 `0xaa00000` 起），否则回放时 eboot 三段会被跳过。本轮验证了 eboot 段与这几帧的 GPU 结果无关，属性已清空。

## 2. 桌面回放（新增选项）

- 回放器跳过本进程放不下的区域（`CanReplayAt`），并丢弃写入这些地址的页，计入 `skipped_area_bytes` / `skipped_pages`。修复前按原地址映射 `0x400000` 会损坏桌面进程。
- `--gpu-replay-renderdoc N`：回放开始时补一个 guest flip 边界，抓“回放开始 → 第 N 次 flip”。本机两个 RenderDoc 版本在 MHR 帧上都崩在 `renderdoc.dll+0xcc4922`（怀疑与稀疏 arena 有关），尚不可用。
- `SHADPS4_GPU_REPLAY_SCALE=<pct>`：诊断用，以不同于录制时的内部分辨率回放。
- 逐 draw 哈希现在也覆盖间接绘制（`DrawIndirect` 系列；MHR 几乎全是间接绘制）。
- `SHADPS4_GPU_REPLAY_DUMP=<dir>` 配合 `--gpu-replay-hash-images`，可选 `SHADPS4_GPU_REPLAY_DUMP_EVENTS=a-b`、`SHADPS4_GPU_REPLAY_DUMP_ADDR=<hex>`：把事件（或配合 `--gpu-replay-hash-draws` 时每个 draw）写过的图像按 level 0 原样落盘，文件名带 uid/地址/尺寸/格式。转换脚本见本会话 scratchpad 的 `conv.py`（R11G11B10、D32、RGBA16F、R16F、R8、A2B10G10R10）。

## 3. 实测（AYN Thor，APK 安装版 host 为本地多次增量构建；桌面 RX 7600M XT）

| trace | 内容 | 回放 |
|---|---|---|
| `mhr_ss4` | 选存档，2 帧，18 次提交，1.10 GB | 238 事件，0 分歧 |
| `mhr_title2` | 标题，3 帧，1.09 GB，设备帧哈希 3 个 | 302 事件，0 分歧 |

- 设备上新包的选存档画面基本正确（香炉、竹桌都在，无大面积重影）。会话开始时用旧包（10-04 14:11）看到的缺几何和重影，换新包后没有再出现，未逐项归因。
- 桌面回放 0.5（与录制一致）时，选存档第 1 帧整体发暗，标题第 1 帧只剩剪影（第 2 帧恢复）。原因是回放从空的缩放计划开始：主深度 `0x201e9f0000` 在第 1 帧中途因 mixed attachment pass 由 0.5 提升为原生，此时深度预通道已在 0.5 下画完，最近邻放大后的深度与原生光栅化的深度不完全相等，前向 pass 的 EQUAL 测试大面积失败。逐个预置原生决定的二分确认只需预置这一张深度。设备上这些决定在会话早期就已做出，所以设备画面正常；这不是 AMD 深度重采样路径的问题（此前的推测不成立）。修复：录制在初始状态中写入纹理缓存的原生决定（新记录 `ScalePlans`，类型 10），回放在流开始前预置；旧 trace 可用诊断变量 `SHADPS4_GPU_REPLAY_PLANS_OUT=<文件>`（回放结束时保存）/ `SHADPS4_GPU_REPLAY_PLANS_IN=<文件>`（回放开始时预置）。`mhr_ss4`、`mhr_title2` 预置后 0.5 回放与 1.0 回放一致（平均亮度 102.7/101.9、128.8/128.4）；`mhr_ss5` 自带 187 条决定，直接回放即正确。
- 桌面回放 1.0 时：选存档屏幕下方 5/6 蒙一层蓝白雾，边界在 y=180。逐 draw 导出定位到事件 190 的第 102 个 draw（体积雾合成 FS `0x9478a2c1`），它以 R32F 读 `0x201e0f0000`。`0x201e0f0000` 由深度拷贝惯用法（`Z_READ_BASE=0x201e9f0000`、`Z_WRITE_BASE=0x201e0f0000`、`DB_RENDER_OVERRIDE=0xf8000000`）写入，走 `Rasterizer::DepthStencilCopy`。根因：为拷贝新建的目标图像带 Dirty 标记（待从 guest 内存上传），拷贝后第一次作为纹理绑定时又从 guest 内存上传，覆盖了拷贝结果。修复：拷贝后把目标标为 GpuModified 并清除 Dirty，拷贝前结束渲染 pass（`vk_rasterizer.cpp` `DepthStencilCopy`）。修复后 1.0 回放的雾层消失，与设备一致。
- 标题 1.0 回放复现了设备上那类彩色亮斑：
  - 青色小点来自材质 FS `0xb0260f9a`（事件 751 第 16 个 draw），与此前定位的法线为 0、rsq 得 NaN 一致，是跨平台的翻译问题，可以在桌面逐 draw 继续查。
  - 底部绿色强光来自 FS `0x3f5d9586`（第 192 个 draw，只写画面最底部 8 行，值高达 166），像大部分在屏幕外的自发光物体（疑为翔虫）。是否过亮需要原生同一镜头对比，未下结论。

## 3b. Android 端回放（新增）

- 用法：`adb shell setprop debug.shadps4.gpu_replay <名字或路径>`（只写名字时取 `files/host/captures/gpu_replay/<名字>.sgpurply`），然后照常启动任一已安装游戏（`am start ... --es game_id CUSA34119`）。属性在时每个会话都改为回放、不加载游戏，**测完要清空**。可选 `debug.shadps4.gpu_replay_options`（逗号分隔：`nopng`、`hash`、`draws=<事件>`、`scale=<百分比>`）和 `debug.shadps4.gpu_replay_debugbus`（`;` 分隔的 DebugBus 命令，首个事件前执行）。
- 输出在 trace 旁的 `<名字>_replay/`：`frame_<n>.png`、`frames.txt`、`replay_summary.txt`（多一行 `replay_seconds`），开 `hash` 时有 `image_hashes.txt`。会话以 `Returned`、返回值 0（complete）或 1（failed）结束；界面停止会让命令处理器在下一个事件前停下。
- 实现：`src/core/host_runtime/guest_gpu_replay.{h,cpp}`。`FexSessionBackend::Prepare` 读属性替换可执行路径；`GuestRuntime::Prepare` 遇 `.sgpurply` 只建回放（打开 trace、按 trace 设置倍率/读回/同步编译等、`SetupReplayRegions`），`Run` 创建图形后恢复初始状态并驱动 `Liverpool::StartReplay`，等结束或取消。`Liverpool::RunReplay` 新增停止检查。该文件不参与 unity 合并（同批文件的 `U64` 宏会破坏 shader IR 头文件）。
- AYN Thor（APK `f0569ffd`）：
  - `mhr_ss4`：238 事件、0 分歧、3 次映射变化，1.2 s；两帧哈希两次回放相同；逐事件图像哈希 277 行中 2 行不同，都是同一张 32×80×32 R32Uint 计算输出（疑似原子写入顺序不定），其余只差纹理 uid。第 0 帧画面正确；第 1 帧不透明场景缺失、画面发暗，与桌面 0.5 回放相同，说明不是 AMD 专有问题，未定位。
  - `mhr_ss5`：282 事件、0 分歧、4 次映射变化，同进程第二个会话也正常。与录制时导出的设备原帧对比，原帧缺大量几何、露出天空，回放画面完整：录制时游戏用默认 `async_graphics_skip`（未编好管线的 draw 被跳过），回放强制 `sync`，差异应来自这里，未逐 draw 核实。
- 血源在 Thor 上抓帧（2 帧）时进程被 LMK 杀掉（`am_low_memory`，当时 xrgamenative 也在后台），trace 不完整、已删除。

## 4. 未覆盖

- 设备帧导出（`<name>_device/frame_<n>.png`）已在 `mhr_ss5` 上验证：设备两帧与桌面回放两帧编号对应。
- **抓帧开始后设备上的静态场景几何消失**（10-05 20:56 `mhr_ss5`，APK `15066a90`）：抓帧前截图正常，抓帧开始（写回 GPU 结果、对可写内存设录制写保护）后设备导出的两帧已缺建筑、道具，返回标题再进入仍缺失。同一 trace 在桌面回放画面完整，说明 guest 内存与命令一致，问题在设备侧缓存状态，未归因。在找到原因前，抓帧会改变被抓会话的后续画面。
- 10-06 起转为整体特性对照，见 [MHR 原生帧与 GCN 帧特性对照](mhr-native-vs-gcn-feature-gap-20261006.md)。
- RenderDoc 抓回放帧不可用。
- Android 端回放：桌面抓的 trace 在 Android 上回放、录制中的映射变化以外的命令（CPU flip、readback）未实测。
- 未做完整回归。
- 工作区里另一会话未提交的 `vk_command_recorder`、`singleton.h` 改动也一起进了本轮构建。

产物：

- 10-05 晚最后一次装机的 APK 是 `15066a90`，含 `ScalePlans` 记录。
- 之后为二分抓帧开始的各个步骤，加了 DebugBus `gpu_replay_probe buffers|images|protect`：只执行抓帧开始的单个步骤，不写 trace。它已构建进 host，但用户改变方向后未在设备上运行。
- 桌面 exe `0f406909`（`D:\workspace\shadps4-win-test\exe-android-replay`）。
- trace 在 `D:\workspace\shadps4-win-test\user\captures\gpu_replay\`：
  - `mhr_ss4` `30f7198e`
  - `mhr_title2` `ed119601`
  - `mhr_ss5` `9667dc53`，含 `ScalePlans`；设备帧在 `mhr_ss5_device`。
- 设备上保留 `mhr_ss4`、`mhr_ss5`、`mhr_title2`。
- MHR 存档在开始前已备份。
