# 上游稀疏 arena 缓冲区缓存移植与 AYN 验证（2026-09-26）

分支 `feature/malos/sparse_arena`（基于 `63a8ee43`），全部为本地提交，未推送。

## 合入内容

| 提交 | 内容 |
| --- | --- |
| `e93f10e8` | 上游 #5047 `965c97c8`：稀疏 arena 缓冲区缓存、Runtime 屏障跟踪、staging pool |
| `2d1a81a3` | 日志迁移遗漏：FEX session backend 直接 include fmt（APK 此前无法构建） |
| `3132a4ec` | 启动自检：绑定一个稀疏块并经 GPU 往返数据，驱动"假成功"时报明确错误 |
| `840b8a0a` | `android_sparse_probe` 增加数据往返与普通缓冲区对照 |
| `a52fd493` | 上游 #5044：链接器查找导出时忽略库版本与符号类型 |
| `4a8221ce` | 上游 #5109：运行时 xbyak 生成桩（仅 x86-64 主机）+ ARM64/Windows `IsExecuteError` |
| `310f3901` | arena 按设备 `maxBufferSize` 定大小；跨 arena 访问在上限内合并整块、否则只取访问所在页 |
| `aed85550` / `89438d05` | `android_sparse_probe` 按缓冲区缓存的实际 arena 大小测试；逐块提交模式、指定大小模式 |
| `7e366a57` | 高通专有驱动 arena 限制在 2 GiB 以下 |
| `8eca8601` | 上游 #5062：主机着色器构建期编译为 SPIR-V，tiling 参数改特化常量 |
| `5ce09d11` | 上游 #5035：关闭时不再存配置、配置加载失败改为致命、热键只在按下沿触发（fork：删掉 Android 迁移分支里残留的 `m_loaded`；Android 设置由 JNI 下发，生产会话不调用 `Load`） |
| `7f331a4b` / `90a1bccc` / `b75ac150` / `500049fd` | 上游 #5000 文档、#5016 ngs2 SystemSetup 校验错误、#4963 删除未用设置代码、#5122 audioin include |
| `0dd825ab` | 上游 #4740：基础 HLE 键盘库（fork：`posix_pthread_rename_np` 的 libkernel 注册已存在，不重复加；键盘实现依赖 SDL，Android host 与 `sdl_mouse.cpp` 一样排除，注册只在桌面，Android 原本就不准入 libSceKeyboard） |
| `6da66f78` | #5044 移植回归修复：裸名字 `NoType` 符号在自身导出里找不到时恢复“未解析”，血源 libSceFios2 `module_stop` 不再让 Prepare 失败 |
| `2f410a1f` / `dc4ea418` | 血源切场景崩溃：guest 写回诊断，以及 DRS 回写只写仍被跟踪、且在新描述符范围内的页，见 [血源 DRS 回写](bloodborne-drs-writeback-20260926.md) |

### #5047 的 fork 适配

- 缓冲区按上游：arena、Runtime 屏障跟踪、staging pool。顶点/索引/间接参数的读取也登记到跟踪器（上游只登记着色器缓冲），后续写入会等待它们。
- 图像保留 fork 的 Image 操作（Render Scale、缩放 blit、融合回读、MSAA backing）：Runtime 的图像接口委托给它们、立即记录自己的屏障；它们读写的缓冲区登记到跟踪器。
- 待提交的稀疏绑定在 submit 回调中、recorder 排空之前入队，持有与提交 worker 共用的队列锁，并向 Android timeline completion 报告 `Submitted`。
- hoist 期间需要批量屏障时先结束被持有的 pass（屏障覆盖 pass 内已跟踪的访问）；按缓冲区的屏障提前（per-buffer barrier hoist）已删除。
- copy-shader HLE 保留提前、guest 镜像与延迟 guest 提交，回读改用 deferred staging；staging 释放改为线程安全（回写在 priority 线程执行）。

