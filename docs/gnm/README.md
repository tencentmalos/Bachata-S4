# GNM / GNMX 与 UE4 GnmRHI 参考资料（2026-10-16 整理）

用途：为 KH3（UE4.17）及其他 UE4 PS4 游戏的模拟提供参考，覆盖：
- PS4 图形接口语义；
- UE4 PS4 后端 GnmRHI 的行为；
- 它们在本仓实现中的对应和差异。

来源包括：
- 用户提供的《GNM / GNMX 资料与源码阅读指南》（`C:\Users\Admin\Downloads\GNM_GNMX_技术资料与源码阅读指南.md`）及其引用；
- 本地 OpenGNM、GPCS4、UE 4.18 公开镜像；
- KH3 eboot 与 PM4 trace；
- Tetris CUSA13427 的 Release-4.26 eboot、PM4/常量抓取及 Odin 3 同进程修复开关对照。

| 文件 | 内容 |
|---|---|
| [gnm-concepts.md](gnm-concepts.md) | GNM/GNMX 概念：DCB/CCB、CUE/LCUE、label、EOP/EOS、缓存动作、Onion/Garlic、提交；GDC 2016 MKX 与 PSSL 2013 的逐页摘要；OpenGNM / GPCS4 的实现边界 |
| [ue4-gnmrhi.md](ue4-gnmrhi.md) | UE4 PS4 后端：各版本发布说明里的 PS4 条目；4.18 公开代码中的 PS4 钩子；帧执行模型与资源生命周期约定；KH3 eboot 中能看到的 GnmRHI（cvar、默认值、函数位置） |
| [kh3-gnmrhi-emulation.md](kh3-gnmrhi-emulation.md) | KH3 实际命令流形态、与本仓 Liverpool 语义的对照、闪黑的候选原因与实验、Android 上的性能问题 |
| [kh3-gnmrhi-reconstruction.md](kh3-gnmrhi-reconstruction.md) | KH3 01.10 / UE4.17 二进制还原：continuous submit、逐 Lock 多缓冲、uniform 与临时池、EOP 延迟释放、query Begin/End；含偏移、布局和 v2 符号 |
| [kh3-gnmrhi-binding-semantics.md](kh3-gnmrhi-binding-semantics.md) | 深入语义还原：loose constants、UB packed resource map、动态 SRV 地址、SGPR / LCUE 双路径、表快照、compute 同步；含 Python 检查器、可编译 C++20 语义库及其他 UE PS4 游戏的定位方法 |
| [kh3-gnmrhi-ue-reference.md](kh3-gnmrhi-ue-reference.md) | 直接关联 UE command list / context / dynamic RHI / FRHIResource；新增 shader / UB / parameter setter 证据与 UE 风格 C++ 参考实现（KH3 4.17 语义、4.18 公共接口校对） |
| [kh3-gnmrhi-graphics.md](kh3-gnmrhi-graphics.md) | graphics/draw 接回 UE：BSS、Set/Update shader、stream/vertex fetch、资源绑定、graphics LCUE、VS→PS semantic linker；新增 UE C++ 参考、共享 C++14 算法测试与 r5 符号 |
| [kh3-gnmrhi-stages.md](kh3-gnmrhi-stages.md) | GS/ES、embedded copy VS、manager ring backing与DCB表、片上预算；LS/HS内部常量与domain绑定、阶段提交顺序；UE GS参考、tess重放和r6符号 |

Tetris 4.26 的补充见 [曝光读回与 KH3 比对](tetris-ue426-gnmrhi-readback.md)和 [graphics/draw、资源绑定与LCUE](tetris-ue426-gnmrhi-graphics.md)：前者闭合Renderer三槽历史→GNM Map/fence→View UB/tonemap，后者恢复pipeline、prepared vertex、UB/loose、普通VS/PS LCUE和direct/indirect draw，并给出与KH3逐字段对照及32³ LUT为何不属于tiny CPU staging补偿的证据。均有独立C++14参考；阶段扩展见 [GS/ES、copy、ring及LS/HS](tetris-ue426-gnmrhi-stages.md)：GS已接UE，tess为给定状态重放；UE HS/DS入口、shader loader和queue worker仍待继续。

进行中的 IDA 分析交接见 [kh3-gnmrhi-ida-20261016](../handoffs/kh3-gnmrhi-ida-20261016.md)。

## 来源（访问日期均为 2026-10-16）

**公开资料**：

