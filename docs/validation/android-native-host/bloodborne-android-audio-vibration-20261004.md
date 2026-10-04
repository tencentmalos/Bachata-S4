# 血源 Android：声音回音/降速、手柄振动与声音修复包发布（2026-10-04，本地未提交）

设备：AYN Thor（`9c2841a4`，Android 13，Snapdragon 8 Gen 2：小核 0–2、中核 3–6、大核 7），血源 CUSA03023 1.00，中央亚楠。

## 1. 声音修复包发布到 Android

- 桌面已验证的音库重载修复（`guest/games/CUSA03023/01.00/sound_reload.cpp`）单独打成 sdk_version 1 包 `bloodborne_sound_fix_v1`（`sound_reload.recipe.json`，包 SHA `eb39b46c…`）。
- `guest_runtime.cpp`：Android 在 `debug.shadps4.guest_patch` 为空时加载 `<UserDir>/guest_patches/<TITLE_ID>.json`；该包的 title 或模块 SHA 与当前游戏不符时记一条错误、游戏照常不打补丁运行（显式属性仍按原规则失败）。**同日已由每游戏多选取代**（`<UserDir>/guest_patches/<TITLE_ID>/<名字>.json` + 选择列表），见 [patch 多选](guest-patch-selection-20261004.md)。
- Thor 上包放在 `files/host/guest_patches/CUSA03023.json`，`guest_patch status` 显示 3 个 hook 与 2 个绑定已安装。该次运行中音库释放先于新资源加载，hold 计数为 0（未发生竞态）。

## 2. 手柄振动

- **现象**：Thor 内置手柄 “Odin Controller”（USB HID，vendor 0x2020/product 0x0111，Android 标为外接）的 InputDevice 没有振子，注册为 `rumble=false`，宿主拒绝每一次 `scePadSetVibration`。旧会话 33795 次调用中 197 次为非零（受击），全部 “host rejected”。
- **游戏侧正常**：振动强度设置（选项结构 `+0x1`）为 10；镜头震动/振动效果管理器（全局 `0x553d6b8`）每个参数都带振动曲线，曲线值非零（如 id 0：大马达 9 帧内 255→149）。
- **修复**：与 citron 默认一致——Player 1 的手柄没有自带振子而机身有振子时，使用系统振子（`VibratorManager.defaultVibrator`，用途 MEDIA）。外接带振子的手柄仍用自己的。
  - Foundation `AndroidHapticsExecutor.kt`：`register(device, systemVibrator)`、`hasSystemVibrator()`；执行器对同一电平不重复下发（半个 one-shot 内）、已停止时不再 cancel。
  - `NativePadBridge.kt`：端口 0 且手柄无振子时以 `rumble=true` 注册并选用系统振子，日志 `device vibrator=true`。
  - `orbis_pad_adapter`：对已停止的马达不再重复入队“停止”（游戏每帧都发 0），无振子设备的拒绝语义不变。
- **验证**：日志 `Controller 'Odin Controller' on port 0: rumble=false, vibrators=0, device vibrator=true`；调用结果 sent 299 / rejected 0；`pad_vibration test 200 0` 在系统振动记录中出现幅度 0.784、Usage=MEDIA 的振动（下一帧被游戏的 0 取消）。实际挨打时的震感由用户确认。

## 3. 背景音回音、“降速”、杂音

### 根因

血源开两个 8 声道浮点 AudioOut 端口：MAIN（类型 0）与 BGM（类型 1），每块 256 帧。PS4 上 `sceAudioOutOutput` 在前一块播完后才返回，FMOD 的输出线程每 5.33 ms 取一块。Android 的 GuestAudio 只在设备队列有空位时放行，而 Oboe 设备每 20 ms 一次取 960 帧，于是每 ~19.8 ms 一次连放 3–4 块（738 批 4 块、258 批 3 块，73% 的块与前一块相隔不到 1 ms）。输出线程一口气取走约 1024 个采样，越过 FMOD 混音器的写入位置，读到上一轮的旧数据。

原始 PCM（`audio_capture`）在 1024 采样（21.3 ms）延迟处的自相关为 +0.54～+0.66，各 2 秒窗口一致，即听到的回音/降速。

### 修复

