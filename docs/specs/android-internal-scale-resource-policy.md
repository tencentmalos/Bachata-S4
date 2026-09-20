# Internal Scale 改良 spec v2：Render Scale / Texture Quality 分离、小贴图保护与动态尺寸

日期：2026-09-20。状态：**推荐方案，尚未实施**。本文件取代 v1 草稿 `internal-scale-resource-policy-20260920.md`。跨机器（macOS / Windows）路径差异见 §2 的 Citron 表。

本轮交付为源码评估和设计，不代表新增 GPU、游戏画质、内存或性能验收。审计基线为 `1db95bffd5e5c6ba509694aad191ea115532582d`；当前五档低倍率实现、既有单向 native 回退与异步退休协议继续作为起点。

## 0. 实施者必须遵守的执行约束

- 两个连续工作包（§9）在一次交付内完成代码、定向验证和证据，不在每个开关/入口处交回微型 spec。
- 不修改 FEX 子仓、Foundation 的 guest 语义；Foundation 只承载通用 texture codec / Vulkan 工具（现有 `foundation/modules/texture_codec`），不放 PS4 地址、描述符、alias 身份或 streaming 启发式。
- 不改 `EmulatorSettings` 现有字段语义；旧 `internal_scale_percent` 继续可读。不改变用户当前设备设置，不为安装新 APK 打断正在运行的游戏会话，除非用户确认。
- 默认只做与改动相关的定向检查，不跑完整游戏回归；所有失败和未覆盖项保留在证据中，不倒写。不做 commit/push，除非用户要求。
- 不得声称 FPS、总内存或画质"已改善"；只报告分组测量（§10）。

## 1. 推荐结论

把现有一个 Internal Scale 拆成两个独立策略：

- **Render Scale**：控制可缩放渲染目标（含 depth、VideoOut 缓冲）及其绘制坐标，保留 0.25 / 0.375 / 0.5 / 0.75 / 1.0，Android 默认仍为 0.5，重启生效。
- **材质质量（Texture Quality）**：控制来自 guest 上传、以采样为用途的资产纹理，三档 **高 / 中 / 低**，对应资产首级倍率 **1.0 / 0.75 / 0.5**，默认高。低档额外丢 1 级已有 mip；中档不是 2 的幂，只复用现有已验证的 resample（非压缩）/ ASTC 转码（LDR BC）路径，不新写转码器。**任何档位都保留长或宽过小贴图的保护**，避免处理后细节直接消失；不跟随 Render Scale 变化。0.25 对静态材质过于极端，不设此档。

分类依据是**资源用途、内容来源和子资源历史**，不是 VkImage usage bits，也不是"静态/动态"。流式上传的材质仍是资产纹理；上一 pass 写出的 RT 即使只被采样，仍属渲染结果。UI、字体、LUT、深度、storage 等各自保留安全限制。

命名：UI 文案为 **材质质量 / Texture Quality**，取值 **高 / 中 / 低（High / Medium / Low）**。Android 设置页已有"Shading Quality"（guest 着色质量），两项并列显示时说明文字要区分"着色"与"材质"。本文用 Texture Quality 指代该策略，高/中/低指代三档。

这一分离对 PS4 和 Switch 都有意义，但它不保证 FPS 或进程内存改善，也不是在模拟器里重建游戏引擎的 streaming 系统。

## 2. 当前实现核对（v2 已按源码逐条复核）

