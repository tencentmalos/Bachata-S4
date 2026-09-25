# AYN MHW：Guest trace / KGSL 与 SGPR、VCC 掩码语义（2026-09-25）

接续 [跨物理段回写修复后的 MHW 超时](backing-write-20260922.md) 和 [subgroup 修复](mhw-swizzle-20260922.md)。用户明确把 AYN 交给 MHW 独占复现；先正常停止 TMNT，再冷启动 MHW。通用操作入口见 [Guest GPU command trace / PM4 trace](../../debugbus-gpu-command-trace.md)，AGENTS.md、CLAUDE.md 均有索引。

**当前结论：** 已确认并修正SGPR/VCC mask写与整数读不一致，最终两驱动各11776项定向检查通过。MHW的GPU超时仍可复现，尚未进入可操作关卡。固定lane Broadcast及关闭异步提交均未消除超时；最终源码撤回这些实验。下一步应捕获间接dispatch的实际维度及输入资源内容，核对首个错误writer；目前的command trace不包含完整资源内容快照。

## 身份与范围

- AYN Thor `9c2841a4`，Android 13 / Adreno 740，MHW CUSA09554 15.23 整合 ZAR，未改游戏文件。
- 基线 APK `694bed42a67ebd579c3690517341f23bd8234920c6f1bafc75b96d876bc601df`，host `36ada4424694fc4df1f9abac9fc91f97f306876850028fd32f9cb806e554d09f`，JNI `c63977bb4500ddb90b554feac49da5b79e096dd4bd3dc1cdbd17c5398af3567f`。
- Turnip `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`，Mesa 26.0-devel `5ac41be677`；保持分辨率 0.5、原配置和异步提交。
- trace 实现 `c694b179a` 经 `a562e8100` 已合入 `malos/main`；本轮源码基于 `3e920b862`。安装包实测两个 DebugBus 别名均可用。
- 本地证据根目录 `build/validation/mhw-gpu-20260925/`，游戏代码、原始 trace、快照、APK 不提交 Git。原 52 个用户数据文件已备份，`pre-userdata.tar` SHA256 `f07e7f581b71b3a58626d4568832f3c30a48827421213058b58603c4343d9e31`。

## 先排除旧快照，再关联本轮故障

run1 复用进程 PID3918/gen2，未输入亦超时：内核 ctx33/ts12445，IB1 `0x4160219000`、IB2 `0x415ed90400`，Present 线程等待 frame 时 DeviceLost。但 KGSL dump 中的 process32289/ctx16/日期是旧现场，SHA `e070…`，不能用于本轮归因。读出旧 dump 后确认 snapshot timestamp 清零，再做 run2。

run2 冷启动 PID19743/gen1，UUID `bee5bc59770d9344452b02b1b8afa03c`。仅以有界 DebugBus Cross 分别关闭 `70a-MW1` 和 `710-MW1` 网络错误，随后加载至 13254 flips 超时：

| 证据 | 本轮匹配值 |
| --- | --- |
| kernel | `[1072505.693197]` TGID19743，ctx21，ts41595 |
| host | scheduler1，tick26507，cached_retired26505，host serial41595 |
| IB1 | `0x40930de000`，65536 DWORD，REM `0xbe60` |
| IB2 | `0x4091c0ebc0`，REM0 |
| snapshot | OS 时间1790336344，ctx21，process_id21007（日志中 PID19743 的 Guest-1 TID） |
| dump | 13,813,320 B；SHA256 `f13b23b1b3ca74c3e2e200046fa824ac8513b70b53ae48d1dbad96cdb9ef8883` |

753 个 section、有 END。分析数据库 `/tmp/mhw-run2-20260925.sqlite`；未核设备内核 Build-ID 对应源码，不把本机 Swan kernel HEAD 当成 AYN 内核身份。工具的 host PM4 命名需再对照本仓 Mesa A7xx XML。当前工具未展开 MVC_V2 / SHADER_V2，原始 section 另行解析；MVC_V2 包含 bit31 标记起始寄存器、后跟结束寄存器和连续值的压缩范围，不能直接视为整段 register/value 对。

按 Mesa crashdec A7xx ROQ 预取校正：ROQ `0x00f8003e`，游标约 `65536−48736−(248−1)=16553`，落在一个 4 B buffer copy 后的 cache clean。源 `0x4040c99310`、目的 `0x40935fdb20` 均有有效 allocation。09-22 v6/v7 也停在类似 copy/cache 阶段；这只定位等待阶段，不能证明 copy 是首个错误命令。

