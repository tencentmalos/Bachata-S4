# Turnip KGSL alloc64 候选驱动：Swan 血源实机验证（2026-09-28，未采用）

## 对象

另一会话的离线报告 `swan-turnip-cp-opcode-20260928.md`（附件，未入库）指出：Turnip
`kgsl_bo_init()` 的 `kgsl_gpumem_alloc_id.flags` 是 `unsigned int`，
`KGSL_MEMFLAGS_FORCE_32BIT = 1ULL << 32` 被截掉，因此 IB/命令 BO 没有被放到低 4 GiB。

- 本仓核实：`references/mesa-turnip`（86ca）的 `msm_kgsl.h` 与 `tu_knl_kgsl.cc:299-331` 确实如此；
  同文件的 sparse 路径注释也写明 `kgsl_gpumem_alloc_id` 只有 32 位 flags。
- 补丁：[`turnip-kgsl-alloc64.patch`](turnip-kgsl-alloc64.patch)，改用 `GPUOBJ_ALLOC` + `GPUOBJ_INFO`。
- 候选包：[`package-manifest.json`](package-manifest.json)。基线是 **a1c3d387**（另一条 Swan 分支，
  带 QCOM surface/FDM/deferred 提交），**不是**本仓发布的 86ca。
  - so SHA `ff7e9980…`，Build-ID `2d71c610…`。

## 实机结果（Swan `PB3110PGL6240001G`，血源 CUSA03023）

| 会话（pid） | 驱动加载方式 | 结果 |
| --- | --- | --- |
| 18181（`ab/`） | 侧载候选（APK 15941659） | 约 1.5 min 后 `CP opcode error interrupt opcode=0`，ib1 `0x05265000`、ib2 `0x06646A10/0x1a4` |
| 23301（s1） | 打包候选（APK 2c91a07d） | 30 FPS 跑满约 3.5 min，无故障（后被下一轮 force-stop） |
| 2941 | 打包候选 | 加载中 `CP opcode error interrupt opcode=0`，ib2 `0x05806A10/0x1a4`（devcd9） |
| 5722 | 打包候选 + scrcpy SDK（APK fe714b6b） | 加载中 device lost：着色器（`BR_SP`）写地址 0，另有 141 条被限流（devcd10） |

另外 24960（s2）被启动脚本误点游戏内 Stop，28103 在 DumpLayer 首次套用预设、重启 spatial runtime 时被
连带杀掉，这两个会话不计入结果。原始行见 [`kgsl-dmesg.txt`](kgsl-dmesg.txt)、
[`logcat-sessions.txt`](logcat-sessions.txt)、[`soak/runs.txt`](soak/runs.txt)。

结论：

1. **补丁生效，但原故障仍在。** IB 已进入低 4 GiB（86ca 下是 `0x41…`），`CP opcode error opcode=0`
   照样出现，且保持与 devcd4/devcd6 相同的特征：IB2 页内偏移 `…6A10`、剩余 `0x1a4`。32 位回绕不是根因。
2. **a1c3d387 基线本身有问题。** 游戏面板全黑：DumpLayer 显示触控按钮和 HUD 正常绘制，但游戏画面区域是黑的
   （[`dumplayer-black-panel.png`](dumplayer-black-panel.png)）。另外出现 86ca 下没见过的写地址 0 故障，
   scrcpy 嵌入式录制在编码 surface 上建 swapchain 时报 `ErrorInvalidExternalHandle`。
   这几项没有与同基线的 control 包对照，不能归到补丁本身。
3. 候选驱动未采用：锁文件、`AndroidTurnip.kt` 与 `vk_driver_android.cpp` 的 SHA 都保持 86ca。

## 下一步（用户决定暂缓 KGSL）

- 若继续：把补丁打到 86ca 上重编，并保留 ROQ-vs-内存对照方法，只改变一个变量。
- 验证脚本 `tools/run_soak.sh` 的存活判断依赖 `adb pidof`，dumpsys 报 Broken pipe 时会误判，
  启动脚本 `launch_bb_pad.sh` 也会在读不到状态时重复点击；再用之前需要改成按 pid 与 generation 判断。
- `ab-failed-pin/`：侧载档第一次被原生固定 SHA 校验拒绝的一轮；`ab-disconnected/`：adb 断连、驱动属性没设上的一轮。
  两者作为失败记录保留。侧载档已按用户要求撤回。
