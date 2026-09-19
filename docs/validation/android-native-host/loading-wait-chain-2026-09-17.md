# TMNT 启动 / loading：FMOD、FIOS 与 guest 堆锁等待链

本轮在现有 Litep 非帧阶段分析上增加 **默认关闭** 的同步诊断，并完成普通 APK 实测。
没有提前返回 `flushCommands`，没有修改音频消费、mutex/condition 的完成语义，也没有性能收益结论。

## 1. 可以优化等待，但不能把 host 收到 PCM 当作 flush 完成

实际调用链已经从 guest 栈和精确导入对应起来：

```text
TMNT_LoadBankAndTrackHandle (eboot+0x42b280)
  → FMOD::Studio::System::loadBankFile (libfmodstudio+0x96a30)
    → blocking flushCommands path (+0x953e0)
      → pending-work helper (+0xebb30)
        → sceKernelUsleep(1000 microseconds)
```

`loadBankFile` 的导入 NID 为 `L51FfEMZa4o`，PLT 为 eboot+0x15c08a0；
提供者入口引用 `System::loadBankFile` 字符串。主程序包装函数保存 bank handle，
维护引用计数，并查询 `bus:/Music`。因此这里不只是向 Android 输出 PCM。

FMOD 的公开契约也要求 `flushCommands` 等待待执行命令和未完成的非阻塞 bank 加载，
参见 [FMOD Studio System API](https://www.fmod.com/docs/2.03/api/studio-api-system.html#studio_system_flushcommands)。
这是 2.03 文档的契约佐证；当前游戏二进制为 2.02.26，实际调用定位来自二进制和本轮记录。

host 接收音频 buffer 只证明输出桥接进度，不能证明 guest FMOD 的 bank、对象和命令状态已发布。
即使以后把命令执行迁到 host，也必须用 **执行完成及结果可见的序号** 确认，而非入队序号。
当前直接返回成功可能使游戏拿到尚未就绪的 bank/handle。

三个主要调用者分组共记录 24,766 次 1 ms sleep，累计约 26.751 s，平均约 1.080 ms。
这是整个 120 s 记录内的三个主要分组，不是单次 flush 的耗时。
缩短 sleep 不能直接消除这 26.751 s：pending work 仍需完成，盲目轮询还可能争抢工作线程 CPU。
完成通知可以降低跨界和轮询成本，但需继续确认 pending 对象的真正发布者及取消/代际协议。
目前没有将候选递减点认定为同一个 live completion 对象。

## 2. 条件等待：大部分时间发生在通知之前

诊断记录 PID14213 / Session generation1 / UUID `9355c1a3e09c96e8f61191aed777c34b`。
主 guest owner 对应 host TID14271、FEX thread1/generation2、owner handle68721672128。
以下同一 condition 对象的阶段通过 `(condition, owner)` 配对；实际选中的通知目标由 domain 记录。

| 主线程 condition 阶段 | 启动到首次真正 present | loading 观察窗口 |
|---|---:|---:|
| 完整等待次数 | 11,775 | 1,017 |
| 总 elapsed | 4.231 s | 9.759 s |
| 入队到被通知 | 3.611 s | 9.676 s |
| 通知到恢复并取得 domain guard | 0.243 s | 0.027 s |
| 恢复后重新取得 guest mutex | 0.377 s | 0.056 s |

启动窗口为 `407574935775090..407596004867694 ns`，21.069 s。
loading 窗口为 `407638259003504..407679828288019 ns`，41.569 s，
由截图/status 观察边界界定，包含 loading 停顿和恢复，不冒充精确的业务起止点。
本轮启用了额外诊断，未做其开销 A/B；不能与上一轮 18.744 s 首帧直接比较性能。

主线程路径是 **TMNT_FiosReadSync → sceFiosFHReadSync → FIOS condition wait**。
精确导入 NID `Bn2ZF4ZjeuQ` 位于 eboot+0x15c1570，调用包装函数为 +0x15551b0。
loading 中 TID14350 发出的 341 次实际选中通知对应约 9.562 s 主线程等待区间；
其余主要来自游戏文件工作线程 TID14356。此窗口所有主线程完整等待均被通知，
没有将超时误当完成，也没有配对错误。

**边界：** FIOS 共用 condition/broadcast，通知选中 owner 不等于完成它所需的同一个 I/O 请求。
尚无 request/job ID 把每一次读盘、解压和 completion 串成严格因果链。
跨线程等待区间重叠，不能相加成 CPU 时间或保证可节约时间。

## 3. 进一步定位到 FIOS 解压任务函数

TID14353 的等待栈进入 FIOS+0x12e90 任务函数：从队列取出 job，准备输入/输出字段，
调用 +0x40e50 初始化、+0x41010 解压。静态可核对的特征包括：

- +0x4990c 为版本字符串 `1.2.8`；初始化函数检查版本首字节和 `z_stream` 大小 0x70。
- 工作结构具有输入指针/长度、输出指针/长度、分配/释放回调，随后进入 inflate 状态处理。
- job 队列、完成标志和回调位于同一个工作函数，仍由 guest 代码执行。

这确认了 FIOS 解压路径的位置，**没有量出 inflate 的 on-CPU 占比**。
TID14350 与 TID14353 的通知关联覆盖约 19.282 s，但后者也长期等待任务；
绝不能把该数字当解压耗时。应在这个已定位的 job 执行区间记录 job 身份、输入/输出大小和实际完成，
再决定是否值得使用经过格式/错误/回调语义验证的 host 解压替换。

## 4. 明确的 guest 优化候选：丢弃的堆统计

两个已确认的主程序包装函数：

| 语义名称 | eboot 偏移 | 当前行为 |
|---|---:|---|
| TMNT_HeapAllocateAndDiscardStats | 0xc6310 | mspace malloc，然后查询统计；只返回 malloc 指针 |
| TMNT_HeapFreeAndDiscardStats | 0xc6380 | mspace free，然后查询统计；统计结果不再使用 |

统计 API 精确为 `sceLibcMspaceMallocStatsFast`：NID `k04jLXu3+Ic`，
PLT+0x15c02d0，libc export+0x31470。两个调用点均在栈上设置有效 header `0x10028`，
统计输出及返回值随后丢弃。提供者内部为该查询再取得并释放同一把 heap mutex。
这在已有 allocator 锁调用之外又增加一次 guest/host lock+unlock。

主线程固定步长定位采样中，stats 路径各出现 173 个 lock/unlock 样本，
malloc 各94、free 各59；它们是 **位置证据，不能据此估算调用频率或百分比收益**。
统计 API 的无效参数分支还有诊断副作用，不能全局改成 no-op。

优先验证方向是仅替换这两个 **exact-SHA + entry-preimage** 的游戏包装函数，
保持真实分配/释放、错误行为和生命周期，去掉输出未被使用的查询。
本轮只完成定位和符号归档，没有安装此优化；仍需原始机器码差分测试及完整 loading A/B。

## 5. 本轮实现、验证与复现

`debug.shadps4.profile_sync=1` 必须在新 Session 构造前设置；关闭同样需要新 Session。
默认 false 时不读取 guest 调用栈，也不初始化定位采样表。

- FunctionAdapter 在选定同步 HLE 边界记录 checked RSP/RBP 链（最多6层）、thread/generation/invocation、参数与结果。
  mutex 使用每物理线程/operation 的独立步长4093采样；condition/sleep 全量记录。
- GuestMutexDomain 的可选 observer 记录 Enqueued/Notified/Resumed/Reacquired。
  默认只有空 observer 分支；不新增 pin、wait，也不修改原先的同步顺序。
- observer 在 domain guard 内调用，不得阻塞、重入或读取 guest；采集会产生额外开销，仅用于诊断。
- Android 独立 condition 测试：关闭67/0、开启92/0；host DSO 与 APK 构建通过。
  早期动态库查找失败不计为通过，无完整回归。
- 新增9个 SHA 固定的语义符号，index revision6；`guest_debug.symbols_prepare` dry-run
  精确 source/runtime identity 验证通过：5 shards、66 symbols。

真实采集120 s，421,307,287字节、50,836,295事件、零 skipped chunks，
无 unmatched scope end；55个边界未结束 scope、7个未结束 condition wait 保留为不完整。
SHA `db45839369b6fd31815d898b5199cf574d2622705e042e1ed578df0b9b932267`。

这个记录超过已安装通用 stage reducer 的50M事件上限；本轮使用归档的定向 reducer，
没有声称通用 MCP stage API 接受了它。它使用匹配 SDK decoder、checked LZ4 解压及
phase/group 上限。复现需原始 `build/loading-waits-20260917/sync.prof`、sidecar、
`diagnostic-run/frame-*.txt`，先执行 `extract-sync.py` 再运行 `summarize-sync.py`。
`sync.json` 为约195MiB中间文件，未提交文档目录。原始大文件与完整反汇编保留在 build 下。

study0 成功反编译主程序 +0x42b280；原始及规范化 FMOD PRX 的建库/索引尝试没有成功，
失败输出保留，其他关键提供者证据来自精确 ELF 导入/导出、字符串和 LLVM 反汇编，
不是虚构的成功反编译。调用者查询对未索引的内部地址返回空不代表不可达。

## 6. 交付与设备收尾

APK `7345216b…`，host DSO `0296f0d7…`，JNI `fa728c03…`；host/JNI RelWithDebInfo，
APK playstoreDebug。精确 source、输入 ELF、产物及记录 SHA 见
[manifest.json](loading-waits-20260917/manifest.json)，统计见
[sync-summary.json](loading-waits-20260917/sync-summary.json)。

诊断 generation1 到达屋顶并通过真实 UI Stop 收到 Cancelled/user_stop。
两个启动属性已清零，新 generation2 已重新到屋顶 MOVE，自动输入结束，TracerPid0，
file capture inactive、ring 保持开启、GPU timing/capture 未开启、系统 Qualcomm 驱动保持。
warmup 自动化两轮均为 TIMEOUT_UNVERIFIED；截图人工确认场景，不提升为移动/攻击/十分钟验收。
本轮没有 SDK/Foundation/FEX-child 改动、spec、commit/push，既有 dirty work 保留。
