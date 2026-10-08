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

## MSAA 选项（Launch 面板，全部游戏）

- 原 Force Disable MSAA（布尔）改为三档 `gpu.msaa`：**Off**（每像素 1 个采样）、**2×**（最多 2 个）、
  **Game (≤4×)**（游戏原值，默认）。全局/每游戏，重启生效；Launch 面板对所有游戏显示，Settings 同一项。
- **任何一档都不会到 8×**：Android 上 `Instance` 在设备 framebuffer 上限之外再硬限 4×，2× 档再限到 2；
  选项里没有更高档，非法值（如 `8x`）被拒绝。只有调试属性 `debug.shadps4.msaa_per_format=1` 才恢复旧的按格式 8×。
- 旧的布尔值继续生效（true = Off，false = Game），在 Launch 面板显示为对应档；在 Launch 保存 MSAA 后，
  该游戏配置里的旧键被移除。桌面 shadps4.json 的 `GPU.force_disable_msaa` 导入不变。
- JNI 新增 `nativeSetMsaaMaxSamples`（只接受 2/4，其余为 0 = 宿主上限）；日志
  `MSAA: framebuffer sample counts …, host samples at most N`。
- 单测：MsaaMode 5/0、RuntimeSettingCatalog 3/0、GameLaunchOptions 9/0、SettingsViewModel 5/0；
  feature/library 17/0、feature/settings 14/0；app 7 项中 1 项失败为 Windows 创建符号链接需特权（与本改动无关）。
- **Swan（APK `e63e95fa` / host `0a5964b0`）**：ASTRO 每游戏设置分别写三档后冷启动，宿主日志与服务日志一致：
  - 未设（Game）：`host samples at most 4`，`Guest MSAA disabled=false max samples=0`；
  - 2×：`host samples at most 2`，`max samples=2`；
  - Off：`host samples at most 4 (MSAA off)`，`disabled=true`，标题画面 30.4 flips/s。
  - 测完每游戏设置按字节恢复（只有 `input.xr_ds4_pose=right`，SHA `b5cb17ba…`），设备回到游戏库。
  - Launch 面板这一行只有单测，未在头显里看过（uiautomator 在 Swan 上只取到系统 dock）。
- **另记（与 MSAA 无关）**：Off 那一轮在会话第 72 s 出现 GuestFault：Guest-28，rip `0x1042e8bb6`，rdx
  `0xfffffffe00980424`，返回链 eboot+0xccf07d / +0xccf5f9 / +0xebc31c。同一 rip 和 rdx 今天还出现过两次：
  16:14（第 541 s，旧 8× 包）、16:35（第 836 s）。这是已有的间歇性故障，未定位。

## 音频杂音

- **现象**：用户反馈 ASTRO 有很重的杂音。日志 17:52:42–17:54:00（进入新关卡加载、约 30 FPS）Oboe `starved`
  每秒涨 6–8 万帧（设备每秒 4.8 万帧）。四个端口在同一次多端口调用里提交，欠载时 MAIN/AUX 一起断，听感为持续噼啪。
  之后场景下欠载为 0，所以只能在这段时间听到。
- **guest 数据是干净的**：`audio_capture` 10 s 四个端口：无 NaN/Inf、无超过 1.0 的值；能量集中在 200–1000 Hz，
  频谱平坦度约 0，块边界无跳变；四个端口每 5.23 ms 一起到达，最大间隔 7 ms。
- **根因（CPU）**：音频线程 Guest-13 平静场景下约 433 ms/s，simpleperf 显示约 35% 耗在
  `GuestAudio3d::SetAttributes` 抛出的 C++ 异常展开上（`__cxa_throw` → `dl_iterate_phdr` → `findUnwindSectionsByPhdr`）。
  ASTRO 每个音频帧对 8 个 Audio3d 对象调用 `sceAudio3dObjectSetAttributes`，带 POSITION 等属性。Android 实现只认
  PCM/GAIN/PASSTHROUGH/RESET，其余返回 `NOT_SUPPORTED`（`0x80ea0008`），对象 PCM 也一并丢弃。重场景下线程预算不够，交不上块。
