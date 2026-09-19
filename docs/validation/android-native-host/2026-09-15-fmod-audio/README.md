# FMOD / Android 音频设计审计证据

对应[设计对照与结论](../fmod-android-audio-design-audit-2026-09-15.md)。本轮只新增审计文档、统计和静音探针，没有修改生产音频代码、安装 APK 或执行新的游戏回归。

## 证据归属

- `manifest.json` 固定检查过的主仓工作区、Citron、Azahar、Oboe、cubeb 源码及探针二进制身份；本地 checkout revision 不代表远端已经发布。主仓当前含此前未提交修改。
- `production-aaudio-excerpt.txt` 来自上轮普通 APK PID21732，证明生产同步流实际 `LOW_LATENCY → NONE`。本轮探针是同 UID 的独立 native 进程，不能当作普通 APK / ART / FEX 验收。
- `loading-window.json` 和 `loading-poll-counters.json` 重算较早 PID5502 的 PROF：`db4612967f9522cf22cdede1a79fb881d0059f22949f4f5f2754a50cd44bcade`。本地 SQLite 为 `build/startup-profile/index-cold/a3fd61cb50bd425cadfecd51ff6e111f.litep.sqlite`，不是当前音频队列版本的新采样。
- PROF 一秒窗口为 `[171090000000000,171091000000000)`；按 interval 起点选取、末端裁剪，排除窗口前已开始的 scope。时长含等待，不是 CPU 时间；clock/sleep 每64次共用采样会产生相位偏差。
- `fmod-nid-matches.json` 记录真实 PS4 模块的公开函数匹配。版本及 helper 判断来自有限静态反汇编；没有运行 inferior 函数、取得闭源引擎源码或证明候选原子计数写入点对应同一动态对象。游戏 ELF 与完整反汇编只保留在本地 build，不加入文档目录。

## 直接 AAudio 探针矩阵

全部请求 OUTPUT、LOW_LATENCY、Game usage、48kHz；除 mode8 外为 Shared。每项约1.2秒静音。结果文件第一行记录实际 UID/PID，其余按 mode 对应。

| mode | 格式 / 声道 | 消费方式 | capacity 请求 / size 调整 |
| --- | --- | --- | --- |
| 0 | Float / 8 | blocking write | 2048 / 不调整 |
| 1 | Float / 2 | blocking write | 2048 / 不调整 |
| 2 | Float / 8 | callback | 2048 / 不调整 |
| 3 | Float / 2 | callback | 2048 / 不调整 |
| 4 | Float / 2 | callback | 2048 / 2×实际 burst |
| 5 | Float / 2 | callback | 默认 / 不调整 |
| 6 | Float / 8 | callback | 默认 / 不调整 |
| 7 | Float / 2 | callback | 默认 / 2×实际 burst |
| 8 | Float / 2，Exclusive 请求 | callback | 默认 / 2×实际 burst |
| 9 | Float / 2，Stereo mask | callback | 默认 / 不调整 |
| 10 | I16 / 2，仅 channel count | callback | 默认 / 不调整 |
| 11 | I16 / 2，Stereo mask | callback | 384 / 不调整 |
| 12 | Float / 2，Stereo mask | callback | 384 / 不调整 |

`aaudio_probe_v1.cpp` 对应第一轮 mode0–4；`aaudio_probe_v2.cpp` 对应第二轮 mode0–8；文件名 `aaudio_probe.cpp` 是第三轮 mode9–12。保留实际执行源码，不把后来的矩阵覆盖前一轮。

对应输出为 `aaudio-probe-appuid.jsonl`、`aaudio-probe-v2-appuid.jsonl`、`aaudio-probe-v3-appuid.jsonl`；每轮都有对应 PID 的 logcat。`perf=10` 是 NONE、`12` 是 LOW_LATENCY；callback policy0 为 SCHED_OTHER。buffer size/capacity 是帧数，不是端到端延迟。

## Oboe 矩阵

固定 Citron Oboe 子仓 `987538b6ec4cb9e699b117a890e686de7a9302fa`，不包含 Citron SinkStream / DSP / guest 行为。

| mode | 配置 |
| --- | --- |
| 0 | Citron 风格 OpenSL ES / I16 / stereo / 48kHz / capacity480 / size480 |
| 1 | 同一配置改 AAudio |
| 2 | AAudio / Float / stereo / 默认 rate、capacity / size2×burst |
| 3 | mode2 再请求 Exclusive |

所有配置请求 Game usage / LowLatency，允许格式与声道转换，并指定 High 采样率转换质量。源码为 `oboe_probe.cpp`，输出为 `oboe-probe-appuid.jsonl`。`api=1` 是 OpenSL ES，`2` 是 AAudio；`sharing=1` 是 Shared；`-890` 是 Unimplemented，不能把不支持的 timestamp / 调参解释为成功。

## 构建复现

在主仓根目录运行；所有输出写入独立 build 子目录，不改 Oboe 源码。

```sh
audio_ndk=/Users/bytedance/Library/Android/sdk/ndk/29.0.14206865
audio_cxx="$audio_ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android33-clang++"
audio_evidence=docs/validation/android-native-host/2026-09-15-fmod-audio
audio_oboe=/Users/bytedance/workspace/emulations/switch/citron/externals/oboe
audio_build=build/audio-design-audit-reproduce
mkdir -p "$audio_build"
"$audio_cxx" -O2 -g -std=c++20 -static-libstdc++ "$audio_evidence/aaudio_probe_v2.cpp" -laaudio -o "$audio_build/aaudio_probe_v2"
cmake -S "$audio_oboe" -B "$audio_build/oboe" -G Ninja \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_TOOLCHAIN_FILE="$audio_ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
cmake --build "$audio_build/oboe" -j4
"$audio_cxx" -O2 -g -std=c++20 -static-libstdc++ \
  -I "$audio_oboe/include" "$audio_evidence/oboe_probe.cpp" \
  "$audio_build/oboe/liboboe.a" -llog -lOpenSLES -ldl -o "$audio_build/oboe_probe"
```

原始构建目录为 `build/audio-design-audit`；重新构建的路径/调试信息不同，二进制 SHA 不保证与 manifest 相同。probe 是诊断工具，需检查 JSON 中 open/start/stop/close 等结果，不能仅凭进程退出码判定通过。

## 执行范围与收尾

设备仅 AYN Thor `9c2841a4` / API33 / 4KiB，没有操作 Swan。通过 `adb -s 9c2841a4 shell run-as com.shadps4.android /data/local/tmp/<probe>` 运行；UID10190、SELinux `runas_app`，日志中有 permissive=1 的执行拒绝记录。未修改设备安全策略，也不建议为探针修改策略。普通 APK 路由和调度准入仍需单独验证。

探针 PID32242、3645、5248、7683 均已结束，创建的四个 `/data/local/tmp/shad-*-probe-*20260915` 程序已删除。`session-after-probes.txt` 保留原游戏进程 PID21732 的 Stopped / user_stop 状态；它不是一次新游戏运行结果。

短静音样本没有 xrun 只说明该负载没有观测到欠载。当前白屏、Circle 后场景推进和长 loading 没有被本轮修复；Oboe / callback 也没有在这些独立探针中取得 LOW_LATENCY，不能承诺换库即解决。