`guest_audio.cpp`：回调队列端口在队列至少半满时按块周期放行（下一次放行时间夹在当前时刻之后半个到一个周期之间，迟到不补发、提前补充不累积）；低于半满立即补充，设备不断流。Oboe 端口队列容量由 2 个设备批次增至 3 个（256 帧块：8→12）。DebugBus `audio_pacing on|off|status`（默认 on）。

### 验证（同一会话、同一设备）

| | 块间隔中位 | 连发（<1 ms） | 1024 采样自相关 |
|---|---|---|---|
| 修复前（标题，20 s） | 0.06 ms，p95 20.4 ms | 73% | +0.30～+0.66 |
| 修复后（世界，10 s，RMS 0.10–0.19） | 5.49 ms，p5 4.41，p95 6.04 | 0% | −0.12～+0.001 |

设备端 `starved=0`。用户确认声音恢复正常。

### 排除的方向（数据保留）

- **唤醒代理**：代理开/关各 15 s，AudioFlinger 混音线程平均排队 73/63 µs、FastMixer 欠载 +1/+0；AJM 线程每次调度等待 103/101/118 µs（关/开/关），FMOD 解码线程 79/73/86 µs，无差别。另：代理在运行时被关闭后，会话重启会按属性重新启用，第一次听感对照因此无效。
- **guest affinity**：运行时关闭（10:50–11:08）使 guest 线程落到小核，FPS 明显下降而占用不高，已恢复默认。属于实验副作用，不是杂音原因。
- **削顶**：新增混音峰值与限幅计数后，世界内峰值 0.960～1.082，过冲很少。
- **AJM**：未见解码错误日志；LibAtrac9 的 `char` 表全为非负值。
- **系统层**：我们的音轨在 AudioFlinger 中 `Underruns 0`；Oboe `starved=0 xruns=0`。

### 同时保留的改动

- `guest_ajm.cpp`：只把解码器实际写入的字节写回 guest（桌面与 PS4 语义一致；原实现开始时快照整块输出、结束时整块写回）。
- Foundation `CallbackMixer`：硬削顶改为立体声联动峰值限幅器（阈值 0.95，瞬时压低，约 80 ms 恢复），统计 `limited_frames` / `peak_millis`；`AndroidOutput::SetLimiter`；DebugBus `audio_limiter on|off|status`，`debug.shadps4.audio_limiter=0` 启动即关。单测新增限幅用例，Windows clang-cl 下 60/0、回调零分配。测试补 `<chrono>`。
- `PrepareAudioStereo`：8 声道下混不再逐声道削到 ±1（仅限 ±8 防异常值），过冲交给最终限幅。单声道/立体声路径不变。
- `audio_capture start <s> | status | save`：每端口原始 PCM 与每块接收时间，保存到 `log/audio-capture/`；关闭时每次输出只读一次原子量。端口打开时记一条 `Opened port` 日志。

## 4. 能否为游戏指定 CPU/GPU 频率

- 普通 app 无法设定具体频率：cpufreq、kgsl 节点对 app 只读，高通 perf HAL（`vendor.perfservice`、perf-hal 2.3）只对系统应用开放。
- AYN 提供三档预设：正常、性能、高性能（`Settings.System performance_mode`，另有 `fan_mode`）。游戏助手（`com.odin.gameassistant`）按应用保存档位（表 `application_config`），前台切换时自动套用，可在游戏助手中把 shadPS4 设为高性能。
- GameManager 对游戏包没有 OEM 干预模式；ADPF HAL 可用（此前测试对帧率无收益）。
- 测量时小核被锁在 2016 MHz，中核下限 2707 MHz，可能已有档位在生效。

## 5. 装机与未覆盖

- 最终 APK `a9535202`（host `1e5f0bbd`），每次安装前都备份了 CUSA01363 存档（`build/validation/bb-thor-20261004/`）。设备属性未改动（测试用的 `wake_proxy`/`guest_affinity` 属性已清空），诊断文件已删除。
- 未覆盖：桌面 cubeb 路径（设备节拍下每次约 2 块）是否有同类问题未测；其他游戏的 AudioOut 节奏未回归；scrcpy 录音（Opus）未解码分析，交给用户试听。