| 已核对事实（含文件） | 对改良的影响 |
| --- | --- |
| [Image 构造](../../src/video_core/texture_cache/image.cpp) 直接读全局 `EmulatorSettings.GetInternalScalePercent()`；`eligible` = 倍率<1 ∧ 2D ∧ 单采样 ∧ 原始宽高均 ≥16。三条路径按序：非压缩且支持 blit+linear（或 depth）→ **resample**；压缩且 `MipDrop()>0` 且 levels>drop → **mip-drop**；LDR BC 且设备支持目标 ASTC → **ASTC 编码** | 16×16 在 0.25 下变 4×4；门槛是原始尺寸而非缩后尺寸。0.375/0.75 下压缩资产必然进入 ASTC 编码；非压缩资产永远走 resample 而不是 mip-drop |
| [InternalScale](../../src/video_core/texture_cache/internal_scale.h)：八分之一倍率；`MipDrop()` 仅 0.25→2、0.5→1；`Levels()` 由缩后最大边 bit_width 决定 | 纯尺寸/mip 规划可保留，但"所有图像同一倍率"的隐含前提要拆掉 |
| [ImageDesc](../../src/video_core/texture_cache/texture_cache.h) 有 `BindingType` Texture / Storage / RenderTarget / DepthTarget / VideoOut；`slot_images.insert(..., info, this)` 只传 `ImageInfo`；`FindImage` 对 Storage 绑定调用 `ForceNative("storage or format alias")` | 用途入口已存在，但分配决策看不到它；必须把意图带进分配 |
| [ImageUsageFlags](../../src/video_core/texture_cache/image.cpp) 广泛预设 sampled / attachment / storage | usage bits 不能当用途分类器 |
| [BindResources](../../src/video_core/renderer_vulkan/vk_rasterizer.cpp)：每 draw 扫描 unsafe 资源（written / atomic / `requires_native_scale` / cube→2DArray / 超过 30 binding / 整数格式）并 `ForceNative`；附件整组：任一附件 `!IsScaled()`、fragment 副作用或 MSAA → 全组 `ForceNative("mixed attachment pass")`，否则 `render_scale_eighths` 取全局值 | 整组决策已有，但"整组倍率一致"目前靠"都由同一全局值创建"隐含保证；拆分后必须显式比较各附件的实际倍率 |
| [Image::ForceNative](../../src/video_core/texture_cache/image.cpp) 是**单向**（scaled→native）；非压缩内容 blit 保留，mip-drop/ASTC 图像标 `CpuDirty` 由 guest RAM 重建（断言未被 GPU 修改）。**没有 native→scaled 的重规划路径** | 拆分后 材质=高 创建的图像若之后被当附件写入，整组 pass 会退回 native。必须新增"首次附件写入前"的受限重规划（§4.3），否则 Render Scale 覆盖率会低于当前耦合版本 |
| [CopyImage / CopyImageWithBuffer / Resolve](../../src/video_core/texture_cache/image.cpp) 在 mip-drop/ASTC/倍率不同/尺寸不同时把**源和目标都** `ForceNative` | RT→copy→纹理链若目标按资产策略（高）规划，会每帧把源 RT 拉回 native。目标必须继承源的物理 plan（§4.4） |
| [DownloadImageMemory](../../src/video_core/texture_cache/texture_cache.cpp)（GC 压力下载、`ProcessDownloadImages` 同步回读）用 `info.size` 的 guest extent 从当前 host image 拷贝，**没有检查 `IsScaled()`** | 现有缺口：缩放 RT 被回读时区域越界或数据错误。必须在回读前提升（有损）并把该身份标为 native-required（§4.5） |
| [image lowering](../../src/shader_recompiler/backend/spirv/emit_spirv_image.cpp)：`SupportsScale` 仅 2D/2DArray、非 storage、binding<30；guest 尺寸/level 从 T# flatbuffer 读；`PhysicalLod` 按 drop 码换算；`EmitImageRead` 按 host/guest 比例映射整数坐标；`ImageQueryLod`/带 offset 的采样/gather 在 [info pass](../../src/shader_recompiler/ir/passes/shader_info_collection_pass.cpp) 标 `requires_native_scale`；`FragCoord` 乘 8/eighths 还原 guest 坐标；**没有额外 LOD bias** | 不另建忽略 guest 逻辑尺寸的纹理系统；不新增 `log2(RenderScale)` bias |
| [PushData](../../src/shader_recompiler/resource.h)：128 字节，`image_scales[2]` 每 binding 2 位（0=native、1=drop1、2=resample、3=drop2），高 4 位 render eighths，`MaxScaledBinding=30` | v1 可继续用这 4 个码；不能把 RT 倍率推断为 sampler 倍率 |
| [Profile](../../src/shader_recompiler/profile.h) `internal_scale` = `percent != 100`，在 [vk_pipeline_cache.cpp](../../src/video_core/renderer_vulkan/vk_pipeline_cache.cpp) 设置；[vk_pipeline_serialization.cpp](../../src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp) `ShaderBinaryVersion=9`、Android `ShaderMetaVersion=9`、`PipelineKeyVersion=5` | 拆分后 profile 标志必须为 `Render≠1 ∨ 材质≠高`；PushData/profile 语义变化时升 `ShaderBinaryVersion` |
| 设置链：[emulator_settings.h](../../src/core/emulator_settings.h) `Setting<float> internal_scale_percent{100}` + 进程级 `SetInternalScalePercent` 只接受 25/37.5/50/75；Android [InternalScale.kt](../../android/shadps4-app/core/runtime/src/main/kotlin/com/shadps4/android/runtime/settings/InternalScale.kt) ID `gpu.internal_scale`，目录 [runtime-settings/android.json](../../android/shadps4-app/core/runtime/src/main/resources/runtime-settings/android.json) / `shadps4.json`，[FexSessionService.kt](../../android/shadps4-app/app/src/main/kotlin/com/shadps4/android/service/FexSessionService.kt) 在 renderer 创建前经 `nativeSetInternalScalePercent` 推送；[status_layer.cpp](../../src/imgui/status_layer.cpp) 与 `gpu_memory` 诊断只显示一个百分比 | 新策略要同样走 catalog → RuntimeProfile → JNI → 会话快照；状态栏/诊断需显示两项 |
| RT extent 来源：[image_info.cpp](../../src/video_core/texture_cache/image_info.cpp) 用 `CbDbExtent hint` 有效时取 hint 宽高，否则 Pitch/Height；hint 由 [liverpool.cpp](../../src/video_core/amdgpu/liverpool.cpp) 从 NOP payload 更新 | guest DRS 改变 hint 会产生**不同尺寸的 cache image**（走 overlap 解析），不是同一 image 改 viewport；§7 的两类"动态"对应两条现有路径 |
| GC：`TextureCache::RunGarbageCollector` 按 tick/压力释放图像，重建时重新走构造决策 | `ForceNative` 原因随 Image 释放丢失，重建后再次缩放→再次提升，产生 churn；plan 决策必须在 Image 生命周期之外按身份保留（§4.1） |
| [内存实测](../validation/android-native-host/internal-scale-memory-20260919.md)：缓存图像 386.85→112.62 MiB，VMA blocks / PSS 未降；约 210 MiB 全尺寸在途上传源 | 降资产分辨率必须同时核算上传临时资源与池保留 |

