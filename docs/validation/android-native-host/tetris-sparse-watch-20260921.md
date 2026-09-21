# AYN / Tetris 稀疏内存 GPU 跟踪修复（2026-09-21）

**Tetris 已经显示方向正确、左右布局正常的 SBS 提示页。** 用户提供的截图与本轮旧 V4 / Turnip PID5763 的[实际截图](tetris-sparse-watch-20260921/turnip-baseline.png)一致，不能再概括为“显示链未接通”。剩余现象是确认后的黑屏及独立启动崩溃。本轮修复了其中一个已证实的 GPU 内存跟踪异常，安装并完成定向验证；**未进入菜单或达到可玩，普通重启仍复现 PC0**。

分支 `feature/malos/beat_saber_fix`，HEAD `63b6d560dd43af031ec7a5de64ae6204beea5de2` 加已有工作区修改。本轮没有 commit/push，没有改 HMD UV、位姿、驱动或 Guest 游戏逻辑。源码、构建与证据 SHA 见 [manifest](tetris-sparse-watch-20260921/manifest.json)。

## 故障与修复

旧 V4 PID7478、PID11015 和诊断版 PID13830 都在运行中出现 `GPU watch requires mapped non-executable memory` 后退出。诊断版给出范围 `0x2017951000 + 0x10000`，其中 `0x2017960000` 是尚未提交的页（state=0）。先 track=true/read=false，异常回滚再 track=false，均覆盖同一个洞。生产调用来自 BufferCache 的 `SnapshotForUpload`，不是 TextureCache 的 `TrackImage`。PID11015 的 pool decommit/recommit 也显示跨界页尚未重新提交；见[映射摘录](tetris-sparse-watch-20260921/fatal2-map-excerpts.txt)与[诊断失败](tetris-sparse-watch-20260921/diagnostic-failure-host.txt)。

BufferCache 原本通过 `CopySparseMemory` 将未映射部分复制为零，但 PageManager 把完整区间传给 Android 页保护后端，后端按合同拒绝映射洞。这两条路径不一致，回滚也再次触发拒绝。不能对整个保留区直接 mprotect，否则会让未提交内存可访问。

- `MemoryManager::ProtectGpu` 在 VM 共享锁内按 VMA 分段，只对有 backing 的片段执行 GPU 页保护。洞保持不可访问；低层仍拒绝非法或可执行映射。
- PageManager 保留逻辑 watcher 计数，物理保护经过分段入口。日志补 track/read，区分上传、读回与回滚。
- 新 backing 发布时，BufferCache 在同一个 region 锁内清除旧 GPU 所有权/读保护并标记 CPU dirty；TextureCache 也失效对应范围。缓存的洞内零数据会被新提交内容替换，旧 GPU 内容不会回写到新 backing。
- `SetRasterizer` 初始回放先取 VM 快照、释放共享锁，再调用 GPU hooks，避免新增失效路径递归获取 VM 锁。现有 Map/Commit 也在释放 VM 写锁后调用 hooks。
- 后端异常包含范围、页状态和权限；TextureCache 异常包含尺寸/布局，便于区分后续故障。

没有改变 CopySparseMemory 的零填充约定，没有将所有未映射 Guest 访问转换为成功，没有新增 GPU idle 等待。

## 验证与安装

| 验证 | 检查 / 失败 | 范围 |
|---|---:|---|
| 旧生产入口反例 | 23 / 1（预期） | 后端收到对洞的保护调用；夹具仍保持洞不可访问 |
| 最终 Qualcomm GPU | 29 / 0 | 实际 PageManager、BufferCache、TextureCache 和 GPU 读回 |
| 最终 Turnip GPU | 29 / 0 | 同一生产路径与字节比对 |
| Android MemoryTracker | 53 / 0 | 新 backing 清理 GPU 状态、保留邻页、只重传变更4096字节 |
| Android futex 既有回归 | 14 / 0 | region 锁基础行为 |
| Android VM/DirectMemory/Pool | 106 / 0 | 映射、提交及并发合同 |

GPU 用例从已映射段 +0x1000 读取0x10000，跨16 KiB洞；逐字节核对已映射内容0x47、洞为零，再提交洞写0x6c，检查同一缓存 buffer 得到新内容。既有 texture extent 往返、血源别名和大回读也通过。首次 Turnip 命令误把文件传为目录，未启动测试，修正后及最终版均通过；不是 GPU 测试失败。Native/APK 构建、git diff --check 通过。未做三款完整回归或性能 A/B。

最终安装身份（完整设备 base.apk SHA 已核对）：

| 产物 | SHA-256 |
|---|---|
| APK | `905b84f8052a690e3e81dc88a0c153c660fe1a9ebd6fee629128c741c96f7bd0` |
| Host | `f4870e3cab078b7cf23d3ba7b9d8a9a0790e4efc2e88d0748704eac44979f089` |
| JNI/FEX session | `ce60440a4673f0f4b20d0934a93f7c76bde5c03115d2cd0ca32ecb868ab99f3d` |

设备仍为 AYN Thor `9c2841a4`，驱动 Mesa26.0.0-devel / 5ac41be677 / Adreno740，SHA `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`。本轮没有更新 Turnip。

## 游戏结果和剩余问题

