# TMNT 堆统计冗余调用优化

仅适用于 CUSA50828 01.08、eboot.bin SHA256 `6122da7190de6b08d921b2c42c3ca9ed11dc4d11524f1139ff67aeceea5b204d`，x86-64 SysV / FEX。不是通用 malloc/free 替换，也不清空 FMOD flush。

已复核原始 `TMNT_HeapAllocateAndDiscardStats`（+0xc6310）和 `TMNT_HeapFreeAndDiscardStats`（+0xc6380）：先通过 PLT 调用真实 guest mspace malloc/free，再用栈上40字节、size/version `0x00010028` 调用 `sceLibcMspaceMallocStatsFast`。malloc 的 RAX 保存到 RBX 并恢复；统计输出不再读取。free 调用者不消费统计状态。栈保护、实际堆操作、所有 malloc/free 输入保持原机器码。

`heap_stats.cpp` 只拦截该 eboot 的 stats PLT（+0x15c02d0，NID k04jLXu3+Ic，RDI heap、RSI output、EAX status）。返回 PC 精确为 +0xc6362 或 +0xc63cd，heap 等于原 guest mspace slot（+0x1e16ac8），且 stack result 的 size/version 正确，才省去查询。其他调用点、heap、参数形态全部转至常驻原始 trampoline；通用 stats API 保留输出/错误语义。装载 base 从已校验的原始 data binding 推导，禁止使用绝对地址或给 RX 数据伪造 R-only binding。

正常有效堆上的 stats fast 只读取聚合量并额外执行一对 heap mutex lock/unlock。坏 size/version 分支有错误诊断副作用，所以不全局替换该 API。这个优化的有效域是游戏正常的、已完成真实分配/释放的 mspace；不能拿它恢复损坏的堆。

调用计数位于 guest 自有、按 cache line 分开的原子变量，只在首次及每4096次通过 custom SDK 报数；其余统计跳过路径全部留在 FEX。计数不是精确实时总量，末尾最多有4095次未上报。对照包仅将 SkipDiscardedStats 设为 false，保留同样的 guest 计数/采样开销。

源码和证据：`build/loading-waits-20260917/tmnt-memory.asm`、`libc-stat-fast.asm`、`imports.json`；原分析 ELF `build/rooftop-profile-20260916/tmnt-analysis-etdyn.elf` SHA256 `504fc22628a2a7c563e593aab298b9273e2a80eaf8814a095ffb6f464152da99`。另一个 font-check ELF 的文件 SHA 不同，不能混用；本次生成器拒绝了它。

`tests/guest_cpu/patch/make_heap_fixture.py` 从该 exact-SHA 本地 ELF 提取原代码（不把游戏 bytes 加进仓库）。`guest_patch_tests <fixture> --heap-only` 在真实 FEX 内执行未改写的两个完整包装函数，只向 fixture GOT 填入效果记录器。差分覆盖 size0/1/16/4096/大于32bit/UINT64_MAX、失败分配、null free，以及通用 stats 的正常输出、坏 heap、空输出、坏版本。检查 malloc/free 的次数/順序、参数位宽、返回指针、外部堆字节一致；明确允许的差异只有这两个调用点的未使用 stats。

首轮错误的 RX-as-data binding 已被准入拒绝，失败日志保留；改为从真实 RW 数据 binding 推导地址后，Android/FEX **85 checks / 0 failures**。这是带模拟 allocator/provider 的包装函数差分，不等于真实堆、线程竞争或游戏性能验收。生产场景及 B/A/B 结果另见 docs/validation/android-native-host 的堆优化报告。

`heap_stats.recipe.json` 是独立验证包；`frame_heap.recipe.json` 在原来的 frame/rooftop recompiled 包上追加本优化，保留已启用的十个原 hook。
