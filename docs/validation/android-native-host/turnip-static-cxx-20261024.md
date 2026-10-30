# Turnip 独立 C++ 运行时（2026-10-24）

## 结论与范围

源码构建的 mainline Turnip 原先依赖 `libc++_shared.so`。现将 libc++、libc++abi
静态链接并隐藏符号，驱动只经 Vulkan C ABI 与宿主交互。Odin3 纯 C 进程复现旧
ELF 缺库，并验证新 ELF 的 HAL → instance → device → queue/fence 完整通过；
生产 `LoadAndroidTurnip` / adrenotools 路径另行通过能力查询。

APK `6748a7b1` 已生成并核验，**本轮未覆盖安装、未启动或结束用户游戏**。
设备原应用 PID 31742 保持；检查时已无 FexSessionService。此记录不是游戏渲染、
性能、长会话或其他设备验收。

## 实现

- `scripts/android/build-turnip-source` 显式传 Meson `cpp_link_args`：
  `-static-libstdc++`、`-Wl,--exclude-libs,libc++_static.a:libc++abi.a:libunwind.a`。
  作为 setup/reconfigure 参数，已有 Meson 构建目录也会更新。
- 纯 Python 读取 ELF64/AArch64 的 PT_DYNAMIC / DT_NEEDED，不依赖 section
  headers 或开发机 readelf。源码输出、release prebuilt、same-source prebuilt、
  host 绑定和 APK 打包都拒绝动态 C++/ABI/unwind 依赖。即使 Mesa commit
  相同，也不能复用旧的动态 STL 产物；保持原有 SHA / source / host pin 校验。
- identity 记录实际 needed 与静态 libc++/libc++abi SHA；NDK toolchain NOTICE
  随产物、跨主机 prebuilt 和 APK 保留。Mesa generator 使用所选 Meson 的
  Python 环境，避免虚拟环境已装 Mako、直接执行 generator 却用系统 Python。
- 宿主/JNI/adrenotools hooks 继续共用应用 `libc++_shared.so`，未改这些跨 DSO
  边界。Mesa 源码、提交和驱动算法均未修改。

## 二进制与包检查

Mesa `68dafb89a26adefeaa12411f08e841aaf0b57384`，NDK `29.0.14206865`，API 33。

| 产物 | SHA-256 | 结果 |
| --- | --- | --- |
| 旧 mainline | `d7e6746a5eb02500f8d4ac3022ae8459a3e683b56ba55b6dc2705b5706f46099` | needed 含 libc++_shared.so，绑定/打包及同源码复用负对照均拒绝 |
| 新 mainline | `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d` | 无动态 STL/ABI/unwind；动态定义符号仅 HMI |
| 既有 R8 | `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09` | 本来就无该依赖，字节未变 |
| 新 host | `ae21fd9e040ac050dacd670fb8f4cfe8107f1b2672793d6d512a56417e06aa03` | 包含新 driver pin，host 链接成功 |
| F-Droid debug APK | `6748a7b1e81db15a6443a1c8e4c0d7c93e98406d2525458daf015378c2128f83` | mainline/R8/host SHA、共享 host STL 与 NDK notice 均核对 |

新驱动 DT_NEEDED 仅 `libhardware.so`, `liblog.so`, `libnativewindow.so`,
`libsync.so`, `libm.so`, `libz.so`, `libdl.so`, `libc.so`。文件由 15,532,320
增至 18,099,248 bytes（约 +2.45 MiB），不是性能优化数据。
旧的 Play Store APK 未重打，本次仅验上述 F-Droid APK。

## 回归与设备证据

1. `python3 tests/video_core/turnip_runtime_dependencies_tests.py`：9/9。
   覆盖 sectionless ELF、无关 runtime 字符串、动态运行时依赖、ABI、越界、
   缺终止符及 file-backed string table；真实旧 ELF 的两个入口负对照均拒绝，
   新 ELF 和既有 R8 均通过。
2. `tests/video_core/android_turnip_load_probe.c` 使用 NDK C 编译器，
   `-std=c11 -Wall -Wextra -Werror`，include Mesa `include/android_stub` 与
   `externals/vulkan-headers/include`，只链接 `-ldl`；其 needed 仅 libc/libdl。
   设备 Odin3 `6edb0593` / Android 15 / Adreno 830，默认 shell linker 路径：
   - 旧 ELF：`library "libc++_shared.so" not found`，exit 1。
   - 新 ELF：`driverID=18` / Mesa 26.3.0-devel / git-68dafb89a2；HAL、instance、
     device、空提交和 3 s 有限 fence wait 均成功，exit 0。
   - `/proc/self/maps` 未映射 NDK `libc++_shared.so`。平台 allocator/系统库
     在 main 前已可加载系统 `libc++.so`，因此不声称整个进程完全没有 C++ 运行时。
3. 生产 `android_gcn_capabilities_probe` 链接新 host，配套四个 adrenotools
   hooks 和 host STL，在独立目录调用实际 `LoadAndroidTurnip` 与
   `Vulkan::Instance`：exit 0；shaderInt64=1、compute/fragment subgroup64=1、
   geometry=1、barycentric=1。该路径的 driverID/API/Int64 校验仍生效，
   没有以系统驱动静默回退替代成功。
4. `build-host-android`、`:app:assembleDebug` 成功；APK 解包核对上述身份。

最初尝试人为设置 `LD_LIBRARY_PATH=/vendor/lib64:/system/lib64` 时出现
system/vendor Binder 库混用符号错误；撤去覆盖，使用设备默认路径后得到上述
正反对照。该失败不计作驱动成功，也没有更改设备全局路径或属性。

本地证据：`build/validation/turnip-static-cxx-20261024/` 中的
`binary-gates.json`、`old-load.log`、`new-load.log`、`production-loader.log`、
`production-user/`、`driver-elf.txt`、`driver-symbols.txt`、`apk-verification.json`
及构建日志。临时设备目录 `/data/local/tmp/turnip-static-cxx-20261024` 已清理。
未改游戏配置、驱动选择或用户库；未提交、推送本轮改动。