| 会话 | 观察 | 结束 |
|---|---|---|
| 旧 V4 PID5763/gen1，run `42c5ad71757431189a0f9343d00973d7` | 正向中文警告与 X/OK，左右布局正常；持续 Cross 400ms 后提示消失、黑屏 | 约181秒后 Guest 异常展开中 VA8 |
| 最终修复 PID18733/gen1，run `9ca2a99bc3bf35dbd9f6e3b4d2841d82` | 再次遇到带洞 GPU buffer 和新映射失效，未抛出旧保护异常；仍黑屏。该轮未单独验收到最终 APK 提示页 | 2455 presents；正常 UIStop → Stopped/user_stop/guest return0 |
| 最终修复普通重启 PID20588/Guest-50，无调试器、无图层 | 约30秒独立 PC0，未触发上面已修复的 GPU watch 异常。没有成功保存本轮 run UUID，不能借用 PID18733 身份 | SIGSEGV/原生进程退出，不是正常 Stop |

PID18733 经 Guest RSP 设置原错误格式化 main+0x11395a0、栈展开 main+0x103c180 的断点，均未命中。一次暂停位于 renderer task 等待，返回 main+0x180c277；之后 presents 继续增长，不能按单次样本判死锁。停止前移除断点、恢复运行并释放 RSP 所有权。RSP 握手未提供完整 PID/启动ticks/Build-ID，使用外部会话记录、基址及现场字节核对；不称完整身份认证。

旧 VA8 的 FEX fault 与归一化 ELF 对应 main+0x103c232/235：栈展开取下一帧指针后读其 +8，此时指针为零。上游 main+0x11395a0 是 fatal 格式化/记录，因此 VA8 是错误报告过程中的二次故障，原始错误仍需提前捕获，没有跳过它来假装成功。

普通 PC0 的完整 tombstone 由 AYN Root/PServerBinder 只读提取，PID、时间与 logcat 相符。LR 附近实际 AArch64 指令表明 guest target（x20）为0，L1 lookup 匹配空条目，最后 blr x1 且 x1=0。编译块元数据为运行地址0x1bda1b3，guest return0x1bda1b9，即 main+0x17da1b3/1b9；现场 Guest 指令和 ELF 一致，为 mov rax,[rdi]; call [rax+0x10]。见[原生指令](tetris-sparse-watch-20260921/ordinary-null-call.txt)、[Guest 指令](tetris-sparse-watch-20260921/guest-null-call.asm)、[tombstone 摘录](tetris-sparse-watch-20260921/ordinary-tombstone-excerpt.txt)。这把“FEXMemJIT/PC0”收敛到实际 Guest 间接调用，**对象为什么失效仍未知**，不是 FEX shadow-return-stack 或 Turnip 根因结论。

下一步优先追踪该对象从全局 main+0x6a8b220 发布、调用到清除/析构的顺序，以及 fatal 首次进入的文本；再关联同一轮按 X 后的 base/overlay 和渲染状态。新增 presents 不等于菜单验收。

## 抓帧边界

旧 V4 system/Qualcomm PID27540（run `b891afd615a3dc31981ea556720a5502`）取得早期 flip2→3 的 system-first.rdc，673,788,331字节，SHA `95bc8fbbee7475d054b39b49b86b22346eba8830d00b85169d4afa94ef3a6d39`。配对 Android remote replay 实查 host SBS draw1726：输出317读取 base3157、overlay3169/3175，每眼 UV=(3/7,6/7)、右眼bias=3/7、flipY=0；此刻 base RGB 近乎全黑、overlay3169全零，host输出与输入一致。但它是极早加载帧，可能是正常fade，**不证明按 X 后黑屏的来源**，不能替代 Turnip 同场景结果。Guest marker 语义不可用，没有用平面 marker 推定 guest pass/眼图身份。

诊断扰动反例也保留：Turnip PID25507 带图层在 KGSL bo 释放路径崩溃；system PID27540 后续 BufferQueue 异常大分配退出；system PID2569 较晚抓帧遭 LMK，残留 RDC 不完整、未 Ready。这些不混入普通 PC0 或 GPU watch 根因。大 RDC、离线DB与详细 replay 保留在 build/validation/tetris-render-20260921，manifest 保存路径与 SHA，不提交大二进制。

## 导入与清理

最终 PID18733 全量2603行：guest_export1146、runtime_bound1082、refused358、not_relocated17，290唯一拒绝符号。[完整绑定](tetris-sparse-watch-20260921/bindings.tsv)及逐库/逐符号/importer [审计](tetris-sparse-watch-20260921/family-audit.json)已归档。本轮没有改变准入，NP、媒体、SocialScreen 等剩余族不因内存修复而成为已兼容。

最终 Library PID25666，session:none、TracerPid0、无游戏服务。所有本轮 RSP 已清理，Guest debug port/wait=0，RenderDoc capture关闭且无replacement/mutation，本轮两个RenderDoc forward移除，最终forward为空。临时Root bridge自动移除；无自有scrcpy，replay bridge/collector已结束。global.json与host/config.json相对开始逐字节相同，保留Turnip/Render0.5/TextureHigh/SBS/gyro，见[清理记录](tetris-sparse-watch-20260921/final-cleanup.json)。停止后旧UI残留仍未修复；无可玩、稳定性、全回归或FPS/内存收益声明。
