# Tetris：SigninDialog、SSL 与 EOS 本地通信边界（2026-09-21）

本轮接续 [DebugBus 导出](executable-export-debugbus-20260921.md) 后分析 Tetris。设备为 **AYN Thor `9c2841a4`**，分支 `feature/malos/beat_saber_fix`；保留原有未提交修改，没有 commit/push。最终 APK 已安装并核对 SHA，**游戏仍黑屏，未到可操作菜单**。

## 已落地的功能

- **SigninDialog 七入口完整离线生命周期与 ImGui 模态窗口。** 复用桌面/Android 的共享状态实现，替换桌面原先统一返回 0 的 stub。Initialize/Open/Close/Status/UpdateStatus/GetResult/Terminate、重复打开、会话取消、并发关闭、过期 UI 响应和输入捕获均有明确语义。窗口说明 PSN 不可用，关闭返回取消结果 1，不制造登录身份；鼠标、Enter/Escape 和 IME 共用的虚拟手柄读取路径可操作，打开时须先释放按键。固定寿命的 ImGui Layer 只持有 Dialog 弱引用，避免 renderer 延迟队列引用销毁对象。
- **SSL 本地池/空信任库五入口。** 逐一管理 Init/Term 的上下文；GetCaCerts 返回明确的空列表，FreeCaCerts 不释放任意 guest 指针；GetMemoryPoolStats 写入正确的 24 字节结构。池实际分配，有边界和释放；没有采用桌面 Ssl2 的 `dummy` 证书字符串。不是 TLS/证书验证/网络传输实现，未开放相关回调和连接入口。统计仅描述本实现的池与头部，不是 Host RSS 或完整 TLS 内存。
- **POSIX 网络离线适配与 `select`。** 全族 20 个 NID 的准入、数字地址转换、POSIX errno、无效描述符/非 socket 错误一起处理；目前 socket 创建明确 ENETDOWN，仍没有本地 socket 后端。`select` 使用 Orbis 的 1024 位 fd_set 和 16 字节 timeval，查询会话描述符，支持定时/无限等待与 Stop 取消，不把 guest fd 当作宿主 fd。等待前释放数据 pin，写回时验证映射身份并整批准入，防止释放后原址重用和部分输出。现有普通文件 fcntl 仍返回 ENOSYS，不声称其完整实现。
- **Random 整族（一个入口）。** 使用系统 CSPRNG 和已校验的最多 64 字节 guest 输出，发布实际 provider，取消旧的缺模块边界。

Signin ABI 对照本机固件 11.00 原始 ELF 与本游戏 Toolkit 调用点：Open 参数 16 字节，Result 4 字节，独立 `0x813500xx` 错误域；并非 CommonDialog BaseParam。精确 SHA、函数地址和结构记录见 [ABI 证据](tetris-signin-eos-20260921/abi-evidence.json)。本游戏 Toolkit 没有 LoginDialog 导入，本轮没有把 Beat Saber 的 LoginDialog 问题视作已完成。

## 实际运行边界

三次普通运行均使用原 ZAR，无 guest patch：

| 版本 / PID | 已越过 | 新观察 |
|---|---|---|
| V1 / 13073 | worker 加载 `libSceSigninDialog` 返回 0，越过此前 `0x805a10ff` | EOS 实际调用 `sceSslGetCaCerts`，类型化 Unsupported import 终止 |
| V2 / 18635 | SSL 五项全部绑定，越过 CA 列表路径 | EOS 后台线程调用 POSIX `select`，类型化 Unsupported import 终止 |
| V3 / 29912 | `select` 和 Random 不再导致立即终止 | 仍黑屏、无 guest flip；长时间等待，UI Stop 可结束，Stopped/user_stop、guest return=4 |

V1/V2 的 terminal detail 中出现 `guest return=0` **不代表成功退出**，两者的 stage/stop_reason 是 Failed/Faulted。V3 的“Running”也不是可运行/可玩验收。SigninDialog 的真实 sysmodule load 已验证；尚未观察到游戏实际 Open 并显示该窗口，因此 UI 验收来自下述定向 ImGui 测试，不能写成在 Tetris 中已经展示。

完整 2603 行绑定盘点保留，包含 NID、可读名、模块、实际 provider 与地址：

| 版本 | guest_export | runtime_bound | refused 行 | not_relocated | 唯一拒绝 symbol |
|---|---:|---:|---:|---:|---:|
| V1 | 1146 | 978 | 462 | 17 | 355 |
| V2 | 1146 | 981 | 459 | 17 | 352 |
| V3 | 1146 | 1018 | 422 | 17 | 333 |