## AYN Thor（9c2841a4）实测

- 设备预先设置了 `debug.shadps4.vulkan_driver=turnip`（强制旧 R8 pin `5ac41be677`，app 默认本为 mainline）。
- 移植版 + R8 Turnip：TMNT 60 FPS 但全黑。同设备移植前构建（`63a8ee43`）100 秒时正常显示主菜单，确认是移植引入。
- 根因：`android_sparse_probe` 数据往返——R8 Turnip `vkQueueBindSparse` 返回成功，但经绑定块写入的数据读回丢失（4080/4096 字节不同，普通缓冲区对照 0 差异）；mainline Turnip `86ca472fc2` 全部通过。高通系统驱动 `maxBufferSize=4294897663` 低于 4 GiB，4/8 GiB arena 创建失败（-1）。
- 临时清空属性（mainline）：移植版 TMNT 主菜单正常，DebugBus 手柄"继续冒险"进入下水道关卡，60 FPS、画面正常；日志无 Render 错误，`EnsureResident` 264 次、无 arena 迁移。
- 启动自检：mainline 日志 `Sparse residency check passed (16 KiB blocks)`；R8 pin 日志 Critical `... 510 of 512 bytes ... did not read back ... On Android select the mainline Turnip driver.`，VideoOutOpen HLE 失败、会话终止（UI 目前只显示黑屏，未显示原因——既有的失败会话 UI 问题）。
- APK：移植版无自检 `E1289B3F…`，含自检 `76B3AE8D…`（前 16 位 SHA-256，均 playstoreDebug，同一 debug keystore `install -r`）。
- 收尾：`debug.shadps4.vulkan_driver` 恢复为原值 `turnip`，app 已 force-stop。**在该属性下，移植版会在启动时按自检失败**；要在 AYN 上运行需清空该属性（使用默认 mainline）。
- 之后用户改用自己维护的 mainline：属性现为 `turnip-mainline`（`turnip` 以外、`system` 以外的值即默认 mainline）。34 GB 旧日志已删除。

桌面（RX 7600M XT）：Bloodborne 进入"未以退出游戏结束会话"提示框，画面正常；PostMessage 按键未送达未聚焦窗口，未进入世界。

## arena 大小与高通驱动（`310f3901`、`7e366a57`）

- arena 上限取 `maxBufferSize`；arena 页为不超过上限一半的最大 2 的幂，夹在 256 MiB–4 GiB。实际：mainline Turnip 2 GiB 页/4 GiB 上限，AMD Windows（`maxBufferSize` 2 GiB）1 GiB 页，高通 512 MiB 页。
- 访问跨多个 arena 时，并集不超过上限就整体合并，否则新 arena 只覆盖访问所在的页；迁移前先刷屏障。诊断计数 `arena_migrations`。
- 高通 Adreno 740 专有驱动 `69e13475cb`：`maxBufferSize=4294897663`、64 KiB 稀疏块、`sparseResidencyAliased=0`。探针逐块模式定位到：2 GiB 稀疏缓冲区中复制区域到达偏移 2^31 时，驱动在 CPU 上录制 `vkCmdCopyBuffer` 即段错误；2047 MiB 缓冲区在所有探测偏移上正常。因此该驱动上限取 `2^31−1`（512 MiB 页，最多合并到 1.5 GiB）。
- AYN 高通驱动：自检通过（64 KiB 块），TMNT 下水道关卡 39 FPS，一次 arena 合并，无错误日志。地面比 Turnip 偏暗、移动时闪屏；用户确认是高通驱动另外的问题、之后自己修，本轮不查，也未做移植前基线对照。

## #5062 移植（`8eca8601`）

