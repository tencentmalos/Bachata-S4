# RenderDoc capture coordinator(Work Package A, §3.2)

2026-09-14，主仓 `codex/android-fex-round2`。实施 spec §3.2 的请求/回执捕获协调器,建在已修复的 loader(83d95fe3)之上。

## 交付

**`CaptureCoordinator`**(src/video_core/renderdoc_capture.{h,cpp}):把旧的 fire-and-forget `TriggerCapture` 换成请求/回执事务。backend 以 `IRenderDocBackend` 注入,核心状态机无需 RenderDoc 库/Vulkan 即可单测。

- 状态:Idle/Armed/Capturing/Ready/Cancelled/Failed。
- `Arm(frames, run_uuid, capture_uuid, now)`:接受返回 Armed + 新 request_id;RenderDoc 缺失立即 Failed(具名原因,**不伪造成功**);已有请求进行中返回原请求(busy,不覆盖)。
- `OnFrameBoundary(now)`:首个边界 StartFrameCapture→Capturing;满 frames 后 EndFrameCapture 并从 GetNumCaptures 增量 + GetCapture 解析回执。**仅当真有新 capture 出现且路径可取才 Ready**(不从「Start 返回」推定文件存在)。
- `Cancel`:进行中的 capture 先 End 再丢弃(不留 RenderDoc 半帧)。
- `Query(now)`:非阻塞;应用 arm 超时(Armed/Capturing 超预算→Failed),故卡死 capture 无需边界即可见。

**生产接线**:
- renderdoc.cpp:`RdocApiBackend` 适配 `rdoc_api`(GetNumCaptures/Start/End/GetCapture 两段式取路径长度);进程级 `GetCaptureCoordinator()` 单例;`NotifyPresentBoundary()`。
- vk_presenter.cpp:`Present` 成功(`presented==true`,真实 swapchain 呈现)后调 `NotifyPresentBoundary()`——真正的可证帧边界,替代旧的 GPU-drain 批次边界。
- diagnostics_commands.cpp:`renderdoc_capture [frames]`/`renderdoc_capture_status [id]`/`renderdoc_capture_cancel` 接真实协调器,回执格式化;`renderdoc_status` 的 capture_backend 改为 `coordinator`。三命令移出 not-implemented 列表。

## 验证

- 本机 `renderdoc_capture_tests`(34 checks,含 happy/multi-frame/absent/busy/no-new-capture/unretrievable-path/cancel/timeout/re-arm/unknown-id):**发现并修复真实缺陷**——初版 timeout guard 用 `armed_ns_!=0`,使 arm 于时刻 0 免于超时;改为按 state 判定。全绿。
- 全量本机诊断回归 **173/0**(tid27+hub28+reg32+**rc34**+cmd44+svc8)。
- 真实 NDK r29 编译:renderdoc.cpp/renderdoc_capture.cpp/diagnostics_commands.cpp exit 0。vk_presenter 的 ImVec2 报错为**既有 ImGui config 问题(HEAD 未改版本同样 9 处),非本改动**;真实 host build 正常。
- host DSO 重链 HOST_LINK_PASS(证明 vk_presenter/renderdoc/coordinator/commands 真实构建通过)。
- **真机(AYN Thor API33)**:两 DiagnosticsInstrumentedTest 用例 PASS。`renderdoc_capture 1` 实测回执:`request_id: 1, state: failed, requested_frames: 1, captures_before/after: 0, run_uuid: 2418a9c6...`——RenderDoc 未注入设备,诚实报 `RenderDoc API not loaded`,**不伪造 ready/文件**。JNI→registry→coordinator→回执 全链真机可用。

## 边界

- 设备未注入 RenderDoc layer,故未产出真实 RDC 文件;需匹配的 Android RenderDoc server/layer 才能到 Ready(spec §3.2 明确此为独立 server 配置)。协调器在 RenderDoc 存在时才会 StartFrameCapture/取回文件,逻辑由 34 项单测覆盖。
- 未实现:显式 device/window(`RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE`)、remote replay、Vulkan interception 生效验证、app-owned capture+helper 启动。当前用 top-level(nullptr)capture,与旧 Start/End 一致。
- 未改 FEX/Foundation/Citron。
