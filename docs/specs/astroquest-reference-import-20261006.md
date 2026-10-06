# AstroQuest 参考引入规划（2026-10-06）

状态：规划 spec，本轮未实现任何条目。

依据：
- 参考仓库 `references/AstroQuest`，固定在 v0.18 `9ff3e43`（GPL-2.0-or-later，与本仓同许可）。它的核心 `shadps4-arm64-main/` 由首个提交 `100bfd8` 导入 zenithblue-oss shadps4-arm64 `be6bc2e`，与本仓 `references/shadps4-arm64` 是同一基线，所以 `git -C references/AstroQuest diff 100bfd8 9ff3e43 -- shadps4-arm64-main` 就是它对核心的全部改动：573 个文件，去掉随仓的 OpenXR SDK 后约 2 万行。各改动的动机见其 44 个提交的说明。
- 本仓源码：分支 `feature/malos/swan_performance`，HEAD `8e38a1035`，含未提交改动的工作树；文中本仓行号以该工作树为准。只读对照了五个方向：音频、VR 输入与追踪、渲染与着色器、游戏专用修正与运行时、PCVR 与 XR 宿主架构。高影响结论另经人工复核，复核项见各条。
- Turnip 源码 `references/mesa-turnip`。
- 既有对照文档 [AstroQuest 与当前 XR 实现对照](../astroquest-vr-comparison-20261006.md)。本 spec 对它的更正集中在第 8 节。

与现有文档的分工：

| 文档 | 范围 |
|---|---|
| [AstroQuest 对照](../astroquest-vr-comparison-20261006.md) | 两个项目的架构与能力比较 |
| 本 spec | 哪些改动值得引入、按什么顺序、移植到本仓架构时怎么改、如何验收；也包括对照中发现的本仓自身问题 |
| [GCN 翻译层 v2](gcn-translation-layer-v2-20261003.md) | GPU 翻译层的长期结构；本 spec 的渲染项都是可单独合入的小修复或测量项 |

路径约定：`AQ:` 指 `references/AstroQuest/shadps4-arm64-main/src/`（9ff3e43），AstroQuest 根目录下的文件写全路径；本仓路径相对 `src/`，`docs/`、`foundation/`、`references/`、`cmake/`、`CMakeLists.txt` 等仓库根下的路径照原样写。“已核实”指读过双方源码，“推断”指没有运行验证的判断。

---

## 0. 结论

AstroQuest 是为一款游戏（ASTRO BOT Rescue Mission，欧版 CUSA12392 的 1.00/1.04）做的移植。可取的东西按价值分成五类：

1. **与具体游戏无关、本仓有同样问题的修复（立即可做，用现有游戏验证）**：
   - GS 输入图元缺 5 种（遇到即 `UNREACHABLE`）；
   - GS 的 invocation id（V7）从未设置；
   - viewport 关闭的槽位被压缩；
   - 桌面内核服务线程的定时器漏唤醒；
   - 桌面首玩家登录与 UserService 事件门控；
   - 存档内存返回大小；
   - 桌面 AudioIn 持锁轮询。
2. **Android GPU 与 XR 性能（先测量再决定）**。这一类不是 AstroQuest 的主要贡献，而是对照它的做法时，在本仓发现的问题：
   - 本仓所有颜色图像带 `MUTABLE_FORMAT` 且不给格式列表，Turnip 因此全部关闭 UBWC 压缩（已核实）。我们的 Android 设备基本 GPU 受限，这是本 spec 里潜在收益最大的一项。
   - XR 线程每个 runtime 帧都整图拷贝一次并在 CPU 上等 fence，重复帧也一样（已核实）。
   - 同一 pipeline 重复绑定，Turnip 每次都重发状态（已核实）。
   - 每次 HLE 调用两次加共享锁并复制 `shared_ptr`（已核实）。
3. **PSVR 通用能力（任何 PSVR 游戏都可能用到）**：
   - VrTracker 的 Recalibrate 状态过渡，以及 DS4 位姿结果；
   - 系统重置视角事件下发给游戏；
   - 头部线速度兜底；
   - `VR_VIEW` 能力位与社交屏端口；
   - 相机帧节拍；
   - 由 XR 驱动的 vblank 节拍；
   - 外倾眼的投影模式；
   - Audio3d 对象空间化、7.1 虚拟环绕；
   - 无触摸板手柄的触摸板模拟。
4. **ASTRO BOT 专用（缺内容，暂不能验收）**：
   - 时间步与固定分辨率（做成 guest patch 包）；
   - governor（需要扩展补丁 SDK）；
   - Android 上的 libSceFiber；
   - 真实麦克风；
   - 用双手位置推算 DS4 位姿。
5. **PCVR（独立的大项目）**：复用本仓 Android 上已验证的 XR runtime、GuestReprojection 和传感器层，不整体搬 AQ 的 `openxr_host`。最大缺口是桌面 13 个 PSVR 帧提交接口全部返回 UNSUPPORTED（已核实）。

不引入的有：Quest 宿主的 glibc 子进程架构、Json2 HLE、未实现导入一律返回 0、FEX TSO 开关、timeline semaphore 改动、cube array 修复（本仓已有等价实现）、MSAA 采样数取交集、KGSL 兼容层、实时信号采样。理由见第 6 节。

**测试内容**：本机 `D:\game\ps4\zar` 和 Thor 上只有 7 款游戏：血源、MHW、MHR、TMNT，以及三款 PSVR 游戏 Beat Saber CUSA12878、Tetris Effect CUSA13427、All-In-One Sports CUSA36289。**没有 ASTRO BOT。** 第 4 类在拿到 CUSA12392 欧版 1.00/1.04 的 dump 之前只能做到单测为止。

---

## 1. 参考仓库的事实

| 项 | 内容 |
|---|---|
| 引用 | `references/AstroQuest`（submodule，`9ff3e43`，见 [references/README](../../references/README.md)），自带嵌套 submodule，读源码不必初始化 |
| 核心改动规模 | 去掉 OpenXR SDK 后：库 8451 行/52 文件，`core/vr` 4809 行，`renderer_vulkan` 2693 行，`texture_cache` 854 行，`known_title*` 1097 行，`guest_cpu` 654 行，`input` 约 1100 行 |
| 自带测试 | `tools/tests/` 下 13 个单测，约 2100 行：`openxr_view_test`、`spectator_view_test`、`headset_fov_cache_test`、`held_pad_test`、`pad_router_test`、`stick_finger_test`、`pad_gestures_test`（及 `touch_game_model.h`）、`virtualizer_test`、`known_title_builds_test`、`json_test`；核心另有 `tests/gcn/test_geometry_inputs.cpp` |
| 作者验证范围 | 0.18 发布说明：佩戴验证了 PC 上的 1.04 和触摸板替代动作，以及 Quest 版 Touch 控制器；其余多数是模拟头显或未佩戴的 Quest 3；Index、Bigscreen、Pimax 是社区报告 |
| 运行时形态 | Quest：GLES 写的 OpenXR 前端，模拟器跑在 glibc 子进程里，画面经 AHardwareBuffer/dma-buf 共享，用 Unix socket 通信。PC：游戏进程内的 OpenXR（enable v1），HLE 直接用宿主指针访问 guest 内存 |
| 署名 | 部分提交与他人合作，例如 1.04 地址表来自 Clodo76（`5d73650`）、SteamVR 修复来自 evertec82（`94b4e21`）。移植时在提交说明中注明来源提交和合作者 |

