# 上游同步（2026-09-30，`feature/malos/upstream_0930`）

基线 `feature/malos/swan_performance` `e6117383`；上游 `shadps4-emu/shadPS4` `main` 截至 `2338a06f`（#5193）。与上游的 merge-base 仍是 `37cacc59`（#5060），fork 按 PR 逐个移植（`cherry-pick -x`，改动过的注明 adapted）。全部为本地提交，未推送。

09-10 以来上游 115 个提交：27 个已是祖先，41 个此前已移植或评估过（#5100 为评估后不合入），待处理 47 个（其中 4 个此前已评估）。本轮 35 个提交：32 个来自上游，涉及 36 个 PR（#5112、#5193 为部分移植）；另 3 个是 fork 配套修改（Android rwlock 相对超时、user data 偏移、缓存版本）。其余 7 个见“未合入”。

## 合入内容

| 提交 | 内容 |
| --- | --- |
| `fd1385ed` `d90304c7` `00cee7e4` `1edcc9cb` `8432fca1` `d3374a65` | 上游着色器指令修复：#5127 V_TRUNC_F64、#5125 V_MIN_F64、#5031 S_SUBB_U32 SCC、#5030 S_ADDC_U32 SCC、#5114 64 位浮点字面量放高位、#5185 V_BFM_B32 |
| `739d7995` `6df3ebf8` `4fd8fb9e` | #5145 Flat 修饰（VUID-06202）、#5131 KHR 路径 BaryCoordSmoothSample 定义 SampleId、#5141 属性加载中的 PackedAncillary 降级 |
| `6b434b2d` `69ab957b` `4bdb333b` | #5128 vdecsw、#5153 修正、#5160 vdec2 恢复原状 |
| `1f1eba20` | #5154 固件版本 0x13520001→0x14008001（仅桌面 kernel/http 使用，Android 不读） |
| `4cf6e598` `c9a413d3` `2992e720` `6f333ad5` `b85146ee` `758de573` `7d0764e6` | #5142 游玩时间竞争、#5136 无更新也可启动、#5161 KosmicKrisp 关 list restart、#5158 is_allocated 越界检查、#5177 system_reserved_addr 初始化笔误、#5178 sceKernelGetProcessType、#5184 memory_patcher 可修补其它可执行文件 |
| `5198457f` | #5137 S# 字段未定义时绑定默认 sampler（每次绑定一条 warning；刷屏时用日志类别开关关，不限流） |
| `deed2c71` | #5138/#5152 mesa-kosmickrisp gitlink → `bac93e02`（只记录 gitlink；本机子模块检出 `c502a84` 保持不动、未 stage） |
| `460de686` | #5123 IsExecuteError 位检查（adapted：x86 Unix 分支 `&0x16`→`&0x10`；Windows 保持 `ExceptionInformation[0] == 8`，未采用上游的 `0xf`） |
| `f82be7a7` | #5144 LIB_FUNCTION/LIB_OBJ 改调公共 `LinkSymbolImpl`（adapted：`SHADPS4_TYPED_HLE_HOST` 的 typed 适配器分支不变；测试 stub 补空定义） |
| `4723bf79` `ac4a809d` | #5175 sce timed rwlock 的超时是相对微秒：桌面移植（`reltimed*_np`，先 try 再按 steady_clock 截止等待）；Android host runtime 另行修正（`RwTimeout::RelativeUs`），两端各自实现、不共用代码 |
| `35a93d64` | #5172 设置修正（adapted：red zone 改为 General `redzone_patches`，旧键仍读；补 `userfaultfd` 覆盖项；fork 没有 `inline_fetch_shader`，删去对应日志） |
| `0ea246f7` | #5150 故障探测不跨出故障页 |
| `1b61718f` | #5193 部分：稀疏 backing 偏移、纹理缓存 GC 下限（Android 保留 `local_memory` 下限）；图像/GDS 屏障部分 fork 已有，跳过 |
| `fde80f5c` | fork 修复：无有效 sharp 的阶段（例如只读 user data 的 VS）匹配 permutation 时仍比较 `start.user_data`（见下） |
| `f54bd9e2` | #5126 compute 着色器使用 COMPUTE_PGM_RSRC1 的浮点模式（此前一律模式 0）；高通 FP32 FTZ quirk 仍按 profile 屏蔽 |
| `a990968f` | #5133 + #5156 + #5171 图像类型兼容：1D/1D 数组降级为 2D、保留 Cube 视图类型、cube 坐标与 V_CUBE* 移到后端（adapted，见下） |
| `0a2319d3` | #5169 ReadLaneElimination 提前（adapted：放在 ConstantPropagation 之后、fork 的 FragmentLdsPass 之前） |
| `95a5bdf1` | #5112 的 sharp 单次拷贝 + #5129（adapted，见下） |
| `11c21c18` | `ShaderBinaryVersion` 25→26、`ShaderMetaVersion` 14/15→15/16（x86/ARM），旧缓存自动重建 |

