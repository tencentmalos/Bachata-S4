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

桌面（RX 7600M XT）：Bloodborne 进入"未以退出游戏结束会话"提示框，画面正常；PostMessage 按键未送达未聚焦窗口，未进入世界。

## 未合入的上游 PR

- #5069 / #5096：Linux userfaultfd 专用（注册、合并 Unmap、按 GPU 线程 tid 走 `assume_locks`）。fork 的 Android/Windows 走信号路径，fault 在 GPUComm 线程上发生时 `SendCommand` 已直接执行，行为等价。
- #5113：针对上游新的 session 调度器；fork 的 `on_submit` 已在结束命令缓冲之前调用。
- #5119：fork 已去掉启动时的 `io.Fonts->Build()`，且有自己的 `RendererHasTextures`（SDF 字体整图替换）实现；上游的 Vulkan 纹理管理与之重复。
- #5062（主机着色器构建期编译为 SPV）：与 fork 的 `tiling.comp`（融合回读 FROM_IMAGE）、运行时编译的 ASTC/BC7 编码器冲突，需单独移植。
- #5100（内存跟踪器重写 + 批量上传）：与 fork 为 Android 调优的 RegionManager（futex 分片、写缺页预放、`SnapshotForUpload`）冲突，需单独设计。

## 未完成

- 高通系统驱动需要按 `maxBufferSize` 动态确定 arena 大小（及跨 arena 合并的回退）。
- 失败会话的原因未在 UI 上显示。
- AYN 上 `files/host/log/android-host_1.log` 为 34 GB（09-26 11:12，日志刷屏时期的旧轮转文件），占用设备存储，未删除。