- **修复**：属性 2–11 与桌面一致按原样保存为对象的持久属性（共用的 `audio3d_mixer` 读 POSITION/SPREAD），上限 4096 字节，
  未定义的 ID 仍为 `NOT_SUPPORTED`。修复后 Guest-13 约 99 ms/s，linker64/异常展开消失（启动后不同时刻，场景不完全相同）；
  Audio3d 报错 0；启动后 2 分钟欠载不再增长。ASTRO 在这段时间只设置对象属性、不推 Audio3d 输出（`audio3d_spatial` 仍为 2 块）。
- **AUX 端口**：MAIN 端口恰好等于 0.894 × type 127（AUX）端口（零延迟、残差 0），AUX 另带后置与 LFE。PS VR 下 AUX
  是电视/社交屏混音，戴头显的人听 MAIN；原来两路都混进输出，声音双份。现在 `GuestVrSensor::HeadsetReady()`
  为真时 AUX 端口音量为 0，仍照常排队、完成，节拍不变（同 AstroQuest）。日志
  `AUX port … (television mix) is silent while a headset is ready (ready now)`。
- **单测**：Swan 上 `guest_audio3d_tests` 5111/0（新增：POSITION/PRIORITY 被接受、ID 12 拒绝、8192 字节拒绝），`guest_audio_tests` 179/0。
  APK `5081b8f1` / host `21c7b05a`。实际听感待用户确认。
- **另记（与音频无关）**：见下一节“`xrEndFrame` 内崩溃”。

## `xrEndFrame` 内崩溃：GPU 挂死后 Pico runtime 层数组越界

- **现象**：Swan 上 ASTRO 6 次进程崩溃在 `xrEndFrame`（`runtime.cpp:1188`）内 Pico `libpxrruntime.so` 的 memmove
  （SEGV_ACCERR，故障地址多为 `0x703d9ff000`）：16:36、17:15、17:44、17:45、18:14、19:38，多在启动后 30–40 s。
  17:44/17:45 两次是 MSAA 三档验证中的 Game/2× 轮（当时 flips 读数为空即因此）。
- **符号**：spatial debug tool `version.query_lib_by_build_id`（BuildId `d1465576…`）在 PDM 无记录，Slardar 有符号；
  `crash.analyze_tombstone` / `crash.unwind_crash_text` 下载到 `libpxrruntime.so`（69 MB，带 DWARF）后卡在解栈步骤 30 分钟未返回，
  改用 llvm-symbolizer 直接解。栈：`oxr_xrEndFrame` → `oxr_session_frame_end`（`oxr_session_frame_end.c:5088`）→
  `submit_quad_layer` → `client_vk_compositor_layer_quad` → `ipc_compositor_layer_quad` → `handle_layer`（`ipc_client_compositor.c:2261`）。
- **runtime 缺陷**（反汇编确认，Pico 基于 Monado）：
  - `xrt_comp_native_layer_quad_client` 把层写到 `icc + slot_id×0x6020 + layer_count×0x600`，每个 slot 16 层、共 3 个 slot，无边界检查。
    崩溃时 x23−x19 = 0x12040 = 2×0x6020 + 16×0x600，即 slot 2 写第 17 层，越过分配末尾；slot 0/1 越界只会踩坏下一个 slot。
  - `layer_count` 只在原生 commit（`xrt_comp_native_layer_commit_client`）末尾清零，`layer_begin` 不清。
  - `layer_commit_impl_vk`（`comp_vk_client.c`）先 `vkCreateFence`，再用 `vk_locked_submit` 往应用的队列提交一个空 submit，
    任一失败都返回 -9，且不调用原生 commit；`oxr_session_frame_end` 只检查 -1，`xrEndFrame` 仍返回成功。
    于是每帧加的层持续累积，约 4 帧后越界。
