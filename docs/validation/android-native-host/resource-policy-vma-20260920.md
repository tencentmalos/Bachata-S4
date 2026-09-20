# Resource policy 与 VMA 内存统计（2026-09-20）

状态：VMA统计与材质档内存策略已实现并完成定向GPU检查及Swan TMNT场景验证；整体资源spec仍有下述未完成验收项。此文件不会把探针通过等同于全部 spec 验收。原需求：[v2 spec](../../specs/android-internal-scale-resource-policy.md)。

追加纠正：旧 `attachment_passes` 实际按 draw 累计，本文 99.7623%／98.5735% 仅是 draw 覆盖率，不能作为 pass 门的结果。血源实测发现并修复的锁重入、真实 cache 回读测试及新版计数见[后续实施记录](resource-policy-remaining-20260920.md)。


## 使用

Settings → Graphics → Texture Quality 选择high/medium/low，重启游戏后同时启用对应材质和内存策略；Render Scale继续独立。按需采样：

```sh
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService gpu_memory request
adb -s SERIAL shell dumpsys activity service com.shadps4.android/.service.FexSessionService gpu_memory status
```

等待`pending=false`且`completed_id`与请求一致。先看`vma_policy`与session身份，再看live/reserved/free、retiring、分组和device_memory_freed；进程PSS/SwapPss独立观察。

## 统计口径

入口保持 `gpu_memory request` → `gpu_memory status`。请求仅排队，renderer 在安全位置发布一份带 cache epoch、request/completed ID、session PID/generation 与 steady-clock 时间的快照；没有 GPU/queue/device idle。`pending=true` 时下面仍可能是上一份快照，不能当本次结果。`internal_scale` 显示活动 renderer 的不可变 Render/Texture/legacy 快照。

| 字段 | 含义 |
| --- | --- |
| `vma_live_allocation_bytes` | VMA 当前活跃 allocation 的真实 requirement size，包括待退休对象。不是由纹理宽高估算的大小，也不是物理驻留测量 |
| `vma_reserved_bytes` | VMA 从 Vulkan 分配而尚未释放的 VkDeviceMemory 总量，包括内存池空闲空间 |
| `vma_free_block_bytes` | reserved − live；不是可立即还给操作系统的独立 allocation。另报空闲区间数量和最大区间 |
| `tracked_retiring_*` / `tracked_nonretiring_bytes` | 待退役资源的合计、数量、事件峰值，以及尚未标记退役的 allocation；两者之和等于 live，重复 tag/重新分类不重复计数 |
| `tracked_*` | 成功创建/实际释放累计字节、次数、当前和峰值；不把分配周转率叫作传输带宽 |
| `device_memory_freed_bytes/count` | Vulkan层真正释放的VkDeviceMemory块累计字节/次数；区别于VMA子分配归还池内 |
| `vma_reserved_peak_bytes` | VkDeviceMemory allocate/free callback 记录的真实块峰值；不靠定期轮询猜峰值 |
| `vma_group` | 按用途、memory type、property flags 分类，包含 current/peak、requested buffer bytes、retiring current/peak |
| `vma_largest_rank` | 当前最多 16 个最大 allocation，列用途、真实大小、buffer request、类型/flags、退休状态 |
| `vma_reconciliation_delta` / `vma_reserved_reconciliation_delta` | 自有 allocation/块账本与 VMA 自身统计的差额；稳定快照应为 0 |
| `vma_consistent` | 若快照与任何创建、释放或重新分类重叠则为 0；不应把该瞬时差额归因于泄漏 |
| heap usage/budget | 已启用 VMA 的 EXT_MEMORY_BUDGET flag，来源明确。VMA 会用分配变化调整最近驱动值，仍是估计，不能等同本模拟器 allocation |
| process RSS/PSS/private/shared/SwapPss | `/proc/self/smaps_rollup` 的进程视角，包含 guest 与 CPU 内存；权限/平台不支持时明确 unavailable |

