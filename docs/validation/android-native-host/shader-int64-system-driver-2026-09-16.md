# 高通系统驱动：Int64 降级实现与真机验证

日期：2026-09-16。分支 `codex/android-fex-round2`，原有未提交工作保留；本轮未 commit/push，未改 FEX/Foundation/驱动子仓。只做定向验证。

## 结论与范围

已解除 **有正确实现支撑的** `shaderInt64=0` 准入限制：设备仍如实报告该特性为 false，host 不请求启用它；guest shader compiler 自动把逻辑 U64 表示为 `{lo, hi}` 两个 u32。不是忽略 feature 检查、截断地址或安装另一份系统驱动。

AYN Thor / Adreno 740 / Android 13 API33 / 4 KiB，serial `9c2841a4`，系统 Qualcomm Vulkan `512.676.53` / API 1.3.128 / build `69e13475cb` 已实际执行探针与普通 APK。系统驱动菜单约 60 FPS、屋顶约 15 FPS，真实摇杆输入令 MOVE → ATTACK。但系统驱动屋顶出现明显块状像素/轮廓错误，**不宣称完整图形兼容或游戏性能问题已解决**。

同一套 U64 降级实现在 Turnip 上强制启用后也进入屋顶并 MOVE → ATTACK，没有复现这些块状错误。阶段 APK 并非逐字节一致，见下方身份与差异。这缩小了问题范围，不能据此证明高通驱动自身有 bug，也未排除其他 shader/资源路径的未定义行为。旧 Turnip 时间线零超时阻塞诊断仍有效，本轮没有修它。

## Turnip 与 Citron 的参考

本机参考的确采用“对外 Int64、内部拆 u32”的方式，而不是 Adreno 原生支持所有 64 位 ALU：

- `~/workspace/bug_reports/references/mesa_turnip` @ `72dc75915001063fa38abf39baa50b8a4657eefa`，`src/freedreno/vulkan/tu_device.cc`：`shaderInt64=true`；SSBO Int64 atomics 由硬件属性决定，shared Int64 atomics=false。
- `~/workspace/bug_reports/references/mesa_freedreno_gallium` @ `c1c734d006f3c579978312850e834142322ba5ea`，`src/freedreno/ir3/ir3_compiler.c` 明确指出硬件不原生支持 Int64，`lower_int64_options=~0`；`ir3_nir.c` 调用 `nir_lower_int64`；`src/compiler/nir/nir_lower_int64.c` 包括 carry/borrow、乘法、移位、比较与有舍入的浮点转换。
- Citron `src/shader_recompiler/ir_opt/lower_int64_to_int32.cpp` 的成对 u32 IR 是另一参考。这里选择共享 SPIR-V emitter helper，因为 shadPS4 的 BDA helper 在 IR 之后生成，单纯加 IR pass 会遗漏它。

上述参考版本不冒充当前私有 Turnip 二进制 `5ac41be677` 的精确源码。指针转换依据 [SPV_KHR_physical_storage_buffer](https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_physical_storage_buffer.html)：PhysicalStorageBuffer 的 OpBitcast 支持两个 32 位整数组成的向量，不必声明 Int64。

## 实现

`src/shader_recompiler/backend/spirv/spirv_int64.cpp` 集中实现 native / pair 双路径，普通有 Int64 设备继续使用原生标量。

- 常量、pack/unpack、u32 扩展/截断；加减进借位、低 64 位乘积、补码取负；0/31/32/63 位逻辑/算术移位。未选中的 OpSelect 分支也不会执行 shift-by-32。GCN 已约束的 0–63 位移位保持语义；不为 IR 未定义的超宽 shift 发明规则。
- 有符号比较先比较 signed 高字，低字 unsigned；相等同时检查两个分量。bitwise 运算保留两个分量。修复原有 FindILsb64 高半区没有加 32 的错误，零仍返回 -1。
- buffer/shared U64 使用 stride=8 的 uvec2；结构索引改为 u32 零，避免把 pair 用作 access-chain 索引；U64/U32x2 phi 与 UndefU64 支持。
- float32/64 ↔ signed/unsigned64 保留精度；整数转浮点按指数、有效位、余数、tie-to-even/定向舍入构造，避免“高低字分别转 float 后相加”的二次舍入。float64 路径只有 SPIR-V 静态验证，Adreno 不支持 shaderFloat64，**没有该路径的 GPU 数值验收**。浮点越界/NaN 转整数维持 SPIR-V 的未定义边界。
- BDA page-table load、page/offset/add/carry/非零判断全部走逻辑 U64；最终 pair bitcast 为 physical pointer，保留真实 host 地址高位。host `fault_buffer_process.comp` 也用 uvec2 输出，CPU 下载 ABI 不变。
- BDA 缺页位图原本 `load | bit → store` 会丢并发置位；改为 **32 位 AtomicOr**。独立测试用 256 个 workgroup 同时置同一 word 中的 31 个不同位，检查 `0xfffffffe`。
- **64 位原子操作不拆成两次 32 位访问**。需要但设备/强制降级 profile 不支持时，在构建 EmitContext 的早期明确抛出错误；不生成非法 vector atomic，也不假装具备原子性。
- SPIR-V Int64 capability 和 TypeInt64 均按 profile 发出。缓存 `ShaderBinaryVersion` 4→5；原有 profile 比较包含 support_int64，避免 native/pair 缓存混用。
- StatusLayer 由 Session driver_identity 显示 System / Turnip / Unknown，去掉硬编码 Turnip。会话建立时读取 `debug.shadps4.lower_int64=1`，可在支持 Int64 的驱动上强制降级作对照；正常路径没有逐帧 property 查询。

