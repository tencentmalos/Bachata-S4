# Android 纹理压缩与物理缩放核对（Ayn / Turnip）

日期：2026-09-19

> 本文保留实现前的审计与计划。后续已落地的 internal scale / Foundation ASTC 及验证边界见 [Internal Scale 实现与验证](internal-scale-20260919.md)；下文“尚未实现”等描述只属于当时状态。

## 结论

当前 Ayn 运行的是自带的 Turnip：`Turnip Adreno (TM) 740`，Mesa `26.0.0-devel (git-5ac41be677)`。本次在应用实际创建的 Vulkan `VkPhysicalDevice` 上记录到：

```text
Compressed texture features: BC=true ASTC_LDR=true
Bc1RgbaUnormBlock: sampled=true transfer_src=true transfer_dst=true
Bc2UnormBlock:     sampled=true transfer_src=true transfer_dst=true
Bc3UnormBlock:     sampled=true transfer_src=true transfer_dst=true
Bc4UnormBlock:     sampled=true transfer_src=true transfer_dst=true
Bc5UnormBlock:     sampled=true transfer_src=true transfer_dst=true
Bc6HUfloatBlock:   sampled=true transfer_src=true transfer_dst=true
Bc7UnormBlock:     sampled=true transfer_src=true transfer_dst=true
Astc4x4UnormBlock: sampled=true transfer_src=true transfer_dst=true
Astc6x6UnormBlock: sampled=true transfer_src=true transfer_dst=true
Astc8x8UnormBlock: sampled=true transfer_src=true transfer_dst=true
```

因此，**当前 Ayn 的 BC 路径可以保持不变**。它不是先解压成 RGBA 再上传：

```text
PS4 FormatBc1..FormatBc7
  → LiverpoolToVK::SurfaceFormat
  → VkFormat eBc*Block
  → ImageInfo::UpdateSize（按 4×4 block 计算 guest 布局）
  → TileManager::DetileImage
  → TextureCache::RefreshImage
  → Image::Upload
  → vkCmdCopyBufferToImage
  → shader 直接采样压缩 VkImage
```

`copyBufferToImage` 只搬运已经按 BC block 排布的数据，不负责压缩格式转换。`VkPhysicalDeviceFeatures::textureCompressionBC=true` 和逐格式 `sampled=true` 表示当前 Turnip 对 BC1～BC7 提供了 Vulkan 压缩采样能力；shadPS4 自身没有 CPU BC 解码、BC→RGBA 转换或 BC→ASTC fallback。当前 `GetSupportedFormat` 仅有 D16/D24/D32 和 R8 sRGB 的替代分支，BC 不支持时会保留原格式并在图像创建阶段失败/告警，不会静默切换到软件实现。

后续已追到 Turnip 的硬件描述符：**Ayn 的 Adreno 740 本身支持 BC 纹理采样解码，Turnip 负责把 Vulkan 格式映射到硬件格式，没有在这条路径里用 CPU 或 compute shader 解码 BC，也没有转成 ASTC。** Ayn 当前 Qualcomm 系统驱动同样暴露 BC 支持。以下源码证据补足了仅靠 feature 查询无法证明硬件实现的缺口。

## Turnip 源码与 Qualcomm 系统驱动复核

### 版本与证据来源

设备上一次应用日志的 Turnip 基线为 `5ac41be677`。本轮从本地 Mesa Git 对象库取出了完整提交 `5ac41be6777ce7106645d83c08a3f65b54e43d16`（2026-01-08），使用 `git show <commit>:<path>` 审计，未使用本地工作树的新版本来代替这个提交。源码仓库为 `/Users/bytedance/workspace/bug_reports/references/mesa_freedreno_gallium`。这是驱动报告的上游基线；不代表对已安装二进制的所有下游补丁做了逐字节还原。

### 1. BC 直接映射到 GPU 纹理格式

