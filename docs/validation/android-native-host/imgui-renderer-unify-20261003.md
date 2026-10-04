# ImGui Vulkan 渲染统一到 Foundation（2026-10-03）

接[血源 Android 瓶颈分析](bloodborne-android-bottleneck-20261002.md)第 4 节。原先仓库里有两份 ImGui Vulkan 后端：

- **shadPS4 自己的** `src/imgui/renderer/imgui_impl_vulkan.cpp`：画 2D 画面上的 HUD、通知和 HLE 对话框。
- **上游 1.92.2b** `foundation/third_party/imgui/backends/imgui_impl_vulkan.cpp`：Foundation 的 XR 状态层和错误层在用。

本轮把两者合并为 Foundation 的一份渲染器 `spatial::imgui::VulkanRenderer`（`foundation/modules/imgui_vulkan`），shadPS4 的旧后端已删除。

## 1. 原有问题

- **shadPS4 后端**：
  - 字体图集每新增一个字形，就把整张图集重新做一遍 SDF（全图两次距离变换）并整张上传；
  - 替换旧纹理前先 `DrainSubmissions` 再 `device.waitIdle()`，在 Present 线程上同步等整个 GPU；
  - `TextureManager::Submit()` 挂在每个调度器的每次 Flush 上（包括 GPU 命令线程），在其中做同步上传（`queue.submit` + `waitIdle`）或销毁（drain + `device.waitIdle()`）。
- **上游后端**：
  - 每个 ImGui context 一份后端实例，状态存在 context 里；
  - 纹理上传用自己的命令缓冲，`vkQueueSubmit` 后 `vkQueueWaitIdle`。
- **两份并存**：同一个 ImTextureID 在两边含义不同；SDF 文字只有 2D 路径有，XR 只能用位图。

## 2. 新的渲染器

**接口**（`public/spatial/imgui/VulkanRenderer.hpp`）：

- 每帧在渲染线程上依次调用 `BeginFrame(frame_serial, completed_serial)`、`UpdateTextures(cmd, draw_data)`（render pass 之外）、`RenderDrawData(cmd, draw_data)`（可多次）。
- `AddTexture`（调用方的 image view）、`CreateTexture`（RGBA8 像素，渲染器持有）、`ReleaseTexture` 可在任意线程调用。
- `SetColorFormat` 也可在任意线程调用，在下一帧开始时重建管线。
- 销毁前对画过的 context 调 `DetachImGuiTextures`，纹理回到 `WantCreate`，换 session 后由新渲染器重新上传。

**做法**：

- **不等 GPU**：
  - 纹理上传录进调用方的命令缓冲，排在使用它的绘制之前；
  - 顶点/索引写入常驻映射的 arena，所在帧完成后复用；
  - 被释放的纹理、替换掉的管线、staging buffer 都按帧序号退休，确认完成后才销毁；
  - 任意时刻释放的纹理，要等释放之后开始的第一帧完成才销毁。
- **帧序号**：
  - shadPS4 传 present 调度器 timeline 的 `CurrentTick()` 和 `KnownGpuTick()`；
  - XR 层只有一个命令缓冲在途，录制前已等上一次提交，传 `serial, serial - 1`。
- **函数表**：Vulkan 入口全部经宿主的 `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` 解析，不依赖任何 vulkan.hpp 分发配置。
- **管线**：支持 render pass 和 dynamic rendering 两种；SDF 管线按 draw 切换。
- **ImTextureID**：宿主可配置为指针或整数，`ToTextureId` / `FromTextureId` 两种都处理。shadPS4 的 `imgui_config.h` 改为 `spatial::imgui::VulkanTexture*`。
- **不属于本渲染器的纹理**：同一设备上另一个渲染器实例创建的纹理照常绘制，但更新和销毁仍交给创建者。
- **图集格式**：RGBA32 与 Alpha8 都支持；上游后端遇到 Alpha8 会断言。

**SDF 局部更新**（`SdfFontAtlas`）：

- `SdfFontAtlasDirtyRect`：给出一次改动会影响到的像素范围，即改动矩形向外扩一个饱和半径 `ceil(2·spread·max(edge, 1−edge))`。
- `WriteSdfFontAtlasRect`：在只比目标矩形宽一个饱和半径的窗口里做距离变换。凡是落在未饱和带内的路径都在窗口内，所以结果与整图转换逐字节相同。
- **用法**：新字形只转换、只上传它的脏矩形；整图转换按 512×512 分块，距离变换内存不再随图集大小增长（4096² 图集原本要 128 MiB）。
- **整图重算**：图集整体是否含字形像素发生变化时（例如空图集出现第一个字形），所有饱和像素都会变，这时转换整张图集。

**使用方**：

- **shadPS4 presenter**：dynamic rendering，SDF 开，repeat 采样器。
- **XR 层**：`XrImguiVulkanLayer` 和 `XrCompositeVulkanLayer` 用 render pass，`sdf_fonts = false`，clamp 采样器，画面与原先的上游后端一致；公开头文件未改。
- **TextureManager**：工作线程解码 PNG 后直接 `CreateTexture`，不再提交命令缓冲或等 GPU；调度器 Flush 里的 `Submit()` 已移除。
- **上游后端文件**：保留在 `third_party/imgui/backends`，供直接编译它的宿主使用（azahar 自己的界面），常驻映射改动也保留。