### 图像类型兼容（`a990968f`）的 fork 适配

- Render Scale：此前 cube 视图就是 `Color2DArray`，会参与缩放；上游改出独立 `Cube` 后，`SupportsScale` 与 texel fetch 重映射的 array 判断都加上 `Cube`，否则缩放后的反射 cube 与 Texture Low/Medium 下的 cube 资产会用错尺寸/LOD。
- `EmitImageRead` 先降级坐标再做缩放重映射；按上游写法会把未重映射的坐标交给 `OpImageFetch`。
- 非归一化采样时数组层号/cube 面号保持原值（上游改走 `get_coord`，会被层数相除）。
- 图像原子的 `OpImageTexelPointer` 也补齐坐标（`FixImageTexelCoords`）；上游仍传 1D 标量坐标，降级后是非法 SPIR-V。
- 1D 立即数 ConstOffset 直接生成 2 分量 S32 常量；上游用 `OpCompositeConstruct` 补零，结果不是常量且混用 S32/U32。运行时 Offset 才用补零。
- `vk_runtime.cpp` 保留 fork 的拷贝路径（上游 hunk 丢弃，`image.cpp` 的 `ConvertImageType` 已覆盖）；image view 保留 `Color2DMsaaArray`。

### sharp 单次拷贝（`95a5bdf1`）的 fork 适配

上游在 sharp 的各 dword 在 flat buffer 中连续时用一次 `memcpy` 取出。上游判定有两个漏洞，fork 均加了条件：dword 0 为立即数而其余 dword 在 flat buffer 偏移 1..N-1 时会被误判；128 位 T#（4 dword）对 8 dword 的 `Image` 也会被误判，拷贝时用 flat buffer 的相邻数据覆盖本应为立即数/零的 dword。因此只有 `num_dwords == N` 且 `load_mask` 全满、偏移连续时才走单次拷贝。另外不引入 `Invalid` 状态，未知位置仍按原来的逐 dword 检查，`Fetch<4>` 的行为不变。

### user data 偏移（`fde80f5c`）

片元阶段先于顶点阶段绑定，顶点阶段的 `start.user_data` 取决于片元着色器用了多少 user data 寄存器。`StageSpecialization::operator==` 在两边都没有有效 sharp 时直接返回相等，只读 user data 的 VS 就可能复用按另一个 `start.user_data` 编译的模块，从 push constant 的错误位置读寄存器。现在这种情况下只要阶段读 user data 就比较 `start.user_data`。上游 #5181 把 user data 改为从 flat buffer 读取、从根本上去掉这层依赖，但同时改 push data 布局（fork 的 `image_scales` 偏移、内部缩放 flatbuf 插入都要跟着改），本轮只做这处窄修复。

## 日志告警处理（AYN 首轮日志）