---

## 2. 内容与可验证性

| 游戏 | 能验证的条目 |
|---|---|
| Beat Saber CUSA12878（PSVR，双 Move） | C1 重复帧拷贝、C2 节拍、C3 视角重置与头部速度、C5 投影、A4 viewport 槽位（推断：每眼一次 draw，都用槽位 0，可能不受影响）、D1 回归（Move 不能受影响） |
| Tetris Effect CUSA13427（PSVR，DS4；NGS2 13 个导入、Audio3d 12 个） | E3 NGS2（Android 上它在更早处就停住了，先在桌面验证）、D1 DS4 结果、E1 空间化、J 桌面 PSVR 提交（它走 WithOverlay，AQ 不支持这个接口） |
| All-In-One Sports CUSA36289（PSVR，VS 输出 Layer） | C 系列回归、A 系列回归 |
| 血源、MHW、MHR、TMNT（2D） | B1 UBWC、B2 重复绑定、B3 深度写开关、B4 HLE 查找；A 系列回归（血源有确定性回放）；E1（MHW、MHR、TMNT 用 Audio3d）；E2（血源有 8 声道端口） |
| ASTRO BOT CUSA12392（**缺**） | H 系列全部；D2、G、E4 的游戏内验收 |

渲染改动统一用血源的确定性回放做回归：同一 trace 的帧哈希与逐事件图像哈希不变（[GPU 回放](gpu-replay-20261004.md)；回放结果按显卡区分，只能和同一显卡的基线比较）。

---

## 3. 工作包

每项依次写：来源 → 本仓现状 → 做法 → 工作量 → 验证 → 风险或前提。工作量 S 约一天以内，M 约数天，L 为一周以上。

### WP-A 通用着色器与渲染修复（先做，一个提交）

**A1 GS 输入图元补全**
- 来源：`AQ:shader_recompiler/backend/spirv/emit_spirv.cpp:25-48`、`AQ:shader_recompiler/backend/spirv/spirv_emit_context.cpp:43-68`；AQ 把它和 Astro 1-4 关卡的崩溃联系在一起。
- 现状（已核实）：`shader_recompiler/backend/spirv/emit_spirv.cpp:26-44` 的 `GetInputPrimitiveType` 缺 TriangleFan、Polygon、LineLoop、AdjTriangleStrip、AdjLineStrip，遇到即 `UNREACHABLE`；输入顶点数一侧（`shader_recompiler/backend/spirv/spirv_emit_context.cpp:45-63`）同样缺。前端 prologue（`frontend/translate/translate.cpp:302-324`）已处理两种邻接 strip，Fan/Polygon 落到 default，只设置了 V0。
- 做法：照 AQ 补齐映射和顶点数，Fan/Polygon 按三角形设置 V0–V2 的偏移。LineLoop 在拓扑映射里本来就不支持（`video_core/renderer_vulkan/liverpool_to_vk.cpp:109-143`），只补到不崩溃为止。
- 工作量 S；只影响当前会崩溃的路径。

**A2 GS invocation id（V7）**
- 来源：`AQ:shader_recompiler/frontend/translate/translate.cpp:229-238`、`AQ:shader_recompiler/backend/spirv/spirv_emit_context.cpp:478`、提交 `e856864`。
- 现状（已核实）：GS prologue 没有设置 V7；`BuiltIn::InvocationId` 只在 TessellationControl 中声明（`shader_recompiler/backend/spirv/spirv_emit_context.cpp:578-580`）；但 `shader_recompiler/backend/spirv/emit_spirv.cpp:479` 照常设置 `Invocations=N`。结果是每只眼睛各跑一次的 GS 读到的眼睛编号是未定义值。
- 做法：GS 声明 InvocationId，prologue 写入 V7；`ShaderBinaryVersion` 37 → 38。
- 工作量 S。

**A3 GS 特化比较**
- 来源：`AQ:shader_recompiler/runtime_info.h:149-158`。
- 现状（已核实）：`num_invocations` 已参与比较；`in_vertex_data_size` 和 `out_vertex_data_size` 没有，而这两项会改变生成的代码（`shader_recompiler/backend/spirv/spirv_emit_context.cpp:568` 的输入数量、`ir/passes/ring_access_elimination.cpp:95-139` 的偏移）。
- 做法：比较中加入这两项。
- 工作量 S。

**A4 viewport 槽位保留**
- 来源：`AQ:video_core/renderer_vulkan/vk_rasterizer.cpp:1496-1510`。
- 现状（已核实）：`video_core/renderer_vulkan/vk_rasterizer.cpp:2885-2889` 遇到 `xscale == 0` 就 `continue`，后面的槽位因此前移。深度范围模拟也按压缩后的序号排列（`video_core/amdgpu/depth_range.h:80-89`），而 shader 按 guest 的 ViewportIndex 选槽位（`backend/spirv/emit_spirv_special.cpp:104-114`）。
- 做法：关闭的槽位占位保留，深度范围索引同步修改。两处必须一起改。
- 工作量 S–M。只有“低号槽位关、高号槽位开”时行为才会变。

**验证（A1–A4）**
- 移植 `tests/gcn/test_geometry_inputs.cpp`；新生成的 SPIR-V 全部通过 `spirv-val`。
- 血源回放的帧哈希和逐事件哈希不变。
- 加一次性统计：GS 的输入图元、invocation 数、是否读 V7，以及写 ViewportIndex 且槽位有空缺的次数，在六款游戏上各跑一次，记录哪款游戏实际走到这些路径。目前没有证据表明我们库里有游戏走到 A2 或 A4，这类修复的价值主要在 Astro 和类似的 PSVR 游戏上。

### WP-B Android GPU 与 CPU 性能（先测量，再决定是否合入）

