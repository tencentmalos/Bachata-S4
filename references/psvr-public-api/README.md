# PSVR 公开资料与 HMD ABI 核对入口

整理日期：2026-09-21。用于 Beat Saber / Tetris 的 PS4 PSVR HLE 重建。
**本目录不是 Sony SDK，也没有取得 Sony 原版 `sceHmd*` 参数帮助页。**
原始资料按来源保存，下载时间、固定提交、大小、SHA256 和失败记录见
[manifest.json](manifest.json)。HTML 是原始页面快照，未打包其外部样式和图片。

## 本地资料

Android 输入与 Vulkan 侧参考：[gyro 坐标与重力校准](android-gyro-notes.md)、[MSAA / viewport 合同](vulkan-msaa-notes.md)。这两项为宿主实现参考，不用于推断 Sony HMD 参数 ABI。

| 资料 | 本地文件 | 可用于什么 | 不能据此认定什么 |
|---|---|---|---|
| Unity 2018.4 Single Pass Stereo | [原始 HTML](snapshots/20260921/unity-2018.4-single-pass-stereo.html) | PS4 支持双宽眼图；shader 按眼选择 UV；后处理整图采样会重复双眼内容 | Sony 参数布局；当前游戏错误的唯一原因 |
| Unity XR 支持入口 | [原始 HTML](snapshots/20260921/unity-xr-support-packages.html) | 指向注册 PlayStation 开发者的 PSVR 支持入口 | 2018 年 PS4 ABI；已取得受限 SDK 文档 |
| Orbital IDA 类型表，固定提交 cc408367 | [原始 JSON](snapshots/20260921/orbital-db-types.json)、[MIT 许可](snapshots/20260921/orbital-LICENSE.txt) | `VrTraceHook` 参数名称、顺序和类型线索 | 结构体成员/偏移、flags 含义、与固件 11.00 完全一致 |
| OpenOrbis Hmd.h，固定提交 0a1aaf9d | [原始头文件](snapshots/20260921/openorbis-Hmd.h)、[许可](snapshots/20260921/openorbis-LICENSE) | 导出名称和待补条目 | `void name()` 占位声明不能证明真实函数没有参数 |
| Sony STEF2022 reprojection | [阅读笔记](sony-stef2022-notes.md) | Sony 对 PSVR / PSVR2 重投影差别的公开概述 | PS4 API 参数；不能把 PSVR2 positional reprojection 套到 PSVR |

官方原文：[Unity 双宽渲染](https://docs.unity3d.com/2018.4/Documentation/Manual/SinglePassStereoRendering.html)、
[Unity XR 支持](https://docs.unity.com/en-us/engine/6000.6/manual/xr/support/support-packages)、
[PlayStation Partners](https://partners.playstation.net/)。
另找到 Unity 官方 [Unite 2015 PSVR 演讲](https://www.youtube.com/watch?v=3RNbZpcfAhE)，
仅保存链接和元数据线索，未取得字幕/视频，不作为 ABI 证据。

## 参数标定：公开线索与独立验证

Orbital 的 `sceHmdReprojectionStartMultilayerVrTraceHook` 给出六项，
按 SysV 顺序为 `layers`、`layerCount`、`reprojectionSettings`、
`trackerState`、`flipArg`、`option`。对应的类型线索分别为
`SceHmdReprojectionLayerSettings*`、`uint32_t`、
`SceHmdReprojectionSettings*`、`SceHmdReprojectionTrackerState*`、
`int64_t`、`void*`。这里引用的是 trace-hook 类型，普通导出的合同仍需独立验证。
固定来源见 [Orbital 原始文件](https://github.com/AlexAltea/orbital/blob/cc408367da6a6ebbf5ac1ac559fefeadc73b2831/tools/ida/db_types.json#L922)。

本地固件 11.00 普通导出 `+0x17a50` 独立确认了六个寄存器的位置。
对 **单层、kind 0**，固件 `+0x1820f..+0x182a4` 转成 legacy Start：

| Multilayer layer 偏移 | Start 参数偏移 | 已确认用途 |
|---|---|---|
| `+0x00/+0x08` | `+0x00/+0x08` | 两个颜色纹理描述符地址 |
| `+0x20` | `+0x10` | sampler 描述符地址 |
| `+0x28/+0x38` | `+0x18/+0x28` | 每眼 float4，当前 HLE 的 tan-to-UV 参数 |
| `+0x48/+0x58` | 此分支不复制 | 其他路径含义尚未确认 |

这是特定固件/分支的读取证据，不是完整 SDK 结构体定义。
`0x80000005` 中高位的完整语义、其他 kind、多层和 Multilayer2 仍未标定。
不要仅凭常量值把它叫作 Y flip / viewport / stereo flag。
不要混淆 output display slot 与 input eye index；前者可以是输出缓冲轮换。

固件身份、现场完整 wire records、采样范围、反例及测试见
[HMD 对照验证报告](../../docs/validation/android-native-host/beatsaber-hmd-log-20260921.md)。
可复用采集入口见 [HMD 日志说明](../../docs/hmd-diagnostics.md)。

## 接续分析

2026-09-21 补充 [Vulkan Layer/ViewportIndex 跨辅助图元阶段笔记](vulkan-layered-primitive-notes.md)。Sports 右眼输入本来就是数组第1层；Guest RenderTargetIndex 在辅助 TCS/TES 路径丢失，修复后真机双眼均有内容。见 [实现与验证](../../docs/validation/android-native-host/sports-layered-primitive-20260921.md)。

1. 新增来源先核对来源、版本、函数后缀和结构体大小；保存原文/提交及 SHA，更新 manifest。
2. 用固定 ELF 的调用点和固件读取偏移校验原型，再用真实输入/输出确认，未知字段继续保留原字节。
3. Beat 是单层 kind 0、共享 2688×1512 颜色图；提交 UV 和 guest viewport 均分左右半区。
   后续实测已把安全提示重影定位为 STORAGE 用途造成颜色附件 MSAA 降级，修复后双眼分离。
   见 [viewport/MSAA 证据](../../docs/validation/android-native-host/beatsaber-viewport-msaa-20260921.md)
   和 [Vulkan 合同笔记](vulkan-msaa-notes.md)；Continue/菜单仍待验证。
4. Tetris 正常双目警告可作为对照；它走 WithOverlay，不能把 overlay 字段直接套到 Multilayer。

检索过函数全名、`SceHmdReprojectionLayerSettings`、`SceHmdReprojectionSettings`、
`hmd_reprojection.h` 和 SDK/documentation 组合。现有搜索命中主要是模拟器 stub、
Orbital 类型表及一般渲染说明。PS5-3.20_Libs 的 HMD 文件是跳转导出，未提供参数实现，
未纳入有效 ABI 资料。没有搜索命中不表示文档不存在。

- [Vulkan subgroup routing and 64-lane requirements](vulkan-subgroup-notes.md): MHW DS_SWIZZLE broadcast/shuffle contract and public source links (2026-09-22).
