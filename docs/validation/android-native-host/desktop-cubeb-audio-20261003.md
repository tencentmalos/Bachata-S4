# 桌面音频：cubeb 输出后端（2026-10-03）

排查血源“主角动作和武器没有声音”时，按用户要求让 PC 与 citron 一致改用 cubeb（citron：PC 用 cubeb，Android 用 Oboe 并强制 OpenSL ES；本仓 Android 继续用 `externals/oboe`）。cubeb 不是那次丢声的原因：Android 同样会丢，根因与修复见 [血源音库重载](bloodborne-sound-bank-reload-20261004.md)。

## 子仓

- `externals/cubeb` → `https://github.com/tencentmalos/cubeb.git` 分支 `citron-submodule-42767df98ed0`，提交 `42767df98ed045e7699c28bbf0616053c55a1b4f`，即 citron 当前所用（上游 mozilla/cubeb `48689ae7` 加一条把 cubeb 自身子模块改指 tencentmalos 镜像的提交）。不改 cubeb 源码。
- 不拉 cubeb 的 googletest/sanitizers 子模块：`externals/CMakeLists.txt` 在函数作用域里关 `BUILD_TESTS`/`BUILD_TOOLS`/`USE_SANITIZERS`、开 `BUNDLE_SPEEX`，`EXCLUDE_FROM_ALL`。只在桌面（非 `BUILD_HOST_CORE`）编译和链接。

## 实现（`src/core/libraries/audio/cubeb_audio_out.cpp`）

- **控制线程**：所有 cubeb 控制调用在一个宿主线程上执行（WASAPI 要求调用线程初始化 COM；打开端口的 guest 线程栈很小）。数据回调在 cubeb 自己的线程。
- **设备节拍**：端口线程把 guest 缓冲直接转换进设备帧环，环里没有空位就等，设备时钟给端口和阻塞在 `sceAudioOutOutput` 的 guest 节拍，与 PS4 一致。`PortBackend::PacesOutput()` 为真时 `audioout.cpp` 的端口线程走 `DevicePacedOutput`，不再跑定时器；`sceAudioOutOutput(s)` 放入缓冲后通知端口线程。cubeb 回调只从环里拷贝，欠载时淡出。
- **声道**：立体声设备用与 Android Oboe 相同的 `PrepareAudioStereo` 下混；6 声道以上的设备按 7.1 输出（`PrepareAudioSurround71`，WAVE 顺序 FL FR FC LFE BL BR SL SR，guest 逻辑顺序 L R C LFE Ls Rs Le Re）。
- **缓冲**：延迟取 `max(cubeb 最小延迟, guest 块帧数)`；环容量 `bit_ceil(max(4 × (延迟 + guest 块), 8192))` 帧。
- **故障**：设备停止回调超过超时（8 个延迟周期，至少 250 ms）后，按标称速率继续给 guest 节拍并丢弃音频；流失败时最多每 2 s 重开一次。
- **设备与设置**：按现有的 SDL 设备名设置选择输出设备，找不到则用默认设备。`audio_backend` 新增 2 = cubeb，**桌面默认改为 cubeb**；cubeb 上下文创建失败时回落 SDL。Big Picture 设置的音频后端下拉增加 cubeb；奖杯音效对 cubeb 沿用 SDL 设备名分支。
- **诊断**：DebugBus `audio_out status`，每端口一行：后端、声道、延迟、队列、回调数、欠载（帧数）、丢弃块数、上次查询以来的每声道峰值。cubeb 自身日志在 `Lib_AudioOut` Debug 级。
- **构建**：`cubeb_audio_out.cpp` 经 `objbase.h` 接触 Windows SDK，`cmake/BuildSpeed.cmake` 的 Windows 头文件正则加入 `objbase`，使它不进 unity 批次。

## 验证

- Windows（WASAPI），血源，测试目录（用户存档副本、音量 20%，测完恢复 100）：连续播放约 714 s，欠载 0、丢弃 0。用户在该实例上游玩。
- 桌面构建通过，新文件无警告。

## 同期：加载阶段日志降级

同一会话里按用户要求（“太多日志影响性能”）把加载阶段逐事件的 Info 日志降为 Debug：文件打开/关闭/删除（`Kernel.Fs`）、稀疏内存驻留（`Render`）、管线与着色器编译（`Render_Vulkan`）、cubeb 自身日志；pass 打断的有界日志改为默认关闭，`gpu_memory request` 之后才输出。同一存档从启动到进世界约 53 s，日志 4635 → 1105 行。需要时用 DebugBus `log_filter`（如 `Kernel.Fs:debug`）打开。

## 未覆盖

- `tests/test_audio_transfer.cpp`（`shadps4_audio_transfer_test`，需 `ENABLE_TESTS`）已写，未编译运行。
- 设备节拍下每次约放行 2 块；Android 上成批放行曾让 FMOD 读到旧数据（[Android 回音](bloodborne-android-audio-vibration-20261004.md)），桌面 cubeb 是否有同类问题未测，桌面试听未发现异常。
- Linux（PulseAudio）、macOS（CoreAudio）未构建。