[最终导入表](tetris-signin-eos-20260921/imports-v3.json)与[按库拒绝族](tetris-signin-eos-20260921/audit-v3.json)。绑定成功仅说明接入，不代表完整在线语义。

## EOS 本地 TCP 通道：新增的直接证据

精确 EOS ELF SHA 为 `65c26cf21afd53ca3c574eb43ac1f31055812632ea6de03d3690f6aea8e6c6af`。通过本轮 DebugBus 从实际 ZAR 导出；分析派生 ELF 只补索引/section，PT_LOAD 字节与虚拟地址保留。Reverse Study workspace `tetris-eos-wait-20260921` 返回 functions=0/xrefs=0，callers 明确 `address_not_indexed`；因此使用 LLVM 精确反汇编并以实际 RSP 停点证明执行，没有用零索引推断不可达。

1. V3 独立诊断 PID3197/gen1，run `e21587fb024d15757de75820e04abe4c`，RSP session `gdbg_68c26700df40423e8a45e3137e9527c3`，明确 little-endian。两次暂停都看到 Guest-1 停在 `pthread_cond_wait`，cond=`0x1001726a78`、mutex=`0x1001726a70`。该线程 RBP=0，不能伪造完整 unwind；RSP 保存的 HLE continuation 栈顶返回地址为 EOS **+0xa66795**，静态恰为 pthread_cond_wait 调用后的指令。见 [寄存器](tetris-signin-eos-20260921/rsp-registers-main.json)、[栈字节](tetris-signin-eos-20260921/rsp-main-stack-bytes.json)、[反汇编](tetris-signin-eos-20260921/eos-cond-call.asm)。
2. EOS Guest-31 停在 `select`，nfds=0、timeout=null；栈顶返回地址为 EOS **+0xcbff7d**。该调用正确语义就是无限等待，不能为了消除黑屏改成立即返回。另一个 EOS 线程也停在 select，但没有把同一组参数外推给它。见 [寄存器](tetris-signin-eos-20260921/rsp-regs31.json)和[栈字节](tetris-signin-eos-20260921/rsp-select-stack-bytes.json)。
3. 再用 V3 PID7249/gen1、run `4c871825d02e6f0f9460d1e72e9f382f` 从启动暂停，RSP session `gdbg_68481efd79c148c0895298776724468e`。真实 EOS PLT socket 停点记录参数 **(2,1,0)**，返回断点位于 EOS **+0x830cd8**，RAX=`-1`。该函数 **+0x830ca0** 的后续成功路径构造 `127.0.0.1:0`，经 bind/listen/getsockname/connect/accept 建立本地双端通道，并混用 `sceNetSetsockopt`、`sceNetSocketClose`、`sceNetErrnoLoc`。此次 socket 在 bind 之前已失败；不声称实际执行了后续成功路径。见 [入口](tetris-signin-eos-20260921/rsp2-socket-entry.json)、[返回值](tetris-signin-eos-20260921/rsp2-socket-return.json)和[完整通道函数](tetris-signin-eos-20260921/eos-loopback-pair.asm)。

**结论：离线启动也有本地 socket 通信需求，当前一概 ENETDOWN 的模型缺少这项能力。** 本地通道失败、主线程条件等待及后台空集合无限等待均已观察；“同一通道失败是所有等待的唯一原因”仍需实现通道后验证唤醒/启动进展，不能直接宣称整个死锁因果已闭环，也没有 GPU/Turnip 故障证据。

此外，V3 中 `libSceNpCommerce`（0xa8）仍被拒，随后 Toolkit 释放 16MiB+6MiB 区域和对应 HTTP context。它是另一条已确认的初始化失败路径；不能称整个 Toolkit 已正常初始化。桌面已有 Commerce ImGui 实现，但使用进程全局状态及原始指针，不能未经适配直接接入 guest。

## 下一批功能族合同