分类覆盖 guest buffer cache、staging/stream/download/device pool、BDA 页表、GDS、资产/RT/native 图像、present 图像、全尺寸上传源、detile/tile scratch、上传临时 buffer。退休标记保持到现有 GPU 完成 + host submit 返回协议实际销毁对象，不为降低数字提前释放。

**边界：** VMA allocation 能精确核对 Host Vulkan 资源的分配；不能由它得出“整个 Host CPU 部分的独立物理内存”。非 VMA 的 ImGui、swapchain/imported AHB、驱动内部对象不属于此总数；RSS/PSS 也不能直接减去 guest 虚拟地址范围来推导 Host。UMA 的 DeviceLocal/HostVisible flags 可重叠，不相加；VMA 与 PSS 不相加；retiring 是 live 的子集；各组峰值可能发生在不同时刻或重新分类前后，不相加。旧 `scale_upload_created_*` 是进程生命周期累计值；新的 VMA ledger 随 allocator 销毁清除。按需 smaps 查询会有 CPU 开销，报告 `memory_sample_elapsed_us`，采样帧不用于性能对照。

实现：[`vma_diagnostics.cpp`](../../../src/video_core/vma_diagnostics.cpp)、[`TextureCache`](../../../src/video_core/texture_cache/texture_cache.cpp)。每个 allocator 一份库内账本，避免 header inline 在 probe/host DSO 各有一份。释放记录在向 VMA 归还句柄前移除，整个过程标记 in-flight，防止并发句柄复用冲掉新对象。账本锁内不调用 Vulkan/VMA 或等待 GPU。

## 策略实现

- Render 五档与 Texture high/medium/low 分离；旧配置的 Render 不变，缺失 Texture 默认 high。两个目录、JSON/TOML 迁移、override、JNI、启动、状态栏和诊断接通。shader binary 10。
- 低档在原短边 ≥128、有 mip 链且采样路径准入时直接去掉 mip0；非压缩和 BC 均适用。**VkImage 不分配 mip0，guest→staging→detile→Upload 都只处理保留尾链**。原 guest 内存不删除；其他用途拥有的 buffer 不被强行释放。
- 中档复用既有 resample/ASTC；小图、长条、单 mip、depth/exact/storage 等保守保护。频繁资产更新回到 native；上传源成本预检是 guest layout 字节估计，实际在途 VMA 峰值另报。
- 用途/内容来源及逻辑/物理 plan 有界保存，GC 后保留 NativeRequired；unmap 新 generation。首次 Asset→RT 重规划、RT→copy 同倍率继承、native 回退保持最新 backing，旧 view/backing 异步退休。未知 streaming 身份变化保守 native；没有跨对象 canonical streaming 系统。
- 变化的 RT extent 不沿用旧 backing 尺寸；保守使用现有同步回读保留最新 guest-layout 内容后创建新身份。回读先提升并标注有损 upscaled；tiled 回读走 tiler，不直接向 guest RAM 写线性数据。此路径可能增加同步成本，需要真实 DRS 进一步测量。
- host 参数上传使用 `StreamBuffer::CopyHost`，不误触发 guest 地址空间初始化；guest 地址上传仍用原 CopySparseMemory 路径。

## 资源策略阶段的定向验证（内存档位接入前）

证据：[GPU 最终两驱动](evidence/resource-policy-20260920/gpu-final-both.log)、[CPU plan](evidence/resource-policy-20260920/scale-policy-tests.txt)、[SPIR-V](evidence/resource-policy-20260920/spirv.json)、[APK/host SHA](evidence/resource-policy-20260920/apk-final-sha256.txt)。完整本机材料 `build/validation/resource-policy-20260920`。

