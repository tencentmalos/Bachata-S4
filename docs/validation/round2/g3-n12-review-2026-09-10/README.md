# N1/N2 与 N3 设计复核证据

被审`dfb759a0`；[复核](../g3-n12-review-2026-09-10.md)、[执行spec](../../../specs/android-fex-round2-g3-n12-fix-n3-probe.md)。本次没有修改正式实现/runner/测试，无commit/push。

- [manifest.json](manifest.json)：source/fixture/FEX库SHA、Build ID、remote、设备、测试构建与tests=OFF符号对照。FEX10个静态库与前一复核一致，embedder在新目录编译。
- [device-results.json](device-results.json)、[canonical-runner.txt](canonical-runner.txt)、`device-results-suites/*.txt`：新构建完整guest104IDs/202checks全过、device contract43/43、host42/43+1SKIP；其他host ABI14/page22和device bionic smoke12通过。进程exit0，V0 10/0/47+3延期，R2正式24NOT_RUN。DEPLOY行核验device/local SHA相等。
- [runner-probe.py](runner-probe.py)、[runner/summary.json](runner/summary.json)：真实main/聚合，分别注入B02/B05/B06 checker失败，均summary.failed=1、overall=false、main返回0。另用实际OS和实际runner子进程验证损坏产物被ENOEXEC判never_started/exit0。合成检查不代表guest执行。
- [gate-probe.cpp](gate-probe.cpp)、[gate-implementation-extracted.inc](gate-implementation-extracted.inc)、[gate-probe.txt](gate-probe.txt)：从当前源码逐字提取实际gate类，用clang++/C++20在macOS运行。private改public仅为持mutex读取`exited_generation_`确认旧owner未退出，未修改类逻辑或私有状态。观测到一次退出确认两次Release；另记录owner timeout/rearm/Release负例。此探针不链接FEX，也不证明Android生产中断故障。
- [历史索引](historical-n2-index.json)、`historical-n2/`：原始11份完整PASS文本及canonical记录/失败记录/脚本。不是本次运行；canonical含部署hash，但源码仅标fd1c8a86+dirty，未绑定完整patch；普通文本没有artifact绑定。
- configure/build/host/nohooks日志保存三组构建输出。tests=OFF的`libguest_cpu_fex.a`中gate相关符号为0；tests=ON为12。没有ELF或静态库纳入文档目录。

实际设备AYN Thor/API33/ARM64/4096页；NDK路径`29.0.14206865`，target35。主入口仍`cmake/fex`，FEX_BUILD_DIR指向固定库，V0_ENABLE_FEX=ON；单独使用V0_BUILD_TESTS=OFF验证排除。构建目录`/tmp/shadps4-g3-n12-review-20260910/{device,host,nohooks}`，fixture输出同根下`fixtures`。配置日志和manifest中的本机路径不能直接当另一机器的环境保证。

gate探针重编：

```sh
clang++ -std=c++20 -O2 -pthread -Isrc \
  docs/validation/round2/g3-n12-review-2026-09-10/gate-probe.cpp \
  -o /tmp/g3-n12-gate-probe
/tmp/g3-n12-gate-probe
```

本次未实现或运行N3汇编出口；其源码事实与候选连接点只属于设计审计。Swan/API36/普通APK仍未验收。
