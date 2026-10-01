# Cinema scenes — Editor review

2026-10-01 v2：按用户反馈（模型、材质都有问题）重做两套 Lite Editor 原生 World，等待用户确认后再接入 Android XR。当前没有打包进 APK。

| 方案 | 文档 | 表达 |
| --- | --- | --- |
| Quiet TV Lounge | `tv-lounge.world.json` | 橡木地板、灰泥墙、深色吸音布背墙 + 两侧胡桃木格栅；壁挂窄边框大屏、悬浮胡桃木凹槽电视柜（陶瓶/碗/书）、两只落地音箱、盆栽；顶部暖色灯槽 + 4 盏筒灯 |
| Dark Room | `dark-room.world.json` | 极简偏暗房间：炭灰墙、烟熏橡木地板、只有暗灯槽，主要靠屏幕照明 |
| Seaside Sunset | `seaside.world.json` | 极简海边：沙滩柚木平台、夕阳海面光路、两盏甲板灯 |
| Void | `void.world.json` | 极简虚无：近黑空间、深色光泽圆台+淡蓝发光边、悬浮屏幕 |
| Dusk Cinema Terrace | `dusk-terrace.world.json` | 柚木甲板、玻璃栏板 + 钢立柱 + 木扶手、两侧混凝土花槽与观赏草、暖色串灯；钢架户外屏；暮色天空穹顶 + 远景城市天际线 |

主机：PS4 用立式底座立在屏幕左侧（客厅放在加宽到 3.6 m 的电视柜左端；露台放在柚木小边柜上），蓝色灯条朝房间中央，带光晕片和蓝色点光。

共同前景：胡桃木圆角茶几（黑钢雪橇腿）、地毯、PSV 支架（底座/托沿/靠背）、原有 PSV 机模（2.5 倍）、原创 PS4（立放，平行四边形侧剖面、亮面/哑光分区、蓝色灯条、前槽光驱口与 USB）、原创 DualShock 形手柄（融合曲面机身、触摸板、摇杆、十字键、四键、肩键、前灯条）。

## v1 → v2 变化

- **材质**：v1 全部为 `KHR_materials_unlit` + 顶点色烘焙的平涂盒子，没有光照/贴图。v2 全部为 Lit PBR：底色 / ORM（AO、粗糙度、金属度）/ 法线贴图 + 切线；亮面塑料、钢、玻璃（BLEND）、自发光 LED/灯泡分开设定。贴图是 `tools/xr/cinema_textures.py` 用 numpy 程序生成的原创无缝 PNG（引擎只解码 PNG/KTX2）。
- **模型**：v1 是 83/85 个缩放单位立方体拼成。v2 在 Blender 5.2（无界面脚本）中建模：倒角 + harden normals、圆角平面轮廓、手柄用体素重网格融合后平滑减面，按真实尺寸（米）导出；每件家具/道具一个 GLB，World 只做摆放。
- **光照**：v1 只有一盏矩形光且材质不受光。v2 打开体素 GI；屏幕本身是 `screen_source=image` 的矩形面光（读取 `models/screen-preview.png` 的网格亮度，运行时可换成 live 源）；客厅另有灯槽面光、电视柜下灯带、4 盏聚光筒灯；露台有低角度暖色平行光、3 盏串灯点光、天光补光。
- **布局**：屏幕仍为 3.2 × 1.8 m、距坐姿眼位 2.5 m、中心高 1.45 m；茶几面 0.52 m；PSV 位姿与 v1 相同。

## 打开与编辑

```powershell
& '<Lite Editor>/lite_editor.exe' --asset-root '<repo>/assets/xr/cinema' '<repo>/assets/xr/cinema/tv-lounge.world.json'
```

主相机 `Viewer - seated`（Numpad 0）。截图中的斜线 X 是 Editor 的矩形光/聚光灯 gizmo，不属于场景。

**注意**：Editor 按路径缓存已加载的模型，重新生成 GLB 后需要重启 Editor（或改文件名）才能看到新几何/贴图；仅 `open` 会继续用旧模型。

## 重新生成

```powershell
python tools/xr/cinema_textures.py                                   # -> build/xr-cinema/textures
<blender 5.x>/blender.exe -b --factory-startup --python tools/xr/blender_cinema_assets.py   # -> models/*.glb
python tools/xr/build_cinema_scenes.py                               # 屏幕/PSV 占位 + 两个 World
python tools/xr/validate_cinema_layouts.py
```

`build_cinema_scenes.py` 会覆盖两份 World；在 Editor 中手改后应先另存。

## 预览边界

- 大屏与 PSV 上的文字/数值是占位图；`Game screen - preview` 在运行时隐藏并由原游戏 quad 填充同一孔径。PSV 数值为 `--`，没有伪造指标。
- 模型目录合计约 28 MB：每个 GLB 内嵌自己的 1024² PNG（同一套贴图会在多个 GLB 中重复）。上机前应改为共享贴图或 KTX2 压缩，这一步还没做。
- 顶棚只被灯槽间接照亮，坐姿视野上沿偏暗，属有意的影院氛围，可再调。
- 手柄握把仍有轻微球链起伏，近看可见；坐姿距离下不明显。

## 设计参考

尺度、初始座位与主屏布局参考 [Bigscreen Creator Club](https://github.com/BigscreenVR/bigscreen-creator-club) 的环境制作说明。所有房间、家具、PS4/手柄几何和贴图都是原创程序化生成，没有下载或复制任何第三方模型或贴图。

验证与截图见 [验证报告](../../../docs/validation/android-native-host/xr-cinema-editor-20261001.md)。

## v5 更新

- 厚版 PS4 按原版实物照片重做（尺寸、内缩分体下壳、前槽光驱/USB、38% 亮面 HDD 盖、接缝灯条、SONY/PS4 字样），立放在屏幕左侧。
- 茶几上为 Xbox（左）和 PS5 DualSense（右）两只手柄；DualShock 4 模型已移除。
- 客厅/偏暗房间：电视下方回音壁，取代两侧落地音箱（`tower-speaker.glb` 已移除）。
- 露台：去掉串灯，城市窗灯调暗调稀，去掉星星。
- 基础环境光只用 World 的天空色/地面色/强度，不再放补光面光。

## v6 更新

- 屏幕 4.40 × 2.475 m、距离 3.0 m、中心高 1.55 m（约 72° × 45°）。曲屏候选已撤回，后续按 OpenXR cylinder layer 另行设计。
- 主机为 PS4 Pro（三层层叠，`ps4-pro.glb`），立在屏幕左侧。
- 手柄为两只黑色 DualShock 4（`dualshock4.glb`）；DualSense / Xbox 已移除。
- 原创封面的 PS4 游戏盒 `disc-case-a..d.glb`。
- 房间 7.0 × 3.1 m，电视柜 0.30 m 高、5.2 m 宽，摆件放在屏幕边缘外。

## 游戏盒封面（本地）

`python tools/xr/game_covers.py CUSA03023=<血源 sce_sys> CUSA09554=<MHW sce_sys> CUSA34119=<MHR sce_sys>`，再运行 Blender 资产脚本和 `build_cinema_scenes.py`。生成的盒子在 `models/local/`（git 忽略），缺失时用原创占位封面。