**B1 UBWC：给图像声明格式列表**
- 来源：`AQ:video_core/texture_cache/image.cpp:177-195` 声明 `VkImageFormatListCreateInfo`（自身格式加对应的 sRGB/UNORM）；视图格式不在列表内时由 `EnsureViewFormat`（`:298`）把内容迁移到允许任意格式的图像。
- 现状（人工复核）：本仓所有图像都带 `eMutableFormat` 且不给格式列表（`video_core/texture_cache/image.cpp:173-209`）。Turnip `references/mesa-turnip/src/freedreno/vulkan/tu_image.cc:604-662` 对这种颜色图像在所有代都关闭 UBWC：a7xx_gen3 虽有 `ubwc_all_formats_compatible`，但 `format_list_ubwc_possible()` 在没有列表时直接返回 false（`:506`）；其他代走 `!mutable_ubwc_fc` 分支直接关闭。推断：Thor、Pocket DS、Swan 上我们的颜色渲染目标和纹理都没有压缩。
- 做法：
  1. 先测量：`TU_DEBUG=perf` 统计 “Disabling UBWC … mutable formats” 的次数和尺寸分布。
  2. 第一步只给渲染目标声明列表（自身格式加本仓实际会建的视图格式）；列表外的重解释走迁移或不压缩的回退。
  3. 在 Thor 或 Pocket DS 的血源上做同会话 A/B：GPU 忙碌率、帧率、`gpu_timing` 的 pass 时间。
  4. 回放哈希不变。
- 工作量 M–L。
- 风险中高：本仓有大量格式重解释、ASTC 重编码、缩放时替换 backing，任何遗漏都会表现为画面错误。

**B2 同一 pipeline 不重复绑定**
- 来源：`AQ:video_core/renderer_vulkan/vk_scheduler.h:445`，同一 pass 内跳过重复绑定。
- 现状（已核实）：`Pipeline::Bind` 每次都绑（`video_core/renderer_vulkan/vk_pipeline_common.cpp:34-45`）；`tu_CmdBindPipeline` 没有“与当前相同就返回”的判断，每次都把描述符、常量、程序标脏并重发状态（`references/mesa-turnip/src/freedreno/vulkan/tu_cmd_buffer.cc:5631-5671`）。
- 做法：在 recorder 或 scheduler 里记住每个 command buffer 当前绑定的 pipeline，相同就跳过；描述符和推送常量仍照常更新。
- 工作量 S–M。先用 simpleperf 看 VkRecord 线程在 Turnip 绑定路径上的占比。

**B3 深度写开关来回切打断 render pass**
- 来源：`AQ:video_core/renderer_vulkan/vk_rasterizer.cpp:157-178`、`AQ:video_core/texture_cache/texture_cache.cpp:906`。
- 现状：`video_core/texture_cache/image_view.cpp:87` 把深度写开关当作 view 的一部分，深度写一开一关就换 view 和 layout（`video_core/renderer_vulkan/vk_rasterizer.cpp:2471-2485`），在 tile 渲染的 GPU 上会打断 pass。
- 做法：先用 pass_log 统计“相邻两个 pass 只差深度 ro/rw”的次数，次数多再改。
- 工作量 M。风险：深度同时被采样时 layout 是否合法、缺失内容跟踪、高通的状态重发问题。

**B4 HLE 查找去锁**
- 来源：`AQ:core/guest_cpu/hle_call_adapter.h:87-95`，无锁数组。
- 现状（已核实）：`HleCallRegistry::Find` 每次加共享锁并复制 `shared_ptr`（`core/guest_cpu/hle/call_adapter.h:522-528`），每次 HLE 调用要查两次（`core/guest_cpu/fex/fex_context.cpp:666, 704`）。血源每帧约 2400 次同步 HLE，此外还有 GNM 等调用。
- 做法：注册在会话启动前完成，之后只读，查表改为下标访问加裸指针，生命周期由 registry 保证。
- 工作量 S。先用 simpleperf 量出锁和引用计数原子操作的占比。

**B5 BDA 页表**
- 现状（已核实）：`video_core/buffer_cache/buffer_cache.cpp:238-244` 无条件分配 2^(40−14) × 8 B = 512 MiB 显存；AQ 只在打开 DMA 设置时分配。
- 做法：评估能否按需分配，前提是没有任何 shader 用到 BDA；MHR 的修复依赖 BDA，需要逐个确认。目的是降低 Swan 等设备的内存压力。
- 工作量 S–M。

**B6 每 N 个 draw 提交一次**
- 来源：`AQ:video_core/renderer_vulkan/vk_scheduler.cpp:70-74`，每 64 个 draw。
- 现状：只在 SubmitDone、带中断的 fence 和空闲时提交。
- 做法：先用 LiteP 看 GPU 在翻译期间是否空闲，确认空闲再试。
- 优先级低。

### WP-C XR 运行时（Swan，与游戏无关的部分）

**C1 重复帧不再拷贝**
- 现状（人工复核）：`video_core/renderer_vulkan/openxr/runtime.cpp:806-847` 在每个 `shouldRender` 的 runtime 帧都 acquire 一张 swapchain 图像、从 mailbox 整图拷贝，`FinishCopy` 中在 CPU 上等 fence（`:391`）。重复帧也是如此；新帧到达时还另有一次拷贝（`:1291-1336`）。
- 做法：只在 mailbox 有新帧时 acquire、拷贝、release；重复帧继续提交引用该 swapchain 的 projection layer 及原渲染姿态，OpenXR 会使用最后一次 release 的图像（AQ 即如此，`AQ:core/vr/openxr_host.cpp:1583-1615`）。同时评估用 semaphore 代替 CPU 等待。
- 工作量 S–M。
- 验证：Swan 上 Beat Saber 的 XR 线程 CPU 时间、GPU 忙碌率和重复帧比例 A/B；双目截图与录像正常；Pico 合成器的行为需要实测。

**C2 由 XR 驱动 vblank 与节拍**
- 来源：`AQ:core/libraries/videoout/driver.cpp:574-799`：
  - vblank 等于头显刷新并与宿主显示锁相，相位落在周期的 55%；
  - 帧按头显刷新的整数倍出（90 Hz 下 90/45/30）；
  - VR 帧在 GPU 画完时就翻页。
- 现状：vblank 固定用设置值，默认 60 Hz（`core/emulator_settings.h:428`）；VR 帧排队到下一个 60 Hz vblank 才翻页（`core/libraries/videoout/driver.cpp:410-457`）；XR 线程独立 `xrWaitFrame`，不把显示时刻反馈给 VideoOut。推断：60 Hz 与头显 72/90 Hz 不同步，会出现一帧显示 1 次、2 次交替的抖动。
- 做法：XR 线程发布预测的显示时刻和周期，VideoOut 在 XR 会话中以此产生 vblank 和计节拍；节拍值来自每游戏设置。
- 工作量 M–L。
- 验证：每个游戏帧占几次显示刷新（应恒定）、重复帧比例；Beat Saber 回归。

