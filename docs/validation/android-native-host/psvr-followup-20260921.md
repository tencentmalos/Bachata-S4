# AYN 三款 PSVR：异步 I/O、模块展开及新阻塞点

2026-09-20 晚至 09-21；`feature/malos/beat_saber_fix`，基线 `63b6d560`，本地未提交。延续 [前一轮](psvr-progress-20260920.md)，设备 AYN Thor `9c2841a4`，使用原有游戏 ZAR，未修改游戏包。常规设置保持 Turnip、SBS、Render 0.5 / Texture High、陀螺仪和初始正前方。

本轮新增可部署的模块信息/信号识别、异步 I/O、虚拟 Tracker CPU 阶段和 AudioIn 无设备桥接。Sports 进入 Unity 初始化后推进到 HTTP 回调；Tetris 越过此前 coredump 拒绝，实际错误已定位为 NP Toolkit 初始化失败；Beat Saber 场景持续可见，但重影和 Continue 仍未解决。三款均不声明可玩。

## 实现与合同

- `guest_process_services.h`：从实际 Linker 模块快照返回 EH frame、首段和模块名；处理 Kernel/Sysmodule unwind、ModuleInfoFromAddr 及精确库后缀。输出范围和结构大小验证失败不写半份结果。`_is_signal_return` 按本地 11.00 libkernel 的 RX 检查和两类真实指令序列识别，**不是固定返回 0**。仍未接入完整信号分发、任意 DWARF 展开、InternalMemoryGetModuleSegmentInfo、debug/backtrace 和模块卸载族。
- Coredump 四入口明确无 provider：注册验证 callback/栈大小后返回 THREAD_CREATE，取消返回 NOT_REGISTERED，写入/类型设置返回 NOT_IN_COREDUMP_HANDLER；不保留 guest callback、不假装注册成功。WriteUserData 按 ssize_t 返回 64 位负值。此能力不是 coredump 回调实现。
- `guest_aio.h`：15 个 AIO 入口通过两个会话所属 worker 执行真实 GuestStorage positioned I/O。60 字节默认参数与本地固件核对；请求、结果、ID、超时均走 guest 数据检查，提交一次取得整个数据批次，防止跨映射退休时分批 pin 死锁。保留文件 description，提交后关闭/reuse guest FD 不改变已提交请求；支持单/多 read/write、poll、wait、cancel、delete，保留已传输字节。queued cancel 立即取消；运行中 I/O 在 chunk 边界响应。Stop 先取消，再 join worker，最后销毁 storage/VM。
- AIO 资源限制：每批最多 128 请求 / 256 MiB，待处理总量 512 MiB、512 个存活 ID。默认调度参数以外返回 ENOTSUP；优先级未映射为 host 调度优先级。提交队列和 map 元数据完成后才发布 guest 输出，元数据 OOM 回滚部分入队。未声明任意参数、优先级性能或全部取消时序验收。
- `GuestStorage::AcquirePositioned` 提取稳定的文件 lease，共用原 positioned I/O、存档配额、ZAR/native 文件路径；没有另造一套 native FD。
- Tracker：虚拟 SBS 的 CpuProcess/NotifyEnd 使用当前传感器结果和注册 handle；验证初始化、模式、保留字段和结构大小。实体相机路径仍报未执行 GPU submit，不伪装光学跟踪。Term 清除所有设备 handle 和内存记录，支持重新初始化。
- AudioIn 四项实际导入接通共享 NullAudioIn 合同：Open 验证参数后返回 NOT_OPENED，不发布录音 port；Close/GetSilentState/Input 返回明确无设备/无效句柄/无效指针错误。guest 输出地址不进入原生录音函数，不产生假静音数据。其余录音扩展入口未准入。

## Sports

V2 PID401/gen1 越过 `_is_signal_return` 和 Tracker CPU 阶段，截图可见 Unity splash，但仍在 AudioInOpen 具名拒绝。V3 PID6853/gen1 已越过 AudioIn，PSVRXRSDKPlugin 按原始 52 字节参数成功初始化，最终在 worker 的 `sceHttpSetRedirectCallback`（`h9wmFZX4i-4`）具名停止。旧 fread/随机设备崩溃未重现；这不等于菜单或玩法验收。

