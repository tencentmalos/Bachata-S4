# ASTRO BOT（CUSA12392）Swan：固件模块打包与 HTILE 模板深度清除（2026-10-08）

设备 Swan `PB3110PGL6240001G`（boot `06cb3ef6-3509-4d36-a767-2a6ef0b99602`），分支 `feature/malos/swan_performance`
（已合入主干，基于 `38b45447b`），Turnip 源码构建 `64817e11`（`a49c6099…`）。

## 1. NGS2 缺失导致启动即 GuestFault

上一包（APK `ae3c2791…`）在 `SysmoduleLoad id=0xb name=libSceNgs2 result=0x805a10ff` 后 GuestFault：
Swan 的 `files/host/sys_modules` 为空，ASTRO 依赖固件 NGS2 的 LLE 实现。

按用户要求把固件放进私有仓库并随 APK 打包：

- 新私有仓库 `tencentmalos/ps4-firmware`（`802771d`），以子仓 `externals/ps4-firmware`（`shallow = true`）引用。
  内容为 `11.00_sys_modules.zip`（436 个文件，固件 11.00）与 `lib.7z`（246 个文件，固件 12.52）原样解压，
  另有逐文件清单（大小、SHA-256、编译 SDK 版本、模块名、导出库、DT_NEEDED、分类）、分类文档与生成脚本。
  版本由模块参数块的 SDK 版本字确认（387 个 `0x11008001` / 230 个 `0x12520001`）。
- `runtime/locks/ps4-firmware-modules.json` 锁定 12 个 11.00 模块：`guest_runtime.cpp` LocalLibrary 表中的 9 个
  （Ngs2、SystemGesture、JpegEnc/Dec、PngEnc/Dec、Font、FontFt、Json2）以及 FontFt 按 DT_NEEDED 依赖的
  FreeTypeOl/Hinter/SubFunc，共约 2.0 MiB。`libSceLibcInternal` 不打包（会让所有游戏切换到系统 libc）。
- `scripts/android/prepare-firmware-modules` 核对 SHA-256 后写入 `assets/ps4-firmware/`；Gradle 任务
  `preparePs4Firmware`，子仓缺失时构建失败并提示，`-Pshadps4.bundleFirmware=false` 可显式跳过。
- `FirmwareModules.prepare`（`FexSessionService` 启动游戏前、Turnip 准备之前）安装到 `files/host/sys_modules/`：
  缺失则安装，已是打包版本不动，旧打包版本升级，用户自放的其他文件保留，记录在 `.bundled-firmware.json`。

结果（APK `ef2726bb…`，host 未变 `b9aeaa75…`）：日志 `Firmware 11.00 modules: installed=12`，
`SysmoduleLoad id=0xb name=libSceNgs2 result=0`，游戏持续出帧。`libSceFreeTypeOt`（id 0x99）仍返回
`0x805a10ff`，不在 Android LLE 表中，游戏继续运行。

## 2. 渲染异常：天空只在锯齿三角形中可见，其余为黑

现象（Swan，`capture_source canvas` 左眼 2592×2400 原图 `decb19b2…`）：星空只在若干三角形/阶梯状区域可见，
其余为黑色；阶梯边长折算到 guest 眼图约 8×8 像素，与 HTILE 一个 tile 一致。同会话 `vk_recorder off`
仍然出现，排除录制线程/pass 提前。

根因：游戏用 DMA 把预先准备好的 HTILE 模板拷贝到深度目标的 HTILE 上来清深度（模板中 zmask=0 表示
tile 处于清除值；VR 标题另在模板里放镜片外区域的零深度范围 tile）。我们的 `Rasterizer::CopyBuffer`
只拷字节，不更新元数据清除状态，深度目标保留上一帧的值，天空等远处几何在 8×8 tile 粒度上被深度测试剔除。
AstroQuest 参考实现中的 `TryHtileClear`（审计 R2b）对应此问题，此前未移植。

修复：`Rasterizer::TryHtileClear`，在 `CopyBuffer`（PM4 DMA_DATA 与 CP-DMA 两条路径都经过）中，目标是
已登记的 HTILE 时读取源模板：非零字中 zmask==0 的占至少一半则 `ClearMeta`，下一次绑定该深度目标时按清除处理。
源区域已被 GPU 修改时不读取（计入 `gpu_source`）。字节照常拷贝，读 HTILE 的着色器看到的仍是 guest 数据。
DebugBus：`upload_diag htile_copy on|off`（默认 on），`upload_diag status` 输出
`htile_copy: copies_to_htile depth_clears rejected gpu_source`。