**C3 视角重置与头部状态**
- 来源：`AQ:core/vr/vr_runtime.cpp:280-366, 520-576`。
- 现状：OpenXR LOCAL 重置只重锚影院和状态层（`video_core/renderer_vulkan/openxr/runtime.cpp:730-733, 773-777`），不向游戏推送 `ResetVrPosition`（`core/host_runtime/guest_platform.h:197-216`）；头部线速度只取 `XrSpaceVelocity`，没有兜底（`video_core/renderer_vulkan/openxr/runtime.cpp:132-145`，`core/host_runtime/guest_vr_sensor.cpp:179-216`）；flip 到显示的延迟报 1（`core/libraries/hmd/hmd.cpp:149-150`）。
- 做法：
  1. 系统重置时向游戏推送 `ResetVrPosition` 事件。
  2. runtime 不给线速度时用位置差分补出。
  3. 延迟报合理值（AQ 用 13000/10000 µs）。
- 工作量各 S。
- 验证：打印 `velocityFlags`，确认 Pico 是否给线速度；在 Beat Saber 中重置视角后画面正常。

**C4 `VR_VIEW` 能力位与社交屏端口**
- 来源：`AQ:core/libraries/videoout/video_out.cpp:420` 的注释说游戏没有这个能力位就不进 VR；社交屏端口的翻页立即完成，按 60 Hz 发 vblank（`AQ:core/libraries/videoout/driver.cpp:55, 413, 848`）。
- 现状：Android 的能力位固定为 0（`core/host_runtime/guest_runtime.cpp:2468-2475`）；只接受主输出，桌面直接断言（`core/libraries/videoout/video_out.cpp:316`），Android 返回 `INVALID_VALUE`（`core/host_runtime/guest_runtime.cpp:4226-4229`）。
- 做法：XR 投影会话中设置 `VR_VIEW`；实现 `AUX_SOCIAL_SCREEN` 端口，翻页立即完成、60 Hz vblank、不显示。桌面和 Android 都做。
- 工作量 S。
- 验证：三款 PSVR 游戏回归；用 Astro 的导入表确认是否用到。

**C5 外倾眼的投影模式**
- 来源：`AQ:core/vr/openxr_view.h` 及其单测 `tools/tests/openxr_view_test.cpp`：交给 runtime 的是与游戏相机一致的平行视图和 FOV，由合成器重投影到真实的外倾眼。
- 现状：本仓把 runtime 每只眼的外参（含外倾角 θ）移到游戏渲染头姿上，并从游戏的平行画面中按 tan 范围裁剪（`core/host_runtime/vr_geometry.h:77-125`、`core/host_runtime/guest_reprojection.h:83-115`）。θ=0 时两者等价。推断：θ≠0 时每只眼的画面整体偏约 θ，双眼错开约 2θ。
- 做法：
  1. 先移植 AQ 的单测思路，写本仓的 CPU 单测：θ 取 0、±5°、±10°，计算无穷远方向在两种方式下的角误差。
  2. 在 Swan 上读出 `xrLocateViews` 的实际 θ。
  3. 增加“平行包络”模式：任一只眼外倾超过 0.5° 时默认启用。
- 工作量 M。
- 前提：θ 只在部分头显上不为 0（Index、Pimax 等）。Swan 推断为 0，因此主要服务于 PCVR。

**C6 补齐 XR 队列锁**
- 现状（已核实）：Begin/End/Acquire/Release 持 `QueueMutex`，但建会话、建 swapchain、列举图像不持锁（`video_core/renderer_vulkan/openxr/runtime.cpp:434, 490, 495`）。AQ 说明 Virtual Desktop 列举图像时会向队列提交（README-PC-VR:408）。
- 做法：这三处加锁。
- 工作量 S。

**C7 会话丢失后重建**
- 来源：`AQ:core/vr/openxr_host.cpp:626-654, 1251-1264`：会话或实例丢失后循环重建，期间暂停游戏。
- 现状：会话丢失、退出或实例丢失时置失败，渲染线程退出，不重建（`video_core/renderer_vulkan/openxr/runtime.cpp:722-741`）。
- 做法：在同一个 XrInstance 内重建会话、swapchain 和输入；“很久不要画面就重建会话”只对 Virtual Desktop 做（AQ 提交 `94b4e21`）。
- 工作量 M。Android 上优先级低，PCVR 必须做。

### WP-D PSVR 设备 HLE

**D1 VrTracker 校准状态与 DS4 结果**
- 来源：`AQ:core/libraries/vr_tracker/vr_tracker.cpp:38-101, 258-301, 364-413, 544-549, 618-625`。
- 现状：
  - 只有一个 pad handle（`core/libraries/vr_tracker/vr_tracker.cpp:40`）。
  - SBS/OpenXR 分支对 DS4 handle 返回 DEVICE_NOT_REGISTERED（`:279-284`），状态恒为 TRACKING（`:290, 312`）。
  - Recalibrate 直接返回 OK，代码里留着 TODO（`:482`）。
  - Android 准入表没有 `tNJrfYsY3wY`、`24kDA+A0Ox0`、`ufexf4aNiwg`（`core/host_runtime/guest_runtime.cpp:1645-1649`）。
  - ASTRO BOT 1.04 会停在轮廓就坐画面，等 CALIBRATING 状态出现。
- 做法：
  - 移植 200 ms 的校准窗口；
  - 允许登记多个 DS4，GetResult 接受 pad handle：没有位姿来源时返回 OK + NOT_TRACKING，有来源时写 `pad_info.device_pose`；
  - 补上 3 个 NID 的准入。
- 工作量 S。
- 验证：host 单测检查 CALIBRATING→TRACKING 的序列；Beat Saber 回归（HMD 与 Move 分支不能动）。

**D2 DS4 位姿来源**
- A 档：Swan 右手控制器的 grip 加偏移当作 DS4，按每游戏开关启用。工作量 S–M。
- B 档：蓝牙 DualSense 的 IMU 加手部追踪，用 `references/AstroQuest/quest-host/cpp/held_pad.h`（纯数学头文件，可直接引入）和 `AQ:core/vr/vr_runtime.cpp:423-500` 的融合算法。本仓需要以下几步，工作量 L：
  - runtime 启用 `XR_EXT_hand_tracking`（Foundation 已有 `XrHandJointTracker`，本仓未引用；`video_core/renderer_vulkan/openxr/runtime.cpp:291-303` 未启用该扩展）；
  - Kotlin 注册索尼手柄的传感器（`foundation/modules/input/android/src/main/kotlin/spatial/input/android/AndroidInputSource.kt:325` 目前为 `sensors=emptyList()`）；
  - Foundation InputHub 透传 `SensorSample`（目前直接丢弃，`foundation/modules/input/src/input_hub.cpp:278-281`）；
  - OrbisPadAdapter 发布角速度与加速度（`core/host_runtime/orbis_pad_adapter.cpp:39-44` 目前只发单位四元数）。
