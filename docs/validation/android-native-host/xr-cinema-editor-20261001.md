# XR cinema — Lite Editor review, 2026-10-01

## 状态

按用户最新要求，先在 Lite Editor 确认效果，再导入实机。本轮交付两套原生 `.world.json` 场景及六张 Editor 实际渲染截图；**没有安装新 APK，也没有完成新场景的设备验收**。推荐电视方案作为默认，露台作为可选环境，最终选择等待用户确认。

## 方案与实际预览

| 方案 | 坐姿 | 侧视 | 桌面 |
| --- | --- | --- | --- |
| 大屏电视 / Quiet TV Lounge | [预览](evidence/xr-cinema-editor-20261001/tv-lounge-seated.png) | [支撑关系](evidence/xr-cinema-editor-20261001/tv-lounge-side.png) | [机模](evidence/xr-cinema-editor-20261001/tv-lounge-table.png) |
| 暮色露台 / Dusk Cinema Terrace | [预览](evidence/xr-cinema-editor-20261001/dusk-terrace-seated.png) | [支撑关系](evidence/xr-cinema-editor-20261001/dusk-terrace-side.png) | [机模](evidence/xr-cinema-editor-20261001/dusk-terrace-table.png) |

两套都保留 3.2 m 宽 / 2.5 m 距离，实体边框、落地支撑、0.52 m 高矮桌、PSV 支架、PS4 和手柄。PSV 原有模型复用，缩放从先前运行时的 3.8 倍候选改为本作者场景的 2.5 倍，中心在初始眼位下方 0.79 m、前方 1.05 m。地面 Y=0，作者相机 Y=1.45 m；整套场景之后需共同映射到固定 LOCAL 锚点。

设计参考与资产说明见 [场景 README](../../../assets/xr/cinema/README.md)。图中文字为明确的占位内容，Editor 的网格/轴线不属于作者资源。

## 复用与验证

- 使用本机已有 `C:/workspace/emulations/3ds/azahar/foundation/editor/build/windows-editor/Lite Editor/lite_editor.exe`。父工程中的 Foundation Editor 较旧，已有 Windows Editor 支持命令行打开、资源根目录、主相机预览、原生 DebugBus 和截图。
- 通过现有 `editor/tools/debugbus.py`，走 Editor 的 Owner/Session 路径打开 World、切换主相机、编辑临时检查视角、截图、撤销。两窗口保留在初始坐姿视角；截图后 `dirty=false`。未修改该 Editor 或其他仓库源码。
- 两套分别 83 / 85 个 World 对象，各引用 16 种模型。Editor snapshot 的 issues 为空，viewport error/unavailable 为空。
- [布局检查](evidence/xr-cinema-editor-20261001/layout-validation.json)：解析实际 GLB 顶点和节点变换、World 父子变换，再做包围盒检查。每套 42,768 次角点投影；双眼 50/64/78 mm IPD，头部左右与前后 ±0.20 m、上下 ±0.15 m，前景相对大屏下缘至少留 6.813°。支架/桌腿/手柄等接触关系检查通过。
- 迭代修正了：初版相机四元数未归一化而被 Editor 拒绝；露台柱顶越入屏幕约 5 cm；手柄离桌面约 5 mm；接触阴影与桌面共面；作者地面与 Editor 网格不一致。旧图和日志保留在 `build/validation/xr-cinema-20261001`。
- [身份清单](evidence/xr-cinema-editor-20261001/manifest.json) 包含实际 Editor EXE、资源、截图 SHA-256。
- Editor glTF 导出：[电视](evidence/xr-cinema-editor-20261001/tv-lounge-export.json)、[露台](evidence/xr-cinema-editor-20261001/dusk-terrace-export.json)。无 missing model；两张占位纹理、矩形光源未被导出。完整资源仍以 World + GLB 文件为准，未将有损导出当成可直接发布的场景包。

## v2 修订（同日，用户反馈“模型和材质都有问题”）

v1 的根因：全部材质是 `KHR_materials_unlit` + 顶点色，场景完全不受光；几何是缩放立方体/球体拼接，GI、IBL、贴图、法线都没用上。Lite Engine 实际支持 PBR 贴图（底色/ORM/法线/自发光/清漆）、切线、体素 GI、4 盏矩形面光（含屏幕图像光源）、8 盏点/聚光和平行光阴影，v2 改为使用这些能力。

