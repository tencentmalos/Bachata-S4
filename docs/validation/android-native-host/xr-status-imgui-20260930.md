# PSVR 独立 ImGui 状态层与 Launcher 布局（2026-09-30）

本轮按用户要求：PSVR 不显示或创建 PSV 机模，状态改为独立 `ImGui::Layer`；普通影院保留机模。Launcher 提供横向平铺的 **Horizontal / Vertical / None**，默认 Horizontal。PSVR 两种布局都锚定视野左上角。源码仍在 `feature/malos/swan_performance` 工作区，未提交/推送。

## 实现

`openxr/status_panel.cpp` 的 `StatusPanel` 继承 `ImGui::Layer`，独立 context、字体 atlas 和 Foundation `XrImguiVulkanLayer`。它使用实际 ImGui draw list，由 Vulkan 后端绘制，不是 CPU 文字位图。复用 PerfHud 快照中的游戏 FPS、CPU、GPU、内存、电池和每眼输出尺寸；无样本显示 `--`，超过 2 s 的快照显示等待。

| Launcher 选项 | PSVR 状态图 | 空间尺寸 | 普通影院 |
|---|---|---|---|
| Horizontal | 1152×192，3 列×2 行 | 1.2×0.2 m | 保留 PSV |
| Vertical | 384×576，1 列×6 行 | 0.4×0.6 m | 保留 PSV |
| None | 不创建/不提交状态图 | — | 隐藏 PSV |

两个 ImGui 布局具有相同像素密度、32 px 字体和 VIEW 空间左上角 `(-1.7, 1.1, -2.5)` m。头部运动时状态保持在视野左上角，双眼共享同一有深度的 quad。普通影院 PSV 仍固定在 LOCAL 空间，不随头移动。PSVR 依据实际 SFO 元数据提前判定，因此开场 2D 电影也不创建机模和 GI。

每游戏配置 `gpu.xr_status_layer` 为字符串枚举；旧布尔值 `true` → Horizontal，`false` → None。Launcher 草稿只在 Launch 时保存，取消不改配置；2D 模式不显示这个 XR 选项。`xr_status visible on/off` 仅改变当前会话，错误 ImGui 面板不受 None 影响。

## 调度与同步边界

- PresentThread 在常规镜像 ImGui 帧之间切到状态 context，最多每 250 ms 更新一次，退出恢复原 context。状态层不接管鼠标、键盘或手柄。
- XR 帧线程只把最后一次已 release 的状态 swapchain 图像附加到 `xrEndFrame`；没有新状态图时保留旧图。
- 生产者对状态层 mutex 使用 `try_lock`；与 `xrEndFrame` 的 release/合成互斥遇忙直接跳过本次状态更新。
- Foundation Render 增加可选非阻塞模式：先轮询之前的 GPU fence，再处理纹理；GPU 未就绪立即复用旧图。`xrWaitSwapchainImage(timeout=0)` 超时后保留已 acquire 的 image，下一次继续 wait，不能对未 wait 成功的 image 提前 release。尚无已 release 图像时不提交该层。
- 预烘焙 HUD 使用的 ASCII 字符和标点，避免游戏中首次出现新数字触发同步字体上传。
- 旧镜像 Vulkan 后端改为显式 host backend 指针，避免纹理工作线程通过全局 GImGui 误取状态层后端；TextureManager 停止标志改为原子，并在队列 mutex 下唤醒/退出，销毁后端前 join。

这消除了状态图刷新时主动等待其上一帧 GPU 完成的路径，也避免每个 PSVR XR 帧串行绘制 PSV/GI。但 **并非零开销或完全独立 GPU 队列**：ImGui CPU 绘制/录制仍在 PresentThread，提交使用现有 graphics queue 与队列锁；GPU 工作和 XR 合成本身有成本，首次字体/管线初始化和销毁仍可同步。本轮没有重写原有 guest mailbox 的 Publish/Consume fence 等待；已加 `XR.Mailbox.*SubmitWait` scope 以便后续单独分析。

## 同会话 LiteP A/B

Swan `PB3110PGL6240001G`，Beat Saber `CUSA12878` 的健康警告场景，无自动推进游戏输入。PID 4983 / generation 1，run UUID `6b5f713e60af7d375f390e124de8bd93`。同包、同进程按 On → Off → On → Off → On 采样。该测量包仍为中下位置的横向 HUD（APK `52b88eb1…`、host `aba3e6eb…`）；最终包只另改左上角位置、布局枚举与 RAM 字段，不能把旧测量当作最终纵向布局的单独 A/B。

| 轮次 | 采样 | 游戏 FPS | 平均帧间隔 ms | P95 ms | 状态更新数 | 更新平均 / 最大 ms | 状态 PreviousFence wait scope |
|---|---:|---:|---:|---:|---:|---:|---:|
| On 1 | 8 s | 30.73 | 32.54 | 58.43 | 23 | 0.372 / 0.824 | 0 |
| Off 1 | 8 s | 36.70 | 27.25 | 49.91 | 0 | — | 0 |
| On 2 | 8 s | 36.25 | 27.59 | 50.57 | 26 | 0.239 / 0.804 | 0 |
| Off 2 | 10 s | 35.64 | 28.06 | 49.84 | 0 | — | 0 |
| On 3 | 10 s | 36.17 | 27.65 | 50.28 | 30 | 0.229 / 0.456 | 0 |

