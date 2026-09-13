# ARM64 SRT、BC 纹理与 Android WSI 修复（2026-09-14）

本轮已越过真实 TMNT 首次 compute shader 的 ARM64 SRT abort、后续宏 BC 纹理断言，并消除了本次 Android Surface 上的逐帧 swapchain 重建。最后一次普通 APK 运行记录 **143 次 guest present**，随后在 **sceAvPlayerSetLogCallback / eBTreZ84JFY / op110 / Unsupported** 停止。120 秒运行用例仍为 **FAIL**，没有可玩场景、正确像素、十分钟或 Swan 验收。

源码为 `f2bcd299` 加本轮修改；[各阶段原始证据](2026-09-14-srt-bc/README.md)、[最终 APK 的源码与产物身份](2026-09-14-srt-bc/latest-build.json)保留 dirty 身份，不回填成提交后的哈希。最终 host Build ID `0fb74d950aa28a1aa8f6355f5444d8c75aeef7d1`，JNI `fe0d2545fbf444aa8a69c539cefce40cf49859f1`。FEX/Foundation 未改。

## 修复与依据

### 现代 SRT，复用桌面 IR 分析

此前 ARM64 的 `FlattenExtendedUserdataPass` 是无条件 UNREACHABLE；旧 reference 的静态 offset walker 不覆盖当前 ReadConstBuffer 动态表达式。现在共用现代 IR 分析、GVN、指针树与布局生成，在非 x86 上生成不可执行的 PortableSrt 计划，含常量/flat 引用、整数运算及 Push/Copy/Pop。每次刷新重新读当前 user data；不保存 IR 池指针，不生成 x86 代码，不增加信号处理器。

计划限制深度和条目数，检查表达式后向引用、索引、复制范围和栈平衡；动态指针采用现有 48 位地址语义。内存读取在 MemoryManager 共享锁内检查映射、权限与边界：实际物理映射走 backing alias，避免 GPU watch 的 PROT_NONE；Linux 非物理路径通过 self process_vm_readv 失败返回零。Apple 对应 mach_vm_read_overwrite 仅有源码，未运行验证；其他非 x86 平台不盲读未知映射。沿用 desktop 的无效 SRT 读取归零策略，无 VM coordinator 锁跨 GPU 调用。

另外修复共用逻辑中的同地址加载覆盖：ReadConst/ReadConstBuffer 不再覆盖先前条目；别名合并同时更新子指针树与动态索引，x86 与 portable 均解析到同一布局。缓存 x86 版本升至4、非 x86 使用5，拒绝旧布局与跨架构代码；portable 缓存按剩余字节验证长度和计划，不执行缓存里的机器码。**这只加固新增计划反序列化，未宣称整个历史 Archive/Info 输入已安全。**

### 宏 BC 与 micro mip 尾部

下一次真实 abort 为 `ImageInfo::UpdateSize` 的 `ASSERT(!props.is_block)`。短期诊断确认纹理为 BC3 / 1024×512 / 11级 / Thin2DThin，非 SRT 垃圾地址。诊断代码已移除。共享桌面 size/detile 路径已按压缩块计算64/128位元素；删除旧禁止断言后，还补齐 CPU allocation 与 GPU 地址计算不一致：CPU 会将小 mip 降为 micro，原 shader 却始终按 macro 寻址。

现在分配和 tiling shader 共用同一降级判定；ImageInfo 在对齐前记录每级 micro mask，tile/detile push constants 都携带该信息。shader 只在有效 mip 范围内选址，超出范围不访问描述符。BC1/BC3 的逐级字面 footprint 和未压缩对照已测试；不把所有 tiling/采样/alt 模式或 GPU 像素 roundtrip 宣称已覆盖。

### SUBOPTIMAL 与离线轮询

Android 保持现有 Identity transform 时，驱动可以持续返回 SUBOPTIMAL。现在查询 Surface capabilities：与创建时一致则继续消费已获得的 image/semaphore，能力变化、OUT_OF_DATE、SURFACE_LOST 仍重建；desktop 保持原策略。最新 TMNT 的 [host 日志摘录](2026-09-14-srt-bc/apk-twentysixth/android-host-current-excerpt.log)中能力未变记录1次、Recreate记录0次；累计日志边界与 SHA 在 [观察记录](2026-09-14-srt-bc/apk-twentysixth/wsi-observation.json)。日志尾部未完全刷出，不用它推导完整帧数。

网络/SSL 按用户要求不扩展。仅给 NPManager 的 CheckCallback / ForLib 接入 desktop 离线空队列兼容：当前未准入 NP 事件注册/生产者，且 shadNet 关闭、离线设置成立才返回0；在线模式和实际注册仍明确 Unsupported，不调用桌面全局 native 回调指针。真实 TMNT 覆盖 CheckCallback，ForLib 未单独执行。

## 定向验证

环境：AYN Thor / Android API33 / ARM64 / 4KiB，APK uid10157，固定 Turnip，无系统驱动回退。完整 base+update 内容沿用此前已核验的44文件，未重新导入内容。

| 检查 | 结果与限制 |
|---|---|
| native SRT/缓存/映射/BC | 479 checks / 0 failures；含每个缓存截断前缀，不是479个独立功能；该 DSO 早于最后 WSI 修改 |
| x86 NDK syntax | SRT、cache、ImageInfo、TileManager 4/4；不是桌面运行 |
| tiling shader | 64/128位 × micro/macro × tile/detile，8种 glslang + spirv-val PASS；没有像素 oracle |
| 最新普通 APK synthetic graphics | 6代生命周期与真实 guest GPU present PASS |
| 真实 TMNT（修复前后） | SRT后2 presents→NP fault；NP后宏BC abort；BC后140 presents→AvPlayer fault；WSI后143 presents→相同AvPlayer fault；长运行用例均保留FAIL |
| 构建 | 实际 Android host DSO 和 APK 链接；host 保留 --no-undefined |

初期 fixture 的未对齐保留区、缺直接内存 Allocate、错误 BC1 边界常量，与真实 SRT 同地址加载缺陷分别归档；不删除失败证据。未做全量回归。一次启动调试在 attach 前因目标退出而失败，未创建 session，adb forward 清理检查为空；不把这次调试描述为成功捕获。

下一项实际依赖是桌面的 **本地 AvPlayer**：SetLogCallback 本身为 desktop 空实现，但 Init/解码缓冲/文件与事件回调需要正确 guest ABI、实例所有权、FEX 回调及取消后排空。不能将 native player 指针、guest 函数地址或未验证输出直接互传，也不能以空 Init/跳过片头代替视频播放。本轮没有实现 AvPlayer。继续直接实现与定向验证，不另拆 spec。
