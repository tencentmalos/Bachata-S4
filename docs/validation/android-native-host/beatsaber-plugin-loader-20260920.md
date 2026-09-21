# Beat Saber：插件加载修复与 AYN 验证

2026-09-20，分支 `feature/malos/beat_saber_fix`，基线 `63b6d560`，本轮未提交。

**两处 `DllNotFoundException` 已修复并在 AYN 验证。SBS 画面仍异常，未达到可玩验收。** `PS4NativeUserSwitchPlugin` 与 `UnityNpToolkit2` 均用实际 52 字节参数完成初始化并返回有效句柄；游戏继续到用户初始化和 HMD 设置完成后的 VR 激活。最终无 guest patch 的普通启动也成功加载插件，出现同样的双眼局部白色元素；没有正常菜单/完整场景或交互验收。

## 修复范围

- Android `GuestRuntime::Prepare` 从挂载视图发现 `Media/Plugins` 下的 `.prx/.sprx`，补入完整 `DT_NEEDED` 图。沿用目录约束、路径去重和 256 模块上限；排除 macOS `._…prx` 隐藏元数据文件。使用挂载层供 loose/ZAR 共用，但本轮真机插件内容是 loose，未执行 ZAR 插件回归。
- 映射、重定位、静态 TLS 布局在创建 guest owner 前完成。插件及仅由它引入的依赖延迟初始化；已有启动模块保持原先的启动语义。没有新增运行中扩展 TLS、任意路径动态映射或模块卸载能力，映像仍保留至会话销毁。已存在而不在准备图中的文件返回 ENOTSUP，真实缺失才返回 ENOENT。
- `GuestModuleLifecycle` 将准备/初始化/完成/失败分开。依赖先初始化，插件 `DT_INIT` 经 FEX 收到原始参数；初始化期间不持模块锁或 guest 数据 pin。重复加载不重复构造，也不改写 `pRes`。同模块并发请求等待完成，等待可取消；同 owner 重入支持依赖环，已跟踪到的跨 owner 模块等待环返回 EDEADLK。异常会唤醒等待者并标记失败。
- `Dlsym` 不暴露仅准备、尚未开始初始化的模块句柄。成功初始化后才发布 sysmodule provider；没有依据 DT_NEEDED 伪造 LoginDialog 等 provider。
- 插件图引入的 LibcInternal 依赖中，批量补充实际 guest libc 导出准入：`memmove`、`_Znwm`、`_Getpctype`、`_Stoll`、`sceLibcMspaceMallocStatsFast`、`__cxa_guard_acquire/release/abort`。仍要求唯一 guest provider、正确库版本及可执行段，不调用 host libc 或用 host guard 替代 guest C++ 初始化。
- 保留前一轮固定正前方位姿、camera 单位四元数和 SBS/Stop 修复。没有修改 driver、倍率、guest 二进制或保存数据，也未扩大真实在线能力。

## 批量绑定核对

加载图从 5 个模块扩展到 8 个。最终 Linker 清单共 1,939 行：guest_export 780、runtime_bound 792、refused 353、not_relocated 14。`runtime_bound` 包括经过检查的 guest libc 兼容转发，不能将它全计作 host HLE。

| 新模块 | guest_export | runtime_bound | refused |
|---|---:|---:|---:|
| PS4NativeUserSwitchPlugin.prx | 4 | 2 | 7 |
| UnityNpToolkit2.prx | 429 | 29 | 2 |
| libSceNpToolkit2.prx | 0 | 125 | 177 |

UnityNpToolkit2 的 **409 个 Toolkit 导入全部实际绑定为 guest_export**。libSceNpToolkit2 的 30 个 LibcInternal 导入已全部获得绑定，其中函数地址落在实际游戏 libc；不是只在源码搜索到名字。完整库/NID/名称/实际绑定见 [清单](beatsaber-plugin-loader-20260920/run3-imports.json) 和 [功能族审计](beatsaber-plugin-loader-20260920/final-family-audit.json)。前一轮 779 项是三个文件的静态未定义符号（含弱/对象项），与当前 Linker 行数口径不同。

剩余项按功能族保留具名拒绝，包括 LoginDialog/LoginService、NP 在线请求/消息/社交/商店/排名/TUS/WebAPI、Toolkit 回调及部分统计接口。`__stack_chk_fail` 也保留失败入口。它们没有统一返回成功；本轮观察到的启动越过了原插件加载边界，不证明这些功能已经完整实现或永远不会调用。

## 实测与反例

设备 AYN Thor `9c2841a4`，Android 13，Turnip Adreno 740。沿用 Mesa `5ac41be677` / SHA `fdd37852…`，Render 0.5、Texture High、SBS 开启。

