# 白屏、视频结束与 guest debugger 实测（2026-09-15）

本轮仍未修复 TMNT 白屏／可玩性。已把本次稳定白屏与此前的 guest 0 FPS 区分开：真实显示器持续白色，但 guest flip 和 host present 继续约 50–60/s。未连接 guest debugger 的独立会话也会白屏，不能归因于 debugger 停顿。以下证据不覆盖此前按 Circle/PLAY 后的原始 0 FPS 现场。

实际环境：AYN Thor `9c2841a4`，API 33、4 KiB、普通 APK、私有 Turnip；没有操作 Swan。Host/JNI 是 RelWithDebInfo，FEXCore Release，APK 为 playstoreDebug。主仓 `4da582b7` 加保留的未提交改动；本轮没有修改 FEX 子仓。最终 APK、库 Build ID、源码哈希与原始证据见 [manifest](guest-white-media-20260915/manifest.json)。不能用 HEAD 单独标识这次实现。

## 一、视频／音频结束证据

新增低频 AvPlayer 生命周期日志：player handle、是否注册 guest event callback、IsActive 状态变化、成功交付的视频／音频帧数、Close 进入／完成。计数在已有 API 锁内更新，不引入每帧日志、额外 PCM 拷贝或跨等待 pin。

本次两段播放均使用 `event_callback=0`，由已有 desktop AutoPlay 路径处理内部事件：

| player | 成功交付视频 | 成功交付音频 | IsActive 最终值 | CurrentTime |
| --- | ---: | ---: | --- | ---: |
| 1 | 243 | 349 | false | 8104 ms |
| 2 | 180 | 0 | false | 6000 ms |

第一段实际 Close 完成，随后打开第二段；第二段 decoder EOF 后，视频 owner 也收到了 false。Oboe source 数从 2 降至 1，剩余 source 的 callback/consumed/presented 继续增加，采样 xrun/error=0。这不证明 FMOD Studio 的全部命令队列完成，但不支持“设备音频停止消费”或“AvPlayer 一直不返回结束”的解释。`StateStop` 的 desktop 日志也不应被误读成执行了 guest event callback——本游戏没有注册该回调。

证据：[生命周期及 Oboe 摘录](guest-white-media-20260915/media-lifecycle-excerpt.txt)。完整本地日志保留在 `build/guest-zero-fps-20260915/breakpoint-runtime-tail.txt`；摘录的行号相对该文件。

### 真正沿 guest 完成路径停住

修复软件断点对 WaitingHle 的误拒绝后，在实际普通 APK 上命中：

- PID `32710`，start ticks `24226362`，Session generation `1`，UUID `243f84c1c10fca43e8b7e63bcb8ef526`。
- `eboot.bin` base `0x400000`；实际 getter RIP `0x192f860`，指令读取 `[rdi+0x418]` 并返回是否非零。
- RDI `0x2655e4920`，`[rdi+0x418]=1`；该对象 player handle 为 `2`。
- 再在紧随调用后的 `0x1927be4` 停住，确认 RAX=`1`，不是仅凭内存推断返回值。
- immediate return PC 从停止时的 RSP 顶部读取为 `0x1927be4`；RBP walker 本身跳过了这个无独立 frame 的 getter，不能把下一条 RBP frame 当直接调用者。
- caller RBX=`0x2655e4628`，`[rbx+0x188]=0xbe`，其 bit `0x2` 已置位。静态指令显示该 bit 防止重复调用后面的 `0x190c610` 完成事件分发路径。主线程反复回到这里，支持“完成分发已调用返回”，但还没有证明所有订阅者消费了事件。

原始 MCP 记录：[getter](guest-white-media-20260915/video-done-breakpoint-fixed.json)、[调用者和对象](guest-white-media-20260915/video-done-caller.json)、[真实返回值与 caller 标志](guest-white-media-20260915/video-done-dispatch.json)。每次都移除断点、恢复并释放 session。这里的 CPU thread generation `2` 不是 Android Session generation `2`，不可混用。

这些地址仅对本次 `eboot.bin` 适用；原文件 SHA-256 `6122da7190de6b08d921b2c42c3ca9ed11dc4d11524f1139ff67aeceea5b204d`。分析用 ELF 是本地 `build/graphics-toolkit-review/renderdoc/tmnt-font-check.elf`；静态反汇编地址为运行地址减 `0x400000`，没有改写游戏代码逻辑或完成标志。

## 二、RenderDoc：白色已经存在于采样纹理

实际 RDC 为 800,518,817 bytes，SHA-256 `3041cda2b857bb4437b3aaa6260fd25c504f8633cae4475c2f7b3a16c5b939dc`。使用配对 RenderDoc 与同一台 Android/Turnip 回放，未在 macOS 回放 Adreno capture，未切系统驱动；配对 renderdoccmd/qrenderdoc 均存在。它来自较早 PID `19992`、UUID `eaae93047ebad1936e6da3f4ac7d78b2`，不是上述断点会话；勿将两者当一次同时采样。

