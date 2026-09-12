# G2 收口独立复核证据

被审 `e24aad69e8b174fa9cbebe713918b42838c8ec3f`，与 origin/codex/android-fex-round2 一致。生产源代码未修改。[复核报告](../g2-close-review-2026-09-09.md)；[manifest](manifest.json)记录 source、FEX 静态库、fixtures、binary Build ID、部署 SHA。

- [guest.txt](guest.txt)：重新编译实际主仓后，完整 95/95、exit 0。
- [contract.txt](contract.txt)：同一设备完整 43/43、exit 0。
- [NX.txt](NX.txt)：仅撤销 guest mapping X，仍执行热 JIT 返回 17；**exit 2 是探针报告契约失败**。
- [rewrite.txt](rewrite.txt)：token 内改 B 后未显式失效仍返回旧 17，exit 2。
- [runs.json](runs.json)：命令、退出状态、耗时；设备进程 timeout 45 秒，主机命令 timeout 55 秒。
- [permission_probe.cpp](permission_probe.cpp)、[setup.inc](setup.inc)、[CMakeLists.txt](CMakeLists.txt)：包含实际 guest harness、实际 main 的初始化，链接 canonical backend；没有修改被审实现。
- [runner_probe.py](runner_probe.py)/[runner-probe.txt](runner-probe.txt)：合成 FAIL+exit0 accounting 负例；新 case 无归属，最终 failed=0。不是把它当作真实 guest 执行故障。
- [configure.txt](configure.txt)、[build.txt](build.txt)、[build-retry.txt](build-retry.txt)：首轮诊断目标缺 fixture 生成依赖而构建失败；补 add_dependencies 后通过，属于本次探针搭建错误，不算被审工程缺陷。

Pocket DS API 33 / ARM64 / 4096-byte pages，NDK target API 35。独立目录重编 embedder，复用 FEX `385a0cc4d` 的 assertions OFF 静态库；不是 FEX 全量 clean build，也没有 APK/ART/Swan 验收。源码与 CMake 内的绝对路径是本机复现上下文。设备上的 scoped 测试文件已清理。
