# KH3：compute 写的目标不再把 G-buffer 拖成原生分辨率 / 偶发过曝的根因与修复（2026-10-13，本地未提交）

设备 AYN Odin3（Adreno 830，Turnip `65b63f66bf5`），王国之心 III（CUSA15072）奥林匹斯存档，Render Scale 75%（每游戏设置 `gpu.internal_scale=0.75`）。

## 1. 现象：GPU 受限，75% 渲染倍率只作用了一半

进场景后 GPU 98–99% 忙、因温控停在 832–880 MHz，约 35 FPS。`pass_log` + `gpu_timing detail`（`build/validation/odin3-kh3-preempt-20261012/Z-game-dbg/passlog.txt`，`passlog_rank.py`）：每帧约 145 个 render pass、848 个 draw，pass 合计 21.4 ms；最重的是 16 draw 的半透明粒子 pass（RGBA16F + 只读深度，3.8–5.5 ms）、180 draw 的 G-buffer pass（7 MRT，2.9 ms）、全屏 B10G11R11 合成（2.0 ms）、单 draw 7 附件 pass（1.5 ms），全部 1600×900。

`gpu_memory status`：`render_scale_percent=75`，但 `scaled_attachment_draws` 只占 44%，1600×900 的附件组全部 `reasons=64`（mixed-pass）`scaled=0`。

## 2. 原因：compute 先把目标当 storage 写，触发 mixed-pass 级联

- 光照 compute（cs `0x6eb33a260adfb6b4`，200×113 组、每像素一个线程，读 G-buffer 后在同一坐标 `image_store`）每帧把结果写进 1600×900 B10G11R11 目标。这张图第一次出现就是 storage，`Image` 构造时 `RequireNative("created as storage")`（这条路径不打日志），同时 `BindResources` 的缩放检查把所有被写的图像都当作 unsafe 而 `ForceNative`。
- 随后一个 graphics pass 把这张原生图和已缩放的主深度一起作附件 → `mixed attachment pass` → 主深度被提升为原生 → 共用深度的 G-buffer、粒子、后处理目标全部级联成原生。
- 1024×1024 RGBA32F（`0x2015000000` 一类）也由 compute 先写，同样触发 1024² MRT 组的级联（代价小）。

同一帧另有区域拷贝 cs `0xb903b5f213ef7d4d`（含 1600×900 与 400×225）、深度降采样 cs `0xf6074445e584dd9e`（800×450 RG16F）写图像。

## 3. 修改：compute 的普通 storage 写跟随图像的缩放

- 运行时（`vk_rasterizer.cpp` `ScalableStorageWrite`）：compute 阶段、非原子、非 `requires_native_scale`、无 post-op、绑定号 < `MaxScaledBinding`、2D 单层 mip0、非整数格式的写入，不再强制原生；纹理缓存还要求绑定视图就是整张图（同地址、同尺寸、同格式、1 层 1 级、非深度/压缩），否则仍按旧规则原生。这样新建的图以未定用途创建（原生 backing），之后第一次作渲染目标时按 render scale 重新分配并保留内容（`ObserveUsage(RenderTarget)` 原有逻辑）；只被 compute 写、从不作附件的图保持原生。
- shader（`emit_spirv_image.cpp` `SupportsScaledStorage` / `ScaleStorageCoords`）：compute 中对 2D storage 图像的写、读和尺寸查询，在绑定的缩放码非零时把 guest 坐标映射为 `floor((g + 0.5) × host / guest)`，与已有的采样读取映射一致；尺寸查询返回 guest 尺寸。缩放码为 0 时坐标不变。dispatch 仍按 guest 网格执行，多个 guest 线程落到同一个宿主 texel（0.75 时碰撞的线程都在同一个 8×8 组内）。
- `ShaderBinaryVersion` 39 → 40（compute storage 访问的 SPIR-V 变了，首次运行重编）。
- 开关：`debug.shadps4.scaled_storage_off=1` / `SHADPS4_SCALED_STORAGE_OFF=1` 恢复旧规则；诊断白名单 `debug.shadps4.scaled_storage_shaders=<hex,...>` 只让列出的 compute 程序缩放。

## 4. 结果

