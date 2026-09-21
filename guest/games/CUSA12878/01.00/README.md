# Beat Saber 1.00：VR 初始化标定与诊断 patch

目标为 `CUSA12878 / 01.00 / eboot.bin`。recipe 绑定原始 SELF SHA256
`9ac1745407a97af986d5f6dfe246a7c0d695857f4049d7d717f896ed219778cf`，不能用于其他更新版本。

- `vr_init.recipe.json`：11 个保留原寄存器的入口观察器，记录调用次数、参数和设备名称列表，不修改选择结果。
- `vr_preference_diagnostic.recipe.json`：16 个 hook。对原始 `[None, PlayStationVR]` 名称列表使用栈上副本调整尝试顺序，并保留原 selector 与 fallback；5 个 HLE 转发 wrapper 记录真实返回码。
- `symbols/index.json`：11 个有静态调用图/字符串证据的语义入口。这些是重建名称，不是供应商符号。
- `vr_hle_results.cpp`：HMD 查询/打开、Camera 是否存在/读帧、Reprojection 初始化的 typed wrapper，完整转发结果。

`None` 优先是实际观察到的配置，尚未证明游戏后续 managed 切换本身有错误；偏好 patch 仅用于独立验证 PSVR provider。它不是 C/C++ 重编译整个 Unity VR 模块，也不是正式的永久启动修复。

分析使用解密 ELF SHA256 `71278f9c85ef93a9b2e8703299d6875020eb4d8f7b821bf98eb36ec73340592b`。
为了让静态工具打开 Orbis ELF，分析副本仅将 `e_type` 两字节改为 ET_DYN；其余字节逐一比对相同。
所有偏移仍相对原始模块，运行加载基址为 `0x400000`；patch 安装校验原始 SELF 和入口 preimage。

构建示例（仓库根目录）：

```sh
python3 tools/guest-functions/build.py \
  --recipe guest/games/CUSA12878/01.00/vr_preference_diagnostic.recipe.json \
  --clang /Users/bytedance/Library/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin/clang \
  --output build/beatsaber-vr-patch
scripts/android/guest-patch 9c2841a4 deploy build/beatsaber-vr-patch/patch.json
scripts/android/guest-patch 9c2841a4 status
```

诊断完成后先 `disable` 当前 package，再 `clear` 下一 session 属性；resident code 只在 session 销毁时回收。tag 日志保存在 app 私有目录 `files/host/log/guest-patch.log`，与主日志分开。

2026-09-20 插件修复后，原始设备顺序已在 HMD 设置完成后自行切到 PlayStationVR，继续验证时默认使用纯日志 recipe，无须 preference 诊断。无 patch 普通启动仍有异常双眼局部元素，未达到菜单/SBS 验收；见 [加载修复报告](../../../../docs/validation/android-native-host/beatsaber-plugin-loader-20260920.md)。

当前证据和限制见 [初始化验证](../../../../docs/validation/android-native-host/beatsaber-vr-init-20260919.md)。

## guest call log（2026-09-20）

`vr_call_diagnostic.recipe.json` 为默认只观察版本；`vr_call_preference_diagnostic.recipe.json`
显式启用历史 PSVR 偏好实验。二者均提供 16 个 hook，日志带 Call/Sequence/Phase 和
原始输入/输出块；未知 C ABI 入口保留完整机器状态。高频采样最多到调用 8192，
不是持续全量 tracing。构建部署用上方相同工具，替换 recipe 路径即可。

按 `package=` 过滤成单次运行的日志后运行 `python3 decode_vr_call_log.py RUN.log`，
同时生成 `.decoded.json`。保留 raw bytes；Reprojection 的 opaque28 不等同已恢复的位姿。
当前真实结论见 [guest log 报告](../../../../docs/validation/android-native-host/beatsaber-guest-log-20260920.md)：
固定朝前与 camera 单位旋转已部署，但游戏仍黑屏，动态插件加载失败尚未修复。
