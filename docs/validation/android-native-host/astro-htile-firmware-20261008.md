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

## 提交

主仓改动（子仓引用、锁文件、打包脚本、Gradle、`FirmwareModules`、`TryHtileClear`、文档）随本记录提交并推送
`feature/malos/swan_performance`；私有固件仓库 `802771d` 先行推送。