## 验证与证据

入口：[归档目录](int64-system-20260916/)、[最终二进制与源码 SHA](int64-system-20260916/artifacts.json)。未改变 guest CPU 算法、guest C++ patch 或 auto-tag 配置。

`android_shader_int64_probe` 链接生产 host DSO，使用实际 EmitContext/emitters，输入来自 SSBO 动态加载；CPU 计算期望值。覆盖边界与随机输入、比较、进借位、跨字移位、popcount/bit scan、真实共享/存储 load/store、float32 RNE/RTZ/上下取整、带高位的真实 BDA 及并发缺页、两类 atomic64 拒绝。

| 验证 | 结果 |
|---|---|
| Qualcomm 系统驱动，自动 pair | **13,059 checks / 0 failures** |
| Turnip，pair + native 对照 | **24,580 checks / 0 failures** |
| 实际 BDA | `0x4000102000`（system），明确大于 4 GiB |
| SPIR-V 静态校验 | lowered、native、完整 EmitSPIRV 入口、fp64-validation、host fault-buffer 全部通过 Vulkan 1.3 spirv-val |
| pair 模块检查 | 无 TypeInt64、Int64 / Int64Atomics capability |
| 构建 | Android RelWithDebInfo host、playstoreDebug APK 成功 |

以上 CLI 探针不是普通 APK/游戏验证的替代。

实际 APK 分阶段：

1. 首轮 system：PID4954/gen1，成功创建 renderer 并出帧，31秒在 `AvPlayer.AudioDecoderThread → avcodec_receive_frame → av_frame_unref` SIGSEGV，fault address=4。保留 [崩溃堆栈](int64-system-20260916/first-avplayer-crash.txt)，没有无证据归因为 Int64，也未声称修复。
2. system 重启：PID5709/gen1 / UUID `2e12e282adc326c100e7649743802a53`，菜单 → 屋顶 → 真实移动 → ATTACK；[warmup](int64-system-20260916/warmup/manifest.json) `GAMEPLAY_REVIEWED` 是主流程/输入验收，**不代表像素正确**。[画面](int64-system-20260916/warmup/frame-0032.png) 有明显错误。Stop 记录只抓到 StopRequested，随即安装新版，不能当成完整 Stop 验收。
3. Turnip 强制 pair：PID8183/gen1 / UUID `b858bc6258d7db83528727a0123c305c`，同一场景、MOVE→ATTACK，[画面](int64-system-20260916/turnip-lowered/frame-0015.png) 无该块状错误；[日志](int64-system-20260916/int64-mode-log.txt) 确认 `Guest shader Int64: u32-pair`。普通 APK Stop 达到 [Stopped/user_stop](int64-system-20260916/turnip-stop.txt)。此轮尚未含最后的 bitmap AtomicOr 修复，其余降级逻辑相同。

最终 system APK（含 AtomicOr 和实际驱动标签）SHA `8ecd6059bd2e9f3ba848a207106fd92e23b854c444c72e8b8fd35f16a1e80cb0`；host SHA `5cae668644561c607d1ae7b76160a16131da4828b03eb1b38a8e84ce373ac5cb`。设备已安装 APK 的 SHA 与本地一致。

4. 最终 system：PID10213 / gen1 / UUID `26e9c15d50b45de8cdc5c875729cbb82`，截图14进入屋顶，19显示移动后 ATTACK，仍有块状错误。此轮未及时提交 warmup review，脚本如实退出 `TIMEOUT_UNVERIFIED`，不能升级成自动流程验收通过；[manifest](int64-system-20260916/system-final/manifest.json) 与原图保留。自动输入已停止，随后同一会话用于[高通性能分析](qualcomm-performance-2026-09-16.md)，未重新切 Turnip 或继续做画面错误 A/B。

## 操作与下一定位点

```sh
cmake --build build/android-host-api33/native --target android_shader_int64_probe -j6
# 将 probe、host DSO、其已有 libc++/GPUReshape 依赖复制到测试目录后运行：
LD_LIBRARY_PATH=. ./android_shader_int64_probe system /data/local/tmp/shad-int64-probe
# 可在同一目录放置已校验的 Turnip 与 adrenotools hooks，再测 native/pair：
LD_LIBRARY_PATH=. ./android_shader_int64_probe /data/local/tmp/shad-int64-probe /data/local/tmp/shad-int64-probe

# APK：切驱动必须在旧会话正常 Stop 后重启进程。
adb -s 9c2841a4 shell setprop debug.shadps4.vulkan_driver system
adb -s 9c2841a4 shell setprop debug.shadps4.lower_int64 0
# Turnip 对照：vulkan_driver=turnip, lower_int64=1；结束后清掉强制降级。
```

用户最新决定：后续 GPU Reshape / RenderDoc 处理画面问题，本轮到此停止像素 A/B，保留 Qualcomm 系统驱动分析性能。不能把“所有 Int64 探针通过”当作全 shader compiler 正确。不要重新移除 feature 检查或把 atomics 非原子化，也不要依据菜单/屋顶瞬时 FPS 宣称消除了帧尾等待。
