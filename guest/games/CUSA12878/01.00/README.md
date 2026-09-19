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

当前证据和限制见 [初始化验证](../../../../docs/validation/android-native-host/beatsaber-vr-init-20260919.md)。