- 覆盖率：`scaled_attachment_draws` 44% → 95%，1600×900 附件组全部 `scaled=1`，`mixed-pass` 转换 27 → 3。
- 同一 APK、同一存档点开/关交替 4 对（`ab_scaled.sh`，冷启动 → 45 s → 两个 10 s 窗口，GPU 均 98% 忙、约 830–880 MHz）：开 46.9 FPS（45.2–48.6），关 35.1 FPS（33.1–37.6），**+33%**，区间不重叠。证据 `AA-scaledstore/ab/`。
- 画面：两边都正常的一对（p1）全分辨率中心区域对比，光照、阴影、材质一致，开的一侧因 75% 渲染略软，未见瑕疵（`ab/p1-compare.png`）。只看了奥林匹斯这一处，其他场景与其他游戏未验。
- 未改：compute 本身仍按 guest 分辨率执行（修改前每帧 14 个 dispatch 合计约 3.4 ms），只省下游 pass 的像素量。

## 5. 偶发过曝（与本修改无关，已修复，见 5.2）

进场景有时整片过曝（白、粉、绿、紫随机），并持续整个会话。10-13 下午约 17 次运行里 15 次出现，A/B 中开、关各 2/4。已排除（`AA-scaledstore/` 下各目录）：

- 本修改：关开关、旧 APK `588e3b13` 都复现。
- 10-12 的 `pool_undefined` / `pool_revive` / `expand_twin`：会话开始即关（会话内核实为 off）仍复现。
- PKG 读取（`pkg_reader=reference`）、跳过模式（`pipeline_cache mode async_accurate`，整局 0 跳过）、`readback_linear_images`、存档以外的设置文件。
- 自动曝光：EyeAdaptation（fs `0x00f0d250a8725c91`）的直方图输入是黑色 1×1，CPU 常量 Min = Max 平均亮度 = 1、ExposureScale = 1，即固定曝光 1.0（`upload_diag cb_dump` 导出）。

线索：一次过曝运行随后出现 KGSL `CP BV opcode error | opcode=0x70970001` → DeviceLost（上午 fault 75 同签名），当天片头保护写 #11 也明显增多；怀疑 GPU 写入落到错误地址、写坏只在加载时生成的资源（反射捕获 cube 数组、天空光 cube、调色 LUT 等），未验证。正常状态的输入纹理统计基准在 `AA-scaledstore/ts-r1/stats/stats.txt`。已另起任务继续排查。

### 5.1 后续定位（过程记录）

- 输入纹理对比（`texstats.sh`、`stats_by_role.py`，正常基准 `ts-r1`、过曝 `hunt2/run2`）：两张调色 LUT、天空光 cube 的 6 个有效面、曝光在好坏运行中一致；过曝时光照之前的场景色已经爆了（基础 pass 输出均值 0.02 → 54）。
- View 常量缓冲（色调映射 fs `0x44f1e20d409f43f7` 的 buf0，0xb10 字节，`upload_diag cb_dump <hash> 1 4096` 整段导出，`viewub.sh` / `viewub_diff.py`，`AA-scaledstore/viewub/`）：0x990–0x9fc 共 7 个 float4（按 UE4 布局为 `SkyIrradianceEnvironmentMap[7]`，天空光漫反射 SH）每轮都不同——过曝 run2 为几百、带 NaN，过曝 run3 约 1e-6，正常 run5/run6 为 0 或约 4e-7。其余字段除逐帧变化的计数外一致。
- 推断：SH 在加载阶段由 GPU 生成，CPU 直接读渲染目标内存（PS4 统一内存）；我们不把 GPU 结果写回 guest 内存，CPU 读到残留，为 0 时看起来"正常"（实际缺天空环境光），为垃圾时过曝/偏色，整局不变。按 UE4 4.18 的 `ComputeDiffuseIrradiance`，读回的是 9×1 RGBA16F 的 `SkySHIrradianceMap`，宽度 9 超过 `readback_linear_images` 的阈值（宽 ≤ 8），这与开该选项不能修好一致。**尚未验证**：下一步记录总像素 ≤ 16 的渲染目标，并对其做 EOP 写回，看 SH 是否各轮一致、过曝是否消失。加载阶段 trace（`loadtrace.sh`、`loaddraws.sh`）尚未抓到天空光捕获的帧。

### 5.2 根因与修复

完整记录（现象、排除项、根因、修复、验证、诊断方法、剩余问题）单独整理在 [kh3-sky-sh-washout-20261013](kh3-sky-sh-washout-20261013.md)，以下为摘要。

验证 5.1 的推断时，先把总像素 ≤ 16 的渲染目标在 EOP 写回，并加 `debug.shadps4.tiny_writeback=dump`：写回前逐 texel 打印 GPU 上的值，写回后打印 guest 内存，并打印触发前最近 768 条图像查找/上传/draw 记录（`AA-scaledstore/tinywb*`、`tinytrace/`、`adopt*/`）。