- 看不见手柄时的放置规则（玩家自设位置 > 最后看见时的相对位置 > 标准位置 (0, −0.17, −0.50)，见 `AQ:core/vr/vr_runtime.cpp:878-946`）两档都需要。
- B 档里“透传传感器数据”对普通游戏的陀螺仪操作也有用。
- 前提：Swan 是否支持手部追踪扩展、握着手柄时追踪质量如何，都未核实，先打印扩展列表。

**D3 相机节拍与社交屏 HLE**
- 来源：`AQ:core/libraries/camera/camera.cpp:57-79, 965`（等下一帧时阻塞，相机帧不超前于 guest 呈现，最多等 200 ms；曝光增益回显到帧元数据）、`AQ:core/libraries/social_screen/social_screen.cpp`（7 个函数）。
- 现状：`core/host_runtime/guest_camera.h` 有 14 个 NID 和黑帧，但每次调用直接递增帧号，不等待也不与 guest 同步；`wgBMXJJA6K4`、`RHYJ7GKOSMg`、`zIKL4kZleuc` 未准入；没有 SocialScreen HLE。
- 做法：
  - 等待时先释放锁，并可被 Stop 取消；
  - 帧号与 guest 翻页计数挂钩；
  - 补上缺的 NID；
  - 新增 7 个 SocialScreen 桥。
- 工作量：相机 M，SocialScreen S。先用 Astro 的导入表确认是否需要。

**D4 对照中发现的本仓小问题**（各 S，修之前逐一复核）
- Android `GetPlayAreaWarningInfo` 在 SBS 模式下算出结果后没有写回 guest（`core/host_runtime/guest_runtime.cpp:4534-4545`）。
- VrTracker 的 `device_timestamp` 用 OpenXR 单调时钟，`timestamp` 用进程时间，两者不在同一时间域（`core/libraries/vr_tracker/vr_tracker.cpp:288-289`）。
- Move 的 OpenXR 分支不填加速度，非 OpenXR 分支填 9.81（`core/libraries/move/move.cpp:66-74, 94`）。

### WP-E 音频

**E1 Audio3d 对象空间化**
- 来源：`AQ:core/libraries/audio3d/audio3d_spatializer.*`，约 140 行纯 DSP：Brown–Duda 头模型、分数延迟的耳间时差、头影搁架滤波、后方低通、块内参数渐变、限幅器。
- 现状：桌面 cubeb 和 Android 共用 `core/libraries/audio3d/audio3d_mixer.cpp`，对象只乘增益、不考虑位置，并且硬削顶；bed 的下混是对的（`:69-91`）。
- 做法：挂到共用的 `ProcessMixQueue`，两端同时受益；预分配缓冲；加开关；保留本仓的 bed 下混。AQ 的 bed 只取第 0、1 声道（`AQ:core/libraries/audio3d/audio3d.cpp:548-551`），这一点不要照搬。
- **前提**：坐标约定冲突。AQ 认为 +Z 在听者后方（`audio3d_spatializer.h:12-13`）；本仓 OpenAL 路径把 z 取反，即 +Z 在前方（`core/libraries/audio3d/audio3d_openal.cpp:682-685`）。先用已知方位的声源确认。
- 工作量 S。
- 验证：`audio_capture` 看左右声道的时差和电平（MHW、MHR、TMNT，以及 Swan 上的 PSVR 游戏）。

**E2 7.1 虚拟环绕**
- 来源：`AQ:core/libraries/audio/surround_virtualizer.*`，用同一个 Spatializer 摆放 7 个虚拟扬声器，LFE 低通后混入；带单测 `virtualizer_test.cpp`。
- 现状：`PrepareAudioStereo` 把 8 声道直接折叠成立体声（`core/libraries/audio/audioout_transfer.h:72-105`）。
- 做法：Swan XR 会话中对 8 声道端口启用；需要每端口可变状态和预分配缓冲，而 Oboe 端的 `Prepare` 目前是 `const noexcept`，在 pin 内调用（`core/host_runtime/guest_audio.cpp:484-504`）。桌面上作为可选的“耳机模式”。
- 工作量 S–M。
- 验证：血源的 8 声道 MAIN/BGM 端口做 A/B。

**E3 NGS2**
- 来源：`AQ:core/libraries/ngs2/*`，新写约 3500 行，真实实现 28 个入口：
  - 结构：采样器语音最多 256 个 → 子混音、混响 → mastering。
  - 格式：PCM 12 种、自写 HE-VAG 解码；ATRAC9 用 LibAtrac9。
  - 线程：渲染在游戏调用 `sceNgs2SystemRender` 时同步完成，没有宿主工作线程。
- AQ 仍是占位的部分：`ParseWaveform*`、`CalcWaveformBlock`、`GetWaveformFrameInfo`、`Pan*`、`Geom*`、`Report`、Stream/Fft；语音回调参数被忽略；没有 UserFx2；EQ 只登记、不处理。
- 现状：
  - 桌面 HLE 是占位，出声为静音；但 `core/libraries/sysmodule/sysmodule_internal.cpp:217` 允许加载用户提供的 `libSceNgs2.sprx`，即走 LLE。
  - Android 没有 NGS2 桥；Tetris 的 13 个 NGS2 导入全部是 `unsupported_import`。
  - 我们库里只有 Tetris 导入 NGS2，而它需要的 `ParseWaveformData`、`CalcWaveformBlock`、`PanInit`、`PanGetVolumeMatrix` 在 AQ 中也只是占位。
- 做法，两条路，先评估代价小的一条：
  1. Android 上把固件 `libSceNgs2.sprx` 加入 LocalLibrary 列表走 LLE（`core/host_runtime/guest_runtime.cpp:4952-4956`），先核对它依赖的模块。
  2. 移植 AQ 引擎：抽出不访问 guest 内存的 DSP 核心，两端共用；Android 新建 `guest_ngs2` 桥，要求：
     - 句柄用会话内的不透明 ID；
     - 波形数据渲染时有界地分块读取；
     - 先渲染到宿主 scratch，所有回调结束后再短暂 pin 住输出缓冲写回；
     - 效果回调和分配回调走 `HleScope::InvokeGuest`，参数放在会话自有的 guest scratch 中，pin 不跨回调；
     - `SystemLock` 改为可取消、带所有者的锁；
     - 回调失败返回具名错误，不 abort；
     - 另补上 Tetris 需要的那 4 个函数。