既有验证边界见 [五档倍率记录](../validation/android-native-host/internal-scale-low-20260920.md) 和 [原实现记录](../validation/android-native-host/internal-scale-20260919.md)：Runtime 109 + Settings 13、Qualcomm/Turnip 各 3,134,415 checks/0 failures、SPIR-V 3/3。这些证明现有路径的行为，不证明本方案已生效；实施后它们是必须保持通过的回归门。

Switch 对照使用本地 Citron 检出，两台机器路径不同，引用时按机器选择并写明 commit：

| 机器 | 相对本文件路径 | 绝对路径 | 检出 commit |
| --- | --- | --- | --- |
| macOS | `../../../../switch/citron` | 按该机 workspace 布局（本机未核对绝对路径；相对路径与 v1 一致） | `3da08ee52defc5003d23683b0c233b0dac88722f`（干净检出） |
| Windows | `../../../switch/citron` | `C:\workspace\emulations\switch\citron` | HEAD `cf2620cc97fc71c629358f13a4d556c593c741ce`；`3da08ee5` 存在于历史 |

两处检出对本文引用的 `image_info.cpp` / `texture_cache.h` 入口一致；跨机时以 commit 为准，不以 HEAD 为准。

- `src/video_core/texture_cache/image_info.cpp` 区分 `rescaleable/downscaleable`，要求单 mip，使用 288 / 512 高度启发式。
- `src/video_core/texture_cache/texture_cache.h` 的 `RescaleRenderTargets` 整组检查 color/depth，`ImageCanRescale` 检查别名链，且有 `ScaleUp/ScaleDown` **双向**路径。
- 可借鉴用途分流、整组一致性与双向重规划边界。不能移植高度阈值、断言单 mip 即 RT，或把其策略当硬件规则；本轮未验证 Citron 的游戏效果。

## 3. 设置及推荐初值

| 设置 | 第一版建议 | 说明 |
| --- | --- | --- |
| Render Scale | 现有五档；Android 默认 0.5 | 作用于准入的渲染目标 / depth / VideoOut；重启游戏生效 |
| 材质质量 = 高（1.0） | **默认**；额外 mip drop = 0 | 不因 Render Scale 缩资产图；guest 自身 streaming / mip 选择照常 |
| 材质质量 = 中（0.75） | 首级 resample 到 0.75（非压缩）或 BC→ASTC 0.75（LDR BC）；原短边 <128 保持原尺寸（缩后短边 ≥96） | 复用现有 resample / `BlitHelper::EncodeAstc` 路径；有全尺寸上传临时源和编码成本，频繁更新的纹理按 update-cost 保持原尺寸；设备不支持目标 ASTC 时 BC 保持原尺寸 |
| 材质质量 = 低（0.5） | 额外丢 1 级已有 mip；保留首级短边 ≥64（原短边 <128 保持原尺寸） | 无转码、无临时源，只对准入且有有效 mip 的资产启用；小图保护始终生效 |
| Texture Quality = Follow Render Scale（legacy） | **仅开发对照开关**，不进公开目录 | 复现当前耦合行为用于 A/B；默认不可见 |
| 单 mip / mip 不足的资产 | 三档均保持原尺寸（v1） | 大尺寸单 mip 多为 atlas / lightmap / LUT，缺语义证据时不降级；后续凭证据放开中档 resample |
| 新的重采样 / 转码实现 | 不做 | 只复用现有已验证路径；Foundation 不新增 codec |

"1.0 / 0.75 / 0.5"描述的是资产**首级**的目标倍率：低档通过丢 1 级 mip 实现；中档通过现有 resample / ASTC 路径实现（当前耦合版本在 Render 0.75 下已走同一路径，probe 已覆盖 0.75 的 BC/ASTC 质量预算）。实际倍率由 §5 的保护规则决定，受约束后可能高于目标。64 / 96（原图 128）是起始保护参数，需用资源直方图与画质验证校准；作为内部 preset 参数，不在设置页暴露阈值，但任何档位都不能关闭保护。

**配置落地要求：**

- 新设置走现有链路：`runtime-settings/android.json` 与 `shadps4.json` 增加 `gpu.texture_quality`（ENUM `high|medium|low`，默认 `high`，UI 文案 高/中/低），新增 Kotlin `TextureQuality.resolve(global, game)`（与 `InternalScale.kt` 同形），`NativeFexSession.nativeSetTextureQuality`，JNI 在 renderer 创建前推送；native 侧 `GPUSettings` 增加字段并保留 TOML 读写、单游戏 override、重置。现有 `InternalScaleTest.kt`、`ShadPs4ConfigManagerTest.kt`、`SettingsViewModelTest.kt` 按同模式扩展。
- **会话内不可变快照**：renderer 创建时构造一次 `ScalePolicySnapshot{render_eighths, texture_preset}`，TextureCache / Rasterizer / PipelineCache / StatusLayer / 诊断都读快照，不在 Image 构造和每 draw 里再读 `EmulatorSettings`。
- 迁移：旧 `internal_scale_percent` 只决定 Render Scale；`texture_quality` 缺省为高。迁移说明写明"资产纹理恢复原质量，长期纹理占用可能增大"。此 spec 不修改用户当前设置。
- 高为默认是首版兼容性建议。若要把默认改为中，先完成 §10 内存与场景验收；不能把"少了 ASTC 临时资源"推导成净内存下降。