**其他**：

- 新增 DebugBus `screenshot overlays|game`，可不经窗口快捷键截取含 ImGui 层的画面。
- Foundation 新增共享目标 `foundation_imgui_text`（SDF 图集、文字着色器、能力协商），imgui 模块、overlay 的宿主 ImGui 模式和渲染器共用一份，避免重复符号。
- Foundation 文档 `docs/guides/imgui-runtime-font-textures.md` 已按新渲染器重写。

## 3. 验证

**SDF 逻辑**（Foundation overlay 测试，`[sdf]`）：

- 9 个用例、54,648 个断言全部通过。覆盖范围：
  - 随机增删字形，边界附近的改动；
  - spread 1 / 2.5 / 4 / 6 / 8，edge 0.4 / 0.5 / 0.65；
  - 每次改动后，局部更新的结果与整图重新转换逐字节相同；
  - 分块尺寸 1 / 7 / 16 / 64 / 99 / 256 的转换与整图转换相同；
  - Alpha8 输入，以及非法参数。
- **负对照**：饱和半径比理论下限小 1 像素时，第一次改动就失败；小 2 像素（即恰好等于下限）时通过。
- 整个 overlay 测试套件 281 个用例通过。

**GPU 像素对比**（离屏 Vulkan，开 validation layer，三帧在途）：

- 同一套 ImGui 帧分三路渲染，逐帧比较三张图是否逐字节相同：
  - 参考后端；
  - 新渲染器，dynamic rendering；
  - 第二个新渲染器实例，render pass 模式，绘制第一个实例创建的纹理。
- **两种参考**：
  - shadPS4 旧后端：SDF，repeat 采样器；
  - 上游后端：位图，clamp 采样器。
- **内容**：
  - 14–72 号字；
  - 第 3 帧起新增字形，第 5、8 帧加入中文；
  - 图集扩容；
  - 第 9 帧在前几帧仍在 GPU 上时替换图片纹理；
  - 每帧把同一份 draw data 平移后再画一次。
- **结果**：AMD Radeon 890M 与 RX 7600M XT 各跑 16 帧，另有一轮 40 帧，均为 PARITY OK，validation 0 错误 0 警告。
  - Alpha8 只比较新渲染器的两种模式，因为上游后端不支持 Alpha8。
  - 记录见[parity-runs.txt](evidence/imgui-renderer-unify-20261003/parity-runs.txt)。
- **探针来源**：[parity_probe](evidence/imgui-renderer-unify-20261003/parity_probe/)。
  - 旧后端需从 `git show 397c40551:src/imgui/renderer/imgui_impl_vulkan.{h,cpp}` 取出，放在探针目录的 `old_backend/` 下；
  - 同时要用当时的 `imgui_config.h`（ImTextureID 为 `ImGui::Texture*`），放在 `probe_config/imgui/`。
  - 实测时旧后端带有本地的常驻映射改动；该改动不影响像素。
- **仓库内探针**：`tests/imgui/overlay_vulkan_probe` 已改用新渲染器，并支持 Windows（LoadLibrary）。SDF 模式与 FPS 模式均通过（[tests.txt](evidence/imgui-renderer-unify-20261003/tests.txt)）。

**构建**：

- 桌面 clang-cl 构建通过。
- Android host `HOST_LINK_PASS`，其中 XR 目标 `shadps4_xr_imgui` 也已编译链接：host `15bb020a…`，APK `5146e7bf…`。
- 另用独立工程按 azahar 的方式完整配置 Foundation（默认 ImGui 配置：64 位整数 ImTextureID、16 位 ImDrawIdx），`foundation_imgui_text`、`foundation_imgui`、`foundation_imgui_overlay`、`foundation_imgui_vulkan_renderer`、`foundation_xr_layers` 编译通过，改动文件没有新增警告。

**桌面运行**（RX 7600M XT，测试目录 `D:\workspace\shadps4-win-test`，存档先从用户目录只读复制）：

- 血源读档进入中央亚楠。
- 用 `screenshot overlays` 截图，Summary HUD（含曲线和图标）、Detail 面板，以及截图通知中的缩略图均显示正常。缩略图走的是 TextureManager → `CreateTexture` 的新路径。

**Android 运行**（Pocket DS，Thermal Status 3，大核 1.786 GHz）：

- 安装 APK `5146e7bf…`（安装前备份了存档）。
- 读档进入中央亚楠，HUD 显示正常：FPS 26.2，CPU 70%，GPU 83%/680 MHz。
- **Present 线程**：2.31 ms/帧。修改前为 4.50 ms/帧，单独做常驻映射时为 2.30 ms/帧。
- **整体**：FPS 27.58，GPU 81%，进程 4.62 核（[线程明细](evidence/imgui-renderer-unify-20261003/android-threads-after.txt)）。

## 4. 未覆盖

- **XR**：Pocket DS 没有 XR，XR 层只做了编译，没有在头显上运行；它的绘制路径在像素对比中由 render pass + 位图模式覆盖。
- **HDR 切换**：运行中切换 HDR（`SetColorFormat`）未实测。
- **场景**：只跑了血源一个场景；其他游戏的对话框与输入法界面未逐一检查。
- **FPS**：本轮没有 FPS 层面的收益结论，Present 线程不在关键路径上。