- 工作量 L。
- 门槛：先在桌面让 Tetris 走到 NGS2 调用；ATRAC9 解码结果与本仓 AJM 的 at9 比对。

**E4 麦克风**
- Android 真实输入（AQ Quest 端的做法：`RECORD_AUDIO`、AAudio/Oboe 输入、`VOICE_RECOGNITION` 预设、有界环形缓冲、可取消的读取、补上 `HqOpen`）：在 Astro 或其他依赖麦克风玩法的游戏成为目标之前只作参考，工作量 M。我们库里目前没有哪款游戏的玩法依赖麦克风。
- 桌面立即修：`core/libraries/audio/audioin.cpp:245` 的 `GetSilentState` 要拿端口锁，而 SDL 的 `Read` 持锁轮询最多 1 秒（`core/libraries/audio/sdl_audio_in.cpp:80-109`）。改为缓存可用状态，不在读取中持锁。工作量 S。

### WP-F 桌面立即修复（各 S，互不依赖）

- **F1 内核服务线程漏唤醒**（已核实）：`core/libraries/kernel/kernel.cpp:70-83` 在 `io_context.run()` 之后才把 `asio_requests` 清零，运行期间新到的请求计数会被抹掉，定时器（`core/libraries/kernel/equeue.cpp:145-165`）可能不再触发。AQ 在取请求时清零（`AQ:core/libraries/kernel/kernel.cpp:75`）。补压力测试。
- **F2 首玩家登录与 UserService 事件门控**：
  - 现状：`input/controller.cpp:391-398` 只在 `controller_count == 0` 时登录首玩家且不判空；`sceUserServiceGetEvent` 没有初始化门控，事件队列也不加锁（`core/libraries/system/userservice.cpp:119-141`）。
  - 来源：AQ 提交 `9eb257a`；`AQ:core/libraries/system/userservice.cpp:131, 197`。Android 已正确（`core/host_runtime/guest_platform.h:118-131, 177-188`）。
- **F3 存档内存返回大小**：应返回“文件大小”和“请求大小”中较大的一个（`AQ:core/libraries/save_data/save_memory.cpp:136`）；本仓返回文件大小（`core/libraries/save_data/save_memory.cpp:238`），桌面和 Android 共用这段。配一个截断文件的单测。
- **F4 诊断细节**：映射失败的断言加上区域信息（`core/memory.cpp:2515`）；地址空间预留时记录 `0x400000`–`0x10_0000_0000` 内被跳过的区间（`core/address_space.cpp:129-185`）。
- **F5** 即 E4 的桌面部分。

### WP-G 触摸板模拟（无触摸板手柄和 XR 控制器）

- 来源：三个无依赖头文件 `AQ:input/moved_finger.h`、`stick_finger.h`、`pad_gestures.h`；单测 `stick_finger_test`、`pad_gestures_test`，以及根据 Astro 读取规则建立的模型 `touch_game_model.h`。
  - `MovedFinger`：落点停留和最长移动速度保证游戏逐帧采样能看到完整轨迹。
  - `StickFinger`：右摇杆当手指，回弹时从峰值位置抬起。
  - `PadGestures`：用按键完成按下、向前滑、向后拉三种手势。
- 现状：
  - Android 的触摸坐标只来自屏幕 overlay 和 DebugBus（`core/host_runtime/orbis_pad_adapter.cpp:121-126`）；
  - 触摸 id 恒为 1（`core/host_runtime/orbis_pad_adapter.cpp:124-125`），而桌面每次新触摸递增（`input/controller.cpp:144-146`）；
  - XR 映射只能“左手握 + 按摇杆”触发触摸板按下（`core/host_runtime/openxr_pad_mapping.h:32-33`）。
- 做法：
  - 头文件放到 `src/input`，桌面和 Android 共用；
  - OrbisPadAdapter 的端口 0 对“没有触摸板”的来源生成触摸；
  - 每次新触摸分配新 id；
  - 按每游戏开关，默认关闭，因为会占用右摇杆和若干肩键。
- 工作量 M；移植 AQ 单测为 host 单测。

### WP-H ASTRO BOT 专用（需要 CUSA12392 欧版 1.00/1.04）

**H1 时间步与固定分辨率：做成 guest patch 包**
- 来源：`AQ:core/known_title.cpp:541-666`。
  - 每次 `sceGnmSubmitDone` 入口：用宿主时钟测帧间隔，平滑后算出步长，限制在 1/60 到 1/20 秒；超过 0.25 秒的帧当作卡顿忽略。
  - 写入游戏的三个全局量：double 帧率、float 秒数、u64 微秒。
  - 把分辨率控制对象的 offset、最高、最低三个字段写成同一个值。
  - 版本识别：装入后、写入前核对镜像内容（`AQ:core/known_title_builds.h:74-115, 184-227`）。
- 本仓的做法：hook eboot 中 `sceGnmSubmitDone` 的 PLT 入口（TMNT 已 hook 过 6 字节的 `ff25` 跳转），用 `shad_sdk_clock_ns` 复刻算法，通过数据绑定写入。血源 60 FPS 包是同类先例。
- 需要两个 eboot 的 SHA256 和 PLT 偏移，AQ 没有给出文件哈希。
- 工作量 S–M。
- 验证：先在桌面做；用 `guest_patch enable/disable` 对比游戏速度和过场的音画同步；装到错误版本时应被改写前字节校验拒绝。

**H2 governor**
- 来源：`AQ:core/known_title.cpp:220-537`。用“帧时长 × GPU 忙碌率”估计 GPU 耗时，GPU 忙碌超过 85% 才缩小尺寸，否则放慢节拍。
- 本仓：补丁 SDK 目前只有 `query`、`clock_ns`、`counter`、`log`，宿主数值传不进补丁。需要扩展 SDK，提供只读的宿主指标（GPU 忙碌率、XR 显示周期）和节拍请求。
- 工作量 M。
- 推断：Android 上的瓶颈多在 CPU 或 GpuComm，这时缩小尺寸无效。

**H3 更大渲染尺寸（PC）**
- 尺寸表和渲染池常量需要“数据补丁”（现有字节补丁只能落在可执行段：桌面 `core/guest_patch_desktop.cpp:215-217`，Android `core/host_runtime/guest_patch.cpp:202-219`）。
- extra dmem 只能用每游戏配置：桌面已有 `extra_dmem_in_mbytes`；Android 后备内存固定 12 GiB（`core/host_runtime/guest_runtime.cpp:505`）。
- 优先级低。