- 9×1 RGBA16F SH 图（Thin2DThin，guest 大小 0x10000）在一个 submit 内被画 9 次，每次一个系数；1×1 RGBA16F（约 0.203）是天空平均亮度，正常。
- 只写回时：CPU 读到的就是 GPU 写的值（View 常量 0x990.w = 20.5 = 系数 0 的 72.75 × 0.282），但 GPU 算出的系数本身是坏的——一轮系数 0 为 (72.75, 0, 114.56)（品红，对应粉色过曝），三轮 9 个系数全是 NaN（画面看着正常）。
- 记录显示系数 0 那一轮：scratch cubemap B（32×32、6 级、`0x20304e0000`）的 mip1（`0x20304f0000`，16×16×6 面）在 B 还不存在时作为渲染目标写入，纹理缓存为它建了独立图；之后采样整张 B 时新建 cubemap（`cands=1`），guest 上传 m0–m5，那张 mip1 图没有拷进来。`ResolveOverlap` 的 left overlap 分支只有在已有合并目标时才 `CopyMip`，FindImage 新建图的路径没有合并。之后的系数复用这张 cubemap 的 mip 视图，所以有时只有系数 0 坏。

修改（`texture_cache.cpp`，开关在 `upload_diag`，默认开）：

- `adopt_mips`（`AdoptGpuSubresources`）：FindImage 新建图后，范围内 GPU 写过、未被 CPU 改写、未绑定在当前 pass、且按 `MipOf`/`SliceOf` 正好是其某个 mip/slice 的旧图，在新图从 guest 上传之后 `CopyMip` 进去并释放，新图记为 GPU 写过。
- `tiny_writeback`（`NoteTinyTargetWrite`）：总像素 ≤ 16、单层单级、非 MSAA 的颜色渲染目标排进 `download_images`，在下一个 EOP/EOS/RELEASE_MEM 写 fence 之前同步写回 guest 内存（Android 上 `completion_fences` 默认关，写回前 `Finish`）。同一张图在相隔 < 250 ms 的连续 submit 中被写超过 4 次就视为每帧目标（曝光 1×1 R32G32、降采样 8×4/4×2/2×1 R16F），不再写回直到停用超过 250 ms；同一 submit 内的多次写只算一次。KH3 每次进场景写回 48 次，都在加载阶段。`debug.shadps4.tiny_writeback=0` 关闭，`=dump` 打开诊断。

两处都需要：只写回时 CPU 拿到的是坏的 GPU 结果（上面四轮）；只合并时 CPU 仍读 guest 残留（修改前各轮）。

结果（APK `3b4a18b4`，奥林匹斯存档冷启动）：有效 5 轮（dump 3 轮、默认 2 轮）画面都正常，9 个 SH 系数与 View 常量 0x990–0x9fc 各轮逐位相同（DC 约 (0.73, 0.70, 0.69)），每轮正好合并 1 张子资源图；另有 2 轮在片头遇到 #11 GPU 故障作废（与本问题无关）。修改前同一场景约 15/17 过曝。修复后天空环境光真正生效，与旧的“正常”（SH 为 0）相比阴影处略亮。未测其他场景、其他游戏和帧率影响（写回只在加载阶段发生，场景内每帧只多一次时间戳比较）。

## 6. 新增诊断（默认关闭）

- `upload_diag cb_dump <program_hash> [count]`：之后 `count` 次绑定该程序时，把各阶段绑定 buffer 的前 64 字节（guest 内存中的 CPU 数据）按 hex 与 float 写入日志。
- `upload_diag image_stats <guest_address>`：下次绑定该地址的图像时下载 mip0，按格式解码，每层（体纹理每切片）记录各通道 min/max/mean 与 NaN/Inf 数，最多 64 层。
- `upload_diag tiny_writeback on|off|dump`、`upload_diag adopt_mips on|off`；`upload_diag status` 增加 `adopt_mips`、`tiny_writeback` 两行计数。
- 脚本：`gamecmd.sh`（新会话 PID 出现后再发 DebugBus 命令，修正了早期脚本把命令发给上一个会话的竞态）、`gameshot.sh`、`texstats.sh`、`hunt_bad.sh`、`ab_scaled.sh`、`passlog_rank.py`。

## 7. 设备状态

`files/host/config.json` 测试中改过两次（readbacks、readback_linear_images），已按原字节恢复（SHA 前缀 `e6b6682ba630ff98`）；中途一次恢复失败使一轮运行时该文件为空，已更正。存档在每次运行时被游戏重写，坏状态时的副本在 `AA-scaledstore/save-1330/`（无更早的好存档）。
