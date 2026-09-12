# G2 审核原始证据

审核结论见 [报告](../g2-review-2026-09-09.md)，下一阶段见 [修复 spec](../../../specs/android-fex-round2-g2-repair.md)。[manifest](manifest.json) 固定被审提交 `5304e5a9`、源码 hash、部署 SHA、Build ID、FEX/设备/host 身份。本次被审生产代码与提交一致，未用修复补丁运行这些探针。

## 观察结果

| 运行 | 环境 | 退出 | 实际观察 |
|---|---|---|---|
| [baseline](baseline.txt) | Pocket DS / API33 / ARM64 / 4096 CLI | 0 | 原 guest suite 85/85 |
| [device contract](device-contract.txt) | 同上 | 0 | 37/37；harness 文案仍写 host-only，以实际部署环境为准，未测 ART |
| [host contract](host-contract.txt) | macOS ARM64 / 16384 | 0 | 36 PASS + 1 SKIP（M13f） |
| [clear_rx](clear_rx.txt) | Android CLI | 124 | Clear(RX) 成功后再次 Invalidate 挂起，被 timeout 3 结束 |
| [foreign](foreign.txt) | Android CLI | 0 | 外 space 的有效 token 被 Clear 接受 |
| [rw_skip](rw_skip.txt) | Android CLI | 0 | 同一 thread A=17 → remap 写 B → Clear(RW) 成功 → 实际仍为 17，期望 34 |
| [admission](admission.txt) | Android CLI | 0 | 协调等待中 CreateThread 成功；协调超时后执行 lease 仍可获取 |
| [cancel](cancel.txt) | Android CLI | 0 | 两原 Run 都 Cancelled；协调释放后至少一个 owner 无 Resume 再执行 |
| [sink-race](sink-race.txt) | macOS host | 0 | sink 阻塞中 Remap/Reprotect 成功，Publish 报成功，byte 为 0 而非 90 |
| [poison-remap](poison-remap.txt) | macOS host | 0 | poison 仍在，Remap(RX) 成功且 metadata 有 Execute；未证明全局 Run guard 被绕过 |

探针是**诊断程序**：打印违反契约的结果后仍可能 exit 0，不是验收 PASS。设备命令和 exit code 在 [runs.json](runs.json)。这些是单次定向复现，不冒充 100 次故障压测；hold 用于让审核窗口足够稳定，正式验收应按修复 spec 改成显式 barrier 与断言。

## 复现方式

- [g2_probe.cpp](g2_probe.cpp) 包含本提交的 guest harness，复用 fixture 和 owner 辅助代码；[setup.inc](setup.inc) 提取同一 harness 的初始化。
- [CMakeLists.txt](CMakeLists.txt) 通过主仓 `cmake/fex` 链接同一个 `guest_cpu_fex`。其绝对路径是本次机器观察值，独立 checkout 需替换主仓路径和 fixture 路径，不可不经核对加载其他版本。
- [host/host_probe.cpp](host/host_probe.cpp) 只依赖 public address-space API，以 promise barrier 保持 sink 正在执行。

把本目录中的探针源码复制到临时工作目录，将路径改成当前被审 checkout，然后按 [build.txt](build.txt) 使用的 NDK/API 配置构建。概要命令：

```sh
cmake -S "$PROBE_SOURCE" -B "$PROBE_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
  -DFEX_BUILD_DIR="$REPO_ROOT/build/fexcore-android"
cmake --build "$PROBE_BUILD"
adb -s "$ANDROID_SERIAL" push "$PROBE_BUILD/g2_probe" /data/local/tmp/g2-review-probe
adb -s "$ANDROID_SERIAL" shell chmod 755 /data/local/tmp/g2-review-probe
adb -s "$ANDROID_SERIAL" shell 'timeout 3 /data/local/tmp/g2-review-probe clear_rx'
adb -s "$ANDROID_SERIAL" shell 'timeout 3 /data/local/tmp/g2-review-probe foreign'
adb -s "$ANDROID_SERIAL" shell 'timeout 3 /data/local/tmp/g2-review-probe rw_skip'
adb -s "$ANDROID_SERIAL" shell 'timeout 3 /data/local/tmp/g2-review-probe admission'
adb -s "$ANDROID_SERIAL" shell 'timeout 3 /data/local/tmp/g2-review-probe cancel'
```

实际构建目录为 `/tmp/shadps4-g2-review-20260909/build`，FEX 固定库来自主仓 `build/fexcore-android`，`ENABLE_ASSERTIONS=OFF`。这是新目录构建主仓 adapter 和 probes，未重新编译 FEX。

host probe 可用同一目录的 `host/CMakeLists.txt` 在 macOS 配置编译；无参数运行 sink 竞争，有任意参数运行 poison remap。每个 probe 都使用自行申请的 guest reservation，不触碰游戏和应用数据。设备上的本次三个临时可执行文件在收集 SHA 后清理。二进制不入 Git。