## 4. 用途模型与内容所有权

### 4.1 ResourceScalePlan

由 TextureCache 决策，Image 执行；至少包含：

```text
identity: session + guest mapping generation + (guest_address, guest_size, pixel_format, type, layers, levels)
usage_history: sampled / attachment / storage / transfer-src / transfer-dst / readback / video-out
content_origin: guest-upload / render-output / compute-output / copy(from identity) / unknown
policy_domain: asset / render / native-required
logical_layout: guest extent, pitch, mip layout, layers, samples, format（不变）
physical_plan: host extent, format, mip mapping (extra_drop), effective render eighths
content_version, plan_version, reason
```

- 不建立两个互不相认的 TextureCache。guest 地址、别名、dirty tracking、内容版本仍由统一 cache 管理；只有分配策略分域。
- v1 对整 Image 保守取各子资源限制的并集，不建多 backing 的子资源协议。
- **决策在 Image 生命周期之外保留**：以 identity 为键的有界表（例如 4096 项 LRU），随 mapping generation 失效或 unmap 清除。GC 释放后重建的图像必须直接得到上次的 `native-required` 结论，不能先缩后提升。表满时新条目按默认策略，不静默丢弃已 native-required 的条目（优先淘汰非 native-required）。
- `reason` 枚举至少含：size-protect、semantic-native、unknown-usage、alias、insufficient-mips、mixed-pass、readback、copy-inherit、update-cost、budget、legacy。

### 4.2 决策表

| 观察到的使用 | 推荐处理 |
| --- | --- |
| guest 上传的只采样材质，含后续 streaming 更新 | Asset；按材质质量档选择已有 mip，再经小图保护 |
| color / depth attachment，随后被 sampler 读取 | Render；继承写入时 backing，采样时不再执行 Texture Quality |
| VideoOut 缓冲（`BindingType::VideoOut`） | Render；倍率必须与写入它的 pass 相同（Presenter 已用 `HostExtent` 取源尺寸） |
| RT 复制 / resolve 到另一张图像 | 目标继承源的物理 plan 与 `content_origin=copy(from)`；不能把复制结果当 guest 资产再缩一次（§4.4） |
| 已确认字体、UI atlas、LUT、查表/精确数据纹理 | NativeRequired；与原图大小无关 |
| writable storage、atomic、整数格式、精确 offset/LOD query、cube→2DArray、超过 30 binding、未支持的 copy/alias/MSAA | 保留现有 native 规则；不自动缩 compute dispatch |
| 被回读（download / GC download / 诊断回读）的图像 | 回读前提升；身份标 NativeRequired（§4.5） |
| 用途或内容来源不明 | 原尺寸；记录原因，后续用明确证据扩大准入 |

"多帧没被写"不证明静态资产；"刚被 CPU 更新"不证明资产；BC 不能单独证明是可降质材质。地址重用、新映射和不兼容描述符必须更新 identity，不沿用旧标签。

### 4.3 角色转换与双向重规划（v1 必须实现的最小集合）

现有代码只有 scaled→native 单向提升。拆分后必须补以下受限的 native→render 重规划，否则 Render Scale 的附件覆盖率会**低于当前耦合版本**：

- 触发条件：图像即将首次作为 color/depth attachment 被绑定（或作为 VideoOut 目标），且 `policy_domain != native-required`，且 `usage_history` 不含 readback / storage / exact-access，且当前物理 plan 与本 pass 的 render eighths 不同。
- 动作：在该 pass 的描述符 / render pass 发布前，从**最新内容**（若 `content_origin=guest-upload` 且 CpuDirty，则从 guest RAM；否则从现有 host backing blit）构造 render-domain backing；旧 backing / view 按现有 scheduler 异步退休（GPU 完成 + host submit 返回）。
- 限制：每个 identity 最多一次 native→render 重规划；之后若再触发 exact / storage / readback 则单向回 native 并标 NativeRequired，不再来回。禁止在 sampled↔attachment 切换时反复缩放。
- 回归门：TMNT 与 Bloodborne 固定场景下，Render=0.5 / 材质=高 的"缩放附件 pass 数 / 总附件 pass 数"不得低于同版本 legacy 耦合策略（§10 用诊断计数比较）。

已知限制必须写进诊断和文档：**把低分辨率 RT blit 回原尺寸不能恢复已经丢掉的像素。** exact / readback 敏感资源要尽量在首次有损写入前排除；后发现的 `ForceNative` 只是停止后续损失，不是 bit-exact 修复，列为兼容性缺口。

### 4.4 复制 / resolve 的 plan 继承

- `CopyImage`、`CopyImageWithBuffer`、`Resolve`、`ExpandImage`、深度拷贝：当目标没有已提交的有损内容（新建、或 `content_origin` 为 unknown/copy 且未被采样过）且尺寸/格式/层数兼容时，目标采用**源的物理 plan**（同 eighths、同 drop），并记录 `content_origin=copy(from source identity)`；这样 host 上仍是同倍率拷贝，不触发提升。
- 只有在无法继承（mip-drop/ASTC 源、尺寸不等、位宽不等、块压缩与否不同）时才走现有双方 `ForceNative`，原因写 `alias`。
- 目标之后若被当资产采样，不得再按 Texture Quality 再缩一次。

### 4.5 回读

