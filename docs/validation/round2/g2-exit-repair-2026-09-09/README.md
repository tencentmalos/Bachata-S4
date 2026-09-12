# G2 出口修复证据

[修复报告](../g2-exit-repair-2026-09-09.md)；[manifest](manifest.json)区分 HEAD 与未提交源代码 hash。FEX 不重编、不改 pin；本次新建 embedder 目录后随修改重编。所有 binary 只在本机临时目录，未加入仓库。

- [results.json](results.json)、[runner-final.txt](runner-final.txt)：V0 与 24 项 R2，子项/环境/最终验收分别记账。
- [guest 原始日志](results-suites/guest_execution_tests.txt)：97 IDs、195 checks，G25/G26 各 100 EPOCH、G27 10 STORE，部署 SHA、设备/adb exit。
- [Android contract](results-suites/guest_cpu_contract_tests.txt)、[host contract](results-suites/api_contract_tests.txt)、[host HLE frames](results-suites/hle_abi_tests.txt)、[bionic](results-suites/fexcore_bionic_smoke.txt)、[device page](page.txt)。contract 文本里的 host-only 是独立测试 scope 提示；实际部署身份以 runs/manifest 为准，不证明 ART/APK。
- [NX](NX.txt)、[rewrite](rewrite.txt)：在修复源代码上重跑原独立探针，均 exit 0。
- [store-disabled](store-disabled.txt)：guest writer 换为 return_only，G27 FAIL/exit 1 是预期结果。[生成负例脚本](g2_exit_negative.py)记录唯一语义替换；[生成源码 SHA](negative-source.sha256)。负例不参与正式 suite 的通过计数。
- [probes-runs](probes-runs.json)、[final-runs](final-runs.json)：完整命令/退出/耗时；独立 probe 有 device timeout 45 秒、host timeout 55 秒。正式 runner 由外层 180 秒控制，实际 guest 约 23 秒；这不是生产取消机制验收。
- [runner unit](runner-unit.txt)、[accounting](runner-accounting.txt)、[G1 evidence](runner-g1.txt)：FAIL+exit0、未运行、异常退出、timeout、SKIP、重复 ID/轮次、缺轮/畸形证据和部署 SHA 拒绝。
- [production-hook-check](production-hook-check.txt)：V0_BUILD_TESTS=OFF 主机 API 构建及符号检查；不冒称生产 APK 检查。
- build/host-build/production/probes-build 日志、各 `*-elf.txt`、[FEX flags](fex-build-flags.txt)、[源代码 patch](source-repair.patch)说明实际产物身份。日志中绝对路径是本次观察上下文；可移植构建入口仍是仓库 `cmake/fex`。
- [初轮 guest 失败](guest-initial.txt)/[runs](runs-initial.json)：G26 陈旧 marker、G10 注入边界提前，exit 4。随后 [第二轮](guest-second.txt)/[runs](runs-second.json)通过；这些是修复过程记录，不代表最终 source hash。最终日志为 results-suites 下文件。

设备为 Pocket DS API33 ARM64 4KiB，普通 APK/Swan 验收未执行。历史 e24aad69 的失败证据保留在相邻 close-review 目录；不得把修复后的日志补进旧 close report。测试创建的设备临时文件在运行后清理。