| 提交 | 告警 | 处理 |
| --- | --- | --- |
| `dd6b7f02` | `Float16/Float64 denorm preserving is not supported by the GPU` | 上游旧逻辑：着色器只用 fp16 时也会同时给 fp64 设模式并告警；Android 无 fp64（Turnip `shaderFloat64=false`），fp64 已降级到 fp32，不会进入 SPIR-V。改为 fp16/fp64 各按实际使用设置。FP32 preserve 以前每次编译报 `Unknown FP denorm mode`，现支持时发 `DenormPreserve 32`（高通专有驱动沿用 FTZ 的屏蔽、保持默认），只刷输入或只刷输出的混合模式 SPIR-V 无法表达，每种类型告警一次。A7xx Turnip 本身只能 flush fp16 denorm（mesa `tu_device.cc`），游戏要求 preserve 时 fp16 告警一次属实。`ShaderBinaryVersion` 26→27。 |
| `f83549a3` | `SanitizeCopyLayers: Coercing copy source layers N and destination layers N+1` | 来自 `ExpandImage`：数组图按层数增长重建（血源 1→6 层各一次），把旧图全部层拷进新图前几层是预期结果。只在目标层数少于源、确实丢层时告警。 |
| `5c099096` | `[Error] GetInstanceLayers: Failed to query layer properties: Success` | 没有安装任何 instance layer 是 Android 常态，查询成功时不再报错；请求了但缺失的 layer 仍逐个报错。 |
| `23b6344b` | `Session file nid=1G3lF1Gg1k8 error=2 result=-1`（Android）/ 桌面 `[Error] Opening path ... failed, file does not exist` | 游戏探测可选文件，不存在是正常结果。改为 `Kernel_Fs` Info 级、带操作名和路径；其它失败仍为告警并带路径。 |

未改：`Skipping lowering for null buffer sharp`（上游 #5008，空 V# 读 0、写丢弃与 GCN 一致，每个着色器排列一次）、`RemoveUnreachableBlocks`、跳过模式的 `RecordEscape`（设计中的一次性回退提示）、加载器 `DT_FLAGS`/`Unimplemented type SCE ...` 与可选扩展缺失提示（均为上游、信息性且有界）。

验证：桌面构建通过；桌面血源（新构建）进中央亚楠、30 FPS、画面正常，原 5 条层数告警消失，无 denorm/Render 错误，`config.json` 逐字节恢复。Android host/APK 构建通过，APK `07104e93…` 已装到 AYN（host `22ed1419…`）；装包时设备前台是其它应用（`com.tencentmalos.xrgamenative`），未启动游戏，Android 侧日志复核待设备空闲。

## 未合入

| 上游 | 原因 |
| --- | --- |
| #5112 其余部分 | 屏障跟踪改回 interval set：GPU 命令线程每 draw 都走，需在 Swan/AYN 做 GpuComm A/B 后再定（建议移植，保留 fork 的 BreakHoist/EndRendering）。无锁页表 `CopySparseMemory`：文件映射没有页表项会读成零（fork 现在读得对），`find()` 无越界检查；fork 已有 GuestReadCache 与独显主机 stream buffer。fetch shader 解析重写：遇到 fork 解析器能处理的写法会 ASSERT，且 `num_elements` + 忽略 `v_mov` 常量会让 `buffer_load_format_xyz` 后接 `v_mov v7, 1.0` 的写法丢掉 w；序言条件被写反（`!fetch_data.Empty()`），不能照抄。sync batch 检查（依赖未合入的 #5100）、纹理缓存改名、`image.cpp` 删同布局写后屏障（上游 #5193 又加回）、GDS 屏障与存储图像转换范围（fork 已有）、`fetch_shader` 改指针（fork 并行预载按值保存）、`inline_fetch_shader` 设置 |
| #5181 | 见上，改用窄修复 |
| #5165 | 回退 #5100 的批量上传，fork 没有这部分 |
| #5157 | fork 的 `Linker::Resolve` 已只在 3 段名时才查库/模块 |
| #5151 | 针对上游 #5100 的稀疏 `cached_pages`（`EnsurePages`），fork 没有 |
| #5164 + 其 revert | 净效果为零 |
| `db510c68` zlib 许可证 | fork 已有（`c08306a5`） |