验证（APK `c8b39f0c…`，host `5435e5f9…`）：

- 运行 2：180 s 内 `copies_to_htile=5126 depth_clears=5126 rejected=0 gpu_source=0`，无 watchdog。
- 同会话 A/B：`htile_copy off` 3 s 后截图（`6a58c47b…`）黑色三角重新出现；切回 `on` 截图（`a468caa4…`）
  星空与银河完整、无黑块。截图含游戏画面，未入库，只记哈希。
- 头显未佩戴，DS4/手柄无追踪；Cross 两次后仍在同一太空视角，未进入就坐校准或关卡，未做佩戴验收与 FPS 对比。

## 3. 其他观察（未处理）

- **间歇卡死**：运行 1（同包）约 60 s、第 908 帧后不再出帧，PM4 消费同时停止，20 s 后 watchdog 转储
  （[watchdog-run1.txt](evidence/astro-htile-20261008/watchdog-run1.txt)）：Guest-1 阻塞在 sync 快路径
  `shadSyncWait`（栈 `eboot.bin+0xebb9ef ← +0xc689fb ← +0xc686c8`），其余线程处于条件变量/信号量/事件等待，
  未见持锁者。运行 2 同包 180 s 未复现；上一包 8 分钟以上也未出现。原因未定，不能归因于 HTILE 修改。
- **外侧边缘横向拉伸**：左眼最左约 115 列、右眼最右侧为逐行常量色（截断取样样貌），出现在两眼的外侧。
  canvas 直接使用 guest 的 UV，推测是游戏在镜片外区域的内容，或 Swan FOV 大于游戏 FOV 时可见；未验证。

## DS4 确认页

越过太空开场后游戏停在人物/DS4 确认页：ASTRO 需要追踪 DS4 灯条，`input.xr_ds4_pose` 默认 Off 时
VrTracker 对 DS4 只回 NOT_TRACKING。Swan 上给 CUSA12392 写入每游戏设置 `input.xr_ds4_pose=both`
（`files/settings/games/CUSA12392.json`，设备配置，不在仓库），重启后日志 `XR DualShock 4 pose source=2`，
游戏随即注册 DS4 并调用 `sceVrTrackerRecalibrate`。当时头显未佩戴、手柄 confidence 0，确认页能否通过待佩戴验证。

## 确认页仍卡住：玩家位置相对摄像头为 0

开 DS4 模拟后游戏进入就坐校准页（“请调整你的位置，与阴影部分大致贴合即可。带有红色光条的控制器也需要保持在摄像头的可视范围内。”），
两只 Swan 手柄 grip 均有效，但仍不能通过。VrTracker 直接给出 OpenXR LOCAL 坐标，头约在 (0.02, −0.05, 0.08)，
即在游戏看来玩家坐在 PS 摄像头上。PSVR 追踪空间以摄像头为原点；AstroQuest 把头相对座位的位置加上
`origin_offset {0, 0, 1.5}` 再报给游戏（`vr_runtime.h:126`），DS4 标准位置为头 + (0, −0.17, −0.5)。

修改：
- `sceVrTrackerGetResult` 的 OpenXR 分支对 HMD、眼、Move、DS4 的位置统一加 z +1.5 m（两空间都面向摄像头 −Z，不需旋转）。
  宿主自身的投影位姿（`RecordHmdQuery` 记录的 LOCAL 眼位）不受影响。
- DebugBus `xr_tracking seat on|off`（默认 on），`xr_tracking status` 输出 `seat_offset` 与 `ds4_source`，便于和 Beat Saber 对照。
- Launch 面板：PSVR 游戏在 XR 模式下增加 “XR DualShock 4 Position” 行（Off / Right Controller / Both Controllers /
  Hand Tracking），保存为每游戏设置；`GameLaunchOptionsTest` 8/0（新增 1 例）。
- 按用户要求 Swan 上 CUSA12392 改为 `right`（右手柄模拟 DS4）。

## 帧率：Litep 抓取（Swan，APK `d8a1d28a`）

- **MSAA 开（默认）**：校准页 10 s PROF，240 个游戏帧（约 24 FPS），另一视角 15.5 flips/s；GPU 98–99% busy、频率 902 MHz（最高）。
  GpuDone 每帧等 GPU 约 40.6 ms，GpuComm `PM4.Resume` 仅 16.4 ms/帧，属 GPU 受限。
