# 血源 1.00 60 FPS：C++ site 补丁与 PC/Android 共用补丁机制（2026-10-02）

基线 `85a39824d`（`feature/malos/swan_performance`）。同日晚间提交：机制为 `dd145b6e5`，血源包与本文随后提交。

## 做了什么

- **共用格式与布局**：包解析、Zydis trampoline、stub/slot/payload 布局从 Android
  `guest_patch.cpp` 抽到 `src/core/host_runtime/guest_patch_format.{h,cpp}`（`BuildPlan`）。
  Android `Manager::Install` 改为调用它，校验/发布/token 流程不变；桌面加载器调用同一个函数。
- **sdk_version 2**：`sites`（任意指令边界的 C/C++ 处理函数，`before`/`replace`，可写
  GPR/flags/YMM，保留 guest MXCSR）与 `patches`（同长度字节替换，安装时校验原字节）。
  SDK 头 `guest/custom/v1/include/shad_site.h`，适配器 `site_adapter.S`。v1 recipe 输出不变。
- **Relocator** 接受 VEX 编码（RIP 相对 disp 重算；EVEX/XOP 仍拒绝）。
- **桌面加载器** `src/core/guest_patch_desktop.cpp`：`SHADPS4_GUEST_PATCH=<patch.json>`，
  每个模块加载末尾检查一次（代码运行前）；身份 = 游戏序列号 + 经挂载层读取的模块 SHA256
  （ZAR 可用）+ 加载后 preimage；payload 映射在模块 trampoline 区之后；SDK 导入绑定
  SysV host 函数；DebugBus `guest_patch status|enable [name]|disable [name]`，原子切换 slot。
  失败记错误日志，游戏不打补丁继续运行。
- **builder**：sites/patches；Windows 下按 `.exe` 找 ld.lld/llvm-objdump；make 依赖解析
  兼容盘符路径（以前在 Windows 上整个构建直接失败）。
- **血源包** `guest/games/CUSA03023/01.00/sixty_fps.*`、`frame_manager.h`：7 个 replace
  site，对应此前桌面 XML v4a 的全部逻辑，见[目录说明](../../../guest/games/CUSA03023/01.00/README.md)。

## 验证

| 项 | 结果 |
|---|---|
| builder 单测（`tests/guest_cpu/test_guest_functions_builder.py`，NDK29 clang，Windows） | 15/15；新增 4 项（site/patch 包、v1 保持 sdk_version 1、超长 replace、不等长 patch）；原 `source_dependency_hashes` 断言改为不依赖路径分隔符 |
| 血源包构建 | `GUEST_FUNCTIONS_BUILD_PASS 36a02c52019cb940ed429df534d7949cbb073be88929b1823511ee9225a41faa` |
| 桌面构建（clang-cl RelWithDebInfo） | 通过，exe SHA256 `b94bc64c…` |
| Android host（`build-host-android --turnip-prebuilt-lock build/drivers/turnip-barycentric-local.json`） | `HOST_LINK_PASS`，`libshadps4_host.so` `71dff200…`；默认源码 Turnip 在 Windows 上不支持（既有限制），首次运行因此失败 |
| 桌面血源（RX 7600M XT，`-g CUSA03023.zar`） | 安装：7 site 全部通过 preimage（在 Windows red-zone 静态补丁之后核对）；ZAR 内 eboot 哈希约 0.28 s。标题 63 flips/s，中央亚楠 51–53 FPS，`frame_delta_us` 稳定 16666；运行中 `disable` → 31.0 flips/s，`enable` → 52.8；启用后连续运行约 8 分钟、2.6 万帧无异常 |

## 未验证 / 边界

- 速度与声音是否与 XML v4a 一致，等用户实际游玩确认（XML v4a 已由用户确认正常）。
- `before` 模式没有在游戏里单独运行；它由已运行的两部分组成（replace 用的同一适配器、
  disable 时执行的重定位 trampoline），单测只覆盖构建产物。
- Android：只编译链接，未上设备（本轮按用户要求只用本机）。`guest_patch_tests`（设备 FEX）
  未运行。同一个包的模块 SHA 与此前 Android 探针使用的 SHA 相同（`6764938b…`）。
- Lance 1.09 补丁中另外 5 处改动未移植，见目录说明。
- 桌面同一进程只装一个包；`patches` 不随 enable/disable 切换。