- `DownloadImageMemory`（含 GC 压力下载与 `ProcessDownloadImages` 同步回读）必须在拷贝前检查 `IsScaled()`：先 `ForceNative("readback")`（有损放大）再拷贝，并把 identity 标 NativeRequired，使 GC 重建后不再缩放。
- 回读结果在诊断里标为 `upscaled`，不能描述为原始 GPU 数据。

## 5. 小资产纹理保护

保护对象是**当前有效最高分辨率内容的首级**，同时覆盖很窄的长条图；不能只检查面积，也不能只看 canonical 原图曾经多大。

**保护在高/中/低所有档位始终生效，不可由 preset 关闭**：长或宽任一过小的贴图（含 1024×32 这类长条图）不得因为另一边够大而被降级，处理后的首级必须仍能承载原有细节层级；无合法 drop 时保持原尺寸。

两档的保护规则分别定义；先执行语义保护（§4.2），再计算尺寸：

```text
低（mip drop）:
    d = 1 if  guest mip1 is resident, initialized and sampleable
          and retained chain implements the guest-visible sampler/view range
          and max(1, W >> 1) >= 64 and max(1, H >> 1) >= 64
    else d = 0   // 保持原尺寸

中（resample / ASTC）:
    factor = 0.75 if  min(W, H) >= 128            // 缩后短边 >= 96
                  and format path eligible        // blit+linear 非压缩，或 LDR BC 且设备支持目标 ASTC
                  and not update-cost limited     // 频繁 dirty 的纹理不反复转码
    else factor = 1.0   // 保持原尺寸
```

先执行语义保护，再计算尺寸。`W/H` 是可确认的当前内容尺寸，不能把对齐 pitch 当宽度。**最低边长只约束被保留的首级，不把 mip tail 的 4×4、2×2、1×1 钳成 64/128。** 小于阈值的原图保留原尺寸，不放大。数组 layer 数、cube face 数与 volume 深度不随 2D 质量档下降。

**路径要求：** 低档一律走现有 `mip_skip` 路径（`Upload` 已按 `mip_skip` 过滤拷贝、`HostMip/HostRange/ShaderScaleCode/image_view minLod` 已按 drop 换算），需把该路径从 `info.props.is_block` 门槛放开到所有可采样格式。中档复用现有 resample（非压缩，blit）与 ASTC（LDR BC，`EncodeAstc`）路径，但触发条件改为材质档而不是 Render Scale；两条路径都在保护规则之后执行。v1 三档对单 mip 资产都保持原尺寸。

| 当前资产尺寸及有效 mip | 中（0.75） | 低（0.5） | 原因 |
| --- | --- | --- | --- |
| 16×16、32×32、64×64 | 原尺寸 | 原尺寸 | 解决极低倍率下小图继续缩小 |
| 128×128 | 96×96 | 64×64（drop1） | 两档都刚好到达各自下限 |
| 256×256 | 192×192 | 128×128 | 与 Render Scale 无关 |
| 512×512 | 384×384 | 256×256 | 不超过各档目标倍率 |
| 1024×32 | 原尺寸 | 原尺寸 | 短边保护，面积门槛无法替代 |
| 65×49 | 原尺寸 | 原尺寸 | 不再因原尺寸 ≥16 而进入有损缩放 |
| 2048×2048，单 mip | 原尺寸 | 原尺寸 | 低档缺可用低 mip；中档 v1 对单 mip 保守不转码 |
| 任意尺寸的已识别 UI/LUT/精确数据 | 原尺寸 | 原尺寸 | 语义保护优先于尺寸 |

大小规则不能识别大字体 atlas 中的小字形，也不能辨认所有 normal / roughness / alpha-test 内容。缺少可靠语义证据时，默认高才能避免普遍降质；中/低只作保守准入的可选策略。作者制作的 mip 优先于运行时重采样，但仍要验收透明边缘、法线与材质细节，不能声称无画质损失。

若后续允许无 mip 重采样，用一个保留长宽比的倍率并检查缩后两边、真实压缩块数及收益；禁止分别 `max(width*s,64)` / `max(height*s,64)` 拉伸比例；sRGB 在线性域过滤，法线、alpha coverage 和数据图没有专门处理时维持 native。

## 6. Streaming LOD 与 guest 采样语义

guest 可能通过 view base mip、替换更小纹理、更新部分 mip、复制或地址重用实施 streaming。模拟器只有在描述符/上传/别名关系能证明时，才知道两个版本属于同一资产；不能从连续地址或相似尺寸猜测跨对象身份。

**v1 规则（可实现）：** 按 cache image 自身的 mip 链决策，view base 由现有 `ShaderScaleCode(base_mip)` 处理（base ≥ drop 时为 native 映射）。检测到同 identity 的尺寸/mip 范围变化而无法确认 canonical 身份时，停止额外降质，保留 guest 当前尺寸。普通 dirty 更新只刷新受影响内容，不改质量档；部分上传没触及某级不等于该级不存在，不访问从未初始化的 mip。

**canonical 身份（WP2 可选）：** 对能确认 canonical mip 身份的资源，按绝对 mip 目标限质，而不是每次在 guest 当前级别上重复 drop：

```text
guest_first = guest 当前有效首级在 canonical 链中的 mip
policy_first = 材质质量档为此 canonical 资产决定的最低保留 mip
host_first = max(guest_first, policy_first)
extra_drop = host_first - guest_first
```

