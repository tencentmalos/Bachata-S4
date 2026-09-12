# H0–H2 独立复核证据

被审 `56cbc7b1`；[复核报告](../g3-h2-review-2026-09-10.md)、[身份 manifest](manifest.json)。主仓生产源代码/正式测试没有为本复核修改。测试设备 AYN Thor API33 ARM64 4KiB，NDK target35；FEX 固定静态库复用，embedder 独立重编。

- [guest.txt](guest.txt)、[guest-retry.txt](guest-retry.txt)：同一个 binary 两轮 202 checks，分别 G24a/G24b 一次失败，均 exit 1；G30–G32 在两轮均通过。
- [contract.txt](contract.txt)：43/43，exit 0。
- [unknown.txt](unknown.txt)、[rejected.txt](rejected.txt)：最终 GuestFault，但 syscall 后 sentinel 已写 42，探针 exit 2 表示契约失败。
- [fp.txt](fp.txt)：native 看到 guest 舍入 2，期望 host 1；exit 2。
- [exception.txt](exception.txt)：native runtime_error 没有被边界捕获，libc++abi terminate，exit 134。
- [review.S](review.S)、[review_probe.cpp](review_probe.cpp)、[setup.inc](setup.inc)、[CMakeLists.txt](CMakeLists.txt)：探针包含实际 guest harness 和实际初始化，汇编另生成头文件，链接 canonical backend。没有替换被审 Handler/adapter。
- [runs.json](runs.json)、[guest-retry-run.json](guest-retry-run.json)：完整命令、退出、耗时及部署 SHA。device watchdog 60 秒，host timeout70秒。异常探针独立进程；临时设备文件已清理。
- [g24-diagnostic.txt](g24-diagnostic.txt)、[g24-diagnostic-run.json](g24-diagnostic-run.json)：只跑 G24 的诊断副本通过；[副本](g24_diagnostic.cpp)、[修改说明](g24-diagnostic-change.txt)。增加失败时 printf，未改变原谓词，不能据此关闭两次完整运行的失败。
- [runner_probe.py](runner_probe.py)、[runner-probe.txt](runner-probe.txt)：合成新子项 FAIL+exit0，无 ownership、V0/R2 failed均为0。不是 guest runtime 故障证据。
- configure/build/build-diagnostic/fixture-build 日志及各 `*-elf.txt` 保存实际构建和 ELF 身份。文件中的绝对路径是本机观察上下文，移植时改为接手机器路径；构建入口仍为主仓 cmake/fex。

没有重新构造他人“202全过”的历史日志，没有用后续局部 PASS 覆盖当前失败。没有 Swan/普通 APK/H3 验收。