| 方案 | 坐姿 | 侧视 | 桌面 | 近景 |
| --- | --- | --- | --- | --- |
| 大屏电视 | [v2](evidence/xr-cinema-editor-20261001/v2/tv-lounge-seated.png) | [v2](evidence/xr-cinema-editor-20261001/v2/tv-lounge-side.png) | [v2](evidence/xr-cinema-editor-20261001/v2/tv-lounge-table.png) | [手柄](evidence/xr-cinema-editor-20261001/v2/tv-lounge-pad.png) / [PS4](evidence/xr-cinema-editor-20261001/v2/tv-lounge-ps4.png) |
| 暮色露台 | [v2](evidence/xr-cinema-editor-20261001/v2/dusk-terrace-seated.png) | [v2](evidence/xr-cinema-editor-20261001/v2/dusk-terrace-side.png) | [v2](evidence/xr-cinema-editor-20261001/v2/dusk-terrace-table.png) | — |

- 管线：`tools/xr/cinema_textures.py`（numpy 原创无缝 PBR 贴图，10 套 + 天空 + 天际线）→ `tools/xr/blender_cinema_assets.py`（Blender 5.2.2 无界面建模导出 14 个 GLB，倒角/harden normals/体素重网格手柄）→ `tools/xr/build_cinema_scenes.py`（World、灯光、GI）。场景说明见 [README](../../../assets/xr/cinema/README.md)。
- 截图来自同一 Editor EXE（SHA 见 [v2 manifest](evidence/xr-cinema-editor-20261001/v2/manifest.json)）。中途发现 Editor 按路径缓存模型（`open` 不重新读取 GLB），曾看到旧的 unlit 地毯，重启两个 Editor 后的截图才是最终版；本轮按原命令行重启了这两个窗口（DebugBus 32135 / 32145）。
- [布局检查 v2](evidence/xr-cinema-editor-20261001/v2/layout-validation.json)：接触关系（茶几落地、PS4/手柄/支架落在 0.52 m 桌面、PSV 落在托沿上）；凡位于眼与屏之间的物体，按包围盒投影在头部 ±20/±15/±20 cm、IPD 50/64/78 mm 下都不得进入屏幕孔径（v1 只检查了桌面道具在屏幕下沿以下）。检查首次就抓到电视柜上的陶瓶挡住屏幕下沿，已降低到 0.115 m。桌面道具距屏幕下沿最小 6.8°；客厅电视柜摆件最小 0.28°（紧贴下沿但不遮挡）。
- 已知不足：模型目录 28 MB（贴图在多个 GLB 中重复内嵌，未压缩），上机前需共享贴图/KTX2；手柄握把近看有轻微起伏；客厅顶棚偏暗。仍未安装 APK、未上机，无性能结论；无 commit/push。

### v3：主机立放到屏幕旁 + 灯效（同日）

- 客厅：电视柜加宽到 3.6 m，PS4 用立式底座立在柜面左端（x=-1.72，屏幕孔径外），陶瓶/碗/书移到右端，音箱外移到 ±2.25 m。露台：屏幕左侧新增柚木小边柜（0.42 m 高），PS4 立在其上。茶几上只留 PSV 和手柄。
- 立放方向：亮面 HDD 区在上，灯条所在侧面朝向房间中央。灯效：灯条自发光加强并延伸到前沿、灯条外加 unlit 渐变透明光晕片（`ps4_glow.png`），以及挂在 PS4 下的 0.6 cd 蓝色点光（距灯条 15 cm、范围 1.4 m）照亮柜面/背墙；第一版点光离机身 4 cm 在机身上产生光斑，已外移。静态常亮蓝光，未做呼吸动画。
- [布局检查 v3](evidence/xr-cinema-editor-20261001/v3/layout-validation.json)：PS4 底面在 0.42 m 支撑面上；首次放在 x=-1.64 时头部左移情况下投影进入屏幕左缘 3 cm，已外移到 -1.72 后通过。截图：[客厅坐姿](evidence/xr-cinema-editor-20261001/v3/tv-lounge-seated.png)、[客厅主机近景](evidence/xr-cinema-editor-20261001/v3/tv-lounge-ps4v.png)、[露台坐姿](evidence/xr-cinema-editor-20261001/v3/dusk-terrace-seated.png)、[露台主机近景](evidence/xr-cinema-editor-20261001/v3/dusk-terrace-ps4t.png)，[manifest](evidence/xr-cinema-editor-20261001/v3/manifest.json)。