## Guest trace 把候选缩小到实际 compute shader

故障前捕获 `74d43777871cef611322be8130a74ea6`：PM4 2,692,295 B、gcmd 1,142,302 B，flips13252..13254，完成2/16帧，因停止推进主动 `save`，保留 `ended_by: save requested` 的 partial。frame13252 含674 draw、93 dispatch、71 indirect dispatch；frame13253 含621 draw、83 dispatch、71 indirect dispatch。共165个 GCN shader、1613个 action。

IB 中最后一个先于 cache 等待的 CP_EXEC_CS 为 **507×1×1、local size64**；SP_CS shader 基址 `0x4093123f80`，取出10,240 B 原生代码。12组 L0 SHADER_V2 的指令字与该 shader 的循环区间大量重合，但没有据此声称精确故障 PC。

Guest trace 中匹配 **cs `0x2ee568a7`、507×1×1**：action152664/submission172272，后一帧 action153487/submission172285。Guest code `0x13df892c00`、4776 B，与设备 shader dump 原始 GCN 逐字节相同。保存原生 IR3、GCN 反汇编和旧 SPIR-V，证据在 `run2/cs-*`。

## 确认的翻译错误与修复

该 kernel 保存 EXEC 到 s20:s21，逐 lane 比较完成标记到 s0:s1，再用普通整数指令比较两个64位 mask：

```text
0x02b0 s_and_saveexec_b64 s[20:21], vcc
0x0d20 v_cmp_ne_u32 s[0:1], 0, v18
0x0d28 s_sub_i32 vcc_hi, s20, s0
0x0d2c s_sub_i32 vcc_lo, s21, s1
0x0d30 s_or_b32 vcc_lo, vcc_hi, vcc_lo
0x0d34 s_cmp_eq_u32 vcc_lo, 0
```

翻译器原来把每 lane 的布尔 `ThreadBitScalarReg` 与普通 U32 `ScalarReg` 分开维护，mask 写只更新前者。旧 SPIR-V 的循环退出条件读到旧 s20:s21 / s0:s1（资源描述符残留），实际比较产生的 mask 没有参与退出条件。这是明确的 SGPR 别名语义错误；故障关联与最终游戏验证应分开陈述。

修复在 ScalarGPR 的 mask 写入点同时生成 ballot 的低/高32位数值视图，未使用时由 DCE 移除。SGPR→SGPR 的 S_MOV_B64 复制已有数值及布尔视图，不能从 ballot 重建普通指针/descriptor。CollectShaderInfo 标记 Ballot 的 subgroup 需求；shader binary version15→17（16 为未发布初版），使旧错误缓存失效。不按游戏 hash 特判，不跳过 dispatch 或放宽 GPU timeout。

## 初版反例与 lane 恢复兼容

初版 APK `0168d2e8` / host `ad66c4b6`，run3 PID10844/gen1/UUID `afb02556cd4279b522d585caf101032a`，未按键启动约两分钟后在 FS `0xd3b68e7b` 的 `MarkReadConstBufferSharpSources` 断言；无新 KGSL timeout。该 shader 在旧 trace 已出现，是引入的编译回归，不能称为越过 GPU 故障点。

`V_READLANE_B32` 先恢复真实32位数据，再以 `SetDst1` 恢复附带 mask bool。旧 shadow 可能未定义或属于普通数据；初版 SetDst1 新增的 ballot 会覆盖恢复的数据及相邻SGPR，破坏 sharp 来源。修复为 SGPR 仅恢复 shadow，保留已有数字视图。对实际 GCN 做同一 CFG / SSA / 常量传播 / ReadLaneElimination / ResourceDiscovery，旧库与修正版均识别386资源，初版断言。离线输入的 PS 插值配置为测试参数，不冒充全套游戏 SPIR-V 编译验证。

## 定向验证

`android_scalar_mask_probe` 解码上述真实 GCN 指令，经过生产 translator / SSA / SPIR-V，在64-lane GPU 上检查 mask 的0、1、31、32、33、63、64位前缀和交错位，保存 EXEC、整数相等比较、mask复制、普通64位数据复制、VGPR lane 暂存/恢复和相邻SGPR保持。

