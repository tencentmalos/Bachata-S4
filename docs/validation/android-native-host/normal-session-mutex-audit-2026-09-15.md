# 正常 Session 回溯与 mutex 唤醒 A/B

2026-09-15，`codex/android-fex-round2` / `4da582b7` 加既有本地改动。
用户先要求找回正常 Session 后的改动，再指定从 Host/FEX mutex 的角度做对照。
本轮保留现有生产实现；临时回退仅用于构建对照包，之后精确恢复原文件。

## 正常现场与后续改动

最近的确证正常画面是 **PID22870 / Session generation1 的 TMNT 主菜单，约58 FPS**。
见[原始审计](guest-debugger-zero-fps-audit-2026-09-15.md)；原图
`build/white-screen-20260915/initial-scrcpy.png` 已按原字节归档为
[normal-menu.png.gz](session-mutex-ab-20260915/normal-menu.png.gz)。
这里“正常”只指该次主菜单画面，不能扩展成 PLAY/游戏流程已正常。

- APK `e6f92f070434d97f2bf27082903eded8fb640dde78228355edf8550c145ad59c`。
- host Build ID `badb52cd40c31a3bc1f24f35742911f37687369e`，JNI `3c0867b6790a9c0be1d48367faff0ba9526452b1`。
- 对应 host 构建 `build/android-host-api33/runs/1789439505418348000`，当地时间10:31:45。
- **这一版已经使用 Oboe、按对象唤醒的 host mutex，以及 SMALLTSCSCALE=false。**

这段历史大多未提交，不能只用 `git log` 比较。按该构建的源码 SHA256 清单逐文件核对，
1,110个记录文件中，**1,099个相同、11个不同、没有缺失文件**。11个旧文件由当时的
HEAD与source.patch重建，并逐一验证回到清单中的原始SHA；清单、差异与旧文件见
[changes.json](session-mutex-ab-20260915/changes.json)及
[原文件与差异归档](session-mutex-ab-20260915/normal-changed-files-and-diffs.tar.gz)。

| 范围 | 正常主菜单版之后的变化 |
| --- | --- |
| GuestAudio / Oboe / Foundation audio | 审计的19个相关文件SHA全部相同，Oboe子仓未变 |
| 原生AvPlayer解码器 | 16个记录文件相同；GuestAvPlayer适配器新增生命周期日志和计数，未改变EOF/Close语义 |
| video_core / shader_recompiler | 分别121 / 117个记录文件全部相同 |
| GuestAddressSpace / host mutex / guest clock | 记录中的实现相同；mutex优化和时钟修复不是主菜单正常之后新增的 |
| guest CPU / FEX adapter | 新增RSP、Step/INT3、debug pause、混合边界、watchpoint，以及相关执行出口处理 |
| FEX子仓 | 原`385a0cc4`之后新增6文件watchpoint IR/pass/JIT变更；归档保存了实际patch |
| GuestRuntime / Session backend | 发布debug模块，按Session读取debug端口/等待属性 |
| Android快速启动 | 后续新增`--open_last_game`；其实施前白屏已经出现 |

1,110不是全仓文件数：该清单没有覆盖整个Android工程、全部子仓工作树或所有生成文件。
新增的`debug/`三个文件、`memory_watch_pass.h`和FEX子仓改动另列，不能误写成“只有11个文件改过”。
Foundation父仓gitlink也不等于当时实际工作树；本结论使用单独音频源码清单核验。

后续阶段的已验证APK依次是：基础debugger `b1e0c784` / host `2961e78f`，
混合栈与watchpoint `37bbf892` / host `7edf2aec`，媒体定位与快速启动 `445e1ec4` / host `2eec5bef`。
前两轮验收主要是合成guest，未给出TMNT画面正常的中间检查点。
另一方面，[9月14日的记录](startup-audio-profile-2026-09-14.md)已有按圈后白屏，
早于本次debugger开发；因此不能仅靠日期断言所有白屏都由debugger引入。

## “Host FEX mutex”实际包含两项不同工作

1. `tests/guest_cpu/compiled_guest/entry.cpp`的C/C++ guest原子快路径，只由
   `cmake/fex`的`compiled_guest_tests`目标引用。生产APK没有注册、替换或调用该payload。
   其host expected-value wait/wake属于独立机制原型，不能解释为已上线的Orbis mutex。
2. TMNT的pthread/Sce mutex导入仍经syscall veneer进入
   `GuestRuntime → GuestMutexDomain::Lock/Unlock/CondWait/CondNotify`。
   实际生效的同期改动是把一个全域`condition_variable_any`改为每mutex/cond自己的等待对象，
   并跳过无竞争时的wait注册，避免每次unlock广播唤醒不相关对象。