### v4：主体不变的三个极简环境候选（同日）

用户要求：场景主体不变，提供偏暗房间、海边、虚无空间几个极简候选。主体（3.2 m 屏幕、茶几+地毯、PSV 支架与机模、手柄、屏幕旁立放 PS4 及蓝色灯效、屏幕面光）与 v3 完全相同，只换环境：

| 候选 | World | 环境 | 截图 |
| --- | --- | --- | --- |
| 03 偏暗房间 | `dark-room.world.json` | 炭灰灰泥墙/烟熏橡木地板、无装饰；只留暗灯槽和柜下灯带，主要靠屏幕照亮；屏幕壁挂、PS4 立在电视柜左端 | [坐姿](evidence/xr-cinema-editor-20261001/v4/dark-room-seated.png) / [侧视](evidence/xr-cinema-editor-20261001/v4/dark-room-side.png) |
| 04 海边 | `seaside.world.json` | 沙滩上的柚木平台、缓坡入海、远处夕阳（太阳在屏幕后方地平线，海面有光路）、两盏甲板灯；户外钢架屏、PS4 在柚木边柜 | [坐姿](evidence/xr-cinema-editor-20261001/v4/seaside-seated.png) / [侧视](evidence/xr-cinema-editor-20261001/v4/seaside-side.png) |
| 05 虚无空间 | `void.world.json` | 近黑空间+稀疏星点、半径 3.4 m 深色光泽圆台和一圈淡蓝发光边；屏幕悬浮、PS4 在黑色方台上 | [坐姿](evidence/xr-cinema-editor-20261001/v4/void-seated.png) / [侧视](evidence/xr-cinema-editor-20261001/v4/void-side.png) |

- 新增模型 `dark-room.glb`、`seaside.glb`、`void-space.glb`、`plinth.glb`，新增贴图（深色灰泥/烟熏橡木/沙/海水/虚无地台/海边天空/虚无天空）同为程序生成原创。截图中的网格、轴线和 X 线为 Editor 叠加层，不属于场景（虚无空间截图里尤其明显）。
- 海边初版太阳灯方向反了（从观众背后照），且海水无反射几乎全黑；已改为从屏幕后方地平线射来并提高海水反照率，海面出现光路。
- [布局检查 v4](evidence/xr-cinema-editor-20261001/v4/layout-validation.json) 五个 World 全部通过；[manifest](evidence/xr-cinema-editor-20261001/v4/manifest.json)。仍未上机。

### v5：厚版 PS4 按实物重做、PS5/Xbox 手柄、回音壁、收敛露台、天光环境（同日）