- `tiling.comp` 保留 fork 的 8bpp 打包解平铺、原子 8bpp 平铺、宏平铺图像的微平铺 mip 与融合回读（FROM_IMAGE）；回读打包方式改为特化常量 12/13，另编 `tiling_image_*` 模块。每种像素宽度 × 微/宏平铺一个模块，其余参数为特化常量。
- `TileManager` 两种管线布局共用一个按（tile mode、bpp、采样数、方向、打包）索引的缓存；旧键缺采样数，96 位像素与 64 位冲突。
- Foundation 的 BC7/ASTC 编码器仍用运行时 GLSL 编译。
- 交叉构建由 `cmake/host-tools` 编出本机 `glslang-standalone`，经 `HOST_SHADER_COMPILER` 传入；本机构建用树内目标（`ENABLE_GLSLANG_BINARIES` 仅非交叉编译时打开）。
- `check-host-ndk-sources.py` 改用 glslang 生成头文件（另接受 Windows NDK 的 `clang++.exe`；其 include 集缺 Foundation foveation 路径，TU 编译在改动前就会失败）；`run_android_r8_tiling.py` 用 `spirv-opt` 把特化值写成默认值。
- 验证：桌面 clang-cl 与 Android host（`HOST_LINK_PASS`，30 个着色器头由本机 glslang 生成）构建通过。AYN mainline Turnip：TMNT 主菜单、下水道关卡 60 FPS；R8 平铺/解平铺真机精确比对 6/6（每例 1 MiB）。桌面 RX 7600M XT：血源进入世界，内部缩放 1.0 与 0.5 均正常，0.5 时创建并使用 `readback pack1` 融合回读管线（测试目录 `config.json` 用后逐字节恢复）。

## 未合入的上游 PR

- #5069：userfaultfd 专用（保留注册、`Unmap` 返回合并后的范围、记录 GPU 线程 tid），并删掉 `SendCommand` 在 GPU 线程上直接执行的捷径、改为显式 `assume_locks`。
- #5096：改的是信号路径 `SignalImpl::GuestFaultSignalHandler`（不只 Linux）：fault 落在 GPUComm 线程上时带 `assume_locks` 调 `Invalidate/ReadMemory`，避免 `SendCommand` 等待自己。fork 保留了 `SendCommand` 在 GPU 线程上直接执行的捷径，效果等价，无需合入。
- #5113：针对上游新的 session 调度器；fork 的 `on_submit` 已在结束命令缓冲之前调用。
- #5119：fork 已去掉启动时的 `io.Fonts->Build()`，且有自己的 `RendererHasTextures`（SDF 字体整图替换）实现；上游的 Vulkan 纹理管理与之重复。
- #5100（内存跟踪器重写 + 批量上传）：见下。

### #5100 评估

上游内容分三块：
1. 批量上传：`ObtainBuffer` 只把区间记入 `sync_batch`，在 fence（EOP/EOS/ReleaseMem/WriteData）、CE `DumpConstRam` 写到待传区间、会话结束时统一拷贝，写进排在本会话主命令缓冲之前的上传命令缓冲；调度器改为"会话"（主 + 上传命令缓冲，一次提交多组）。
2. 内存跟踪器：去掉 readable/writable 位图，位图更新向量化；PageManager 重写；`CopySparseMemory` 改按物理段从 backing 读。
3. 暂存池非同步请求（平铺暂存复用不等 GPU）、DebugState 批次计数、若干工具类（`small_vector`、`DomIntervalList`）。

与 fork 的关系：批量上传的目标（上传不打断 pass、少发拷贝）fork 已用录制线程 + pass 提前实现（血源诊所每帧因 buffer_upload 重开的 pass 约 4 个）；跟踪器重写会替换 fork 为 Android 调优的 RegionManager（256 KiB 分片 futex、写缺页预放、`SnapshotForUpload`、mprotect 合并）；fork 平铺暂存用自己的 VMA 临时缓冲而非 staging pool。整体搬入需要在这些机制上重新设计并实测，没有直接可取的独立修复。

## 未完成

- 失败会话的原因未在 UI 上显示。
- 高通驱动下地面偏暗、移动闪屏（用户后续处理）。
