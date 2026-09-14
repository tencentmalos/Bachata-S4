# DiagnosticsHub 实机验证(Work Package A, step 10)

2026-09-14，主仓 `codex/android-fex-round2`。**首次在真机上验证** [图形调试工具整包 spec](../../specs/android-graphics-debugging-toolkit.md) §3.1 控制/状态入口端到端可用,并修复实机测试暴露的一个真实集成缺陷。

## 设备与构建

- 设备:**AYN Thor(API 33, arm64-v8a)**,adb serial `9c2841a4`。
- host DSO:`scripts/android/build-host-android --api 33`(NDK r29, RelWithDebInfo, c++_shared)→ `HOST_LINK_PASS`,含全部 diagnostics 源。
- APK:`:app:assemblePlaystoreDebug -PfexBuildDir=build/fexcore-android-api33`(正确的 API33/c++_shared FEX 预构建;`local.properties` 原指向 API35 的 `build/fexcore-android`,配置检查正确拒绝——非本改动引入,已用 `-PfexBuildDir` 指向正确目录)。APK 327MB,install Success。

## 实机测试暴露并修复的缺陷

新增 `DiagnosticsInstrumentedTest`(app androidTest)首跑 **FAILED**:session 到 RUNNING 后 `debug_status` 仍 `session: none`。

根因:`FexSessionBackend::Prepare` 的 `DiagnosticsHub::Register` 只在 `SHADPS4_TYPED_HLE_HOST` 生产分支(有 executable)里;CPU-smoke 路径(`nativeStart` 空 executable,走 decrement-loop)从不注册。**排除了「双 hub 副本」假设**:`nm -D` 确认 host DSO 导出全部 DiagnosticsHub 符号(T),JNI lib 0 个定义、DT_NEEDED `libshadps4_host.so`——两侧共用同一 hub。真实缺陷是 smoke 路径漏注册。

修复:CPU-smoke 路径返回前也 `Register(generation, getpid())` + `MarkAvailable(GpuRetire,false)` + `SetRunUuid` + `SetStage("cpu-smoke")`。图形推进信号在此保持 0(smoke 无 GuestGraphics,不 present),语义正确。

## 实机通过证据

重建 host + APK,重跑 `:app:connectedPlaystoreDebugAndroidTest` on AYN Thor:**BUILD SUCCESSFUL,1 test PASS**。设备 logcat 实测 `debug_status`(pid 20843):

```
session: active
generation: 1
pid: 20843
phase: 0
run_uuid: d53da20e258105cd807d537c0855fb03
stage: cpu-smoke
guest_flip: 0 (never)
pm4_consumed: 0 (never)
host_draw: 0 (never)
queue_submit: 0 (never)
gpu_retire: unavailable
host_present: 0 (never)
overlay_redraw: 0 (never)
generation=1 terminal=1 PASS
```

断言(全部真机通过):无 session 时 `session: none`;RUNNING 后 `session: active` + 精确 generation + 真实 32-hex run_uuid(非 unknown);**`gpu_retire: unavailable` 而非 0**(§3.1 关键区分,真机证实);七信号齐列;`renderdoc_status` 报 `renderdoc_api_loaded:` + `capture_backend: not-implemented`;pending 命令(renderdoc_capture/profiler_ring/gpu_command_trace)均 `status: not-implemented` 且不含 `ready`;help 列真实命令;Stop 后 `terminal=1`(CANCELLED)且 hub 恢复 `session: none`(Destroy 的 Revoke 生效)。

## 交付

- `session_backend_fex.cpp`:CPU-smoke 路径 hub 注册修复。
- `FexSessionService.kt`:`dump()` override 把 `adb shell dumpsys activity service .../FexSessionService [cmd]` 路由到 `nativeDebugCommand`(与 JNI 共用同一 registry)。service `exported=false` 未改(安全姿态保留);dumpsys 需 service 运行时可用。
- `DiagnosticsInstrumentedTest.kt`:上述真机断言。

## 边界

- **实机可用已证实**:hub 状态经 JNI/`nativeDebugCommand` 真机可读,producer(smoke 无图形故为 0)、run_uuid、unavailable 语义、生命周期 Register/Revoke 全部真机通过。
- `phase: 0` 未接(SessionCore 拥有 Phase;stage 字符串承载真实标签)——下一步。
- 图形 producer(guest_flip/queue_submit/host_present)需真实渲染 session 才非 0;本测试是 CPU-smoke,不是渲染验收。真机渲染 session 的信号验证需真实内容,后续。
- `dumpsys` 路径已接但需 service 处于运行态;JNI 路径无条件可用。未改 FEX/Foundation/Citron。