- **PS4 厚版**：联网核对 [Wikimedia Commons 原版 PS4 产品照（前左/前右）](https://commons.wikimedia.org/wiki/File:Sony-PlayStation-4-PS4-Console-FL.jpg) 与 [Push Square 评测](https://www.pushsquare.com/news/2013/11/hardware_review_playstation_4_-_for_the_players)：275×53×305 mm，平行四边形侧剖面；上壳盖过内缩的下壳，中间是深槽（亮面区下方为光驱口，接缝右侧两个 USB）；从正面看左侧约 38% 为亮面 HDD 盖、右侧为磨砂纹理面，下壳也在同一线上分开；蓝色灯条在顶面接缝里；亮面前脸 SONY、磨砂前脸 PS4 字样。v4 的问题：下壳不内缩/不分体、没有字样、磨砂面和亮面一样反光、比例不对。v5 按以上重建（仍立放在屏幕左侧，亮面朝上，灯条朝房间中央）。PS 标志图形未做。
- **手柄**：去掉 DualShock 4，茶几上放两个——左 Xbox（碳黑、左摇杆偏上、圆形十字键、彩色 ABXY、白色发光 Xbox 键），右 PS5 DualSense（白色两翼承载十字键和四键、黑色中带放摇杆/PS 键、白色触摸板两侧蓝色灯条）。双色用平面切分机身得到干净分界（初版投影贴片边缘锯齿已弃用）。均为原创简化造型，非产品尺寸复刻。
- **回音壁**：客厅两侧落地音箱改为电视下方电视柜上的回音壁（0.95 m）；偏暗房间同样加回音壁。
- **露台**：按反馈“天空盒灯浮夸、不实用”，去掉串灯/灯柱/灯泡及 3 盏串灯点光；城市窗灯密度和亮度大幅降低、去掉楼顶红灯和星星。
- **环境光**：去掉露台/海边“Sky fill”和虚无空间“Top fill”补光面光，基础环境光全部由各 World 的 sky_colour / ground_colour / ambient_intensity 设定（客厅暖、露台暮蓝天+暖地、偏暗房间极低、海边天蓝+沙色、虚无近黑）。注意：本机 Lite Editor 与 shadps4 Foundation 当前的 `World::ApplyEnvironment` 把天空色和地面色取平均作为平坦环境光（GI 探针未命中时也用它），还没有按法线上下插值；颜色已按半球语义设置，引擎支持后会直接生效。
- [布局检查 v5](evidence/xr-cinema-editor-20261001/v5/layout-validation.json) 五个 World 全部通过（PS4、两只手柄、PSV 支架、回音壁落在各自支撑面上；眼屏间物体不进入屏幕孔径）。截图：[客厅坐姿](evidence/xr-cinema-editor-20261001/v5/tv-lounge-seated.png)、[PS4 前侧](evidence/xr-cinema-editor-20261001/v5/tv-lounge-ps4f.png)、[DualSense](evidence/xr-cinema-editor-20261001/v5/tv-lounge-ds.png)、[Xbox](evidence/xr-cinema-editor-20261001/v5/tv-lounge-xb.png)、[露台](evidence/xr-cinema-editor-20261001/v5/dusk-terrace-seated.png)、[偏暗房间](evidence/xr-cinema-editor-20261001/v5/dark-room-seated.png)、[海边](evidence/xr-cinema-editor-20261001/v5/seaside-seated.png)、[虚无](evidence/xr-cinema-editor-20261001/v5/void-seated.png)；[manifest](evidence/xr-cinema-editor-20261001/v5/manifest.json)。偏暗房间/海边/虚无截图拍摄于 DualSense 双色修正前（其余内容一致）。仍未上机。

### v6：大屏、曲屏候选、PS4 Pro、两只黑色 DualShock 4、游戏盒（同日）

- **屏幕加大**：平面屏 4.40 × 2.475 m（16:9，约 200 英寸），距坐姿眼位 3.0 m，中心高 1.55 m（比眼高 1.45 m 略高），坐姿视场约 72° × 45°（原 3.2 m / 2.5 m 约 65° × 40°）。房间加宽到 7.0 m、加高到 3.1 m，屏幕墙后移到 z=-3.12；电视柜降为 0.30 m 高、加宽到 5.2 m，摆件只放在屏幕边缘以外。
- **曲屏候选（已撤回）**：曲屏候选已按用户要求撤回（用户认为不对，需要先找原型并与 OpenXR cylinder layer 对接，由用户后续自行处理）；`tv-lounge-curved.world.json` 与曲屏模型、验证器的圆弧分支均已删除。原记录：曾新增 `tv-lounge-curved.world.json`，曲屏以观众为圆心，半径 3.0 m、±40°、高 2.475 m，两侧细落地支架。预览贴图按角度展开（`curved-screen-preview.glb`）。注意：运行时游戏画面目前是平面 quad，曲屏需要宿主侧把游戏图像贴到同一圆弧网格上才能实际使用。Editor 会把带图像源的矩形面光画成可见平面，曲屏的屏幕光已放到圆弧后面，否则会与圆弧穿插出现重影。
- **PS4 Pro**：按 Commons 产品照（[正面三视](https://commons.wikimedia.org/wiki/File:Sony-PlayStation4-Pro-Console-FL.jpg)、CUH-72xx 正/侧面）重建：约 295 × 55 × 327 mm，三层同尺寸磨砂层叠、两道内凹槽，前后面同向倾斜；下槽前部有光驱口、两个 USB、电源/弹出键和蓝色细灯条；顶层前脸左 SONY、右 PS4。顶面 PlayStation 标志未建模。仍立在屏幕左侧，顶面朝房间中央，灯条和蓝色点光在正面。
- **手柄**：按用户反馈恢复 DualShock 4，并对照 [PS4-Console-wDS4](https://commons.wikimedia.org/wiki/File:PS4-Console-wDS4.jpg) 补全：大触摸板、前沿灯条、分离式十字键、彩色 △○✕□ 符号、扬声器孔、SHARE/OPTIONS、PS 键；两只均为黑色，分放 PSV 两侧。DualSense / Xbox 模型已移除。
- **游戏盒**：PS4 游戏盒（135 × 14 × 171 mm，蓝色盒体、顶部蓝色 PS4 条），封面为原创虚构占位（NIGHT TIDE / IRON BLOOM / ECHO RUN / SKYFORGE），茶几左侧两盒、电视柜右端三盒。
- **验证**：布局检查改为按网格节点包围盒采样、按角度判断（同时支持平面和圆弧屏幕），撤回曲屏后五个 World 全部通过；客厅回音壁距屏幕下沿最小 0.276°（回音壁为 56 mm 高）。[检查结果](evidence/xr-cinema-editor-20261001/v6/layout-validation.json)、[manifest](evidence/xr-cinema-editor-20261001/v6/manifest.json)；截图：[客厅坐姿](evidence/xr-cinema-editor-20261001/v6/tv-lounge-seated.png)、[客厅全景](evidence/xr-cinema-editor-20261001/v6/tv-lounge-wide.png)、[PS4 Pro](evidence/xr-cinema-editor-20261001/v6/tv-lounge-pro.png)、[手柄与游戏盒](evidence/xr-cinema-editor-20261001/v6/tv-lounge-cases.png)、[露台](evidence/xr-cinema-editor-20261001/v6/dusk-terrace-seated.png)。偏暗房间/海边/虚无同步更新了大屏和道具并通过检查，本轮未单独截图。仍未上机。

### v6.1：游戏盒用用户自己游戏的封面

- 新增 `tools/xr/game_covers.py`：从用户本机游戏的 `sce_sys/icon0.png`（带标题字样的主视觉）合成 PS4 盒封面（顶部蓝色 PS4 条）。Blender 为每个 `cover_CUSA*.png` 生成 `assets/xr/cinema/models/local/disc-case-<ID>.glb`；该目录 `.gitignore` 全部忽略：发行商美术不提交、不从网络下载。缺少某款时自动使用原创占位封面。
- 已完成：血源（CUSA03023，来源 `D:/game/ps4/installed_roms/CUSA03023/sce_sys`），放在茶几和电视柜。MHW（CUSA09554，冰原）与 MHR（CUSA34119，曙光）的游戏文件不在本机和 Swan 上（之前在 AYN，当前未连接），暂用占位；拿到 sce_sys 后运行同一脚本即可替换。
- 五个 World 检查仍通过。

### v7：实机验证（Swan，2026-10-01）

**接入方式（本地未提交）**
- 新增 `src/video_core/renderer_vulkan/openxr/cinema_environment.{h,cpp}`：用 Foundation `World` 加载打包进 APK 的 `xr/cinema/*.world.json`（首次按安装戳解压到缓存目录，作为 Lite Engine 资源根），作为最底层不透明投影层绘制；游戏画面仍是原 OpenXR quad，PSV 状态层在最上。World 中的 `Game screen - preview`、静态 PSV、相机由运行时替换/删除；PSV 状态层改放到 World 的底座（`StatusScene::Place`）。锚点由影院 quad 位姿反推（作者屏幕中心 (0,1.55,-3.0)），recenter 时一起移动，不重建 GI。
- 影院几何改为 4.4 m 宽、3.0 m 远、中心比眼高 0.10 m（`vr_geometry.h`，对应单测已更新），与 World 一致。
- 环境层默认每眼分辨率 ×0.5（`debug.shadps4.xr_cinema_scale`），World 选择 `debug.shadps4.xr_cinema_world`（默认 tv-lounge，`off` 关闭），DebugBus `xr_cinema status | world <key|off>` 运行时切换；创建失败只记日志，不中断 XR。
- Foundation（子仓，未提交）：`XrSceneLighting` 增加可选 RGB 环境光（World 天空/地面色均值 × 强度；原层每帧覆盖成灰色标量）；`engine/world` 三处 nlohmann 引用前后 `push/pop_macro(assert_invariant)`（`utils/debug.h` 的同名宏在 Android Release 下导致 World.cpp 编译失败）。
- 户外三个 World 关闭体素 GI：Swan 实测天空穹顶把 GI 体积撑到约 29 万体素/25 万探针（室内约 2 万/3344）。
- Windows 主机构建：`scripts/android/build-turnip-source` 新增 `SHADPS4_TURNIP_PREBUILT`，复用已在 Linux/macOS 编好的同源 Turnip ELF（要求当前 Mesa 树干净且 `git diff <prebuilt源提交> HEAD` 与其记录的源码差异哈希一致；本轮核对设备上 a95b15df 的源码即本地 Mesa `351a4847`）。CMake：scrcpy capture SDK 缺 `snapshot_session.h` 时不编译内嵌截图（本机 my_mcp_tools 的 SDK 没有该扩展），**因此本轮 APK 没有内嵌截图功能**。

**Swan 结果**（设备 PB3110PGL6240001G，血源 CUSA03023 影院模式，头显未佩戴，姿态固定）
- 最终安装 APK `376d326d…`，驱动仍为 `a95b15df`（宿主已重新绑定）；五个 World 均能加载并渲染（加载 0.5–1.2 s），系统合成截图里游戏画面落在电视孔径内、上方灯槽/墙面、户外天空正常。
- 内存：第一版（1024² 贴图、每个 GLB 各自内嵌）环境层 GL mtrack +367 MB、PSS +531 MB，系统可用内存 1.41→0.79 GB；在血源读档加载时出现一次内存告急崩溃（lowmemorykiller critical pressure 后 `StagingBufferPool::RequestLarge` 分配断言）。改为 512² 后 GL mtrack 增量约 0–40 MB、PSS 增量约 50–300 MB（采样噪声大），资源 47→18 MB，加载 0.53 s。
- 2 分钟持续运行：游戏 30 FPS（上限），环境层约 22 Hz，期间 0 次 LMK 杀进程，可用内存约 1 GB。环境开/关交替测得 XR 循环 19–26 Hz、游戏均 30 FPS，差异在噪声内；XR 循环偏低在开环境前即存在（血源使 GPU 99% 忙）。
- 未验证：佩戴后的视觉/舒适度、桌面道具与 PSV 在底座上的对齐（头显未佩戴只能看到固定朝向）、其它游戏、长时间稳定性、动态屏幕光（屏幕光仍用静态占位图）。会话保留运行（tv-lounge），属性已恢复为空（默认）。

## v8：DS4 重做 / 光盘平铺 / ASTC 压缩纹理 / 云端 Turnip / 去畸变截图（2026-10-01）

用户反馈 DS4 仍完全不对、游戏盒平铺在桌上、用 bug_reports 的 Ubuntu 云机构建 Turnip、截图带畸变看不清、改用压缩纹理。

- **DS4**：照 Commons `Sony-PlayStation-4-PS4-DualShock-4.png` 正视照重画布局（162×98 mm）：方向键与面键下方的灰色圆形底盘、四枚五边形箭头键、前中大触摸板及其两角 SHARE/OPTIONS、触摸板下扬声器孔、摇杆靠内偏下且中间 PS 键、短而粗的握把、前缘 L1/R1 与 L2/R2、前脸灯条。机身改为俯视轮廓挤出 + 握把球链体素融合，旧版握把过长、整体成管状的问题已改；握把起点压低，不再顶出面板埋住下方向键。底盘/触摸板改为扇形环带贴合曲面（旧网格法边缘锯齿）。证据 `evidence/.../v8/tv-lounge-dstop.jpg`（俯视）、`tv-lounge-dsf.jpg`（玩家视角）。`blender_cinema_assets.py -- --only dualshock4` 可单独重建（约 2 s）。
- **游戏盒**：桌面三盒平铺一排（MHW / MHR / 血源，左手柄后方，各自小角度），控制台上的叠放保留。本机与 Swan 只有血源的 `sce_sys`，MHW/MHR 仍用原创占位封面；拿到 `sce_sys/icon0.png` 后执行 `tools/xr/game_covers.py CUSA09554=<dir> CUSA34119=<dir>` 再重建模型即可（封面仅进 git 忽略的 `models/local/`）。截图含发行商封面，不入库。五个 World 布局校验仍全部通过（`v8/layout-validation.json`）。
- **压缩纹理**：新 `tools/xr/compress_cinema_textures.py` 在打包时把每个 GLB 内嵌 PNG 换成 KTX2（ASTC 4×4 + 完整 mip，基色/自发光 sRGB、其余线性、法线 mip 重新归一化，按源字节缓存），Gradle `compressXrCinemaTextures` → `prepareXrCinemaAssets`。作者 GLB 仍为 PNG（Editor 在桌面 GPU 上无 ASTC）。78 张贴图按引擎 KTX2 规则逐级校验 0 错误。磁盘 11.1→37.4 MiB（ASTC 在 zip 中压不动），GPU 端每纹素 4→1 B 且免设备端生成 mip。
- **云端 Turnip**：新 `scripts/android/build-turnip-remote` 经 `ssh workbench-hub` 在 `/data00/bug_reports_work/shadps4-turnip` 内用 docker（Ubuntu 24.04 + bison/flex/glslang/Meson 1.4.1 + NDK r29）构建锁定的 Mesa `351a4847`，下载并核对 identity；Windows 主机构建用 `SHADPS4_TURNIP_PREBUILT=build/turnip-remote`。产物 `d767cd73…`（与设备旧 `a95b15df` 同源不同构建环境，二进制不同）。首轮缺 glslangValidator 失败，补包后通过。
- **截图**：`adb screencap`/scrcpy 镜像的是面板（双眼、旋转、镜头预畸变后）。scrcpy SDK 的一次性截图接口 `SnapshotSession` 在本机/远端均未发布，宿主内嵌截图仍编译关闭；其 app 内捕获只有游戏 2D 画布、无 XR 图层。改用 Pico OS 系统截图（`com.picoxr.systemui` `SCREEN_CAPTURE`）：合成所有 OpenXR 图层、无畸变、2560×1440。新增 `tools/xr/swan_xr_screenshot.py`（唤醒→触发→拉取→只删自己的文件）与 skill `.claude/skills/xr-headset-screenshot`。旧包会话时实测得到影院+血源画面的无畸变图；头显休眠时请求被忽略（`powerState=SLEEP`），未佩戴时会话 PAUSED/追踪丢失，只能得到黑帧+加载环或“前摄像头被遮挡”提示——须佩戴或对准后再截。
- **Swan**：安装 APK `d4a4be77…`、host `7849514e…`、Turnip `d767cd73…`；血源影院 tv-lounge 加载 413 ms（重载 163 ms）、ASTC 资源 44.1 MiB、GI 21189 体素/3344 探针，持续出帧。环境开/关同会话：GL mtrack +37 MiB、EGL mtrack +18 MiB（含交换链）。头显未佩戴、追踪丢失，**本包没有拿到可视截图，DS4/平铺光盘的实机外观待佩戴确认**；会话保留运行。无 commit/push。

## v9：1024 贴图 / 进关卡 / scrcpy SDK 截图录像修复与单双目（2026-10-01）

- **贴图**：用户认为 512 太糊，作者端恢复 1024（`cinema_textures.py SHIP=1024`）。ASTC 后每纹素 1 B，APK 内模型资源约 110 MiB；Swan 血源同会话环境开/关：GL mtrack +37 MiB、EGL +18 MiB，可用内存约 0.94 GB，无 lowmemorykiller。早先 1024 PNG 为 +367 MB。
- **进游戏**：上一轮只停在标题。DebugBus 手柄 notice/title/offline/continue 四次圈键后进入猎人梦境，SDK 截图确认（关卡内 30 FPS）。
- **scrcpy SDK**：所有分支都没有 `SnapshotSession`（宿主 `capture_recorder.cpp` 依赖的接口从未发布），故此前 APK 内嵌截图被 CMake 关闭。my_mcp_tools master `08a0356` 补齐：快照状态机、`SelectEye/LayoutName`；同时把 scrcpy_pico `092e1a3b` / spatial_mcp_publish `5686785` 中晚于 09-24 单仓导入的 Windows 校验与会话恢复移入 `d4284d0`。
- **单双目**：宿主 `capture_screenshot request TOKEN [left|right|both]` 与 `capture_video start [EYE]`/`start_live TOKEN [EYE]`；PSVR 并排画布报 `layout: stereo_sbs` 并按眼裁剪/编码，普通/影院画布报 `layout: mono` 忽略 eye。录像中画布布局变化时编码器重建。
- **MCP**：capture_app_screenshot / record_app_video（SDK，eye）、capture_headset_view / record_headset_view（Pico systemui SCREEN_CAPTURE type 0/1，单目合成、无畸变，黑帧给 paused 警告，只删自己的设备文件）；25 项单测，本机安装 local.20261001.undistort1。
- **Swan 实测**（APK `e8159f50`）：Beat Saber 安全页 PNG 5184×2400 / 左 2592×2400 / 右 2592×2400，录像左眼 2592×2400 119 帧、全宽 5184×2400 97 帧；血源 PNG 2592×1458（eye=left 报 mono）、录像 65 帧；头显截图/3 s 录像成功（未佩戴为黑帧，正确告警）。本机无 ffmpeg，`--mp4` 需另行转换。
- **仍未完成**：佩戴后的视觉/对齐验收；MHW/MHR 真封面（需 sce_sys）；GPU hang 未修。

## 早期设备基线与清理

在用户增加 Editor 确认门槛之前，已对既有 Swan 安装启动 Bloodborne 影院并读取状态，通过 `my_mcp_tools` 中的 scrcpy MCP 实现建立持久会话、抓取现状。它证明的是旧包基线，不是这些新场景上机。

- 设备 `PB3110PGL6240001G`，旧会话 PID 7491 / generation 1，原驱动 SHA `a95b15df…`；未安装任何候选 APK。
- scrcpy session `73a14b67156a4bfa80f8f401c8f096e3` 已正常停止，`gracefulStop=true`、native exit 0。初次 256×256 黑色就绪图是无效基线，保留负结果；后续持久会话截图为有效左眼图。
- 原生 app SDK 只捕获游戏画布，不能代表最终 XR 合成；所以基线采用 scrcpy 系统镜像。未把游戏画布截图当成完整影院。
- 已通过正常 Stop UI 结束自有基线会话：`session=none`、`stage=Stopped`、`stop_reason=user_stop`、`guest return=0`。Swan 的普通 tap 坐标不能直接命中 XR 中的 Compose 对话框；最终使用 Tab 明确聚焦 Stop 再 Enter。
- 自有 JPEG 与两个临时 UI XML 拉回核 SHA 后删除，检查设备路径已不存在。未动其他会话、缓存或历史 GPU 诊断现场。

## 待用户确认后的工作

把确认后的同一套 World/模型资源接入现有 Lite Engine XR 层；支持电视/露台选择，替换预览屏与 PSV 占位纹理，共用影院锚点及重定位。之后完成 Android 编译、包/宿主/源码驱动 SHA 核对、安装、scrcpy 双眼与原图检查，再做佩戴验收。

最初的手写环境 renderer 与 Windows 构建尝试已经从工作区源码撤下，仅在 `build/validation/xr-cinema-20261001/deferred-native-draft` 保留 patch/源文件。该候选早于 Editor 方案，不能直接作为两套场景实现。Windows Turnip 构建仍需处理 win_flex 默认生成 `<io.h>` 的交叉编译问题；先前文件锁、编码、路径修正也存于该草稿中。没有宣称 Android 完整构建通过、性能收益或 GPU hang 修复。无 commit/push。
