# bbport（deadinside28/bloodborne_pc）参考引入与调研复核

整理日期：2026-10-12。本文复核一份外部调研摘要（结论截至 2026-10-09），并把官方仓库作为参考源码引入 `references/bloodborne_pc`。只做了源码与发布记录核对，没有运行 bbport，也没有在本仓实现任何改动。

## 引入的参考库

| 项 | 值 |
|---|---|
| 路径 | `references/bloodborne_pc`（submodule，`branch = master`） |
| 来源 | <https://github.com/deadinside28/bloodborne_pc> |
| 固定提交 | `3c57da4198ad62039f63f33bfd205b9916518acf`，即 0.51 发布（2026-10-10） |
| 许可 | GPL-2.0；`gpu/VENDOR.txt` 写明 vendored 的 shadPS4 部分为 GPL-2.0-or-later |
| 调研引用的提交 | `daa71654083b`（0.4，2026-10-07），是固定提交的祖先 |

只引入官方仓库。以下项目只记录链接，不作为 submodule：

- [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc)：bbport 的 GitHub fork，`windows` 分支，Windows 1.6（10-08，在线功能测试版）。
- [Mrsuss60/bloodborne_pc_windows_port](https://github.com/Mrsuss60/bloodborne_pc_windows_port)：bbport 的 GitHub fork，Windows 移植，10-12 已到 v0.4.1。
- [Yharnam-Hunters/Paleblood](https://github.com/Yharnam-Hunters/Paleblood)：独立的逐函数反编译项目。仓库的 `game/`、`symbols/` 是从游戏可执行文件导出和重写的代码；目前 157757 个函数只验证了 21 个，游戏跑不起来。

嵌套 submodule（`gpu/third_party/fsr-vulkan`、`gpu/third_party/imgui`、`third_party/LibAtrac9`）没有初始化，读源码不需要。仓库不含游戏文件，README 要求用户自备 CUSA03173 v1.09。README 顶部声明它与 shadPS4 项目无关，问题应发到它自己的 Discord。

## 复核结论

| 原文说法 | 结论 | 依据 |
|---|---|---|
| 主线 0.4，“今天”发布 0.5-pre1 | 当时成立，**现已过时** | 0.5-pre1 发布于 10-08 21:36 UTC（北京 10-09 早晨）；之后 pre2–pre4（10-09）、0.5 与 0.51（10-10） |
| Windows 版 Supermedo 1.6，在线测试版，玩家召唤未确认 | 成立 | 1.6 发布说明：“Summoning between players is new and not confirmed yet” |
| Mrsuss60 Windows 0.2.1 | **已过时** | 10-12 为 v0.4.1 |
| Paleblood 极早期、不能运行 | 成立 | README 进度表 |
| bbport 是血源专用运行时 + 图形翻译器，原版 eboot 离线处理后直接运行 | 成立 | `README.md` “How it differs from shadPS4”：eboot 由 `scripts/` 离线转换并链入 PS4 libc/Fios2，C 加载器映射后跳入游戏；`src/runtime_*.c` 只实现血源用到的系统函数 |
| 图形核心来自 shadPS4 `b4e7ae776…`，GPL-2.0-or-later，改动带 `bbport:` 注释 | 成立 | `gpu/VENDOR.txt`。该提交是上游 09-24 的 #5102，比本仓最后一次上游同步 `2338a06f`（09-30）早 65 个提交；VENDOR 另列了之后回移的 9 个重编译器修正 |
| “x86-64 直接执行”不能解释提速，本仓桌面版同样直接进 guest 入口 | 成立 | `src/core/linker.cpp:42` 的 `RunMainEntry` 在 x86-64 上直接 `jmp` 到入口 |
| 两阶段绘制流水线：A 解析 PM4、寄存器、选管线；B 做纹理/缓冲、渲染目标、屏障、描述符、录制；传寄存器增量与 user data 快照；明确缓存归属 | 成立 | `docs/parallel_gpu.md` “Two-stage draw pipeline”；`gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h`。包在 16 MiB 环里传递；B 有包在途时独占纹理/缓冲缓存，A 动这些状态前先排空 B |
| A/B 80.6 → 96.1 FPS，另一配置 83.9 → 110.9 FPS | 成立，需带条件 | 前者：噩梦边境站立、FSR 4、16 线程、同次运行开关；后者是第二轮、FSR 4 Ultra Performance。作者机器为 RX 7800 XT |
| 比本仓“只把 Vulkan 录制搬出去”更进一步，**接近 GCN v2 目标架构** | **部分成立** | 都把 PM4 解码与录制分开；但切分点不同。GCN v2（`docs/specs/gcn-translation-layer-v2-20261003.md` 第 2 节）把资源解析放在前端、录制线程只碰 Vulkan；bbport 的 B 线程仍拥有纹理/缓冲缓存，作者把“缓冲侧移到 A”列为下一步 |
| 运动向量：场景深度 + 相机矩阵算相机运动；保存前一帧顶点位置、匹配 draw 给物体补运动；jitter、场景缩放、原生分辨率 UI；匹配失败退回相机运动 | 成立 | `vk_camera_motion.h`（相机来自场景常量）、`vk_object_motion.h`（按 draw 的索引区间保存 clip 位置，匹配含顶点流、索引内容和基址，未匹配退回相机运动）、`docs/motion_vectors.md`（俄文，含 jitter 相位）。FSR 是时域超分，不是插帧 |
| 新内存模型：GPU 直接用共享的游戏内存，常读数据驻留 VRAM，AMD 走 dma-buf | 成立 | `gpu/shim/bbport_guest_memory.cpp`（0.4 时 270 行，dma-buf 导出/映射；0.51 时 738 行） |
| 0.5-pre1 加入 NVIDIA host-memory import、GPU 写 EOP label、真实遮挡查询；新模型默认关闭，NVIDIA 未验证 | 当时成立，**默认值已变** | 0.5-pre1 说明一致。但 0.5（`docs/CHANGES_0.5.md`）起 *Auto* 模式在 **AMD 上默认使用新模型**，NVIDIA/Intel 默认仍为旧模型；NVIDIA 可手选，部分场景比旧模型慢 |
| 只在 Linux + RX 7800 XT 充分测试，未验证通关 | 成立 | README “Status” |
| 常量环导致天空异常、ROG Ally 上 FSR 3.1 输出异常 | 成立 | issue [#68](https://github.com/deadinside28/bloodborne_pc/issues/68)、[#85](https://github.com/deadinside28/bloodborne_pc/issues/85)，10-12 仍未关闭 |
| 0.5-pre1 加入崩溃时存档损坏修复 | 成立 | 0.5-pre1 说明；issue [#64](https://github.com/deadinside28/bloodborne_pc/issues/64) 已关闭 |
| 对方也发生过“延迟 fence 写进 guest 已释放内存” | 成立 | `docs/parallel_gpu.md` “Fixed: guest heap corruption with the draw pipeline (2026-10-01)”：A 解码完所有提交就发出 GPU idle（放行 `sceGnmSubmitDone`），而录制线程上的 EOP fence 还没写，游戏释放了仍会被写 label 的对象。修复是在 GPU idle 前排空流水线并等这些延迟信号 |

调研之后（10-10 的 0.5、0.51）的新增内容，原文没有覆盖：

- 0.51 加入在线功能（独立模块 `libbbnet.so`，连 shadNet 服务器），AppImage 内置 DLSS；作者未测试在线登录与召唤。
- 0.51 修复了新内存模型下美版等区域进入世界崩溃（#119）、转动镜头时动态模糊拖影（#122），以及 4 GB 显卡上客体内存副本分配失败。

## 对本仓的参考价值

1. **两阶段流水线的线程所有权与同步边界**：`vk_draw_pipe.h`、`docs/parallel_gpu.md`。
   - 可参考的点：哪些 PM4 包必须排空 B、fence 在 B 上按流顺序执行、`WaitRegMem` 遇到已交给 B 的 fence 视为满足、`BB_PIPE_VERIFY` 定期比对 A/B 两份寄存器。
   - 它在 B 上保留缓存所有权，和 GCN v2 前端解析资源的切分不同；吸收时应以 GCN v2 的切分为准。
2. **fence 与 guest 内存回收的顺序**：bbport 的 GPU idle 事故和本仓 GCN v2 阶段 A 的 label 改写（`docs/validation/android-native-host/gcn-stage-a-fences-20261004.md`）同属一类：fence 提前放行后，游戏回收了仍会被写入的内存。本仓若引入类似拆分，GPU idle / `SubmitDone` 前也要等所有延迟写入。
3. **血源的时域超分输入**：相机与物体运动向量、jitter、场景缩放和原生分辨率 UI 是一整套。本仓目前只有 FSR1/SGSR1 这类空间超分（`host_passes/spatial_upscale.h`），引入时域超分需要先评估这套输入在 Turnip 上的开销。
4. **新内存模型**：依赖 AMD 的 dma-buf / 稀疏缓冲和 NVIDIA 的 host-memory import，是桌面 x86-64 + 大显存的路线。能否用于 Android/Turnip（KGSL、统一内存）需要单独评估，不能直接套用。

所有帧率数字都来自作者的 RX 7800 XT 与特定场景，不能外推到本仓的设备。
