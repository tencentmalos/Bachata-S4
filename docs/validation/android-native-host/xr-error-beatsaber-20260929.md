# ImGui XR 报错层与 Beat Saber 启动定位（2026-09-29）

基于 `feature/malos/swan_performance` / `b0778a7f` 的未提交工作区，接续
[2D/XR 与双目复核](openxr-mode-stereo-20260929.md)。本轮不证明 Beat Saber 已可玩。

## Azahar 对照与实现

只读参考本地 Azahar `5b39a31eb9c81a88e89c9c5c5b2c567a87320ffc` 的
`OpenXRManager.cpp`（append_imgui_layers / 射线命中 / 空间重定位）和
`docs/manual/self/plans/2026-09-28-swan-turnip-immersive.md`。

使用同一 Foundation `XrImguiVulkanLayer`：ImGui context → `XrImguiRenderedLayer`
→ 独立 Vulkan XR swapchain → `XrCompositionLayerQuad` → `xrEndFrame`。
不会把 Android Compose 窗口或 Android Surface 当成已经进入 XR 的 UI。
宿主自己绘制错误内容，没有引入完整 Foundation LayerManager/设置反射模块；
抽出 `XrImguiLayerData.h`，让 GPU renderer 仅依赖其真正需要的数据契约。

- 错误 UI 在 Guest 彻底退出后独立启动，不依赖游戏曾经提交画面。ImGui 的创建、
  绘制、字体与 GPU 资源释放均在 XR pump 线程，先销毁 renderer 再销毁 context。
- 1280×720，距初始眼位 2.5 m，宽 3.2 m，高 1.8 m；以初始头部朝向定位，
  固定在 LOCAL 空间。LOCAL 原点变更在 changeTime 生效后重新定位。
- 使用标准正高度 Quad；Swan 系统合成截图证明文字方向正确，不照搬其它路径的负高度补偿。
- 两只 OpenXR 手柄的有效 aim pose 转为同一 LOCAL 空间射线，再映射命中点到 ImGui 像素；
  trigger 点击、摇杆滚动。普通手柄 D-pad 四方向、A/Cross 确认、B/Circle 返回。
  入场/失焦后先等释放，避免进入报错界面的残留按键立即触发操作。
- 显示真实失败首行，技术详情可展开，提供 Retry / Return to library。
  重试先排空旧 UI，再启动 Guest。每个 UI owner 有 token，旧 Compose 清理不能关闭新 owner。
- 官方 ImGui Vulkan backend 会加载 WSI 函数；独立 XR owner 仍启用 Android surface
  instance extensions，但不创建 Android VkSurface。此前 Headless 初始化缺函数的反例已修复。
- `debugbus xr_error status` 显示已提交帧数与异步失败原因；提交计数本身不作为显示成功证明。

## Swan 驱动兼容边界

相同 UI / 相同设备 / 同一 Guest fault：系统 Vulkan 的 Quad 图像与系统合成截图正常，
发布版 Turnip 86ca 的 Quad 内容有条纹、残缺文字。原始图层证据见
[Turnip 源图](evidence/xr-error-20260929/turnip-86ca-source.png)，
[系统合成初版面板](evidence/xr-error-20260929/system-initial-panel.png)。

这与 Azahar 已定位的 Swan OPAQUE_FD 布局兼容问题一致：Runtime 以系统 Vulkan 分配图像，
应用驱动导入，系统 GLES 消费。86ca 不含 QCOM surface metadata 适配。
BGRA 对照虽在枚举列表中，但 Runtime 报不支持/分配尺寸不足；实验已撤回，见
[负结果](evidence/xr-error-20260929/bgra-negative.txt)。

按用户明确要求，**交付和后续迭代锚定 Turnip，不使用系统驱动兜底**。
系统只用于本轮定位时的对照，临时增加的 Swan XR 系统驱动选择和相关模式门禁已全部撤回。
原有禁止进程内切换 ICD 的约束保留。

主仓默认 mainline 改为**编译现有 Mesa 子仓源码**，采用 Azahar Swan XR 的
`1b588fceef705b65495e84df274a7dfeb09412fc` 基线，包括 QCOM OPAQUE_FD 布局导入
与单层 FDM 修复。没有引入 Azahar 预编译 ZIP。

- `references/mesa-turnip` 工作分支为 `codex/shadps4-xr-turnip`，原 8 个修改文件完整保留：
  Mapper5 编码 Surface 修复和默认关闭的 SDS 对齐 / KGSL RB、FG 抢占诊断开关。
  切换前后未提交 patch 逐字节相同；这些诊断不是已验证的 GPU hang 修复。
- `runtime/locks/turnip-bionic-mainline.json` 记录源路径、基线、NDK r29/API33。
  `shadps4_host` 的依赖目标增量编译当前 Mesa 工作区（包含未提交修改），生成驱动
  ELF、commit/patch/hash 清单和 native 编译期 SHA。之后绑定实际 host SHA。
