# KH3 进场景偶发过曝/偏色（#16）：天空光 SH 丢了 GPU 写的 mip，CPU 读回不到 GPU 结果（2026-10-13，已修复，本地未提交）

设备 AYN Odin3（Adreno 830，Turnip `65b63f66bf5`），王国之心 III（CUSA15072，Ver 1.10）奥林匹斯存档，Render Scale 75%。问题汇总见 [bugs/kh3-cusa15072 #16](../../../bugs/kh3-cusa15072/README.md)，排查过程的前半段记在 [kh3-scaled-storage-20261013](kh3-scaled-storage-20261013.md) 第 5 节，原始数据摘录在 [evidence/kh3-sky-sh-washout-20261013](evidence/kh3-sky-sh-washout-20261013/)。

## 1. 现象

- 冷启动读档进奥林匹斯，场景有时整片过曝或偏色（白、粉、绿、紫），持续整个会话，不会自行恢复。10-13 下午约 17 次进场景 15 次出现；开关 Render Scale 的 storage 缩放、10-12 的池内存特性都照样出现。
- 偏色每次不同。用户最初怀疑与 10-12 的 32×32×32 LUT 不重传改动有关；实际无关（第 2 节）。
- 看起来“正常”的运行其实也不对：天空光的漫反射项是 0 或 NaN，只是没有表现成偏色（第 4 节）。

## 2. 排除项

| 怀疑对象 | 做法 | 结论 |
|---|---|---|
| 10-13 的 storage 写缩放 | `debug.shadps4.scaled_storage_off=1`、旧 APK `588e3b13` | 照样出现 |
| 10-12 池内存特性（含 LUT 不重传 `expand_twin`） | 会话开始即 `upload_diag pool_undefined/pool_revive/expand_twin off` | 照样出现；两张 LUT 在好坏运行中一致 |
| PKG 读取 | `pkg_reader=reference` | 照样出现 |
| 管线跳过模式 | `pipeline_cache mode async_accurate`，整局 0 跳过 | 照样出现 |
| 自动曝光 | `upload_diag cb_dump` 读 EyeAdaptation（fs `0x00f0d250a8725c91`）常量 | 直方图输入为黑色 1×1、Min = Max = 1，曝光固定 1.0 |
| 线性小图回读 | `readback_linear_images` | 照样出现（SH 图是 tiled、宽 9，不在其范围内） |
| 天空光 cubemap、两张调色 LUT | `upload_diag image_stats` 按地址逐层统计，与正常基准对比 | 好坏运行一致 |

关键线索来自 View 常量（色调映射 fs `0x44f1e20d409f43f7` 的 buf0，`cb_dump` 整段导出）：0x990–0x9fc 的 7 个 float4 每轮都不同，其余字段一致。按 UE4 布局这是 `SkyIrradianceEnvironmentMap[7]`，即天空光漫反射的 3 阶 SH（9 个系数 × RGB）。过曝的 run2 为几百且带 NaN，正常的 run5/run6 为 0。场景色在光照之前的基础 pass 就已经爆了（输出均值 0.02 → 54）。

## 3. 根因

### 3.1 UE4 怎么算天空光 SH

UE4 4.18 的 `ComputeDiffuseIrradiance` 在加载时对天空 cubemap 做 SH 投影：

1. 对每个系数 c（共 9 个）：
   1. 把天空 cubemap 乘上第 c 个 SH 基函数与立体角权重，画进 scratch cubemap 的 mip0（6 个面各一次 draw）；
   2. 逐级降采样：mip m 读另一张 scratch cubemap 的 mip m−1，两张 scratch（A、B）按 mip 奇偶交替，直到 1×1；
   3. 读最后一级 1×1 的 6 个面求和，画进 9×1 RGBA16F 的 SH 图（`SkySHIrradianceMap`）第 c 个 texel。
2. CPU 用 `ReadSurfaceFloatData` 读回 SH 图，换算后写进 View 常量的 `SkyIrradianceEnvironmentMap`。

KH3 里两张 scratch 是 32×32、6 级的 RGBA16F cubemap（本例 A = `0x2022c80000`，B = `0x20304e0000`），SH 图是 9×1 RGBA16F、`Thin2DThin`、guest 大小 0x10000。同一批还画一张 1×1 RGBA16F（约 0.203，天空平均亮度），也由 CPU 读回。

### 3.2 GPU 端：新建 cubemap 时丢了先写好的 mip

第一个系数时两张 scratch 都是第一次用（`flight-record-coeff0.txt`，诊断见第 6 节）：

1. A 的 mip0 六个面依次作为渲染目标写入。每个面的请求层数递增（1→6 层），同地址扩展（`ExpandImage`），内容保留，最终 img616 = A mip0 的 6 个面。
2. B 的 mip1（`0x20304f0000`，16×16×6 面）作为渲染目标写入。此时 B 这张 cubemap 还不存在，纹理缓存为 mip1 单独建了图（同样逐面扩展，最终 img608）。这一步读的是 A（新建 img613，从 img616 扩展，mip0 保留）。
3. 下一级读整张 B（`find 1 req 0x20304e0000 32x32 L6 A8`）。缓存里与之重叠的只有 img608（`cands=1`），于是新建 img607，并从 guest 内存上传 m0–m5。**img608 里的 GPU 结果没有拷进 img607 的 mip1。**

代码位置：`TextureCache::FindImage` 先找完全匹配，再对每张重叠图调用 `ResolveOverlap`。旧图是新图的某个 mip（left overlap）时，只有在已经存在合并目标（`merged_image_id`）的情况下才 `CopyMip`：

```cpp
// We need to have a larger, already allocated image to copy this one into
if (merged_image_id) {
    merged_image.CopyMip(cache_image, mip, slice);
    FreeImage(cache_image_id);
}
```

FindImage 新建图的路径不再处理这些旧图，新图整张从 guest 内存上传。由于渲染结果不写回 guest 内存，那里只有该段池内存的残留（各轮不同，可能是 0、大数或 NaN 的 bit pattern）。残留沿 mip 链降到 1×1，SH 系数随之变成大数或 NaN。之后的系数复用 img607 的 mip 视图写入，不再丢失，所以有时只有系数 0 是坏的。

实测（`sh-map-texels.txt`，此时只加了写回、还没修这里）：一轮系数 0 = (72.75, 0, 114.56)，其余系数正常，即品红色 DC 项，对应那轮的粉色过曝；三轮 9 个系数全为 NaN 或部分通道 NaN。

### 3.3 CPU 端：读回 9×1 SH 图读到的是 guest 残留

shadPS4 的渲染结果留在 VkImage 里，不写回 guest 内存。PS4 是统一内存，游戏可以在 GPU 完成后直接读渲染目标所在的内存。已有的写回只覆盖两类：buffer cache 的 readback 模式；以及 `readback_linear_images`（线性或宽 ≤ 8 的图，默认关）。9×1 tiled 的 SH 图不在其中，CPU 读到的是该地址的残留。

### 3.4 两处叠加

| 情形 | GPU 上的 SH 图 | CPU 读到（View 常量） | 表现 |
|---|---|---|---|
| 修复前 | 系数坏（3.2） | guest 残留，与 GPU 结果无关 | 残留是有限大数时偏色/过曝；为 0 时看似正常但缺天空光 |
| 只加写回 | 系数坏 | GPU 的坏结果 | 1 轮粉、2 轮偏绿、4 轮 NaN（看似正常，缺天空光） |
| 写回 + 合并 mip | 正确 | 正确，各轮逐位一致 | 5/5 正常 |

修复前 viewub/run3 过曝但 View 常量 SH 约 −4e-7，这一轮不能用 View 常量解释；可能是 GPU 上坏掉的 SH 图被其他 pass 直接采样，或 1×1 平均亮度读回到残留，未验证。修复后两条路都已正确，5 轮均未再出现。

## 4. 修复

均在 `src/video_core/texture_cache/texture_cache.cpp`，开关在 `upload_diag`，默认开。

### 4.1 新建图时合并 GPU 写过的子资源（`adopt_mips`）

`TextureCache::AdoptGpuSubresources`：FindImage 在重叠解析之后仍需新建图、且范围内原有其他图时调用。对范围内每张旧图，满足以下全部条件即视为新图的一个子资源：

- 起始地址在新图之后（同地址的由 `ResolveOverlap` 扩展或复用）；
- GPU 写过（`GpuModified`），且没有被 CPU 改写（无 `Dirty`）；
- 没有绑定在当前 pass（`binding.is_bound/is_target`，这种情况保留原来的重新绑定处理）；
- 采样数相同，`MipOf`/`SliceOf` 能确定它正好是新图的某个 mip/slice（同时保证尺寸、pitch、格式兼容，满足 `CopyMip` 的断言）。

对这些旧图：先对新图做 guest 上传（`TrackImage` + `RefreshImage`，与 `ExpandImage` 的先上传后拷贝一致），再逐个 `CopyMip` 进去并释放旧图，新图记为 `GpuModified`。KH3 一个会话只触发 1 次（就是 3.2 的那张 mip1）。`upload_diag adopt_mips off` 回到旧行为，`upload_diag status` 的 `adopt_mips` 行给出次数。

### 4.2 偶尔写入的极小渲染目标写回（`tiny_writeback`）

`TextureCache::NoteTinyTargetWrite`（`FindRenderTarget` 调用）：总像素 ≤ 16、单层单级、非 MSAA、非深度的颜色渲染目标排进 `download_images`，在下一个 EOP/EOS/RELEASE_MEM 写 fence 之前写回 guest 内存（Android 上 `completion_fences` 默认关，此时 `Finish` 后同步写回，保证 CPU 等到 fence 时数据已在内存里）。写回走已有的 `RecordImageDownload`，按 guest 的 tiled 布局重新铺排（实测 SH 图 texel 依次落在 +0x0、+0x8、+0x20、+0x28、+0x80……）。

每帧都画的小目标（KH3 的曝光 1×1 R32G32、降采样链 8×4/4×2/2×1 R16F）不能每帧同步：同一张图在相隔 < 250 ms 的连续 submit 中被写超过 4 次就不再写回，停用超过 250 ms 后重新计数；同一 submit 内的多次写只算一次（SH 图一个 submit 内被画 9 次）。KH3 每次进场景写回 48 次，都在加载阶段，场景内每帧只多一次时间戳比较（`per_frame_skips` 计数）。

开关：`upload_diag tiny_writeback on|off|dump`；启动时 `debug.shadps4.tiny_writeback=0` 关闭、`=dump` 打开诊断（环境变量 `SHADPS4_TINY_WRITEBACK=0` 同效）。

两处缺一不可：只写回时，CPU 拿到的是坏的 GPU 结果（第 3.4 节表）；只合并时，GPU 上的 SH 正确，但 CPU 仍读残留。

## 5. 验证

APK `3b4a18b4`，奥林匹斯存档冷启动，每轮约 20 s 后截图并导出 View 常量（`runs.md`、`view-ub-sky-sh.txt`）：

- 有效 5 轮（dump 3 轮、默认配置 2 轮）画面全部正常，中心区域均值 RGB 约 (135, 123, 90)，与修复前看似正常的轮次一致。
- 9 个 SH 系数在 GPU 上各轮逐位相同：DC 项约 (0.730, 0.697, 0.689)，其余在 −0.22…0.86；View 常量 0x990–0x9fc 各轮逐位相同（0x990 = 0.0243, 0.0090, 0.2204, 0.2006）。
- 每个会话正好合并 1 张子资源图，写回 48 次。
- 另有 2 轮在片头遇到 #11（`CP Protected mode error`）作废，与本问题无关。

修复前同场景约 15/17 过曝。修复后天空环境光真正生效，但在这个场景里贡献很小，画面与之前看似正常的轮次肉眼无差别。

未覆盖：其他场景、其他游戏（凡是“先画 mip 再整张采样”或“CPU 读回小渲染目标”的都可能受影响，但未实测）；帧率对比（写回只在加载阶段发生，合并只在新建图时检查，预计无影响，未测）；Pocket DS、桌面。

## 6. 诊断方法（同类问题怎么查）

- **CPU 侧常量**：`upload_diag cb_dump <pgm_hash> [count] [bytes]` 导出某着色器绑定的 buffer（guest 内存中的 CPU 数据），多轮比较能直接看出哪段常量每轮不同。脚本 `build/validation/odin3-kh3-preempt-20261012/viewub.sh`（冷启动 → 截图 → 导出色调映射 fs 的 View 常量 → 收集写回日志），`viewub_diff.py` 逐 float4 比较。
- **GPU 侧纹理**：`upload_diag image_stats <guest_address>`，下次绑定该地址时下载 mip0，每层统计 min/max/mean/NaN；≤ 16 texel 的图逐 texel 打印。
- **CPU 读回的小目标**：`debug.shadps4.tiny_writeback=dump`（需在会话开始前设置）：每次写回前逐 texel 打印 GPU 内容，写回后打印 guest 内存前 512 字节；第一次写单行小目标（高 1、宽 ≥ 5，如 SH 图）时，打印此前最近 768 条记录：
  - `find <用途> req <地址>+<大小> 宽x高x深 L<级数> A<层数> <格式> t<类型> -> img<id> <地址> … view m<mip> s<slice> cands=<重叠图数> created_by=<0 新建/1 扩展> uploaded=<…>`（用途 1 采样、2 storage、3 渲染目标）；
  - `upload <地址> … guest mips: m0 m1 …`：从 guest 内存上传了哪些 mip；
  - `action 0:<vs> 3:<ps>`：draw 的着色器，之前的渲染目标查找与之后的纹理查找属于这个 draw；
  - `adopt img<id> … -> img<id> m<mip> s<slice>`：4.1 的合并。
  读法：先找 SH 这类结果图第一次写入前，被采样的图是否 `cands>0` 却新建并整张 `upload`——这就是“GPU 结果被 guest 残留覆盖”的特征。
- **调试器**：10-13 起本机（Windows）已配置 host LLDB（NDK r29 lldb 21 + CodeLLDB 适配器，`libshadps4_host.so` 符号按 Build ID 匹配）和 guest debugger 权限，可用于定位游戏 CPU 何时读回、读的是哪个地址。

## 7. 剩余问题与边界

- `ResolveOverlap` 的 left overlap 分支在旧图正被当前 pass 绑定为渲染目标时（`is_target`），仍是置 `needs_rebind` 后直接释放、不拷贝内容；本次没有改，KH3 中未观察到触发。
- 合并只处理按 `MipOf`/`SliceOf` 精确对应的子资源；部分重叠、格式不同的情况仍按旧规则。
- 写回只覆盖总像素 ≤ 16 的颜色渲染目标。CPU 读回更大的渲染目标、或每帧都读回的小目标（连续 4 个 submit 之后），仍读不到 GPU 结果。通用做法是对 GPU 写过的图所在页加读保护，CPU 读时再下载，尚未实现。
- 修复前 viewub/run3（SH ≈ 0 却过曝）的具体通路未验证，见 3.4。
- 加载阶段的同步写回会让 CPU 等 GPU（48 次），对 loading 时长的影响未测。
- 另见 dump 日志：会话开始时每帧的 R16F 降采样链（4×2、2×1）里有 −50304、−1963 这类值，是 logo/菜单阶段的曝光链读到了未初始化输入，与本问题无关，未查。

## 8. 证据位置

- 提交的摘录：`evidence/kh3-sky-sh-washout-20261013/`：`runs.md`（各轮 APK、改动、亮度、SH）、`view-ub-sky-sh.txt`（各轮 View 常量 0x990–0x9fc）、`sh-map-texels.txt`（GPU 上 SH 图逐系数内容与写回后的 guest 字节）、`flight-record-coeff0.txt`（系数 0 那段的图像查找/上传记录）。
- 本地原始数据（未提交，含游戏截图）：`build/validation/odin3-kh3-preempt-20261012/AA-scaledstore/` 下 `viewub/`（修复前）、`tinywb/`、`tinywb-dump/`、`tinytrace/`（只写回）、`adopt/`、`adopt-default/`（修复后）；APK 在同级 `apk/`（`kh3-cbdump2-9474878f`、`kh3-tinywb-b9eba66a`、`kh3-tinywb2-42293d18`、`kh3-tinytrace-47e82ab7`、`kh3-adoptmips-3b4a18b4`）。
