# Android 2D/XR 启动策略与 PSVR 双目复核（2026-09-29）

接续 [OpenXR 首版](openxr-20260929.md)，基于 `feature/malos/swan_performance` / `b0778a7f` 的未提交工作区。
包、native 身份及验证结果见 [manifest](evidence/openxr-mode-stereo-20260929/manifest.json)。

## 启动模式

参考本地 Azahar `XrDisplayMode.kt` / `GameAdapter.kt` 的做法，把用户偏好和本次实际模式分开，并在创建游戏画面前选 Activity。
参考仓库 commit `5b39a31eb9c81a88e89c9c5c5b2c567a87320ffc`，没有修改参考仓库。

设置页 System 新增 **Display Mode: 2D / XR**，默认 2D，支持每游戏覆盖。只在下次启动应用：

| 游戏 | 选 2D | 选 XR |
|---|---|---|
| 普通游戏 | 普通 Android 画面 | XR 单目影院，同一画面供双眼观看 |
| SFO 标记支持或必需 PSVR | XR | XR |

- PSVR 使用 `ATTRIBUTE` 的 bit 14（support）或 bit 26（require），与 `src/common/elf_info.h` 一致。不按游戏名称猜测、不重写用户设置。
- 启动前经 native 的独立 app0 mount 读取实际 `param.sfo`，支持文件夹、ZAR、链接的 ZAR 以及更新覆盖，避免仅依赖旧 UI 元数据缓存。
- `DisplayModeGate` 在组成 `SessionScreen` 之前完成解析，因此在 ViewModel 启动 service、创建 Vulkan 之前完成模式选择。Library、最近游戏、`game_id` intent 和恢复的 session 导航都经过同一入口。
- Service 再校验实际模式与已配置 Activity 是否一致。配置在交接期间变化时返回明确失败，要求重新启动，不带着错误模式创建 renderer。XR 初始化失败不自动退回 2D。
- 游戏运行时保留其既有模式。点击另一个入口会返回当前模式；旧 Activity 的迟到 pause/destroy 不得清除新 Activity 的 JNI 引用或前台状态。
- 保留一个普通 launcher；OpenXrActivity 为内部 session Activity。XR session 返回 Library 时切回 MainActivity。

## 双目结论和修正

**双目合成的像素级检查通过，但不能将其表述为完整 PSVR 已正确实现或游戏已可玩。**

1. 保持 guest `eyes[0]` / `eyes[1]` 顺序，独立 descriptor、数组切片、base/overlay sampler 与 UV；不因共享图像而合并左右眼。
2. Prepared SBS 左半图交给 LEFT，右半图交给 RIGHT；投影实验也是按左、右两 view 排列。图像无额外镜像操作。源和目标 SBS 宽度均保持偶数，避免中间 texel 跨眼。
3. 原来的显式 UV 保留游戏的正/负 Y scale。GPU 探针验证了两种符号、动态 atlas 裁剪、独立 overlay、边框采样；它证明生产 shader 按已恢复的 UV 数据工作，不证明尚未捕获的新游戏 ABI。
4. HMD 眼偏移从 runtime LOCAL 位置减去头部平移，再乘头部逆旋转。新增三个旋转轴及不对称左右眼偏移测试，避免旋转两次或把 LOCAL 位移当作 IPD。
5. **修复影院比例来自 mirror buffer 的问题。** 普通游戏按实际源图比例；PSVR 透视眼图按已公布的虚拟 FOV；`ReprojectionStart2d` 按实际 UV 裁剪比例。比例与图像一起传递，不因 Android Surface 尺寸或上一个游戏改变。
6. **修复重复 gamma 编码。** PP 已将 SDR gamma 写入 UNORM。旧 UNORM→sRGB blit 将 128 再编码到约 188。现在先在 UNORM mailbox 缩放，再 raw-copy 到兼容的 sRGB XR 图像。GPU 正反对照验证 128 保留以及旧路径确实产生 188。当前 XR 要求 RGBA/BGRA sRGB swapchain；不支持时明确报错，HDR tone mapping 尚未实现。
7. `ReprojectionStart2d` 标记为非透视，不能进入实验性 projection。实验性 projection 另要求左右眼位置及方向均有效。

OpenXR 使用 Vulkan 图像的左上角作为原点；左右 view 顺序与 eye visibility 的定义参考
[Khronos OpenXR 规范](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html)。