再施加小图下限、有效子资源和可采样范围检查。例：4096 原图，低档绝对目标 mip1/2048；guest 当前 mip0/4096 则额外丢 1 级；guest 已在 mip1/2048 或更低则额外丢 0 级，不能再变 1024。身份断开时不套用。

新增策略必须分别验证：

- guest `textureSize`、level count 保持原描述符语义（现有 flatbuffer T# 读取），view base、sampler min/max LOD 与显式 LOD 统一映射；不泄露更小 VkImage 尺寸。
- drop `d` 时显式 LOD 按 view-relative 剩余 drop 转换；base mip 已越过被丢区间时不再减一次。
- 隐式 LOD / 显式梯度使用实际 host 尺寸产生物理 footprint；**不新增** `log2(RenderScale)` 或重复 mip bias（现有代码没有 bias，保持）。
- 理想同视角下物理 LOD 相对原生约变化 `log2(texture_factor / render_factor)`，只用于推导检查。
- integer fetch、gather/offset、LOD query、深度比较、超出 30 个 binding 继续采用已验证映射或 native 回退。两个设置独立后，**Render=1 且 材质≠高 必须开启 `profile.internal_scale` 与 per-binding 码**；反向组合亦然。

v1 不实现基于反馈的 host 自动纹理 streaming、磁盘资产缓存或动态显存预算控制器。

## 7. RenderTarget、小型 pass 与 dynamic size

### 7.1 RT 不能直接套资产图的 64/128 门槛

RT 有主场景、G-buffer/depth、阴影、反射、UI、bloom/曝光金字塔、时序历史等角色，可能单 mip、可能被采样。保护小材质不应让低分辨率后处理失去原比例。

整组附件的确定性决策（扩展现有 `BindResources` 附件循环）：

1. 合并 color/depth 的已知用途和现有安全限制。所有附件必须使用同一坐标变换：**显式比较各附件的 `ScaleEighths()`**，不再靠"都 IsScaled"隐含一致。
2. 未分类且短边 ≤64 的小 RT 先保留 native；该值是初始启发式，不宣称识别用途。已确认屏幕相对的后处理链继承 render 策略，不套资产首级下限；允许合法 1×1 尾端。
3. 任一必须 native 的附件，或无法支持的尺寸/采样数组合，使该 pass 整体 native；不将一张附件单独抬到 64/128、另一张仍乘 0.25。
4. 小 RT 与大附件共用 pass 时，保守回退可能损失性能，诊断必须可见；后续以 pass/资源证据解除，不按游戏名硬编码。
5. standalone 阴影/UI/历史缓冲不凭面积或宽高比认作主场景；无法确认安全缩放规则时保留 native。

UI 资产原尺寸不保证文字清晰：若文字仍绘制到 0.25 的场景 RT，输出受低分辨率限制。只有 guest 本来有可区分的 UI 合成阶段时才在该阶段保持 native；v1 不宣称通用拆出 HUD。

### 7.2 区分三种"动态"

| 动态行为 | 现有机制 | 第一版要求 |
| --- | --- | --- |
| guest 重新创建/复用不同 extent 的 target | `CbDbExtent hint` 变化 → `ImageInfo.size` 变化 → `FindImage` 不精确匹配 → overlap 解析 / ExpandImage / 新 image | 用本次真实逻辑 extent 重算 host backing；区分 mapping/content/plan generation；overlap 得到的旧图像不得把旧 plan 强加给新尺寸 |
| guest 分配固定最大 RT，只改 viewport/render area | hint 不变，viewport/scissor 按 `render_scale_eighths` 乘 | backing 按分配 extent 缩放，active rect 单独转换；不能拿 active rect 冒充 allocation 并改掉 UV 归一化基准 |
| emulator 为追帧率主动改 Render Scale | 无 | 本版不启用；保留重启生效，另行设计控制器与历史缓冲处理 |

设本次 guest allocation `G=(W,H)`，准入的 session render factor `r`：

```text
host allocation = max(1, floor(G * r))  // 分量计算；仅逻辑 extent，不改 guest pitch
host viewport / active rect = 按同一 render 映射转换（现有 floor/ceil scissor）
source mip / sampled image extent = 使用该资源自己的 plan
```

保留当前负 viewport 高度与深度/fog 修复；scissor 起点 floor、终点 ceil 后裁到合法 host 范围。奇数尺寸通过 GPU 边缘像素验证，不用名义倍率替代真实 mip extent 的 fetch/copy 映射。

配置语义为 **Relative to guest**：游戏从 1920×1080 降到 1280×720，r=0.5 时 host 从 960×540 变 640×360；诊断同时显示 guest active 尺寸与 host 尺寸。只乘一次；RT 在后处理被采样时不再乘 Texture Quality 或 Render Scale。

若以后需要"总输出最多 960×540、但 guest 已降低时不再砍半"，另做 **Absolute ceiling** 策略；不暗中把 Relative 改为追随 Surface 或固定 1080p。

已有 guest DRS 切换必须保留游戏的内容、清除和历史有效性语义，不擅自清空 TAA 历史。之后若实现 host 动态倍率，必须处理历史重投影、jitter 和 motion vectors；不能处理的 history 链保持固定分辨率/native。不每帧销毁重建整个 cache，也不无上限缓存所有出现过的尺寸。

## 8. 上传、压缩与内存成本

优先次序：**native 压缩采样 → 直接使用既有压缩 mip（mip_skip，低档）→ 现有已验证的重采样/转码（中档）**。RT 继续用 attachment 格式，不做每帧 BC→ASTC。