- **触发条件（19:38 复现，全程录 logcat）**：
  - 19:38:26 KGSL `MISC: GPU hang detected`，`shadps4.android` ctx 48 ts 5474，902 MHz；
    BR IB1 `0x41922B7000`/0x1a71，IB2 `0x423D017100` 已取完；生成 snapshot。
  - 19:38:27.443 起 runtime 每帧 `Could not submit to queue: -4`（`VK_ERROR_DEVICE_LOST`）+ `vk_locked_submit Failed!!!!!`。
    宿主日志里同时出现 Turnip `submit failed: Protocol error (VK_ERROR_DEVICE_LOST)`（XR 线程）。
  - 19:38:27.829 崩溃。
  - 17:15/17:44/17:45/18:14/19:38 的崩溃各有一份同时间的 KGSL snapshot（`/data/vendor/gpu_snapshot/kgsl-*-devcd2..6.bin`），
    已拉到 `build/validation/astro-swan-20261008/gpu-snapshots/`，分析见“GPU 挂死定位”。
- **结论**：根因是 ASTRO 的 GPU 命令挂死，进程崩溃是 runtime 在设备丢失后的二次故障。设备丢失后会话本来也无法继续；
  XR 线程现在在每次 `xrEndFrame` 前检查设备是否丢失（见下一节）。
- **另一种崩溃（18:29）**：`UniqueBuffer::Create` 断言，分配 16 MiB HostUncached 缓冲时 `ErrorOutOfDeviceMemory`，
  当时 heap usage 4272 MiB、budget 4277 MiB（budget 随系统内存下降，本次启动初期为 6272 MiB）。

## 设备丢失后停止 XR 帧

- **改动**（`openxr/runtime.cpp` `CheckDeviceAlive`）：每次 `xrEndFrame` 前查询 XR 拷贝用的 fence；Mesa 在任一提交发现设备丢失后，
  fence 状态查询立即返回 `VK_ERROR_DEVICE_LOST`。丢失则抛出，由 `Run` 的 catch 记下原因并结束 XR 循环，不再进 runtime。
- **验证**（APK `5cc5e0c2`）：repro2（20:30:39）、repro4（20:57:02）两次 GPU 故障后，宿主日志
  `OpenXR stopped: Vulkan device lost (GPU fault or hang); XR frames stopped`，Pico runtime 只报 `Could not submit to queue: -4`，
  无新 tombstone，进程存活。
- **停在最后一帧**（上面改动后的现象）：XR 模式下 flip 只能经 XR 完成，XR 停止后游戏停在最后一帧。现场：渲染线程 Guest-21 持有主线程要的 app 锁，
  在 `0x100cc47f0` 里 `while (frame_id − 已完成 flip 数 > 2) cond_wait`，已完成 flip 停在 804；GPU 线程全部空闲，会话不报错退出。
  这就是之前记录的“908 帧后 Guest-1 卡在 `shadSyncWait`”。

### XR 停止后会话以错误结束

- **改动**：
  - `Runtime::SetFailureHandler`：XR 帧循环因异常停止时，在 XR 线程上调用一次。`GuestGraphics` 注册它，转为与 GPU 故障相同的
    `Fail`（请求停止 GPU/VideoOut/presenter，再取消 guest），teardown 前清除。不依赖后续 flip 到达 presenter。
  - 取消后 guest 线程会自己再出故障：ASTRO 的 `sndx_file_dequeue_0` 信号量等待返回 EINTR，随后 Guest-28 在 rip `0x1042e8bb6`
    （eboot+0xccf07d）GuestFault。原先子线程故障优先于后端错误，结果被报成 `Faulted`。现在 GPU 故障先于任何 guest 故障时
    （`graphics_fault_first`）报告 GPU 错误。今天另外 3 次同一 rip 的 GuestFault 可能也是会话取消引起的，未核实。
  - `Swapchain::~Swapchain`：提交线程失败后 `DrainSubmissions` 每次都重抛，析构里抛出会 `std::terminate`（第一轮验证即此崩溃，
    tombstone_18 `~Swapchain → ~Presenter → ~GuestGraphics::Impl`）。teardown 时改为记日志后继续释放；重建 swapchain 时仍抛出。