首轮 On 的 30.73 FPS 明显较低，原因没有单独证明，不能省略或直接归为预热。后两轮开关差别处在本次波动内；只支持“这些短采样里状态更新没有 fence 等待”的结论，不能据此宣称零性能损失或所有场景收益。更新 scope 是包括抢占/提交的 elapsed 时间，不是精确 on-CPU，也未测得独立 GPU 耗时。原有 mailbox Publish 平均仍约 16–20 ms、Consume 约 7–11 ms。

PROF 由当前 vendored SDK 生成。LiteP 服务 reader 因 wire provenance 不明确拒绝打开，未强行按任意 PROF v3 解释；使用同源码树 `foundation/third_party/profiler_sdk/skills/spatial-trace-analyzer/scripts/decoder.py` 解码，源码来源见 SDK UPSTREAM 文档（导出合并 `0b467a861569345a64fd88cb2e5daa8ff542c14f`）。所有 chunk 可读、跳过 chunk 为 0；采样边界存在未闭合/未配对 scope，decoder 标记 truncated，统计仅使用线程内完整配对。原始 PROF 保存在 `build/validation/xr-status-imgui-20260930`；SHA、统计、所有解码诊断及可复算脚本保存在 [evidence](evidence/xr-status-imgui-20260930)。

## 修复过程中的负结果

第一候选在 ImFont::GetFontBaked 因显式 `io.FontDefault` 为空发生 SIGSEGV（PID 32051，证据 `first-font-init-crash.txt`）。虽然 AddFontDefault 已添加字体，显式预烘焙仍需保存其返回值；已修正，后续运行/正常停止均越过该路径。

初版使用了不存在的 `perf.memory` key，HUD 内存显示 `--`；改为 PerfHud 的 `perf.ram`，影院画布一起修正。用户随后否决居中位置和 checkbox UI，最终改为本页所述左上角与三项布局。`launcher-checkbox.*`、`psvr-imgui-on/off.*` 仅为中间版本证据，不代表最终界面。

## 构建与设备验证

Host 与 APK 构建通过；core/runtime 140 项、feature/library 11 项，0 失败。覆盖 enum 与旧 bool 兼容、全局/游戏覆盖、2D 禁用、Launcher 草稿取消与单游戏持久化、不覆盖其它设置。`git diff --check`（主仓与 Foundation）通过。

最终包 SHA：

- APK `58ab9c70bfafc29f6ef77c0566da32fc2b8a46f1e986d80ce40df6a17f230b79`
- host `fa85cb212df7e3b8f3fe235f09467de62f5017670faad18a57fd9690cc881478`
- Turnip `a95b15df82ed9e49e7d2166a5e33f959b06a6655ec626f735cd6a2c98159dec3`，未改驱动
- 可安装包 `build/apk-release/shadps4-xr-status-58ab9c70-playstoreDebug.apk`

前一内存修正版 `8125431b…` 已实测 Bloodborne 普通影院，`cinema-status.txt` 显示 `mode=cinema`、2592×2400、samples=1、LOCAL-fixed、GI=true，系统合成截图 `cinema-psv-retained.png` 实际可见机模；该截图头部已偏转，不用于验证正前方构图。正常 Stop 为 guest return=0。此轮未注入游戏确认输入或修改存档文件。

### 最终布局实机验收

- APK/host 安装后 SHA 与构建产物一致，见 `installed-layout-sha.txt`；APK 内 `assets/native-turnip-mainline/vulkan.ad07xx.so` 与运行日志 SHA 一致。
- None：旧 `false` 配置在新包 PID 10135 / generation 1 实际解析为 `visible=false`，没有创建 ImGui 状态图或机模。返回 Launcher 后 UI 中 None 为 checked。
- Vertical：从真实 Launcher 选择并 Launch，保存字符串 `vertical`；PID 15088 / generation 1，状态 `layout=vertical anchor=top-left canvas=384x576`，系统合成截图可见完整六项信息、RAM 13.4 GB、电池 100%。随后正常 Stop，guest return=0。
- Horizontal：从真实 Launcher 选择并 Launch，保存字符串 `horizontal`；PID 16057 / generation 1，状态 `layout=horizontal anchor=top-left canvas=1152x192`，系统合成截图确认左上角。最终保持这个 Running 会话供用户体验。
- `launcher-layout-{none,vertical,horizontal}.xml` 分别确认三种选项的实际 checked 状态；`launcher-layout-options.png` 是 Android 启动面板截图，XR 截图来自 Pico 系统合成路径，不是游戏镜像。

[横向头显截图](evidence/xr-status-imgui-20260930/layout-horizontal.png)、[纵向头显截图](evidence/xr-status-imgui-20260930/layout-vertical.png)。Pico 返回的文件扩展名为 png，实际编码是原始 JPEG，未进行旋转、裁切或重采样。`after-none-stop.png` 拍摄于停止之后，不能作为游戏 None 状态的视觉证据。

本轮所有 6 份自有 PROF 和 7 份系统截图在本地保存后，经 SHA 匹配删除设备副本；临时 uiautomator XML 已删除，详细匹配见 `cleanup.json`。没有活动 LiteP streaming capture，没有新 debugger、adb forward 或 scrcpy 会话。游戏配置与本轮前备份相比，只有用户要求的每游戏 status layout 新增为 horizontal；不覆盖 FSR1、输出档位或左右手设置。旧 GPU 属性、缓存 CleanupPending 均未动。

旧 GPU hang、歌曲游玩与佩戴舒适度不属于这轮已验证结果。