- Qualcomm 与 Turnip 各 **13,327,138 checks / 0 failures**，各自包含原 legacy **3,134,415 / 0**。新增五档 Render × 三档 Texture、8/16/32/64/65×49/长条/128/256/512、单 mip/storage、数组层、实际 GPU 像素、非零 view base 映射、首次附件转换、复制及再次复制、readback sticky、micro/macro detile 尾链。
- 4 线程 ×32 次真实 VMA 分配/释放后余额一致；创建/释放次数各128，两种 reconciliation 均0。最终释放所有 probe 资源后 live=0；Turnip 仍有64MiB VMA块预留，明确展示 pool retained 与 live 的区别。
- 512×256、5 mips、2 layers：guest layout 1,396,736 bytes；tail staging/detile 348,160 bytes。Turnip VkImage allocation 1,400,832→352,256 bytes；Qualcomm 1,441,792→385,024 bytes。此为单项分配验证，不是游戏 PSS 收益。
- Runtime112 + Settings13，0失败；CPU identity/GC/unmap/budget28/0；SPIR-V3/3。APK构建通过。
- 保留失败：第一轮独立探针因 Copy 的 guest VM 初始化失败，改用 host 参数入口后通过。首个实景 APK `1cc5eb68` / host `dc85afec` 在 PID29004 屋顶加载阶段触发 `BeginRendering:1063` 深度附件 `needs_rebind` 断言。backing 重规划保留同一 ImageId，不应标记为“cache ImageId 已替换”；修正后新增两项检查，最终 APK `dd4d050a` / host `baecd679` 的 TMNT high 屋顶与正常 Stop 已通过。

## 实景测量

首版 PID29004 菜单阶段稳定快照两种差额为0，固定 utility allocation 1,308,688,384 bytes（约1.219GiB），其中 staging pool 512MiB、BDA页表512MiB、stream64MiB、download32MiB、device128MiB、GDS64KiB。该阶段尚未修改这些容量，后续内存档位调整见下文。该首版随后断言退出，不能作为最终场景验收或低档内存收益证据。

尚不宣称完整游戏回归、全语义/DRS验收、FPS/总内存/画质改善。最终场景与清理状态见下文。

修正版 TMNT high：PID32638/gen1，UUID b4619dc2d29970c54bd1e02642d5ee51。Turnip86ca472f，Render0.5，TextureHigh，进入真实屋顶 MOVE 教学，移动后人物位置改变/教学提示消失，7,399 presents 后正常 UI Stop→Stopped/user_stop。证据 `tmnt-high-fixed-*`。两个静止场景快照 VMA live 均2,664,329,216 bytes、reserved均2,948,382,720 bytes、cached images均293,285,888 bytes；PSS从2,967,168,000变为2,425,981,952 bytes，不能把这个不同采样时刻的PSS变化归因于策略。两种差额均0；快照CPU耗时约73–96ms，不计作无扰动性能证据。

低档 TMNT：PID1160/gen1，UUID0403b509ca5d208f348326f27ae4b103，Render0.5/TextureLow。屋顶可见、移动后进入 ATTACK 教学，随后正常 UI Stop→Stopped/user_stop。两个静止快照 cached images 均149,438,464 bytes，VMA live均2,528,919,552，reserved均2,956,820,480，free均427,900,928，核对差额均0。实际资产记录有1024→512、128→64和drop1；小图保持native。

| 屋顶快照（MiB） | 高 | 低 |
| --- | ---: | ---: |
| 缓存图像 allocation | 279.70 | 142.52 |
| VMA live allocation | 2540.90 | 2411.77 |
| VMA reserved | 2811.80 | 2819.84 |
| VMA池内空闲 | 270.89 | 408.08 |
| 第二次进程PSS | 2313.60 | 1958.34 |
| 第二次进程SwapPss | 384.78 | 419.81 |

这是同APK/驱动/初始屋顶的资源观察，不是完全匹配的质量或性能A/B：高档272张BC资产、低档271张，render资源122/123，预热长度不同；用户头显视角也在变化。VMA预留没有随缓存图像同比下降，PSS受系统回收/交换影响，不能据此宣称整机RAM收益。上传源约331MiB与detile/tile scratch约358.59MiB在此时均在途待退役，仍计入live，不能提前从分配总量扣掉。