- **验证**（APK `e8c8d0c1`，`debug.shadps4.vsc_warmup=0` 复现挂死）：第 70 s KGSL 故障，3 ms 后
  `run: terminal outcome=BackendFailed detail=BackendFailure in GuestRuntime::Run: OpenXR stopped: Vulkan device lost (GPU fault or hang)`；
  无新 tombstone，进程存活，同一 OpenXR Activity 里起了错误面板会话（72 FPS），约 14 s 后 Activity 被 Pico 系统界面暂停（当时有手柄输入）。
  属性已清空。中间版本（无后两项）的那一轮：结果 `Faulted`，随后 teardown SIGABRT。

## GPU 挂死定位（KGSL snapshot）

- **快照**：16:19、16:36（MSAA 改动前，8×）、19:38、20:30（4×）四份解析，20:57 第五份（devcd8）KGSL 状态相同。
  - 都是同一个 pass：1440×1536，RGBA16F 颜色 + D32F 深度 + S8 模板。GMEM 模式下每个 bin 先 LOAD 三个附件，8× 时 16 个 bin，4× 时 9 个 bin。
  - BR 的 IB2（64 dword 的每 bin store IB）已取完，IB1 停在第 4–5 个 bin；BV 已越过最后一个 bin。
  - RBBM_STATUS `0x00FE0107` = CP 忙 + **PC_BUSY** + slice 忙，SP/HLSQ/UCHE/VPC 空闲。
  - 12 个 SP 的 L0 指令缓存行都含 `end`，没有波停在循环里，不是着色器死循环。
- **对照**（Swan，同一场景，默认约 33 s 挂死）：

  | 设置 | 结果 |
  |---|---|
  | 默认 GMEM | 3/3 挂死（repro1/2/4） |
  | `TU_DEBUG=sysmem` | 150 s 无故障，约 29 FPS |
  | `nolrz` | 仍挂死 |
  | `nobin`（GMEM，不用可见性流） | 81 s 无故障 |
  | `gmem_warmup`（初始 VSC 流加大） | 91 s 无故障 |

- **根因**：快照里该 pass 的 VSC 流仍是 Turnip 初始大小（`VSC_PIPE_DATA_PRIM_LENGTH 0x4000`、`DRAW_LENGTH 0x1000`，每 pipe 16/4 KiB）。
  Turnip 检测到溢出后只给之后录制的命令缓冲翻倍；A8xx 上发生溢出的那个 pass 本身就让 PC 卡在被截断的流上。
- **修复**（`vk_driver_android.cpp`）：加载 Turnip 前把 `gmem_warmup` 并入 `TU_DEBUG`（保留 `debug.mesa.tu.debug` 原有值），
  初始流为每 pipe 512/16 KiB，32 个 pipe 共约 16.5 MiB；`debug.shadps4.vsc_warmup=0` 关闭。
  更重的场景仍可能超过 512 KiB；彻底修复要在 Turnip 里让溢出的 pass 不依赖可见性流重画，待做。
- **验证**（APK `ad4e852d` / host `3bc8bd07`，无任何调试属性）：日志 `Turnip TU_DEBUG=gmem_warmup (large initial VSC streams)`；
  150 s 4322 帧、稳定约 30 FPS（与原 GMEM 相同），无 KGSL 故障、XR 未停止（repro5）。只测了这个场景，关卡内未测。
- **驱动确实收到**（APK `e8c8d0c1`，正常启动）：Turnip 在每进程首个 `vkCreateInstance` 用 `call_once` 读一次 `TU_DEBUG`（先环境变量、后
  `debug.mesa.tu.debug` 属性）；应用内四处取驱动都经 `LoadAndroidTurnip`，进程内只加载一次，`PrepareTurnipDebugFlags` 在加载前执行。
  设 `debug.mesa.tu.debug=startup` 让驱动打印解析结果：宿主日志 `Turnip TU_DEBUG=startup,gmem_warmup`，驱动日志
  `TU: TU_DEBUG=0x4000000001`（bit 0 startup、bit 38 `GMEM_WARMUP`）。反例：`debug.shadps4.vsc_warmup=0` 两轮分别在 41 s、70 s 挂死。