- **pass_log（约 3 帧、120 个 pass）**：
  - 1440×1536 的 4× MSAA RGBA16F + D32S8 眼图 pass 约 8 ms/帧（单个 2.3–4.2 ms）；
  - 两个无 draw、只 clear、以 image_copy 结束的 pass 每帧 2–6.6 ms；
  - 一个单 draw 全屏 RGBA16F pass 约 4 ms/帧；
  - 宿主 XR 合成 `GPU.HostPrepare` 约 6.4 ms/帧（每眼 2592×2400 + FSR1）。
  - MSAA 图像被标为 semantic-native，不随 render scale 缩放。
- **MSAA 关（每游戏 `gpu.force_disable_msaa=true`，重启）**：标题画面 8 s PROF 正好 30.0 FPS（帧长 33.3–34.4 ms），
  GPU 68% busy、频率降到 726 MHz，GpuComm 3.7 ms/帧。每帧 GPU 工作约 18 ms（按 902 MHz 折算）仍超过 16.7 ms，错过隔帧 vsync 落到 30。
  场景不同（标题 vs 校准/天空），非严格 A/B，但 GPU 由满载变为不满载。AstroQuest 在 Quest 上同样以 msaa=1 运行 ASTRO。
- **下一步候选**：XR 输出/超分开销（HostPrepare）、MSAA 关闭后重新排 pass 耗时、上述 clear pass 与全屏 pass。

## MSAA 慢的原因与修复

- **原因**：ASTRO 用 4 颜色 / 8 深度采样（PS4 EQAA；颜色 `NumSamples()` 取 fragments=4，深度 8）。Turnip 不支持
  颜色/深度采样数不同（无 mixed samples），管线键把颜色升到深度的 8×（`vk_pipeline_cache.cpp` “Force all color
  samples to match depth samples”）。同一颜色目标在带深度的 pass 要 8×、在只有颜色的 pass 要 4×，
  `Image::SetBackingSamples` 每次都换 backing 并用全屏 MSAA blit 拷贝（pass_log 中无 draw 的 2–6.6 ms pass）。
  8× RGBA16F + 8× D32S8 在 1440×1536 上本身也很重；且 Turnip 声明的 `framebufferColor/Depth/StencilSampleCounts`
  只有 1|2|4。
- **修复**：`Instance::HostSamples` 把客体采样数夹到设备声明的 framebuffer 颜色/深度/模板采样数上限（Turnip 为 4）。
  ASTRO 变成 4/4，不再升格与来回拷贝，深度采样减半；着色器读 MS 纹理已按实际采样数取模，无需改。宿主
  `ms_image_blit.frag` 读源采样时夹到源采样数以内（4×→8× 时原为未定义读）。`SHADPS4_MSAA_PER_FORMAT=1` /
  `debug.shadps4.msaa_per_format=1` 恢复旧的按格式 8×。Beat Saber 的 8× 也随之变为 4×（此前按格式保留 8×）。
- **Swan 同场景 A/B（标题画面，MSAA 开，APK `169b51f1` / host `7e2fcad5`）**：
  - 旧 8×（`msaa_per_format=1`）：17.2 flips/s，GPU 99%、902 MHz；
  - 新 4× 上限：30.3 flips/s，GPU 77%、826 MHz，画面正常；日志 `framebuffer sample counts 0x7, host samples at most 4`。
  - pass_log（120 个 pass）：4× 合计 35 ms，无 draw 的清除 pass 10 个共 5.25 ms（8× 时同类 pass 每个 2–6.6 ms，3 帧共 24.6 ms）。
  - 仍停在 30：GuestCommands 约 7.4 ms/批（每帧 2 批）+ HostPrepare 约 6.4 ms/帧。校准页与关卡未测；调试属性已恢复为 0。

## 右手柄 DS4 偏高

用户反馈右手柄模拟的 DS4 比实际位置偏上。Swan grip 的 −Z 近乎朝上（`xr_tracking status`：沿 −Z 75 mm 的点 Y 高 6.8 cm），
原偏移 (−0.08, 0, −0.02) 还额外把 DS4 往上抬约 2 cm，且 Swan grip 原点位置未经实物标定。新增 DebugBus
`xr_tracking ds4_offset X Y Z`（grip 局部坐标，米，进程内有效）用于佩戴时实时调整，确定后再改默认值。

## 提交

主仓改动（子仓引用、锁文件、打包脚本、Gradle、`FirmwareModules`、`TryHtileClear`、文档）随本记录提交并推送
`feature/malos/swan_performance`；私有固件仓库 `802771d` 先行推送。