- Gradle 从 host SDK 读取源码构建产物目录，仅打包与当前源码及 host 匹配的驱动。
  Kotlin 从包内 identity 读取 SHA；native 再核对自身编译期 SHA。源码改后忘记重编 host、
  换错 ELF 或混用 host 时打包失败。无需手动同步两处硬编码 SHA。
- 后续 GPU hang 修复直接在此子仓中迭代；构建说明见
  [Turnip 源码构建](../../guides/turnip-source-build.md)。主仓 gitlink 指向远端已有基线，
  本地补丁尚未提交；后续发布时须先提交并推送子仓修改，再更新主仓 gitlink。
- 系统 Vulkan 仍是设备 Runtime 的共享图像生产端，本轮文件 SHA 为
  `79cb05de3ebb7dece7e0545d0062ebdcbe390757a95581f157d23182c94a141a`，与 Azahar 的已验证组合一致。
- macOS 构建显式优先现代 Homebrew Bison/Flex，修正系统旧 Bison 不支持
  `-Wcounterexamples` 的构建失败。合并源码 QCOM surface 10667、FDM 6、Mapper5 1402 项检查通过。

## Beat Saber 实际失败

后续已修复此处启动故障链并进入语言选择界面，见
[GuestFault 修复与 MSAA 验收边界](beatsaber-guest-fault-20260929.md)。以下保留本轮最初故障记录。

设备有效内容为 CUSA12878 v02.04，更新覆盖基础包。更新把三个可选自动发现插件置为
0 字节（PS4NativeUserSwitchPlugin / PSNCommon / PSNCore），旧逻辑把它们当 ELF 加载而失败。
现在只在可选插件自动发现阶段跳过空文件，读取有效挂载视图，不恢复被更新覆盖的旧文件。
显式入口/DT_NEEDED/非空损坏文件仍报错。模块生命周期测试在 Swan 为 249 checks / 0 failures。

跨过该失败后，libSceFios2、libc、PS4Util 的 DT_INIT 完成，进入
Il2cppUserAssemblies.prx 初始化后触发 GuestFault，RIP `0x84070ee0`；系统/Turnip 相同。
尚未定位首个错误跳转来源，不能因 unsupported import 列表而认定某个导入就是根因。
[初始化日志摘录](evidence/xr-error-20260929/module-init-excerpt.txt)。

## 验证与清理

最终源码版 APK `8e11de1c…` / host `80b7bb6a…` / JNI `40732e1d…` / Turnip `87aeb405…`，
[完整身份与实测](evidence/xr-error-20260929/manifest.json)。host/APK 构建通过；
Kotlin runtime 133、app 7 项测试无失败。源码与产物匹配正例及三种负例（源码未重编、ELF 被修改、
host 混用）全部通过，见 [一致性检查](evidence/xr-error-20260929/source-contract-checks.json)。
无变化增量构建保持 Mesa ELF/头文件/绑定文件不动，host 仍被构建系统重新链接但字节 SHA 相同。

Swan 安装后核对设备 APK SHA。PID12099 日志明确为 `source=turnip`、
`Mesa 26.3.0-devel (git-1b588fceef)`、`sha256=87aeb405…`，进程 maps 也指向同一 SHA 目录。
[首轮实际头显截图](evidence/xr-error-20260929/turnip-source-headset.png)与
[重试后截图](evidence/xr-error-20260929/turnip-source-retry-headset.png)均清晰完整，
未见旧 86ca 的条纹与文字残缺。首轮/重试状态分别提交 708 / 1591 帧、failed=0；
按键重试启动第二轮 Guest 并正确重建 XR 面板，B 返回 MainActivity 后面板 inactive。
这验证了错误面板的显示与重建；游戏在渲染前失败，因此不构成游戏图像、FDM 或 GPU hang 回归验收。

系统截图是 Pico SCREEN_CAPTURE 的实际合成画面；普通 scrcpy 一次性截图在此场景多次取到
全黑，不能据此宣称头显黑屏。DumpLayer 工具未识别输出的单 Quad BMP，报告超时；
已按工具实际输出文件名取回 BMP 并转为 PNG 查看，没有改动图像内容。

本轮按键注入由本任务执行，不计为用户操作或实体手柄验收。射线命中代码已接入，
实物手柄点击与佩戴舒适度仍待用户验收。原 GPU A/B 的属性、缓存与 CleanupPending 保留。

本轮最终包保存在 `build/validation/openxr-20260929/beatsaber-startup/shadps4-b0778a7f-turnip-source-xr.apk`。
设备保留此包，停在游戏库。没有本任务持有的输入、debugger 或 adb forward。
此前导出的 302 个可执行文件已逐 SHA 核验到本地，再清理本任务设备导出目录；原始 GPU A/B
缓存与待采 devcd15 不属于此次清理。Turnip / Foundation / 主仓改动均未提交或推送。