下一族是 HTTP 回调与离线完成协议：Redirect/AuthInfo/CookieRecv/CookieSend/SSL/RequestStatus 的对象归属、继承、取消、销毁和 guest 调用桥。桌面当前这些 setter 多为 stub，不能直接准入为成功，更不能把 guest 函数指针转成 native callback。完整剩余项见 family audit，未将第一个失败当全部任务。

## Tetris

V2 PID1772 和 V3 PID18159/gen1 均进入 libc/EOS 初始化后的游戏启动，随后发生 FEX JIT 内 VA8 访问。FEX fatal 文件的 mapped RIP `0x143c235` 对应原始 ELF `+0x103c235`：`mov (%rdx),%rdx` 后 `mov 8(%rdx),%rax`。这是调用栈打印器遍历到空帧后的二次崩溃。

使用 guest RSP 在 `eboot.bin+0x11395a0`（VA `0x15395a0`）设置有界软件断点；现场 32 字节与精确 ELF 相符。PID18159、run `69f8bd4f38796f628128f212701a174c`，stop epoch2：RCX 指向 UTF-16 文本 **`PSN: Failed to initialize the np toolkit`**，RDI 为 `LowLevelFatalError`，行号 755。保存了寄存器和经验证的 RBP 链；RSP 模块协议未提供 Build-ID，不能把地址核对写成完整运行 SHA 握手。

同一日志前有 `libSceNpToolkit2` load 成功、`libSceNpUtility` provider 不可用。后者是需要追踪的上游候选，还没有拿到 toolkit 内部失败返回链，不能断言它是唯一根因。AIO 已做实际文件单测，但本轮未证明游戏执行到所有 AIO 读写路径，也不归因 AIO 导致这次崩溃。没有 ZAR 缺文件证据，没有改游戏断言或伪造 NP 在线成功。

原生 debugger 默认工具链版本无法核对，显式 CodeLLDB/NDK 组合又在 attach 时崩溃；失败 journal 已导出，native session `41f623465e454ec4b6b4faedee90e65c` 已 cleaned、TracerPid0。随后使用的是唯一 guest RSP controller；正常 stop_session 移除断点/forward 并释放 ownership，guest 继续后重现已知二次崩溃。guest debug 属性恢复 0。

## Beat Saber：分开记录驱动与捕获证据

V3 普通 Turnip PID19776/gen1，run `ddd162378a35d28d78e51f518c5b9bde`：背景、安全提示、光剑可见，文字和光剑仍重叠。保留同版普通截图；未完成 Continue/Move 指向/受控物理转动验收。

新捕获 SHA `b5d28cb6932ed771595f91b49aa472a5862e05b13271117aea4807305232b485`，239,481,345 B，来自 **Qualcomm** PID10929、run `58fb4097351cebd9892003841065a786`，guest flip1039→1040。113 draw 的 schema/marker/使用链和 review_texture 实际图像均已保存：

| 选择器（mip0/slice0） | 观察/使用链 |
|---|---|
| E640 / R1972 / sample0 | 早期左右场景，尚未绘制安全提示 |
| E1320 / R1972 / sample0、1 | 2688×1512、2 samples，左右提示和光剑已分别存在 |
| E1341 / R2002 / sample0 | 从 R1972 读取，1344×756；文字异常在此之前已存在 |
| E1736、1750 / R1567 | 两次最终 guest 绘制读 R2002 和 R2142；输出2688×1512，额外出现每眼侧转 |
| E1761 / R606 | host 读取 R1567；RGB 有画面、输出 opaque，仍保留侧转 |

本轮 MCP 的 bindings/常量查询实际可用，记录最终右眼 viewport push 数据 xoffset2016/xscale672/yoffset756/yscale-756 及对应 shader ID。该证据将 **Qualcomm 的侧转** 收窄到 guest 最后合成步骤；不能冒充 Turnip 重影的唯一根因，不能靠旋转最终截图掩盖问题。精确 UV/顶点数据及两驱动差异仍待验证；`finalOutputEquivalence=unverified`。