| 编号 | 来源 | 说明 |
|---|---|---|
| S1 | [Pochanayon, *Capturing and Visualizing RealTime GPU Performance in Mortal Kombat X*, GDC 2016](https://media.gdcvault.com/gdc2016/Presentations/Pochanayon_Adisak_Capturing_GPU_Performance.pdf) | 66 页；本地 SHA256 `9ba8d2950b8ab524482e110084109064bbf47212733cb78a2e41a535394b7809` |
| S2 | [Stenson / Ho, *PlayStation Shader Language for PlayStation 4*, GDC Europe 2013](https://media.gdcvault.com/gdceurope2013/Presentations/825424RichardStenson.pdf) | 50 页；SHA256 `ab7684d97cdbdf976f4edf21e61d80cbe0dc29def32615177f6fbd5acaefdb3d` |
| S3 | Epic 论坛“4.x Released”帖（4.8–4.27），用 Discourse raw 导出 | 主题号见 [ue4-gnmrhi.md](ue4-gnmrhi.md)；4.15 起 PS4 说明只发给主机开发者 |
| S4 | [UE 4.26 Preview 帖](https://forums.unrealengine.com/t/unreal-engine-4-26-preview/151165?page=14) | 列出 UE-100910：GnmRHI 的 RHICopyTexture 在 PF_FloatRGB 下出错 |
| S5 | [Epic 主机开发获取说明](https://dev.epicgames.com/community/learning/knowledge-base/B9xk/getting-started-with-console-development-for-unreal-engine-and-epic-online-services) | 主机代码需平台资格与 Epic 授权；普通 GitHub 访问不含 GnmRHI |
| S6 | [`windystrife/UnrealEngine_NVIDIAGameWorks` @ `b50e6338`（4.18-GameWorks）](https://github.com/windystrife/UnrealEngine_NVIDIAGameWorks/tree/b50e6338a7c5b26374d66306ebc7807541ff815e) | UE 4.18 加 NVIDIA 改动的公开镜像；逐文件读取，含 `Stats2.h` 的 `STATGROUP_PS4RHI` |
| S7 | Epic Games Japan 2019《UE4を用いた大規模開発事例紹介 ~スクウェア・エニックス様をお招きして~》（docswell，`EpicGamesJapan` 名下 5 份） | 写明 UE4.17.2 基线；与 KH3 的对应是推断 |
| S8 | [OpenGNM](https://github.com/PS4-OpenGNM/opengnm/tree/4b295ca54c82c83acf308d1c646a2dfa9ae57350)（子仓 `references/opengnm`）与[文档](https://ps4-opengnm.github.io/opengnm/) | 只到命令构造层；Generic 后端的 submit 不做任何事 |
| S9 | [GPCS4 @ `88480de3`](https://github.com/Inori/GPCS4/tree/88480de36f983c66f7dd2c5951fead1a740d47a5) | 历史模拟器；丢弃 CCB，同步执行 |
| S10 | [shadPS4 上游](https://github.com/shadps4-emu/shadPS4)：gnmdriver.cpp / pm4_cmds.h / liverpool.cpp / vk_rasterizer.cpp | 本仓对应文件见各文档中的路径 |

**本地材料**：
- 本地 UE 5.8.3 源码 `C:\workspace\UnrealEngine`：release ZIP，commit `396c9f05`，无 git 历史、无主机平台代码。只用来确认 4.18 之后才有的接口。
- 下载原件与抽取文本在 `build/validation/kh3-gnmrhi-20261016/`：
  - `gnm-research/`：PDF、论坛 raw、`out/*.txt`；
  - `ue418/files/`：4.18 镜像中读过的源码与发布说明；
  - KH3 字符串、cvar、PM4 文本解码。
- 这些下载件不进仓库。

**没能访问的资料**：
- docs.unrealengine.com 4.27 版发布说明与 unrealengine.com 博客：403。
- dev.epicgames.com 发布说明：只返回空目录页。
- EpicGames/UnrealEngine 私有仓库：本机 gh 账号 404；按规定不用账号密码登录。
- 公开资料里找不到点名 KH3 的 CEDEC/GDC 渲染讲座。

## 使用约定

- UE 版本之间差别很大。下列内容都不能直接套到 KH3，需按版本区分：
  - 4.11 的“64 KB 命令块”；
  - 4.18 公开代码的行为；
  - 5.8 本地源码。
- KH3 的事实以 eboot 和 PM4 trace 为准，见 [kh3-gnmrhi-emulation.md](kh3-gnmrhi-emulation.md)。
- 标 **[推断]** 或 **[未核实]** 的条目在用作结论前需要第二份证据。