捕获控制最初报 deadlineExceeded，之后文件完成并成功回放。因此这是“文件有效／回放成功”，不是 capture coordinator 超时行为已通过。见 [捕获 sidecar](guest-white-media-20260915/white.rdc.json)。大 RDC 留在 `build/guest-zero-fps-20260915/white.rdc`，未复制进 docs。

明确 event/resource selector 的 `review_texture` 图像已实际检查：

- 游戏目标 resource `601`：event 31、57、64 后黑色，event **71** 后白色，78 后仍白色；event 88 采样它进行后续 blit。
- event **71** pixel shader `1652` / pipeline `1656`：采样两张纹理并乘 vertex color 与 mask alpha。
- binding 0 resource **3794**：1920×1080 RGBA8，名称包含 guest VA `0x276890000`，在 event 71 采样前已经全白，导出 RGB 均值约 `0.9999866`。
- binding 1 resource **1658**：4×4 白色不透明 mask。
- resource_usage 对 3794 在本帧只记录 event 71 的 PS read，没有本帧 writer。

因此 event 71 是本帧目标变白的位置，但**不是已证明的错误生产者**：白色源纹理来自捕获之前。下一次有价值的捕获应覆盖这张源纹理最后一次产生／更新，而非重复抓同一个稳定白帧。shader 78 的无纹理颜色输出不能拿来解释 shader 71。

可复核 selectors、shader、bindings、usage、MCP image blocks 归档在 [rdoc71](guest-white-media-20260915/rdoc71)。`finalOutputEquivalence=unverified`：没有把 PNG 域相同当成完整外部显示等价；物理显示白色另有 [实际截图](guest-white-media-20260915/media-state-before-reinstall.png)。本轮没有新增 GPU Reshape 全量验证。

## 三、就地修复与验证

1. **挂接 debugger 的高频 HLE 开销。** 原实现首次 Pause 后，每次 HLE 都抓 native unwind；实际挂接时帧率一度约 5 FPS，detach 后恢复。现在每个 physical Run / attachment 只抓一次 entry template，明确标记 `run-hle-entry-template`；等待及 nested callback 仍保存自己的具体边界。guest continuation 每次保持新鲜，未挂接路径不增加 native unwind。实际挂接并恢复后仍约 49 FPS；不是白屏修复。
2. **WaitingHle 软件断点准入。** 原 `active_runs!=0 && !debug_parked` 一概拒绝后台 HLE，真实游戏返回 E16。现在要求每个 owner `debug_pause && !running`，然后仍由 `DebugPublish` 的 quiescence 检查执行租约与所有 pin，并经原缓存失效路径发布。新建／回调／返回路径在进 JIT 前遵守暂停；单独 Continue 的 HLE owner 仍拒绝 patch。只修改调试准入，不修改未挂接调度。定向测试覆盖 HLE 下插入/命中/恢复、新 owner 先暂停、live pin 拒绝、选择性恢复拒绝；最终真机 **85/0**，随后实际 TMNT 软件断点两处成功。
3. **AvPlayer 测试握手。** 新日志时序暴露旧 manual Start 测试只等 stream count，而 FindStreams 在 StateReady 前已发布 stream count。保留原 `1130/7 FAIL`，改为等待实际 Ready callback 后 Start，最终 **1207/0**。未把 production Start 改成伪成功。
4. **`--open_last_game`。** Activity extra、热启动 onNewIntent 和 dumpsys 请求已接到真实 Session 导航，历史在 Service 接受启动后写入，IO 查询失败可报告；已有活动 Session 不重复启动。本轮普通 APK 冷启动、热重复请求、dumpsys 重复请求已验证，无历史／删除内容分支未做设备验收。

证据：[85/0](guest-white-media-20260915/debug-hle-breakpoint-tests.txt)、[1207/0](guest-white-media-20260915/media-ready-tests.txt)、[原失败](guest-white-media-20260915/media-state-tests.txt)、[原 E16](guest-white-media-20260915/video-done-probe.json)。APK 构建成功，`git diff --check` 通过；没有全量回归。

快速启动：

```sh
scripts/android/open-last-game 9c2841a4 --open_last_game
```

## 四、当前交接点

本轮更支持继续检查**完成事件消费后的转场与末帧纹理更新**，不支持继续把 AvPlayer false／guest 完成标志当作缺失。FMOD 某个命令的完成等待仍不能仅凭 Oboe 流量排除。保留旧 post-Circle/PLAY 0 FPS 为另一个待复现现场，不拿此处 50–60 FPS 代替它。

合理的下一次连续调查：在第二段结束前覆盖 RGBA 源图像更新及其 guest NV12 输入；同时记录完成事件的实际消费者与转场状态，确定白色是否是正常末帧被永久保留，或上传／转换错误。得到 producer 证据之前，不改 IsActive、强制 Close、跳过视频、写完成标志或任意放宽音频 drain。

最终正常 UI Stop，Session `Stopped/user_stop`；两轮 Stop 记录已归档。所有本轮 guest sessions 释放、断点移除、RenderDoc capture/remote controller 关闭、三个 PROF sessions 关闭、scrcpy owned session 停止；guest debug/RenderDoc loader 属性清空，四条原有 adb forwards 保留。APK 留在设备，应用回到普通界面。尚无白屏修复／输入推进／可玩性验收。