| 驱动 | 旧库 | 新库 |
| --- | --- | --- |
| Qualcomm Adreno740 | 7680检查 / 4160失败 | 7680 / 0 |
| 固定 SHA Turnip | 7680检查 / 4160失败 | 7680 / 0 |

生成8份 SPIR-V 均通过 `spirv-val --target-env vulkan1.3`。初次 Turnip 命令传错目录，尚未加载驱动即退出，修正后取得表中结果；这些启动错误不计为 GPU 测试。decoder `selftest` 通过。

实机修复包仅替换基线 APK 的 host DSO，保留既有 SCAP2、JNI、Surface 修复和全部其他条目；签名、安装后全包及 DSO SHA 已核对。五个修改的 translation unit 从当前源码独立编译并替换链接，其余对象复用与已安装基线 host SHA 一致的构建，不改其它任务的构建产物。符号完整 host SHA `ce533ccde47619dbe6597d8b8f0d2ee80db40c8a6388b81d5fb240feb7573ecf`；安装 APK `284b37b6ba36a86932f380faa3716da764dff15ba600ba5a55436edf88776625`，strip-debug host `e01ab6cc57c5099011fda9b77bcd7305cf2f3db45cfbef03356df362b40ec33f`。

## V2 继续超时与 VCC 数值视图

run4 PID19678/gen1/UUID `b487bd13eec442bfa463377e60d2b49c` 在2931 flips超时：ctx29/ts10516，scheduler1/tick5865/cached5863；快照SHA `33fe5294c08310a904c5cb698393920e685716885bf4c7314f10be3c6e4dee2c`。run5 PID23053/gen1/UUID `fdcd1a415ea304411cbe8eb5062919a6` 经两个网络错误提示后8038 flips超时：ctx29/ts25932，快照SHA `ff9748db5ec0901ce24bc0211db968d72d19d85e3d1322b8fc18bf0eba472b43`。两轮捕获的14464 B原生compute代码完全相同。

run5末尾 partial trace `8f76d9f504478302f8fa7a54102272bc`（8036..8038，2帧）将间接参数地址后缀 `ddc0 → dde0` 与宿主 `cdc0 → cde0` 的顺序关联，候选缩小到 local size32×2×1 的 **cs `0xa38aae6c`**。原生与Guest代码中还有相同常量 `0x7ef312ac`；没有据此声称精确故障PC。

同一类遗漏还存在于VCC：该shader在0x1220 `v_cmp_eq_u32 vcc,0,v11`，随后在0x1224/0x1228用 `vcc_lo/vcc_hi` 做整数减法。原来的SetDst1只写VCC bool；V2 SPIR-V读取了前次标量算术的旧VCC。修复扩展到VCC的比较/进位写，S_MOV_B64在SGPR与VCC间复制现有两个视图，V_READLANE_B32仅恢复shadow，不重写相邻word。

扩展测试含VOPC、VCC→SGPR mask复制、SGPR→VCC普通64位数据、VCC lane恢复及高word保持。Qualcomm旧库11776检查/6912失败、Turnip旧库11776/7232（未定义旧值使计数可不同）；新库两驱动均11776/0。原FS资源回归仍386。真实新shader SPIR-V的两个整数减法已改读比较后的ballot。

V4 APK `221a251eab4031a9fc928b9eccf7a2229f5b201e7fe80c399ea5f54a72a368e8`、host `894f5c96fd870776b8b1763f695607ad0193c23521dc340d7942ae4f729518c3`（binary19），run7 PID8828/gen1/UUID `17b2105c555b566431767dff0c0e8310` 仍在2867 flips后DeviceLost/exit33：ctx32/ts10403、scheduler1/tick5739/cached5737；快照14,206,848 B、SHA `36e80446ac60360a5ec071a4f606f0e6fdb01141bfefb28b712ca5bfd2b5e4a0`，OS1790340161/process9758（Guest线程）。IB1 `0x40939c3000`、IB2 `0x4093a16380`、CS `0x4093a12b00`；实际新原生代码SHA `7cbee496283fc4e083a258bb8c5b8a0640411d435b72fd07c4fb25e02d33ea52`，区别于V2 `bd650a02…`。该轮arm命令发出前进程已退出，不能声称拿到了run7 Guest trace。

## 固定 lane 对照与设备干扰