## 验证

- 桌面 clang-cl（x64-Clang-RelWithDebInfo）：全部 35 个提交后编译、链接通过。
- 桌面血源（RX 7600M XT，缓存版本已升，全部着色器重新翻译）：上次会话提示框 → 片头 → 标题 → 继续 → 中央亚楠，走动并转镜头，画面正常、30 FPS；日志无 Render 错误。本次生成的 310 个 SPIR-V 全部通过 `spirv-val --target-env vulkan1.3`（标题 27 个，其余为世界场景）。测试用游戏覆盖 `custom_configs/CUSA03023.json`（`dump_shaders`）用后删除；新构建启动时会按 `g_scm_rev` 重写 `config.json`，测试后已逐字节恢复（`CB175989…`）。本轮没有确认场景中出现 1D 图像或 cube 采样，cube 路径只有 spirv-val 与代码层面的检查。
- Android host：`run-host-build.cmd` 在 `shadps4_turnip_source` 失败——基线提交 `12e8d00a`（09-30 新增源码构建 Turnip）的 `scripts/android/build-turnip-source` 只支持 Darwin/Linux（`import fcntl`、工具链目录映射、meson/bison），与本轮合入无关；这台 Windows 机器上 host `.so` 与 APK 目前都无法构建。改为用 `compile_commands.json` 里的 NDK 命令编译 host 目标的全部 112 个翻译单元（24 个 unity + 88 个单独文件；不链接，`turnip_identity.h` 用仅供编译的占位头文件，产物放在临时目录）：0 失败。链接与 APK 未验证。
- 旧 Turnip 构建（用户要求“turnip 先用旧的”）：新增默认关闭的 `build-host-android --turnip-prebuilt-lock`（CMake `SHADPS4_TURNIP_PREBUILT_LOCK`），用 `runtime/locks/turnip-bionic-mainline-86ca.json`（12e8d00a 之前的默认 mainline，发布包 `035fecd1…`、ELF `ea4853bf…`、`git-86ca472fc2`）代替源码编译 Mesa；`build-turnip-source prebuilt` 校验包与 ELF SHA、写同一套 identity/头文件/许可证，`bind-host`/`package` 对 prebuilt 只校验锁文件不再比对 Mesa 源码，`fcntl` 改为仅源码编译时导入。默认源码编译路径不变。此后 Windows 上 host `HOST_LINK_PASS`（`libshadps4_host.so` `2a468678…`）、APK 构建成功（`TURNIP_PACKAGE_PASS ea4853bf…`，R8 对照 `fdd37852…` 照旧）。
- AYN Thor（`9c2841a4`，Turnip mainline 属性）：APK `6ad38d42…` 用同一 debug 证书 `install -r` 安装并核 SHA（原包 `5a53463b…` 已备份）。血源 `game_id` 直接启动，日志与 maps 确认加载 `Mesa 26.3.0-devel (git-86ca472fc2)`、`ea4853bf…`；缓存按新版本号重建（删除 876 个旧文件）。片头 31 FPS → 中央亚楠世界约 23 FPS，打开物品栏、走动、拾取采血瓶，画面正常，host 日志无 Render 错误。新出现的 FP16/FP64 denorm preserve 不支持提示来自 #5126（compute 现在请求游戏的浮点模式，驱动不支持的模式照旧不发）；`SanitizeCopyLayers` 5 条层数收敛警告在 09-28 旧日志中同样存在，非本轮引入。约 1.5 分钟后会话被 `com.android.launcher3`（pid 13041）force-stop、随即重新打开 app（设备上有人工操作），未继续 TMNT 等后续测试。存档仅血源 4 个文件正常推进、无新增删除，`global.json` 不变。
- 未测：TMNT、MHW、Swan；高通专有驱动；1D 图像与 cube 在场景中的实际命中；sharp 单次拷贝的 SRT verify 计数。