中档（仍为内存策略接入前的同一APK dd4d050a）：PID3041/gen1，UUIDfeefaba4f6fb44e388efce9c98f9eebe，Render0.5/TextureMedium。屋顶可见并实际移动进入ATTACK提示；正常UI Stop。cached images198.15MiB，VMA live2471.52MiB、reserved3079.97MiB、free608.45MiB；265张ASTC缩放资产，7张资产因上传预算回退native，差额均0。中档转换及资源群与高/低不同，不能把材质档当作预留内存的线性旋钮。

## 根据材质档调整 Host GPU 内存策略（用户追加）

策略随renderer创建固定，跟随Texture Quality，重启生效；legacy保留原高档内存参数。生产入口：[HostMemoryPolicy](../../../src/video_core/host_memory_policy.h)、VMA CreateAllocator/CreateImage/CreateBuffer、BufferCache staging、TextureCache闲置资产回收。

| 参数 | 高 | 中 | 低 |
| --- | ---: | ---: | ---: |
| VMA大heap首选block | 256MiB | 128MiB | 64MiB |
| staging ring | 512MiB | 256MiB | 128MiB |
| 分配策略 | 原VMA默认 | MIN_MEMORY | MIN_MEMORY |
| 额外闲置资产保留（submit epochs） | 不启用 | 600 | 180 |
| 每16次submit最多额外退休 | 0 | 4 | 8 |

首选block不是总预留上限，也不限制必须满足的dedicated/大allocation。低档的固定staging allocation比高档少384MiB；BDA页表512MiB、其他utility容量不变。所有三个staging Map入口已检查：普通buffer同步原有临时fallback；图像超大上传新增临时buffer+flush+deferred retirement；WriteData按实际ring容量判断并补非coherent flush。单次超大请求不会因为低档变成空指针，也不会永久把ring长大；可能增加wrap等待/临时分配，必须实测。

额外闲置回收只处理Asset+Upload且无GpuModified/GpuDirty的图像，使用现有LRU与双完成退役；不为了统计强制idle、同步回读或丢GPU内容。`idle_asset_evictions`和`idle_asset_retired_allocation_bytes`是申请退休累计值，不是归还系统内存；实际VkDeviceMemory释放由`device_memory_freed_bytes/count`回调记录。

VMA本仓`VmaBlockVector::Free`通常保留一个完整空闲块，其他满足条件的空块自动释放；部分空闲块不可直接缩小，本次不移动仍在用的allocation、不运行有资源搬迁要求的defragmentation。参考[官方分配块参数](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/struct_vma_allocator_create_info.html)和[内存池说明](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/custom_memory_pools.html)。

内存档位验证使用新APK `9d6504bf` / host `e4414aca` / JNI `5c82b208`，已完成下述定向检查及场景验证；不能用前面旧APK数据代替新策略验证。

同版legacy（PID6589/gen1）屋顶35.343s区间，273670/274322=99.7623%；独立策略高档屋顶44.544s区间288566/292742=98.5735%，可见32×32/1024×32等小RT保守native。因此不得把“首次Asset→RT已重规划”概括为原spec的覆盖率不下降门已通过；小RT用途识别仍需要真实pass证据，不能为追比例取消安全保护或加游戏名规则。

真实VMA第一轮Qualcomm探针保留1项失败：测试最初错误地要求“小block的突发峰值不增加”。80×1MiB实际高/中/低峰值为96/112/120MiB，释放后保留64/16/8MiB，实际归还32/96/112MiB；live均0、账本差额0。VMA逐级增长导致不同分配序列的峰值不单调，小block不构成峰值上限。测试改为验证此策略真正承诺的“释放后闲置预留减少”，峰值继续原样输出并保留原始失败日志；没有针对单个序列改参数掩盖峰值。

最终Qualcomm/Turnip各13,327,401 checks/0 failures（含原3,134,415 legacy像素/检查）；三档VMA burst/idle结果一致。4线程×32分配/释放、退休重复tag/重新分类/销毁检查通过，所有稳定账本差额0。CPU policy34/0，Runtime112+Settings13/0。新内存策略没有再改shader代码，SPIR-V仍沿用前述3/3验证。

