# AYN / MHR 重连复核与未完成的 TLS 修复（2026-09-22）

本轮用户先要求实机验证 MHR，随后明确优先定位 MHW 的 DLC 错误；MHR 工作保留在下述边界。没有 MHR 可玩或全部崩溃已修结论。

## 已实施与实测

- AYN `9c2841a4`，Android 13 / A740；安装离线诊断包 `3330298e`，原定向 rwlock 60、诊断 1233、网络 380、NP 751 项均 0 fail。完整身份见本地 `build/validation/mhr-device-20260922/tests.json`。
- 部署用户自有 11.00 `libSceJson2.sprx`，SHA256 `6a9d3894497cdfbe8720a5720cc0ab06f4aefbc73dff6e5c86885770b58097c5`；MHR 63 个所需导出实际 guest_export，DT_INIT 完成。不是只有静态匹配；模块留设备、不入 Git。
- rwlock 日志捕获第 4097 次初始化误回 ENOMEM。去掉服务域的累计 4096 限制，保留真实分配失败与回滚；运行时为 rwlock 独立分配 256 字节槽、16 KiB 页，避免借用 mutex 域的容量。
- 新测试覆盖 5000 个同时存活对象及 5000 次属性创建/销毁。旧 host 73 项 / 9 fail；修复 host `daf83da7` 73 / 0，诊断 1233 / 0。APK `f91cdcf` 已实际安装，完整 SHA 见 `installed-fixed.json`。此修复并未消除后续 MHR 崩溃。

## 新证据：主模块静态 TLS 布局错误，尚未改代码

`src/core/linker.cpp` 的 PrepareGuest 将模块 TLS 对齐强制提高到至少 32。MHR 主模块 PT_TLS 为 filesz=4、memsz=16、align=8，初始化字为 `ffffffff`。游戏指令从 `FS-16` 读取线程私有索引；实际初始化内容位于 `FS-32`。

PID 6688 的 Guest 26/27 拥有不同 FS base，但 `FS-32` 都是 -1，`FS-16` 都是 0；两线程因此选中同一个 allocator pool。在 main+0x3f97860/66 观察到零长度 chunk 循环。原始线程、寄存器、内存、暂停 epoch 与清理结果保存于 `build/validation/mhr-device-20260922/guest-tls-evidence.json`，静态字节位于 `allocator-entry.asm`、`allocator-spin.asm`。

这确认了初始化位置与游戏访问位置不一致。尚未取得最初损坏写入，也未证明旧三份 tombstone 全由它引起。下一步在主仓修正 TLS 布局，按模块 align/memsz 验证初始化、DTV 与 TLS relocation 一致，并用多线程生产 FEX 夹具及 MHR 实机复测。未修改 FEX 子仓。

## 证据与边界

本地证据根：`build/validation/mhr-device-20260922/`，[身份索引](mhr-device-20260922/manifest.json)。保留旧崩溃、rwlock 修复后反例与失败的 CodeLLDB attach，不把停机/调试扰动当成正常退出。所有已建立 Guest RSP 会话已清理；后续 MHW 终态与整轮配置/存档复核由 [MHW 续修记录](mhw-dlc-silent-20260922.md) 归档，目前设备交给 OCR 任务，终态比较与属性恢复待完成。没有存档格式化，没有 commit/push。