Texture Quality 切换与 guest dirty 是不同事件。内容未变、只是 RT 尺寸/绑定改变，资产纹理不得重新 detile/encode。持续更新的纹理若转码开销过高，停止额外降质或走 mip_skip 直接路径，不因"动态"进入 RT 策略。

存储估计按每 mip 压缩块：`ceil(W/block_w) * ceil(H/block_h) * block_bytes * layers`；再分别记录 Vulkan allocation、VMA block 与进程 PSS。直接丢 mip 只有在上游 detile/staging 也跳过被丢级时才减少对应上传工作（现有 `Upload` 已过滤 `mip < mip_skip` 的拷贝；需确认 detile/staging 侧同样跳过）。

可选重采样/ASTC 路径进入推荐 preset 前，要求有界 scratch、明确在途峰值、格式/颜色空间/alpha 验证和创建速率统计（现有 `MemoryDiagnostics::upload_image_*` 计数）。复用满足命令顺序与跨队列依赖；资源退役仍要求 GPU 完成和 host submit 返回；不加 device/queue idle 压低内存数字。池用尽时走有预算的原生路径/既有可取消背压。

## 9. 推荐实施范围、文件入口、顺序与回归门

按两个连续工作包落地；每包完成代码、针对性验证和证据。

**WP1：独立策略及正确性闭环（顺序即依赖顺序）**

1. `ScalePolicySnapshot` + 配置分离（Kotlin catalog / resolve / JNI / GPUSettings / TOML / 迁移），status_layer 与 `gpu_memory` 显示两项；`profile.internal_scale` 改为 `Render≠1 ∨ 材质≠高`，升 `ShaderBinaryVersion`。
2. `ResourceScalePlan` 与身份表；`BindingType` / usage history 传入分配（`TextureCache::FindImage` → Image 构造参数或构造后 `AdoptPlan`）。
3. 资产保护与路径：低档用 mip_skip 并放开到非压缩格式；中档复用现有 resample / ASTC 路径但由材质档而非 Render Scale 触发；保护规则在两条路径前统一执行；legacy 开关保留耦合行为。
4. 回读修复（§4.5）——独立小改动，可最先落地。
5. 复制 / resolve 的 plan 继承（§4.4）。
6. 首次附件写入前 native→render 重规划（§4.3）与整组显式倍率比较（§7.1）。
7. streaming 身份变化时停止降质、dirty 语义（§6 v1）。
8. RT 动态 extent / active rect（§7.2），确认 overlap 路径不把旧 plan 强加新尺寸。
9. 资源诊断：按用途/原因分组、logical/physical 尺寸、有效 drop、上传与在途成本、附件 pass 缩放计数。

**WP2：成本与真实场景闭环。** 复核上传路径与峰值；定向 GPU probe；两个游戏同场景画质/内存/性能对照；必要时收紧 preset 与默认；canonical streaming 身份（§6）视证据决定。scratch 池 / 融合 detile-scale 是成本优化候选。

| 入口 | 改动职责 |
| --- | --- |
| `android/.../runtime-settings/android.json`、`shadps4.json`、`runtime/settings/TextureQuality.kt`（新）、`NativeFexSession.kt`、`fex_session_jni.cpp`、`FexSessionService.kt` | 两个独立配置、迁移、session 快照、继承/重置；UI 不堆策略细节 |
| `src/core/emulator_settings.h/.cpp` | `GPUSettings.texture_quality`、override、TOML、`ScalePolicySnapshot` 数据来源 |
| `texture_cache/internal_scale.h` | 纯尺寸与 mip 规划，接受 per-plan 输入；不再隐含"所有图像同一倍率" |
| `texture_cache/texture_cache.h/.cpp` | 用途证据、身份表、最终 plan、内容版本与 dirty 传播、回读前提升、复制继承、GC 重建沿用决策 |
| `texture_cache/image.h/.cpp`、`image_view.cpp` | 按 plan 分配、mip_skip 泛化、native→render 受限重规划、子资源映射、异步退休 |
| `renderer_vulkan/vk_rasterizer.cpp` | 整组附件显式倍率比较、首次附件重规划触发、viewport/scissor/render area、最终倍率发布 |
| `renderer_vulkan/vk_presenter.cpp` | VideoOut 目标按 render 域规划 |
| `renderer_vulkan/vk_pipeline_cache.cpp`、`vk_pipeline_serialization.cpp`、`shader_recompiler/profile.h`、`resource.h`、image lowering | profile 标志组合、per-binding 码、版本升级 |
| `src/imgui/status_layer.cpp`、`src/core/diagnostics/diagnostics_commands.cpp`（`gpu_memory`）、`TextureCache::AppendMemoryDiagnostics` | 显示两项设置；按用途/原因分组的资源诊断 |
| `tests/video_core/android_internal_scale_probe.cpp`（CMake 目标 `android_internal_scale_probe`） | 扩展现有生产 GPU probe：五档 × 三档、小图/长条/单 mip/view base/数组层、复制继承、回读提升 |

## 10. 验收要求