- **统一本地 socket 对象域。** POSIX 和 sceNet 共享 guest 描述符与实际对象生命周期，同时核对两种返回错误形式及 errno 发布；不能仅给 POSIX 创建另一张互不识别的表，也不能把 guest fd 直接传给宿主。先覆盖实际 EOS 所需 TCP loopback 创建、bind/listen/connect/accept、地址查询、fcntl/nonblocking、选项、send/recv、close/shutdown、select/epoll 唤醒和 Stop 取消。公网/PSN 仍保持离线策略；本地通信与在线登录分开。线程等待不得持 guest pin/全局 VM 锁，关闭/复用 fd 必须保留稳定 lease。用双端发送唤醒、EOF、超时、取消、地址越界/回收/重用和跨 POSIX/sceNet 调用验证整族，然后回到 EOS 初始化。
- **Commerce 离线模态族。** 准备 Initialize/Open/GetStatus/Update/GetResult/Close/Terminate、图标显示/隐藏与取消；深拷贝 targets，保留 userData 原值但不作为宿主指针解引用，正确返回取消/不可用，不能伪造购买或授权。先提取桌面共用状态/UI，再接 Android checked bridge；当前主 ELF+Toolkit 共 14 行/7 个唯一导入，连同 Close/GetStatus/内部别名/图标布局一起核对。
- 继续保留其它 333 个唯一拒绝项的库级清单。在线 NP、视频编解码、Audio3d 等不因没有先触发而被标成已完成。先验证 EOS 及 Toolkit 初始化退出，再检查首帧和可操作性。

## 验证、调试器边界与交付

最终 AYN 定向测试 **935 checks / 0 failures**：Signin/ImGui 96、SSL 192、POSIX/Random 139、实际文件/ZAR I/O 508。Signin 用生产注册 Layer 和真实 ImGui 帧验证渲染命令生成、按住打开键不误关闭、取消/重开、输入捕获释放；没有做实际游戏中的模态画面、物理手柄或 GPU 像素验收。测试日志和精确被加载 Host SHA 见 [最终测试](tetris-signin-eos-20260921/tests-final.txt)。早期 UI 测试错误地在 Render 后用窗口相关 IsPopupOpen 查询，产生测试进程 SIGSEGV；已改为 AnyPopupId/AnyPopupLevel 查询并保留反例，非游戏故障。

原生 LLDB 首次因版本身份未知未启动；第二次 session `148993b72d794adea0c1a84e32564da3` 的 CodeLLDB 在 attach 解析模块期间 SIGBUS/DAP stream closed，**没有取得 native 栈**。journal 首个失败 `host.request.failed` / host sourceSequence=8，后续 cleanup 已完成、TracerPid=0。见 [原始 evidence](tetris-signin-eos-20260921/native-attach-failure/evidence-manifest.json)、[adapter 日志](tetris-signin-eos-20260921/native-attach-failure/logs/adapter.stderr.log)及 [host shard](tetris-signin-eos-20260921/native-attach-failure/journal/host-000.ndjson)（SHA `23a53a961f07fff644518a3f9e561863b7501f59288a90e9c6a23e696c1e99e1`）。这是调试器失败，不能算 Tetris 自行崩溃。

两轮 guest RSP 使用工具独占控制，均已移除断点、恢复 guest、释放 forward；模块基址来自 qXfer，但目标没有提供 Build-ID/instance nonce，PID/run 与导出 ELF SHA 是外部身份记录，不冒称工具完成强绑定。RSP mixed stack 明确只是边界历史状态且受 RBP 链覆盖限制。一个带活动软件断点的 continue 被工具拒绝，删除入口断点后才继续至独立返回断点；没有把重复命中当成执行进展。

最终 APK `c7ee68f3549f74f352d4c677d8709ec5c0f6b0666bd73e6828464d5c52ae3b8c`；Host `ff62888e10f23edb535f68aefcbc496544c98400eb8d4c2292297648539e131f`；JNI `41f72000887388913373c7f0a883864dbd0f4eae73bf251f79e3e706cd7f362b`。安装包重新拉回、全包 SHA 相同，Host/JNI 打包身份已记录于 [build identity](tetris-signin-eos-20260921/build-identity-v3.json)。Native 与 APK 构建通过、git diff --check 通过；未做完整游戏回归。

三轮 V3 普通/诊断会话都由 UI Stop 结束，user_stop/return4；已有“Stopped 后界面残留最后帧”问题仍在，确认 native session:none 后才重开应用回到 Library。最终 PID9886、TracerPid0、无游戏服务、无 debugger forward，guest_debug_port/wait 均0、patch/auto_tag 空。global.json 与 host/config.json 逐字节未变，保留 Turnip/0.5/High/SBS/gyro。见 [清理结果](tetris-signin-eos-20260921/cleanup.json)、[最终状态](tetris-signin-eos-20260921/status-final.txt)与[证据 manifest](tetris-signin-eos-20260921/manifest.json)。游戏/固件二进制保留在用户本机原路径及 Downloads/build，不纳入此证据目录。
