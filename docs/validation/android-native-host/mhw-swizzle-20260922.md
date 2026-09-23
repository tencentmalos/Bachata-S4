# MHW subgroup 语义续修（2026-09-22）

接续 [DLC / 静默提示 / LDS 修复](mhw-dlc-silent-20260922.md)。本轮用户要求继续修复 MHW。AYN 在17:21后由另一任务交还，本轮完成GPU定向验证、TMNT诊断包部署；随后明确交回该任务复现战斗后的堆错误。MHW游戏复测等待再次交还。源码保留本地未提交。

## 已取得的证据

- V4 shader 0x15e44dcc 的五个 DS_SWIZZLE 位掩码操作按 lane XOR16/8/4/2/1 选取不同来源，生成的 SPIR-V 却使用 OpGroupNonUniformBroadcast。对实际 SPIR-V 的五个索引表达式逐 lane 求值，均得到64种值；两处后续读取31/63为常量。离线审计保存于 `build/validation/mhw-swizzle-20260922/v4-broadcast-audit.json`。
- [Khronos 合同与源码对照](../../../references/psvr-public-api/vulkan-subgroup-notes.md)确认 broadcast 的索引一致性要求与此不符；静态 spirv-val 成功不足以发现这种运行时一致性错误。语义错误成立，但尚不能声称它是 V4 ErrorDeviceLost 的唯一原因。
- 既有同设备 GPU probe 日志记录：Qualcomm 默认 subgroup64，固定 SHA 的 Turnip 默认128。MHW 在归约后读取31/63；片段阶段原来没有请求64，不能假设 host 分组与 guest 一致。

## 修改

- ReadLane 统一使用 OpGroupNonUniformShuffle，声明对应 capability。初版仅改非常量；实机Turnip在常量31/63仍有2048个像素失败，Qualcomm通过。相同探针改用Shuffle后两驱动全部通过，旧失败日志保留；不把它称为所有驱动Broadcast均不可用。覆盖DS_SWIZZLE和固定lane读取。
- quad 模式逐 lane 可使用不同来源，改为四个常量 quad broadcast 后选择，避免把动态排列索引直接传入 QuadBroadcast。
- 片段着色器实际使用 lane/wave 操作时，按设备 subgroupSizeControl、min/max 和 requiredSubgroupSizeStages 显式请求64。Compute 既有检查也补 min 和阶段支持；不伪造不支持设备的完整64-lane模拟。
- 序列化/反序列化保存 uses_lane_id/uses_group_ballot，避免预加载缓存漏掉64要求。shader binary15，meta x86=10 / portable=11。
- vkQueueSubmit 失败时补 generation、scheduler、tick、cached_retired、host_serial、elapsed、wait/signal 数量。仅读取缓存，不追加 GPU 查询/等待，保留原失败传播。失败 submit 仍可能晚于真正出错的命令，不把这组 metadata 当成首个错误 GPU writer。

## 验证状态

NDK Android host 和 `android_swizzle_probe` 已编译；首次常量 unsigned 重载错误与探针 IR 类型错误已修，原构建日志保留。新增探针使用生产 GCN decoder / IR / SPIR-V 后端，覆盖五个 XOR 模式、256个 quad 排列及固定31/63，共263种；分别标记来源 lane 是否 active，跳过无定义来源结果，并要求每种模式至少半幅 active 来源覆盖。附4组 shader meta round-trip。GCN invalid-source 返回0的完整 EXEC/WQM 语义仍未实现；本轮不将 Vulkan inactive-source 的未定义结果作正确性验收。

最终263种模式，Qualcomm与固定Turnip各 **808199检查/0失败**；每种1024个有效来源像素，四组metadata往返正确。生成263份SPIR-V通过Vulkan1.3验证。初版Turnip808199/2048保留为反例。原APK `a69073d1` 未安装；最终实现随 [TMNT诊断包](tmnt-backtrace-20260922.md) `ade81d86` / host `75b7a709` / JNI `a58e6d46` 部署并核全包SHA，已验证TMNT标题菜单。**没有在最终包上重验MHW，不声明DeviceLost消失或已进入关卡。** GPUprobe的完整host SHA见manifest；它与最终包之间仅有调用栈诊断改动，GPU代码相同。

GPU探针仅在自有临时目录运行并清理。APK部署、前轮诊断属性恢复、存档/配置比对及设备交接见TMNT记录。未改ZAR、固件或FEX子仓，没有commit/push。
