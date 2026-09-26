# 血源切场景崩溃：DRS 回写覆盖游戏堆（2026-09-26）

分支 `feature/malos/sparse_arena`，AYN Thor `9c2841a4`，mainline Turnip（`debug.shadps4.vulkan_driver=turnip-mainline`），Render 0.5。全部为本地提交，未推送。

## 1. 启动即失败：#5044 移植引入的链接器回归（`6da66f78`）

用户 18:51 / 18:52 两次启动血源，以及 18:56 用 `open_last_game` 启动，都在 Prepare 阶段失败（logcat `ProductionRuntime: prepare: failed: unresolved guest data import: module_stop#libSceFios2#1#libSceFios2#NoType`，host 日志停在 `symbol visibility !=0`）。

- 原因：libSceFios2 用一个带非默认可见性的全局 `NoType` 符号重定位自己的 `module_stop`，名字不带 `#`。移植上游 #5044 之前，这类名字直接判为未解析、保留加载值；移植后先在模块自身导出里找，找不到时 guest 后端把它当数据导入抛异常。没有导出的模块（如 eboot）还会走到 `unknown module/library`。fork 适配时另给其它裸名字加了抛异常，也比移植前和上游更严格。
- 修复：导出里找到时照上游绑定；找不到时恢复移植前的“未解析、不绑桩函数/HLE 记录”，其它裸名字按上游记错误返回。
- 结果：同 APK 血源越过 Prepare，进入提示框和标题菜单（30 FPS）。

## 2. 切场景崩溃：定位

崩溃（19:02:39 PID 7493，及后续复现）：`Guest-1` `SIGSEGV SEGV_MAPERR fault 0x7e007e007dfe`，`fex-fault` 块 `0x2486db3`、RIP `0x2486db7` = eboot+0x2086db7。反汇编（`tools/ps4-guest-code`）为 `DLRegularHeap::Free`（eboot+0x2086c90）合并下一个空闲块：`rdx = [next+8]`，`mov [rdx-2], rax` 时 `rdx = 0x7e007e007e007e00`（四个 FP16 NaN）。被释放块起于 0x25bc90000，下一个块头在 0x25c0f0040。与桌面记录的 Dantelion 堆 panic（同一函数、块头被清零）是同一函数。

新增默认关闭的 guest 写回诊断 `guest_write_watch`（`2f410a1f`）：GPU 产生的数据进入 guest 内存只经过 `MemoryManager::TryWriteBacking`（缓冲下载、图像回写、copy-shader HLE 提交、EOP/EOS 标签）和 `CopyGuestRegions`（HLE guest 拷贝），在这两处检查 64 位模式（默认 `0x7e007e007e007e00`）或地址范围，记录写入者。`debug.shadps4.guest_write_watch=1` 在渲染器启动时开启。

开启诊断后的一轮复现（PID 17063）在崩溃前 11 秒记录：

```
guest write watch #0 t=34724ms image_writeback detail=0x25bc90000 dst=[0x25bc90000,0x25c110000) matches=589824 first=0x25bc90000
```

即 GpuComm 把 0x25bc90000 上一张图像整 0x480000 字节写回 guest 内存，全部是 FP16 NaN（589824 × 8 字节 = 1024×576 RGBA16F）。游戏在这个地址重新分配的块只有 0x460040 字节，下一个块头 0x25c0f0040 落在写回范围内。

## 3. 根因

`TextureCache::ResolveOverlap` 的 DRS 分支（fork c0d6352c 引入，上游没有）：新请求的渲染目标/深度/VideoOut 与缓存图像地址相同、尺寸不同时，为保留“历史像素”，把旧图像按旧的完整 guest 大小同步写回，再释放。

血源切场景时释放旧渲染目标，游戏堆在同一地址重新分配更小的资源，并在旧图像范围内写下一个块的块头。CPU 这次写入触发写保护，图像被标成 CPU 脏并取消跟踪；但 DRS 分支先 `TrackImage` 重新跟踪整张图（丢掉了这次 CPU 写入的记录），`RefreshImage` 又为了保护 GPU 修改的数据不重新上传，随后整段写回，用旧像素覆盖了块头。几秒后游戏释放块、合并时读到 NaN 指针。

GC 淘汰路径此前已按同类问题改为只写回仍在跟踪的页（`tracked_only`），DRS 分支没有跟上。回写队列（`readback_linear_images`）默认关闭，且只含线性图像；这张图像是 tiled 渲染目标，只可能来自 DRS 分支。

## 4. 修复

DRS 分支不再重新跟踪和刷新旧图像；只写回仍在写跟踪中的页，并截断在新描述符的 `[地址, 地址 + guest_size)` 内。新图像只从这段读取历史像素，范围之外的内存归属已不明确。本例中堆已经写过这些页（图像已取消跟踪），因此不再写回任何字节。各回写路径在诊断里分别标为 `image_writeback_queue` / `_drs` / `_gc` / `_diagnostic`。

## 5. 验证

修复前（均为冷启动 → 提示框 → 离线 → 继续，即标题到猎人梦境的一次切场景）：有效 5 轮中 2 轮在加载中崩溃（19:02 PID 7493，PID 17063），另有用户手动操作两次崩溃。

修复后（APK 内 host `b1ff90ca…`，写回诊断开启）：自动化脚本 6 轮，每轮都进入猎人梦境（截图确认），每轮检查约 128 万次 guest 写回、0 次 NaN 写回、0 次崩溃。中间一组 5 轮因脚本等会话就绪后按键落空、停在主菜单，没有经历切场景，不计入；其中一次“崩溃”是另一个进程（`.relWithDebInfo` 的 `VulkanPresentAs`），与 shadPS4 无关。

诊断属性已恢复为空；最后一轮的血源会话留在猎人梦境。

## 6. 未做 / 边界

- 诊断只覆盖 GPU 数据进入 guest 内存的路径，不覆盖 guest CPU 自身写入。
- 那张 1024×576 RGBA16F 图像全部是 NaN，说明它的 backing 可能从未被真正绘制（例如缩放后的 backing 未初始化），是否存在单独的渲染问题未查。
- 只在 AYN 上用“标题 → 猎人梦境”一次切场景复现和验证；其它区域传送、死亡重生等切换未逐一测试。