仍未完成：原始 PSVR 重投影记录与这张图的精确绘制姿态对应；时间/空间变换的实机端到端验收；真实游戏的双眼内容及体感确认。
默认仍是立体影院。`debug.shadps4.xr_projection_snapshot=1` 是最近 HLE 查询快照的近似实验，不是准确重投影。

## 影院距离

按用户最新决定保留 **2.5 m 距离 / 3.2 m 宽度**。16:9 内容高度 1.8 m，水平视角约 65°。
屏幕在第一次有效头部姿态处按水平朝向定位，之后固定在 LOCAL 空间；收到 LOCAL 原点改变事件后，在指定 `changeTime` 重新定位。
这样距离是从初始眼位计算，而不是从一个可能偏移的 runtime 原点计算。

4–5 m 不是所有头显通用的最佳值，虚拟屏幕距离也不是设备光学焦距。
[Microsoft 舒适性说明](https://learn.microsoft.com/en-us/windows/mixed-reality/design/comfort)区分了虚拟物体放置范围与光学焦距，并说明沉浸式设备的适宜范围会随光学焦距改变。
本轮没有改动用户已确认保留的距离/宽度，也未宣称 Swan 的特定光学参数。

## 验证和边界

| 检查 | 结果 |
|---|---|
| Kotlin runtime / data / app | 133 / 82 / 14 tests，全部通过 |
| GuestVrSensor + 几何，macOS UBSan | 64 / 0 |
| GuestVrSensor + 几何，Swan | 64 / 0，exit 0 |
| Guest reprojection ABI/提交，Swan | 91 / 0，exit 0 |
| 生产 PP shader + 数组切片/左右 UV/颜色拷贝，Swan 系统 Vulkan | 17,440 / 0，最终 exit 0 |
| Android host、APK、instrumentation Kotlin | 编译通过；APK 内 host SHA 与最终构建一致 |
| 新 APK 的 Activity 交接 / 真实 OpenXR runtime / 佩戴体验 | 已安装并打开 MainActivity；模式交接和实际 XR 仍待验收 |

GPU 探针首轮像素检查全部通过但退出 134（日志线程未显式关闭，析构时访问已销毁 mutex）；修正 probe 的日志生命周期后复测正常。
首轮失败输出保留在证据目录，不将它记为成功进程。GPU 测试不创建 OpenXR session，不覆盖运行时的 image acquire/release 或真实 XR layer 显示。
探针运行后新增了 Presenter 偶数宽度约束，已重新构建；探针直接调用 PP，不能代替 Presenter 帧队列验收。

首次 Kotlin 广泛运行还遇到两个旧测试问题：macOS `/var` 与 `/private/var` 的临时目录断言差异（改用 `-Djava.io.tmpdir=/private/tmp` 运行），
以及已不存在的 Drivers route 测试引用（更新为现有 Session route）。最终完整上述 suite 全部通过。

设备 `PB3110PGL6240001G`，boot `20c5024e-15c5-4f6a-981f-7630a933df4c`；独立 probe 运行在唯一的 `/data/local/tmp/shad-xr-mode-*` 目录。
运行日志已取回，自有目录均删除。独立探针阶段没有安装 APK、启动游戏、改存档、恢复/替换缓存，也没有修改实验属性。
此前 GPU 的 `debug.mesa.tu.debug=kgsl_preempt_rb`、暂存缓存和其他 CleanupPending 仍保留，不能把本轮 GPU 合成通过当作 CP opcode 故障修复。

候选包：`build/validation/openxr-20260929/shadps4-b0778a7f-openxr-mode-stereo.apk`，SHA `42db1f49…`。
仍打包正式 mainline 86ca 驱动（`ea4853bf…`）；没有提交、推送。

## 后续安装供用户体验

用户要求安装后，已在同一 Swan 上执行 `adb install -r`，保留应用数据。设备 base.apk SHA256 与候选包完整匹配（`42db1f49…`）。
`MainActivity` 冷启动成功（480 ms），未代替用户启动游戏或更改显示模式。普通游戏进入 Settings → System → Display Mode 选择 XR，下次启动应用；PSVR 标记游戏自动强制 XR。
安装没有修改原 GPU A/B 属性/缓存。见 [安装记录](evidence/openxr-mode-stereo-20260929/install-swan.json)。