[`src/vulkan/util/vk_format.c:196`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/5ac41be6777ce7106645d83c08a3f65b54e43d16/src/vulkan/util/vk_format.c#L196) 将 Vulkan BC 格式映射到 Mesa pipe 格式；[`fd6_format_table.c:308`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/5ac41be6777ce7106645d83c08a3f65b54e43d16/src/freedreno/fdl/fd6_format_table.c#L308) 再映射到 Adreno 的硬件纹理格式：

| Vulkan 格式族 | Mesa 格式族 | Adreno 硬件格式 |
| --- | --- | --- |
| BC1 | DXT1 | `FMT6_DXT1`（0xb3） |
| BC2 | DXT3 | `FMT6_DXT3`（0xb4） |
| BC3 | DXT5 | `FMT6_DXT5`（0xb5） |
| BC4 | RGTC1 | `FMT6_RGTC1_UNORM_FAST / SNORM_FAST` |
| BC5 | RGTC2 | `FMT6_RGTC2_UNORM_FAST / SNORM_FAST` |
| BC6H | BPTC_RGB | `FMT6_BPTC_UFLOAT / FLOAT`（0xbe / 0xbf） |
| BC7 | BPTC_RGBA | `FMT6_BPTC`（0xc0） |

表中的 `_T_` 宏写入 `.tex = FMT6_*`，表示纹理采样格式；并不是一个软件 decoder 的入口。硬件编码值定义在 [`a6xx_enums.xml:140`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/5ac41be6777ce7106645d83c08a3f65b54e43d16/src/freedreno/registers/adreno/a6xx_enums.xml#L140)。

### 2. 格式值进入硬件采样描述符

[`fd6_view.cc:202`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/5ac41be6777ce7106645d83c08a3f65b54e43d16/src/freedreno/fdl/fd6_view.cc#L202) 调用 `fd6_texture_format(args->format, ...)`；随后 A6xx/A7xx 分支在 246 行通过 `A6XX_TEX_CONST_0_FMT(texture_format)` 写入纹理描述符。BC 数据地址、尺寸、mip 和格式一起交给 GPU 纹理单元。Adreno 740 使用这一 A7xx 路径；并没有把采样 shader 改写成 BC 解码程序。

### 3. 上传按压缩 block 原样复制

[`tu_clear_blit.cc:1804`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/5ac41be6777ce7106645d83c08a3f65b54e43d16/src/freedreno/vulkan/tu_clear_blit.cc#L1804) 的 `copy_format` 把 8 字节 block 临时视为 `R32G32_UINT`，16 字节 block 视为 `R32G32B32A32_UINT`。`copy_compressed`（2401 行）把像素坐标/extent 换算为 block 数；`tu_copy_buffer_to_image`（2428 行）用 `r2d_ops` 记录复制命令。

这是按 block 原始位模式执行的 GPU 复制，整数格式只是复制用视图，不是解压后 RGBA 像素。图像采样时仍使用上一节的 BC 硬件格式。驱动内部使用 2D copy/blit 引擎也不意味着新增了一次全屏渲染或 BC 解码 pass。

### 4. 缺少硬件能力时，源码直接关闭支持

[`tu_device.cc:397`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/5ac41be6777ce7106645d83c08a3f65b54e43d16/src/freedreno/vulkan/tu_device.cc#L397) 明确标注 A702 没有 BC6H/BC7，并把 `textureCompressionBC` 设为 `!is_a702`。`fd6_texture_format_supported` 也会拒绝 A702 的 BPTC 格式，没有在这里安排软件解码补齐。这一例外不能套用到当前 Adreno 740，也说明不能概括成所有 Qualcomm/Android GPU 都支持完整 BC。

### 5. Ayn 的 Qualcomm 系统驱动也暴露 BC

本轮重新执行 `adb -s 9c2841a4 shell cmd gpu vkjson`，不切换游戏驱动、不重启游戏。查询返回：

- GPU：`Adreno (TM) 740`。
- 驱动：`Qualcomm Technologies Inc. Adreno Vulkan Driver`，`driverID=8`。
- Driver Build：`69e13475cb, I1df7ad3aa9`，日期 `12/27/23`。
- `textureCompressionBC=1`，`textureCompressionASTC_LDR=1`。
- Vulkan 格式 131～146（BC1～BC7 全部 16 个 RGB/RGBA、UNORM/SRGB/SNORM/浮点变体）均返回 `optimalTilingFeatures=128001`，包含 sampled、linear filter、transfer src/dst。

这份查询证明系统 Qualcomm 驱动也提供 BC API 能力；结合 Turnip 的硬件格式/描述符链，结论是 **BC 支持来自 Adreno 740 硬件，Turnip 将其接到 Vulkan 接口**。本轮未反汇编闭源 Qualcomm 驱动，未测量两种驱动的 BC 性能差异。设备查询摘要、原始输出 SHA256 和审计源文件 SHA256 已存于 [ayn-bc-native-20260919.json](evidence/ayn-bc-native-20260919.json)。

### 对接策略

保留当前原尺寸 BC 上传/采样路径。0.5 有现成 mip 时仍可直接保留 BC 并物理减少 host mip 链；只有需要实际重采样、没有可用源 mip 的情况才进入计划中的 ASTC encode 链路。本轮只核对源码和能力，没有修改编码或上传实现。

## 与物理缩放的关系

### 1.0：继续使用现有 BC 路径

默认模式不改格式、不改 guest 地址和 mip 布局。BC 图像在 Ayn 上继续以 `eBc*Block` 创建并通过 `copyBufferToImage` 上传。

### 0.5：优先采用 mip 级跳过

对于只读、仅被采样的纹理，可以把 guest mip 1 作为 host mip 0：

- host image 的首级尺寸约为原图一半，后续 mip 同步右移一级；
- host 只分配保留的 mip 链，实际 Vulkan allocation 和采样带宽都会下降；
- 源仍然可以保持 BC，若设备不支持 BC，再进入 ASTC 转码分支；
- guest 原始 `guest_address/guest_size`、mip offset 和脏页覆盖范围必须保留，用于 cache 失效和 detile；
- 不能对 render target、storage image、深度图、别名资源或需要原始尺寸的 copy/resolve 直接套用。

这不是改变 `copyBufferToImage` 的语义，而是改变 host image 的 mip 映射和分配大小。实现前必须在 TextureCache 的 overlap/reuse 判断中把缩放 image 与未缩放 render target 分开，否则会把不同尺寸的资源错误复用。

### 0.75：不能只靠 mip 跳过

0.75 没有对应的整数 mip 级别。要保持真实的 0.75 尺寸，需要：

```text
guest BC/未压缩数据
  → detile + block decode（或直接读未压缩 staging）
  → 0.75 重采样
  → ASTC 4×4/6×6/8×8 encode
  → ASTC VkImage + copyBufferToImage
```

这条链会增加一次 warmup，但能真正减少长期纹理 allocation 和采样带宽；不能用普通 RGBA 中间图冒充“保持硬件压缩”。ASTC 是目标存储格式，必须由真实 encoder 产生合法 ASTC block。

### Render Target

纹理 mip 缩放和 render target 缩放是两套逻辑。render target 需要同时处理 image extent、viewport/scissor、resolve/copy、guest readback 和别名关系；当前尚未把它宣称为已完成。最终 guest→host 上屏仍保持单次现有路径，不能在 Presenter 再额外做一次 FDM/blit。

## Foundation 下沉边界

Foundation 应提供可复用的 Vulkan 能力层，而不是让每个模拟器各自复制编码器：

1. 压缩格式能力查询：BC/ASTC feature、逐格式 sampled/transfer、block 尺寸和字节数。
2. `AstcEncodeRequest`：输入格式、尺寸、stride、色彩空间、ASTC block 规格、质量/速度档位、输出 buffer。
3. Vulkan staging、image barrier、dispatch/copy 和异步回收；不把模拟器的 guest 地址、tile mode 或 cache 句柄带进 Foundation。
4. encoder backend 明确区分 GPU runtime encoder 和 CPU warmup fallback；没有合法 encoder 时返回“不支持”，禁止返回原始 RGBA 并假称 ASTC。

当前 shadPS4 只落了能力探测和路径审计，**尚未把 GPU ASTC encoder 接入生产上传链**。0.5 mip 跳过可以先独立实现；0.75 和 BC→ASTC 需要 Foundation encoder 合同稳定后再接入。

补充本地 fork 核查：`/Users/bytedance/workspace/emulations/3ds/azahar` 已有 `EncodeAstc4x4`、`CompressAstcIfEligible` 和编码完成后回收 Scaled 图像的链路。此前仅查公开版本，不能代表该本地实现。现有 `astc_encode_4x4.comp` 对每个 4×4 块求平均后写 ASTC void-extent 常量色块，合法但会丢失块内细节；可以参考 Vulkan 生命周期和压缩资源回收方式，不宜直接作为正式画质编码器。

## 验证边界

- 本轮验证的是应用实际使用的 Turnip `VkPhysicalDevice` 能力，不是仅查询 Android 系统驱动的 `vkjson`。
- 已重新构建 host/APK，并在设备上启动 TMNT；格式能力日志已出现且进程保持运行。
- 本轮没有改变 BC 上传语义，也没有宣称 0.5/0.75 缩放已经落地。
- 下一步应先实现只读采样纹理的 0.5 mip 映射和缓存隔离，再以 ASTC encoder 合同为基础接 0.75；render target 缩放另开验证批次。
