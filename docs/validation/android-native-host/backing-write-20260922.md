# GPU 回写跨物理段修复（2026-09-22，本地未提交）

检查 TMNT 堆错误相关的共享内存写回路径时，发现 `MemoryManager::TryWriteBacking` 在每段 memcpy 后只递减剩余长度，没有推进源指针。跨相邻 Direct VMA 或单个 Pooled VMA 的多个物理段时，后段会重复复制源缓冲区开头。另一个错误是有效前缀后遇到无 backing 的区间仍返回成功，并已经修改前缀。

现在在持有 VM 共享锁期间先验证完整目标、收集每段源偏移和长度，再按这些偏移复制。遇到未映射/无物理 backing 的后段时整批拒绝，不留下部分写回；保留 GPU 回写绕过 CPU 页保护的既有用途，不新增 CPU 写权限要求。不改变 GPU 同步、资源退休或 deferred 回写的 generation 合同。

## 同一程序正反对照

测试使用生产 MemoryManager、实际 GuestAddressSpace 和可检查字节的测试 backing，验证两块不连续 Direct 物理区的全范围/非对齐跨界/短跨界复制、有效前缀后的洞，以及一个 Pooled VMA 内两块不连续物理区；对整个 backing 比较，包含未写入的 guard 区域。

AYN 9c2841a4 / API33 上同一个 Android 测试 executable：

- 旧 host `33825d4e`：18 检查 / **6 失败**，对应三种 Direct 字节错误、洞错误返回及前缀被修改、Pooled 字节错误。
- 新 host `a1d24cae`：18 / **0 失败**。
- 完整内存并发/映射测试：186 / **0 失败**。

[测试与精确二进制 SHA](backing-write-20260922/tests.json)，[旧版失败](backing-write-20260922/old-backing.log)，[新版完整结果](backing-write-20260922/new-full.log)。该夹具验证 CPU 侧实际 backing 写入，不冒充游戏 GPU 首个错误 writer 定位。

## 部署和游戏边界

APK `4758939e` / host `a1d24cae` / JNI `232f0c06` 已安装 AYN，整个设备 APK SHA 与本地一致，包内 host 也已核对，见[安装身份](backing-write-20260922/installed.json)。此前 TMNT PID16363/gen1 巢穴正常 UIStop/user_stop/guest return0 后才安装；保留用户要求的隐藏触控设置和只读 scrcpy 观察窗。

**尚未证明这处错误就是 TMNT 战斗后 Garlic 堆错误的原因。** OCR 任务交回时未进入战斗、没有复现新 GuestFault；其最终包轴操作成功与首次生命升级、未完成的自动路线归该任务证据，不作堆修复验收。MHW 同包仍在自动存档提示后的流程出现 GPU DeviceLost，不能将本修复或 subgroup 定向通过写成游戏已修。

MHW PID31129/gen1/run fb7b44375b9ab09a790ef0ac1ab006b1 这轮包含用户手动操作。用户确认最后是自动存档提示页，约一分钟后闪退；本任务未发送 DebugBus 按键。19:25:51 BackendFailed/exit33；失败提交 scheduler2/tick5499/cached_retired5498/host_serial14838，内核同进程 ctx17/ts14838 GPU timeout。仅为同次提交的匹配，不代表已找到最初错误命令。

首个读取的 KGSL dump 是旧 Cemu Main/PID11848、seconds1789343100 的快照，明确排除，未用于 MHW 根因归属。读取完后 snapshot timestamp 归零；继续同包重现获取匹配本次进程的新现场。完整日志、快照、配置和存档只读备份保留本地 `build/validation/backing-write-20260922/`，不入 Git。存档由游戏正常管理，未格式化或外部改写；本轮无 commit/push。
