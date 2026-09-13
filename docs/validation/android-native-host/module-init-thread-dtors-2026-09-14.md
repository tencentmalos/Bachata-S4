# 零偏移 DT_INIT、模块复用与 libc 线程析构

在 `61805270` 后继续直接修复。**完整资源 TMNT 普通 APK 三次同进程启动已越过 FMOD 空指针，稳定到 `sceAjmInitialize`（`dl+4eHSzUu4` / op385 / Unsupported）**，Turnip `graphics=ready`，`guest_presents=0`。这是新边界，仍不可玩；网络与 SSL 按用户要求保留桌面兼容范围，不扩展在线实现。

## 根因与修复

1. 原 `sceKernelLoadStartModule` 请求的真实路径是 `/app0/modules/libfmod.prx`，随后是 FMOD Studio；二者已经由依赖图加载。对照 desktop `Linker::LoadAndStartModule`，现在返回原句柄、不再初始化、不改 `pRes`。输入通过 guest mount 和受限路径解析，拒绝越界/逃逸路径；缺失文件返回 ENOENT。实际存在而尚未加载的模块仍具名 Unsupported，动态 TLS/ELF 加载与卸载没有被冒充为完成。`sceKernelDlsym` 仅返回指定模块内的 guest 导出地址，输出经过检查，不查 native HLE 地址。MntPoints 的 host path 解析改用 mount 快照，避免使用解锁后可能失效的容器指针。
2. 放行模块复用后，TMNT 在 FMOD 中读 `0x28` 崩溃。host LLDB 在 FEX signal handler 断住，从原始 ucontext 与匹配 FEX JIT header/tail 还原到 `libfmod.prx+0xbd09b`，`testb $1, 0x28(%rdi)`，RDI=0；FMOD 全局对象的内存池成员为零。**三个真实 ELF（libc、FMOD、FMOD Studio）都有值为零的 DT_INIT 标签。共享 `Module::Start` 的 `if (!init_virtual_addr) return 0` 错把合法零偏移当成缺失标签，跳过了构造器。** 现单独记录标签存在性，零偏移调用 `module base + 0`，缺失标签才跳过；ModuleInfoEx 与运行日志同步修正。这也更正此前“六模块 DT_INIT 完成”的过度结论：旧日志在跳过后也会打印完成，不能作为初始化证据。没有修改 FEX 翻译器。
3. 真正运行 libc 初始化后暴露 `_sceKernelSetThreadDtors`。现按 desktop pthread_exit 顺序，保存 runtime 私有的 guest 回调地址，正常线程返回/显式 pthread_exit 时，保留该 owner 的 TLS，通过 FEX 调用 libc hook，再做最多四轮 key destructor。无 VM/registry 锁跨 guest 回调；非法 callback 被拒绝，fault/cancel 沿原路径传播，避免重复 Finish 重跑析构；析构中递归 pthread_exit 明确拒绝。
4. 同一初始化函数还注册 Count/Report hook。实际 libc 指令传入 64 位函数地址，旧 ARM64 参考的 s32 参数声明不正确。现在保留完整地址并验证执行权限；与 desktop 一样没有额外 reporting consumer，也不宣称动态卸载引用计数已完成。禁用 ASan 时 New/Malloc replacement 查询返回 null，绝不把错误码当函数指针。没有开放通用“所有缺失导入返回零”的路径。

## 定向验证与证据

AYN Thor / API33 / ARM64 / 4KiB / 普通 APK uid10157。FEX `385a0cc4`、Foundation `5388ef45` 均未改变。没有完整回归或 Swan 验收。原始结果均在 [本轮证据目录](2026-09-14-module-init/)，[最后产物身份](2026-09-14-module-init/latest-build.json)保留构建时 `61805270+dirty` 及 SHA/Build ID。

| 阶段 | 结果 |
|---|---|
| apk-eleventh | 诊断确认真实 FMOD 路径；原 unsupported 行为保留 |
| apk-twelfth | 原始模块查找用例3轮通过；TMNT进程真实 SIGSEGV，按 FAIL 归档 |
| LLDB | 捕获 session `544783eacbd24dd59db0e06ff9b9823d`、epoch1、PID23397/TID23844；[原始字段与 RIP 推导](2026-09-14-module-init/module-null-fault-analysis.json) |
| apk-thirteenth | DT_INIT 零/非零/缺失各3轮，共9轮；重复加载不重跑构造器、pRes不改、Dlsym调用、坏路径/指针都通过。TMNT暴露新的 libc ctor import，旧 graphics-ready 断言正确失败，未降标记PASS |
| apk-fourteenth | 8-import 析构用例4轮通过；TMNT到 Count hook，旧渲染断言失败仍保留 |
| apk-fifteenth | 扩展12-import析构用例4轮：3正常，1非法地址Faulted，随后恢复；每正常轮验证两种线程退出、TLS与析构先后。TMNT3轮同PID27077到AJM初始化，无原FMOD空地址崩溃 |

LLDB 初始 NDK liblldb 21/19 均在主机 SymbolLocatorDebugSymbols 崩溃，不是游戏崩溃证据。可用组合为 CodeLLDB1.12.0 自带 LLDB21.1.7 + NDK21 arm64 server，同时提供 host 与 FEX JNI 的精确符号。实际捕获没有调用 inferior helper；CPUState.rip是陈旧的 `0x100021920`，未当作故障指令。调试中目标退出导致工具 cleanup 状态保留 incomplete；已核验目标和本次专属 server 身份，单独清理遗留 server23725/目录，host adapter也已退出。[清理实证](2026-09-14-module-init/debugger-owned-cleanup.json)。不提交原始游戏文件、反汇编文件或内存转储。

## 当前边界

接下来是生产 AJM 音频解码路径。桌面现有 AjmInstance/FFmpeg/AT9 可复用，但 static context 表、包含客体地址的 batch 描述和异步输出寿命不能直接照搬。网络/DNS/socket/HTTP/TLS扩展不属于当前阶段。已有图形环境与更远的初始化不等于首帧、输入可玩或十分钟验收。