新APK已安装，PID10156/gen1，UUID754c886999510be501ec3c6394248ed6，Turnip，Render0.5/TextureLow。启动阶段快照实际staging134,217,728 bytes、preferred block67,108,864；已退休4个闲置上传资产（3,133,440 allocation bytes），当时device_memory_freed_bytes=71,598,080、两种核对差额0。此为启动期，不替代场景结果。

新策略TMNT低档屋顶两个快照相隔61.641s：

| 指标（MiB） | 快照1 | 快照2 |
| --- | ---: | ---: |
| VMA reserved | 2043.84 | 2171.84 |
| VMA live | 1779.34 | 2009.19 |
| VMA池内空闲 | 264.50 | 162.66 |
| live中待退役资源 | 459.69 | 689.53 |
| live中非退役资源 | 1319.66 | 1319.66 |
| 当前缓存图像 | 123.94 | 123.94 |
| 固定utility allocation | 864.06 | 864.06 |

本次reserved峰值2299.84MiB；稳定快照账本差额均0。两次间额外idle回收累计均105个/35.27MiB，未看到此静止窗口的重复闲置淘汰；进入场景已重建所需资源。device_memory_freed累计增长（块真实归还），但包含整个allocator的所有释放，不能全部归因于idle回收。

对比同TextureLow但未接入内存策略的旧APK，固定utility从1248.06→864.06MiB，明确少384MiB的staging。旧样本reserved2819.84MiB/live2411.77MiB；新样本分组及在途深度不同，只报告上述观察，不把全部差额归因单一参数。新快照1进程PSS2288.38MiB、SwapPss72.00MiB，旧快照2为1958.34/419.81MiB：PSS方向与VMA方向不同，进一步证明不能用VMA推导进程物理驻留收益。缩小ring可能增加wrap等待，尚无严格匹配的FPS/P50/P95/CPU上传成本结论。

最终新APK已实际移动到ATTACK教学（仅观察到提示，不代表完成攻击验收），PID10156/gen1，TracerPid0，overlay输入buttons0。无新的RDC、native debugger或自动输入循环。

## 尚未完成的资源spec验收

VMA策略不是整个资源spec的完成声明。小RT导致的覆盖率回归门尚未通过；本次新APK未完成Bloodborne可操作场景、新策略极端Render0.25/0.375与Render1High画质对照、真实guest streaming有效/无效mip与部分dirty、DRS/hint往返/history、全shader Grad/LOD/gather组合或严格同相机CPU/GPU/P50/P95成本对照。首轮GPU检查覆盖Image/TileManager，不等同完整TextureCache回读+guest地址生命周期的端到端fixture。超出128MiB的真实游戏单次图像上传fallback尚未实际触发，已审阅所有staging调用路径并保留边界；没有完整游戏回归、FPS或总物理RAM收益声明。

## 最终交付与清理

最终APK9d6504bf67ed161cf13b0fdfb09e3a0c3982d7e4834566aa08308bc7a79b0fa3；host e4414acad6568e1d7e288c6db5de8bd44dcfeb6874c7e5f234df8af87dfa1ac4；JNI5c82b2082e090626a64b17930c8d40b97ad977834c60c1fc67bf865bb4ad107c。构建run1789889759494997000。新APK已安装Swan，最终PID10156/gen1正常UI Stop→Stopped/user_stop，guest return0。

已删除本轮临时CUSA50828 override，全局JSON与原始备份逐字节一致（Render0.5、Texture缺省High），legacy诊断属性恢复空值，驱动保留Turnip。没有活动游戏；gpu_memory按设计返回no renderer。未修改FEX/Foundation/Turnip子仓，未commit/push本轮资源策略，保留独立dear_imgui目录。原先ZAR/Turnip交付commit5fdf9682已推送，不混为本次未提交内容。

[证据清单](evidence/resource-policy-20260920/manifest.json)包含逐文件SHA；本机原始截图/日志保留在`build/validation/resource-policy-20260920`，不把房间截图或游戏内容放入提交材料。