| 验收组 | 必须验证的行为 |
| --- | --- |
| 配置 | 五档 Render × 三档材质质量（高/中/低）；旧配置迁移；单游戏继承/重置；启动日志、status 与 `gpu_memory` 一致；Render=1/材质降级、Render 降低/材质=高 两种关键组合都开启 shader 处理；现有 Runtime 109 + Settings 13 保持通过（按新增项扩展） |
| 小图与 mip | 8/16/32/64/128/256/512、65×49、长条图、单 mip/不足 mip、非零 view base、数组层；首级下限正确且 tail 未被抬高；低档非压缩格式走 mip_skip，中档 resample/ASTC 由材质档触发且 Render=1 时也生效 |
| 语义保护 | 大型字体 atlas、LUT、integer/storage/atomic、精确 offset/LOD query、MSAA、cube→2DArray；未准入路径保持 native 且诊断给出原因 |
| 用途转换 | Asset→RT（首次附件重规划一次）、RT→sample、RT→copy→sample（继承同倍率、不提升）、depth→sample、GPU 写后 CPU 读（回读前提升并 NativeRequired）、地址重用、别名、部分 mip 更新、GC 释放后重建沿用决策；无双重缩小、旧 RAM 覆盖、stale view 或来回缩放 |
| streaming | 同 identity 的 guest mip0→1→3→1；未知身份的更小重建；有效/无效 mip 与部分 dirty；无重复 drop 或未初始化访问 |
| dynamic target | hint 变化与固定 allocation/active rect 变化各测；1920×1080/1600×900/1280×720 往返，含奇数尺寸、MRT/depth 混合和 history 内容检查 |
| shader/GPU | 五档 × 两个质量策略、隐式/显式 LOD/Grad/fetch/query、view 与 sampler 范围、binding 上限、安全 fallback；Qualcomm 与 Turnip 真实 GPU 回读，现有 probe 3,134,415/0 量级保持 0 failures；SPIR-V 验证通过 |
| 生命周期 | in-flight 旧 view/backing、延迟 submit 返回、Stop/取消；无提前回收、无新增全局 idle、身份表与预算有界 |
| 覆盖率回归门 | 同版本 legacy 耦合 vs Render=0.5/材质=高：缩放附件 pass 比例不下降；ForceNative 次数/原因直方图不出现每帧重复提升 |
| 真实场景 | TMNT、Bloodborne 实际可操作场景；HUD/字幕/图标、近距材质、移动/镜头、透明边缘、阴影/反射/后处理；guest streaming/DRS 未触发的项目标为未覆盖 |

对照最少包含：legacy 耦合策略、Render 相同 + 材质高、Render 相同 + 材质中；极端 Render=0.25/0.375 增加小图/UI 审查。固定 APK/驱动/存档/相机/预热，分别测 GPU 时间、呈现帧时间 P50/P95、CPU 上传/编码成本、缓存 allocation、scratch 峰值、VMA blocks 和 PSS。Render=1/材质高 作为原质量参考。跨视角或跨场景数字不得直接归因于策略。

诊断至少能解释"为什么某张图没有缩小 / 为什么某个 pass 回到 native"，按 §4.1 `reason` 区分。默认不逐 draw 打印；按请求输出聚合与有界 top-N，绑定 session 与采样时刻，复用 `gpu_memory request|status` 的无 GPU 等待模式。

验收分别报告 **语义检查、画质观察、内存、CPU/GPU 成本、呈现性能**；不能用算术/回读检查代替游戏可读性，也不能用缓存 allocation 下降代替总内存下降。本轮不要求完整游戏回归；新增阈值和所有未覆盖角色必须保留边界。

## 11. 明确不做（v1 非目标）

- host 自动纹理 streaming、磁盘资产缓存、动态显存预算、host 动态分辨率控制器。
- 新的重采样 / 转码实现或 Foundation codec 扩展；中档只复用现有 resample / ASTC 路径。
- 通用 HUD/UI 合成阶段识别、按游戏名硬编码策略。
- 多 backing 的子资源级 plan；跨对象 canonical 资产身份推断（WP2 视证据决定）。
- 修改 FEX / Foundation guest 语义、完整游戏回归、Swan/VR 验收、FPS 或总内存收益声明。

## 12. 设计依据

- [Khronos：VkImageUsageFlagBits](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageUsageFlagBits.html)：sampled 与 attachment 是可组合的用途能力，不能将 Image 当成永久互斥的两种类型。
- [Khronos：Images / SPIR-V Image Query Instructions](https://docs.vulkan.org/spec/latest/chapters/images.html)：image view base mip 与 level count 影响尺寸/级数查询，因此物理 mip 裁剪必须维护 guest 逻辑映射。
- [Epic：Texture Streaming Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/texture-streaming-overview-for-unreal-engine)：引擎 streaming 依赖可见性、纹理需求和预算；模拟器通常不具有等价场景语义，故采用保守 host 资源策略。
- [Epic：Dynamic Resolution](https://dev.epicgames.com/documentation/en-us/unreal-engine/dynamic-resolution-in-unreal-engine)：动态分辨率涉及 view、缓冲尺寸和时序历史；支持把 guest 动态尺寸兼容与 host 自动调节分开设计。
- 本地 Citron（macOS `../../../../switch/citron`、Windows `../../../switch/citron`，commit `3da08ee5`）的 `RescaleRenderTargets` / `ImageCanRescale` / `ScaleUp` / `ScaleDown`：整组一致性与双向重规划的参考，不移植其阈值。

上述来源支撑通用语义；preset、阈值、迁移方式和实施范围是本 spec 的工程建议，需由本仓实现与验证确认。