**H4 Android libSceFiber**
- 来源：`AQ:core/libraries/fiber/fiber_fex.cpp`，拿到整组寄存器的 HLE 函数切换 rbx、rbp、r12–r15、rsp，跳板的 `ret` 从新栈弹出返回地址。
- 现状：fiber 只在 x86_64 构建中编译（`CMakeLists.txt:1042-1047`）。我们的 HLE 返回时会写回全部通用寄存器（`core/guest_cpu/fex/fex_context.cpp:137-149, 761-762`），并且已有直接操作寄存器帧的 HLE 处理函数（`core/host_runtime/guest_graphics_hle.h:25`），所以 AQ 的方法可以照做。
- 做法：照做之外，补上 MXCSR 和 x87 控制字的保存，按会话管理挂起状态，处理 Stop、取消和跨线程恢复。
- 工作量 M。
- 风险：FEX 的 call-ret 栈在换栈后预测失败，应退回 dispatcher，需要实测。
- 我们库里的 7 款游戏都不导入 libSceFiber。

**H5 用双手位置推算 DS4 位姿**：即 D2 的 B 档，加上 PS + D-pad 调整位置和持久化（`AQ:core/vr/vr_runtime.cpp:94-102, 597-627`）。

**H6 麦克风吹气**：真实麦克风（E4 的 Android 部分）加按键替代吹气（`AQ:core/vr/openxr_host.cpp:1019-1025`，Quest 在 `references/AstroQuest/quest-host/cpp/pad_router.h:56`）。

### WP-I 诊断：卡死自动转储

- 来源：`AQ:core/guest_cpu/guest_watchdog.cpp`。20 秒没有新帧，或出现 `dump_threads` 文件时，打印每个 guest 线程当前的 HLE 调用和 guest 返回链。
- 本仓：调试器的线程信息已含 HLE 编号（`core/guest_cpu/debug/rsp_server.cpp:204, 241`），另有 `hle_sync` 和崩溃报告，但没有“无帧时自动转储”。
- 做法：在已有记录的基础上加自动转储。不移植它用实时信号采样原生栈的部分，在 Android 上会与 FEX、ART 的信号处理冲突。
- 工作量 S–M。

### WP-J 桌面 PCVR（独立项目）

原则：复用本仓 runtime、GuestReprojection 和已经跨平台的传感器层。AQ 代码中可以直接移植的只有带单测的小模块：`openxr_view.h`、`spectator_view.h`、`headset_fov_cache.h` 的身份校验、Index 控制器绑定表、Windows 音频设备名查询。

| 步骤 | 内容 | 工作量 | 依赖 |
|---|---|---|---|
| J1 构建 | Windows 加 `ENABLE_OPENXR`；Khronos OpenXR loader 按子仓规则 fork 后以 submodule 引入、静态链接；首版不含影院环境（lite engine 只在 Android 构建，`cmake/SpatialFoundation.cmake:79`） | M | — |
| J2 平台层 | 把 JNI/Activity、`XR_KHR_android_create_instance`、`__system_property_get` 等抽成适配层（`video_core/renderer_vulkan/openxr/runtime.cpp:4-9, 262-304, 567, 1113-1120`）；Windows 用 `XR_USE_PLATFORM_WIN32` 和 `XR_KHR_win32_convert_performance_counter_time` | M | J1 |
| J3 Vulkan | 去掉 `vk_platform`、`vk_instance`、`vk_presenter` 中的 Android 门控；用 runtime 指定的 GPU 覆盖 gpu_id；C6 的队列锁；XR 激活时强制 SDR（目前遇到 HDR 会抛异常，`video_core/renderer_vulkan/openxr/runtime.cpp:1295`） | S | J2 |
| **J4 桌面 PSVR 帧提交** | 13 个接口目前返回 UNSUPPORTED 或 INVALID（`core/libraries/hmd/hmd_reprojection.cpp:12-176`，已核实）。接到 `GuestReprojection` 和已跨平台的 `VideoOutDriver::SubmitVrFrame`（`core/libraries/videoout/driver.cpp:400-460`）；GPU 读完画面后再清完成标志 | L | 可与 J1–J3 并行，是关键路径 |
| J5 Presenter 与观众窗 | 帧按 XR 输出尺寸分配；桌面观众窗（双眼并排、单眼、合并，可裁剪） | M | J3 |
| J6 断连恢复 | 即 C7；实例丢失时首版退回窗口 | M | J3、J4 |
| J7 输入 | 增加 Index 与 `khr/simple` 绑定；桌面上 XR 控制器当 DS4 改走 GameController 注入 | S–M | J2 |
| J8 节拍与拷贝 | 即 C1、C2 | M | J4 |
| J9 投影模式 | 即 C5，外倾头显必须做 | M | J3 |
| J10 音频设备跟随 | cubeb 选择头显的音频设备，并在设备变化时跟随 | M | — |

约束与风险：
- enable2 要求启动时已经有头显；Virtual Desktop 的 runtime 在头显连上之前不报告设备。
- 本机显示接在 890M，独显是外接的 7600M XT，runtime 指定的 GPU 可能不是独显。
- OpenXR runtime 必须在 `Memory::Instance()` 预留地址空间之后加载（AQ 提交 `94b4e21`）；不要在启动器进程里探测 XR。
- runtime 与游戏共用图形队列，`xrEndFrame` 持锁的时长需要实测。

验证阶梯：Monado 冒烟测试 → SteamVR null driver（帧循环、观众窗）→ Virtual Desktop 加 Quest 3（晚接入、会话续期、音频设备）→ 外倾头显（投影）。游戏依次用 Tetris Effect、Beat Saber，血源用来回归 2D 影院。

---

## 4. 推荐顺序

| 阶段 | 内容 | 是否需要 Astro | 是否需要设备 |
|---|---|---|---|
| M1 立即 | WP-A；WP-F（F1–F4 与 E4 的桌面部分）；C6；D4 | 否 | A 用回放；其余桌面单测 |
| M2 测量后决定 | B1（最高优先）、B2、B4、C1、B3、B5、B6；E1、E2 | 否 | Thor / Pocket DS / Swan 同会话 A/B |
| M3 PSVR 通用 | D1、C3、C4、D3、C2、C5、WP-G、D2 A 档、WP-I | 用 Astro 验收最好，Beat Saber 等做回归 | Swan |
| M4 Astro | WP-H、D2 B 档、E3（或 NGS2 LLE）、E4 Android | **是** | Swan；PC 可选 |
| M5 PCVR | WP-J | 否（Tetris、Beat Saber） | Windows + OpenXR runtime / 头显 |

E3 与 M4 无关的部分（NGS2 LLE 评估、Tetris 走到 NGS2 调用）可以提前到 M2 末尾。