构建清单首次记录当前mutex SHA的时间为9月14日19:33；9月15日正常主菜单仍是同一个SHA。
详见[mutex构建历史](session-mutex-ab-20260915/mutex-build-history.json)、
[实际改动](session-mutex-ab-20260915/mutex-only.patch)。

## 对照方法与结果

设备仅用AYN Thor `9c2841a4` / API33 / ARM64 / 4KiB，普通APK，固定私人Turnip。
未挂接guest/host debugger，debug端口、wait与RenderDoc loader属性为空。
没有清理游戏内容、存档或shader cache，没有切换音频后端。

- **A**：原先安装的当前实现，APK `445e1ec4`，host `2eec5bef`。
- **B**：仅把`guest_mutex.h`恢复到HEAD的旧全局唤醒实现，重新构建host并打包，
  APK `81c3e1a393578f4bc25f0e51f20ace9d6f74a092a433fa25843c644bbc789709`，
  host `a7a210c50889d2ee14415d65d5b7882c7112ed13`。
- **A2**：恢复原mutex文件后，通过同一构建入口重新构建的当前实现，
  APK `c4d5d0271ce9679e69825f248b9b87943cfc2ef66c41d0b061568e0f124f93ad`，
  host `ad41b9a01f6f8f99f4c473d5ebe5b0f9ee4c90db`。

A/B包内逐项解压哈希比较，只有`libshadps4_host.so`不同，JNI、其他库、资源与DEX相同。
B/A2构建的1,114个源码记录文件也仅`guest_mutex.h`不同。
重新configure会重建版本信息及更新部分依赖构建产物，所以额外保留A2这一轮，
不把重新打包的结果冒充与原APK字节相同。实验源码、构建记录和APK均保存在
`build/mutex-ab-20260915/`；APK/游戏内容不加入git。

| 检查 | A：按对象唤醒 | B：旧全局唤醒 |
| --- | --- | --- |
| 真机condition定点测试 | 66 checks / 0 failures | 66 / 0 |
| 真机services定点测试 | 70 / 0 | 70 / 0 |
| 游戏截图 | 按圈前已白屏；按圈后仍白屏，约50–54 guest flips/s | 150s仍能显示厂商视频画面；180s转为白屏，约28 guest flips/s |
| 重复圈键 | 白屏阶段4次，每次200ms | 启动120–129s 4次；确认白屏后再4次，均未恢复 |
| 已确证结果 | 白屏期间仍提交和present | 白屏期间仍提交和present；实际Stop到达Stopped/user_stop |

A2恢复确认：PID22146 / generation1，115s在按圈前已白，120–129s四次圈键后仍白；
150–180s guest flips从3212增到4734，约50.7/s。实际Stop在第二次轮询即到达
`Stopped/user_stop`，PID仍存活、TracerPid=0。最终设备安装SHA与本地A2 APK一致，
host/JNI与之前保持RelWithDebInfo，FEXCore Release。B及A2都没有复现“按圈后完全0 FPS”。

B的独立日志从启动前文件偏移开始截取：player1结束为243个video/349个audio帧、8104ms，
Close orderly=true；player2结束为180个video帧、6000ms。两个视频结束后仍白屏。
圈键由实际主屏触控注入；本轮没有额外开启Input PROF，不能把注入回执当作每次guest消费的证明。
启动速度和FPS只作为现场采样：两轮输入发生的guest阶段并非严格等同，
没有受控频率/温度及多次统计，不能给出优化百分比。

## 结论与未完成的因果验证

本轮**没有支持“这次mutex按对象唤醒优化导致稳定白屏”的证据**：恢复旧实现仍白，
并且以前正常主菜单已经使用当前实现。尚未接入生产的compiled guest mutex不能作为已上线回归点。
这不排除原有host mutex协议、VM锁顺序或时序竞争，也没有复现并解释最初的post-PLAY 0 FPS。

下一个有区分力的对照是正常主菜单版与当前版之间的guest CPU/debugger变更，并保留配套host/JNI/FEX身份。
当前`fex_context.cpp`在每次ExecuteThread前，无条件把FEX的generated
SIGTRAP/SIGILL/SIGSEGV出口改到stop_spill；未挂接路径也会执行，退出后只有SIGTRAP专门分类。
这是值得定点验证的默认行为变化，**尚未观察到白屏现场触发它**。
这里指FEX生成的guest异常出口，不是所有真实host SIGSEGV的处理器。
也应继续追踪白色源纹理的生产者与视频完成事件的消费者；本轮没有篡改寄存器或绕过完成条件。

本轮未修复白屏、未做完整回归、未新增spec、未提交或push；所有原有未提交改动保留。
最终身份、原始状态/日志/截图、两种mutex版本与源码比对见
[manifest.json](session-mutex-ab-20260915/manifest.json)。