| 运行 | 结果 |
|---|---|
| PID30740（第一版） | Prepare 误读 `._PS4NativeUserSwitchPlugin.prx`，在执行 guest 前失败；随后补了旁车过滤。 |
| PID31993（第二版） | 两插件与 Toolkit 的 DT_INIT 均成功；随后调用 `__cxa_guard_acquire` 未准入，产生 guest fault。保留为 libc 批量修复前反例。 |
| PID2616/gen1，`4a81c28ad854aa38d3ecf0c6fed82fb4` | 最终 APK，PSVR 偏好诊断。两个插件句柄 5/6、各 bytes=52/init=0；Toolkit 句柄 7/init=0。出现双眼局部元素，4,978 次 guest flip 后正常 UI Stop，user_stop/EINTR。 |
| PID4419/gen1，`aa61cbfa17eaa5844ff6b08eaa87ccfa` | 同 APK，纯日志诊断、原始设备选择顺序。先选 None，插件加载后输出用户信息；`OnHmdSetupDialogCompleted: Finished, OK` 后游戏自行选择 PlayStationVR。画面同样异常。关闭探针时 FinishDrain 超时，随后出现 AcquireDataBatch 超时并失去服务；该运行不计作正常 Stop。已留存证据并清理此进程。 |
| PID5577/gen1，`5f4fba929053703c1af298baa6f3d78a` | 同 APK，`disabled_at_startup`，无 guest patch 普通运行。两插件及 Toolkit 同样初始化成功，出现同样双眼局部白色元素。4,970 次 host presents 后正常 UI Stop：Stopped/user_stop，detail 为 guest return=131（不是 0），TracerPid=0。 |

诊断日志按启动前文件字节偏移分割，避免混入旧运行；第二、三、四轮分别 174/202/216 条完整记录，decoder 的 incomplete/malformed 均为 0。原来的两个 DllNotFoundException 不再出现。可选 Unity 通用图形回调的 Dlsym 仍可返回 ESRCH，未将它们伪造成存在的导出。

两项明确后续边界：

1. Unity NP 初始化仍记录 `0x804101C8`，即 `ORBIS_NET_ERROR_ENOTINIT`（网络库未初始化）。游戏捕获后继续输出本地用户信息；这不是插件文件加载失败，也没有被替换为在线成功。
2. LoginDialog sysmodule 仍返回 `0x805a10ff`（无 provider）。用户切换功能族尚未完成。画面仍缺少正常菜单/场景，不能依据 draw、present 或 Unity VR 激活宣布 SBS 完成；下一步需重新追踪修复后实际场景的渲染输出。

原始顺序运行有 18 个 Tracker 输出样本，camera 均为单位四元数；保留固定正前方策略。没有进行世界视锥、完整 Move 交互、保存/在线流程、性能 A/B 或完整游戏回归。

## 验证与二进制

- 新生命周期测试：本机 **222/0**，AYN **222/0**。覆盖原参数、延迟初始化、依赖共享/环、重复加载、异常、并发、取消及旁车过滤。
- 真 FEX/生产 Linker 的既有模块查找回归：Android JUnit 1 项包含 **9 轮**，DT_INIT offset=0/nonzero/absent 各 3 轮，全部 guest return=51966（0xcafe）。
- Android host、APK、测试 APK 构建成功，`git diff --check` 通过。最初一次构建因构建中源文件变化被身份检查拒绝，重新以稳定源码构建完成，未将该失败隐藏。
- APK `c0e8e6d32312fa622b52375a03492b13708b442a47a59c19cfcda9b2c5065125`。
- host `5b0de1ac338e42cda01da1a5b730ae955258ac0a6ff329b63c880123cecc9b26`。
- JNI `30e5ec0178a531a2a93d3b915813851c7f859819d8ae8287e90ca1eb662056f7`。

设备安装 APK、提取后的 host/JNI 与本机构建 SHA 相符。仍沿用此前的 native 库提取安装策略，未修复或宣称通过 APK 内直接 mmap 的严格打包检查。独立 ELF 检查由 host 构建流程完成。

便携证据：[manifest](beatsaber-plugin-loader-20260920/manifest.json)、[运行摘要](beatsaber-plugin-loader-20260920/runtime-summary.json)、[安装身份](beatsaber-plugin-loader-20260920/installed-binaries.json)。本地原始构建/分析产物位于 `build/validation/beatsaber-plugin-loader-20260920/`。不提交游戏二进制、反编译源码或测试生成物。

最终无活动游戏、自动输入或 RenderDoc 采集；guest patch/auto-tag 属性清空，普通运行 patch disabled_at_startup，未连接调试器。保留用户 Turnip、倍率、材质和 SBS 设置。