保留捕获反例：Turnip+捕获层启动在 kgsl_bo_finish 崩溃；Qualcomm 完成写出 RDC 后发生 queue timestamp/提交失败及 GPU worker 崩溃，故不是稳定性通过。Qualcomm RDC 尝试 Turnip 回放因 storageBuffer8BitAccess 不满足而拒绝，之后改为同设备 Qualcomm 回放成功。两次远端 idle 断连的空结果丢弃；仅使用有有效 controller 的图像/状态，不将失败 usage 空表当无使用。所有 replay controller/进程和本轮转发已清理。

## 验证、全量绑定与边界

V3 主机/APK 构建通过，安装文件 APK/host/JNI SHA 与本地产物一致，见 [身份](psvr-followup-20260921/binaries-v3.json)。AYN 定向检查：Process/Tracker/AudioIn **80/0**、AIO **72/0**、原文件 I/O **501/0**，共 **653/0**。先前 process 测试使用不满足映射粒度的 Protect 导致3失败，修正测试映射边界后通过，失败记录保留。未重跑整套 GPU 或所有游戏。

| 游戏（V3 完整实际绑定） | 行数 | guest_export | runtime_bound | refused 行/唯一 | not_relocated |
|---|---:|---:|---:|---:|---:|
| Beat Saber |1939|780|797|348 / 323|14|
| Sports |2303|969|922|401 / 352|11|
| Tetris |2603|1146|934|506 / 387|17|

每项保存 importer、完整 NID/库后缀、名字和实际 binding；所有拒绝项按库保留，不声称在线/回调/信号族已经齐全。[证据 manifest](psvr-followup-20260921/manifest.json)；原始游戏/固件 ELF、RDC 和大体积工具日志只留本地 build 目录。

下一批优先共享 NP Toolkit 离线初始化/用户/LoginDialog 全流程，随后 Sports HTTP callback 族；Beat 同时核对最终两次 guest 合成的 UV/顶点及驱动差异。不得以返回0、复制眼图或绕过断言完成验收。

V3 最终 Beat 正常 UI Stop，2265 presents，`Stopped/user_stop`、guest return2147614724（不是0），input token/buttons0、TracerPid0、捕获层/patch/debug关闭，global.json/host config 按原字节恢复。没有 commit/push、全游戏回归、性能提升或三款可玩声明。

## 最终部署 V4

V3 实景之后补了 AIO 提交元数据 OOM 的事务性回滚：先完成 map/queue 分配，再发布 guest result 和 ID；部分插入失败撤销入队并返回 ENOMEM。未做分配失败注入，不把源码检查当作该异常分支实测。最终重新构建 host/APK，并在 AYN 重跑 Process/Tracker/AudioIn80、AIO72、文件 I/O501，共 **653/0**；`git diff --check` 通过。

当前安装为 [V4 身份](psvr-followup-20260921/binaries-v4.json)：APK `cf224d561b2d65dadc7616b0e2313265bbb524eb7134cf4d4e795181a9229261`，host `fc567f0c0098d00b35c8fd1d6279b27c81d9ee86d23aaf07a2c7025264c150b4`，JNI `6ce2dd2f5162015f3c6474cab046da6ecbfdb93c32f9f03789cf1d90908e45fb`。APK 内库、本地库和实际安装文件 SHA 一致，身份文件另存关键源文件 SHA。

V4 Beat 普通 Turnip PID26160/gen1，run `a051a830c340e969662e779a10dbc21f`：再次确认安全提示/背景/光剑可见但重影未修；1907 presents 后正常 UI Stop，`Stopped/user_stop`、guest return2147614724。输入 token/buttons0、TracerPid0、profiler capture idle、RenderDoc API 未加载；调试属性和 GPU layer 关闭、本轮 forwards 为空，global.json/host config 与备份逐字节相同。Sports/Tetris 在最后这次 OOM 防护后未重跑；上文三款全量绑定、fatal 现场和 RDC 都明确属于 V3，不能迁移为 V4 实景通过。