---

## 5. 移植规则

1. **许可与署名**：AQ 为 GPL-2.0-or-later，保留 SPDX 头。提交说明注明 AQ 的来源提交，有合作者的照写（如 `5d73650` 的 Clodo76、`94b4e21` 的 evertec82）。
2. **guest 内存与回调**：
   - Android 不得直接用宿主指针读写 guest 内存，必须经 `GuestAddressSpace` 的检查或 pin，pin 不跨回调、不跨等待；
   - guest 回调走 `HleScope::InvokeGuest`；
   - 所有等待都可被 Stop 取消；
   - 句柄不把宿主指针暴露给 guest。
3. **不伪造成功**：未实现的导入保持具名拒绝；不照搬 AQ 的“返回 0”和占位实现。
4. **游戏专用行为的位置**：通过 guest patch 包或每游戏设置实现，不在 renderer 或会话循环里按 title 判断（AQ 的 `known_title` 逻辑全部改为补丁包，必要时扩展补丁 SDK）。
5. **着色器缓存**：改变着色器输出时升 `ShaderBinaryVersion`。
6. **测试**：每项先移植 AQ 已有的单测（第 1 节表）；渲染改动以回放哈希不变作为回归门槛；性能改动用同会话交替 A/B，记录设备、热状态和场景。
7. **工作区**：工作区里有其他会话未提交的改动，按 hunk 只暂存本项内容。

---

## 6. 不引入的部分

| 项 | 理由 |
|---|---|
| Quest 宿主（GLES 前端 + glibc 子进程 + AHB/dma-buf + socket） | 本仓是同进程 bionic 宿主加 Vulkan mailbox，已经覆盖其功能，换架构没有收益 |
| `openxr_host.cpp` 整体 | 使用 enable v1、全局单例、Astro 专用的手柄定位；只取带测试的小模块和做法 |
| Json2 HLE（`AQ:core/libraries/json`） | Android 已按需 LLE 加载固件 `libSceJson2.sprx`，TMNT 47 个、MHW 41 个导入都已绑定；AQ 只覆盖其中 28 个和 30 个，而且把宿主指针存进 guest 可见的结构 |
| 未实现导入一律返回 0（`AQ:core/guest_cpu/hle_call_adapter.cpp:45`、`AQ:core/linker.cpp:822`） | 违反本仓“具名拒绝”的规则 |
| FEX TSO 开关（`AQ:core/fex/fex_guest_engine.cpp:263-267`） | AQ 自己注释 Astro 关掉 TSO 后半分钟内会停住；本仓保持 FEX 默认 |
| timeline semaphore 改动（`AQ:video_core/renderer_vulkan/vk_master_semaphore.cpp:70-150`） | AQ 对原因的解释与 Mesa 源码不符：Mesa `vk_sync_timeline.c` 总是按顺序完成；本仓 tick 只增不减，等待可取消，呈现前有 `WaitSubmitted` |
| cube array 层号 | 本仓 `shader_recompiler/backend/spirv/emit_spirv_image.cpp:125-128` 与 AQ 公式相同 |
| MSAA 采样数与 framebuffer 上限取交集 | 本仓按格式查询保留 8×，之前开 validation 的测试 39230 项 0 失败 |
| FXAA 代替 MSAA 解析、锐化 | 本仓默认真实 MSAA，锐化已由 FSR1/SGSR1 的 RCAS 提供；FXAA 只可作为 Force Disable MSAA 模式下的画质选项，优先级低 |
| KGSL 兼容层、实时信号采样原生栈 | 前者是 glibc 子进程在 Android 上运行所需，本仓不需要；后者与 FEX、ART 的信号处理冲突 |

---

## 7. 风险与待定问题

1. **缺少 ASTRO BOT**：M4 无法验收；AQ 的地址表只覆盖欧版 1.00/1.04，其他地区和版本没有。
2. **UBWC 改动的正确性**：格式重解释、ASTC 重编码、缩放时替换 backing 都要覆盖到；分阶段做，每一步都用回放哈希把关。
3. **Swan 的能力未核实**：是否支持 `XR_EXT_hand_tracking`、是否提供头部线速度、实际外倾角，都需先在设备上打印出来。
4. **NGS2 的语义**：AQ 的滤波器编号等是按单一游戏逆推的（`AQ:core/libraries/ngs2/ngs2_dsp.h:19-22`），没有与固件对照；回调可能重入 NGS2。
5. **Audio3d 的坐标约定**：AQ 与本仓 OpenAL 路径的 Z 轴方向相反，必须先确认。
6. **XR 节拍**：Pico 合成器是否允许不 acquire 新图像而重复提交上一张、锁相后的时延变化，都需实测。
7. **PCVR**：enable2 在各 runtime 上的支持情况、双显卡下 runtime 选中的 GPU、晚接入头显的处理。

---

## 8. 对既有对照文档的更正与补充

1. **NGS2**：本仓桌面虽是占位 HLE，但可以加载用户提供的 `libSceNgs2.sprx` 走 LLE（`core/libraries/sysmodule/sysmodule_internal.cpp:217`）；Android 没有 NGS2 桥。AQ 的 NGS2 也不完整（占位函数见 E3），并且没有宿主工作线程，渲染在 `sceNgs2SystemRender` 中同步完成。所以需要审计的是 `SystemLock` 持宿主锁返回游戏、以及效果回调在注册表锁内被调用，而不是“音频工作线程”。
2. **Audio3d**：对照文档漏了 AQ 的对象空间化；AQ 的 bed 只取前两个声道，这一点比本仓差。
3. **音频设备热插拔**：本仓 cubeb 默认设备已能失败重开（`core/libraries/audio/cubeb_audio_out.cpp:351-379, 443-458`）；只缺“指定名字的设备拔掉又插回时切回该设备”。
4. **GS**：本仓 prologue 已按输入图元设置 V0–V6，AQ 修的“第三个顶点寄存器没设置”在本仓不存在；本仓缺的是 V7、InvocationId 声明，以及特化比较中的顶点数据大小（A2、A3）。另外两项对照文档未列出：viewport 槽位压缩（A4），以及它连带的深度范围索引。
5. **UBWC**：对照文档未涉及。AQ 给图像声明格式列表，本仓没有，导致 Turnip 关闭所有颜色图像的 UBWC（B1）。
6. **XR 拷贝**：本仓 runtime 在重复帧上也做整图拷贝和 CPU 等待（C1）；对照文档“mailbox 也存在 GPU 完成等待”的说法偏轻。
7. **桌面 PSVR**：桌面版的 13 个 PSVR 帧提交接口全部返回 UNSUPPORTED 或 INVALID，这是 PCVR 的前提缺口（J4）。