## 显存预算：18:29 `ErrorOutOfDeviceMemory`

- **失败的不是驱动**：缓冲与图像都带 `VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT`，VMA 在用量 + 请求超过驱动预算时直接拒绝，随后断言。
  Turnip 的预算 = 用量 + 0.9 × `/proc/meminfo` MemAvailable，18:29 时余量只有 5 MiB。
  Android 的 MemAvailable 不含 zram（Swan 交换区 20 GB、约 18.9 GB 空闲）和 lmkd 可回收的后台进程。
- **用量构成**（sysmem，标题画面）：驱动用量 2458 MB / 预算 6191 MB。
  - VMA 1262 MB，其中 XR 呈现帧 `image/present` 8 张 × 50 MB（5184×2400，池大小按 Android 窗口 swapchain 张数 +1）。
  - 非 VMA 约 1.2 GB（Turnip 内部、导入的 XR swapchain 144 MB 等）。
  - 18:29 时非 VMA 同样约 1.2 GB，增长的是 VMA（1.26 → 3.07 GB，关卡资源）；guest 内存（Shmem 4–5 GB）同时增长，二者把 MemAvailable 挤到接近 0。
- **纹理 GC 原先不会加压**：Android 上 GC 预算取整个 heap（11550 MiB），算出 pressure 6634 / critical 9911 MiB，实际耗尽前永远到不了。
  所以 GPU 写过的 render target、暂存图像从不会在压力下回收（未用过 16 帧的普通贴图本来就会回收）。
- **修复**：
  - `vma_diagnostics.cpp`：VMA 因预算拒绝时记一次超预算，去掉 `WITHIN_BUDGET` 重试一次，由驱动/内核决定。驱动也失败才走原断言。
    日志 `VMA budget reached (…): heap N usage=… budget=…; allocating past it (overrun N)`，前 16 次与 2 的幂次打印。
  - `texture_cache.cpp`：GC 同时按驱动实时预算余量判断：余量 < 1 GiB 为 pressured，< 512 MiB 或上一轮后发生过超预算分配为 aggressive；
    清理中的降级与第二轮也用这套条件，暂存图像同样跟随。状态切换日志加 `headroom`、`overruns`。
  - 未改：XR 呈现帧池（可省约 200 MB）、非 VMA 的 1.2 GB 尚未细分。
- **验证**（同上 APK）：游戏运行中用 root 在 tmpfs 写入 2.5 GB，把 MemAvailable 从 2.84 GB 压到约 1.09 GB，
  日志 `Texture GC pressured: used 2237 MiB (… headroom 1023 MiB, overruns 0)`，游戏照常约 31 FPS；
  释放后 `Texture GC idle (… headroom 1041 MiB)`。aggressive 档与“超预算重试”分支未在设备上触发（需把可用内存压到 0.5 GB 以下，
  有触发系统查杀的风险，未做）；tmpfs 已卸载删除。

## 右手柄 DS4 偏高

用户反馈右手柄模拟的 DS4 比实际位置偏上。Swan grip 的 −Z 近乎朝上（`xr_tracking status`：沿 −Z 75 mm 的点 Y 高 6.8 cm），
原偏移 (−0.08, 0, −0.02) 还额外把 DS4 往上抬约 2 cm，且 Swan grip 原点位置未经实物标定。新增 DebugBus
`xr_tracking ds4_offset X Y Z`（grip 局部坐标，米，进程内有效）用于佩戴时实时调整，确定后再改默认值。

## 提交

主仓改动（子仓引用、锁文件、打包脚本、Gradle、`FirmwareModules`、`TryHtileClear`、文档）随本记录提交并推送
`feature/malos/swan_performance`；私有固件仓库 `802771d` 先行推送。
