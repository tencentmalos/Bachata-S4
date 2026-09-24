# scrcpy 嵌入式录制 SDK 来源与首验

日期：2026-09-24。SDK 正式来源是私有
[`tencentmalos/my_mcp_tools`](https://github.com/tencentmalos/my_mcp_tools)
`master` 的
[`890ce3d`](https://github.com/tencentmalos/my_mcp_tools/commit/890ce3df2e632b45317484247d95f2449655bbec)，路径为
`dev_tools/mcp/scrcpy/capture-sdk`。Foundation `main` 的
[`3c66847`](https://github.com/tencentmalos/foundation/commit/3c668471de11653682e2c68f9ecf8bfb784ced1b) 提供可选 CMake 接入；shadPS4
仅在 Android 构建指定该源码目录时编译 Vulkan 最终渲染目标录制适配层。
`build-host-android` 同时接受私有仓根目录和 `capture-sdk/native` 目录，并将 SDK
源码摘要写入构建记录。普通构建不要求克隆私有仓。

本次在独立工作区以 NDK 29、arm64-v8a、API 33 配置并链接完整
`shadps4_host`、`host_library_smoke`、`host_dlopen_smoke`。产物
`libshadps4_host.so` 是 AArch64，动态依赖包含 `libmediandk.so`、
`libandroid.so` 与 `libc++_shared.so`；SHA-256 为
`01f3d02f7d113c89a8880059aa0b8a84a4c522b9036b1e9a76d946235e0a2c40`。
本次未安装 APK 或重跑设备录制；下述实机结果来自 2026-09-23 的首验。

此前在 AYN Thor `9c2841a4`（API 33、Adreno 740）验证了同一 SDK/适配层源码：
NDK 29 arm64 宿主链接、debug APK 组装安装通过；AllInOneSports 两次 H.264
录制分别解码出 124 和 201 帧，均为 1920×1080、0 skipped、EOS true；最后一轮
控制路径调整后另解码 47 帧。录制中游戏继续翻帧，解码画面含游戏像素而不含宿主
状态层。原始本机产物保留在 `build/validation/scrcpy-capture-sdk-20260923/`，
不随源码提交。文件模式输出 Annex-B `.h264` 和 codec PTS CSV；工具生成的 MP4
只使用名义 60 fps，PNG 是视频解码帧，不是无损渲染目标读回。

这些结果仅证明该 AYN 配置中的基本录制与控制路径，不代表精确 producer frame
关联、延迟/开销、长期背压、受保护缓冲区、Swan 双眼或其它游戏已验收。
`start_live TOKEN` 仍需其独立的消费端测试。