V3曾试验compute常量lane使用Broadcast（保持动态Shuffle和fragment原路径），binary18、APK `14b9c3ca` / host `3455b4a5`。32×2工作组的全wave、低32、高32分歧控制流，Shuffle/Broadcast两个版本各6个case，两驱动均1536检查/0失败。该有限测试不证明真实shader无问题。

run6 PID32564/gen1/UUID `e2843dcc5f00fc749fae72c34f8c5db2` 运行至12941 flips。20:34另一任务安装并启动 `org.spatial.editor.aotprobe`，MHW失去前台；20:35:01系统 `Stopping service due to app idle`，guest正常返回Cancelled/7205761603303115028，snapshot timestamp为0。没有新GPU故障，该轮亦不能当作完整通过。V4撤掉Broadcast以单独验证VCC，复现上述超时；后续V5再做固定lane的单变量对照。

V5 APK `1e06db9eafb09f94710624ad3a96c15299a0028b0cf4ef899941ad197a2a7fa7`、host `5e4772d097fa7802cb7fafe716c28806cf899a0dd1c76c27454f503957965816`，run8 PID14825/gen1/UUID `797e5c5510790258bafb7a7ef465156a` 仍在2941可显示帧后超时（trace包含后续已消费命令至2943）：ctx32/ts10568、scheduler1/tick5885/cached5883，snapshot SHA `abec4611191a1e9e5e5c3507b4d1b4ca11216cf5de2d4b74a1935beee60c1609`。实际新CS为 `0x409332ea00`、13184 B；SPIR-V确认固定lane已用Broadcast。partial trace `d4862475e322ee2759813c7eb06a880e`，PM4 SHA `6b61be639882a5045cd31e2a753c36cde483fd6a983a801ce9e0a38c29379e00`、gcmd SHA `bf891783eeccfe22be6822c5929a7f230a61a9d3670322830c92447d745c7bfe`。不支持将常量Shuffle单独认定为根因，最终撤回Broadcast。

run9重新安装V4（binary19，SGPR+VCC修复、原Shuffle），只将 `debug.shadps4.async_submit` 临时设0。PID20388/gen1/UUID `055f3cfd3168dd0c99fc1e82e72f07cf`，2612帧附近仍超时：ctx29/ts9696，IB1 `0x4093112000` / IB2 `0x409316d380`；snapshot SHA `0c8be0b5f6c27cb8b8a50503bef6ed9d997db9cc805f5cf500bd5471a367deec`。最终上层为GuestFault（op71），不是异步worker的exit33路径；有匹配内核GPU timeout，不能据上层错误类别排除GPU故障。属性已恢复1。

## 最终交付与收尾

保留SGPR/VCC数值视图修正、64位复制及lane恢复兼容、Ballot能力收集与定向GPU测试。最终shader binary version21，避开本地实验版本16–20的缓存；未改变DS_SWIZZLE动态路由、驱动、超时阈值或跳过dispatch。没有Mesa/FEX/Foundation源码改动。

最终V6（仅将V4的缓存版本19提升到21；shader翻译行为相同）安装APK `3bf534a6098786a816778dee5443d7639bec9518d7ea425edb6973c5c8032283`，host `d8563331113406ab4f26d7af5327c59bc0a331e154fec7cdbe45be603a2a9a15`，符号完整host `0b05dfc68198b927c0a5a38556ae0a9039c761754f12e5642b68cc876f2f6193`。6个生产TU及测试从最终源码独立编译，沿用基线其余对象；两驱动最终各11776/0，生成的每驱动8份SPIR-V全部通过Vulkan1.3验证。APK仅host条目改变，安装后核对全包及DSO SHA。V6已回Library，未再重复MHW失败轮次，不将定向probe当成游戏通过。原52文件中50个SHA不变，只有MHW的 `memory.dat` 及其 `sce_backup/memory.dat` 随游戏启动改变；无新增/删除，配置逐字节保留，未格式化或恢复覆盖存档。前后tar及完整SHA清单保存在本地证据目录。

尚未完成完整可玩、关卡、战斗或存读档验证，GPU超时仍是开放问题。

设备收尾：无活动FexSessionService，未附加调试器、无ADB forward或持有输入；仅本任务scrcpy会话已正常停止。`async_submit=1`、`shader_dump`空值恢复；本任务 `/data/local/tmp/shadps4-mhw-gpu-20260925` 探针目录清理，选定原始证据留本地。
