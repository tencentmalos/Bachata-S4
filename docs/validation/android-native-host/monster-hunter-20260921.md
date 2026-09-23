# MHW / MHR 完整 ZAR、AYN 安装与启动修复

> 本文的最终 V2 是历史验证点。最新 V4 临时区、内存修复及两款当前故障见[2026-09-22 续修记录](monster-hunter-followup-20260922.md)。

2026-09-21 开始，09-22 凌晨收尾。分支 `codex/tetris-runtime-fix`，基线 `605053a6` 加原有 Sports/Tetris 未提交改动。本轮没有 commit/push，也没有修改 FEX 子仓。

两款完整 ZAR 已制作、逐文件回读比对、安装至 AYN Thor `9c2841a4` 并核对设备 SHA。修复了游戏 libc 的重复初始化，补了文件可达性查询；两款仍停在后续 HLE 缺口，**未到主菜单、未验收可玩**。

## 成品、来源与包含内容

| 游戏 | ID | ZAR 内容 | 原内容字节 | ZAR 字节 | 减少 |
| --- | --- | --- | ---: | ---: | ---: |
| Monster Hunter Rise / 崛起 | CUSA34119 | `app/` 本体 + `update/` 16.01 + `dlc/` 303 项，含 Sunbreak | 38,434,055,197 | 37,864,228,626 | 1.4826% |
| Monster Hunter World: Iceborne / 世界 冰原 | CUSA09554 | `app/` 15.23 整合版 + `dlc/` 236 项，含 Iceborne / Deluxe Kit | 58,485,645,393 | 58,358,764,365 | 0.21694% |

每款仅一个 ZAR；不再分别生成四个包。DLC 的名称、Content ID、原 PKG SHA、SFO 和对应包内目录均在来源清单中。539 个 DLC 全部处理，没有默默跳过；其中元数据型 DLC 的 PFS 文件数为 0，但其 SFO/许可等元数据仍保留。独立 guest patch 不入包，后续仍可另做 `.zar.db`。

- MHR 来源：`/Users/bytedance/game/ps4/Monster Hunter Rise/` 的本体七卷、16.01 补丁三卷、`CUSA34119 DLC.rar`。本体 APP_VER 01.00，更新 APP_VER 16.01；未加入修改器 ZIP。成品为 `/Users/bytedance/game/ps4/zar/CUSA34119.zar`，SHA256 `318e805a18d63f666b012d2eb88b750d71179accc2b7c536618b72971ff4c7fb`。3,461 文件完整比对。见[来源及内容清单](monster-hunter-20260921/CUSA34119-provenance.json)、[所有文件名及大小](monster-hunter-20260921/CUSA34119-files.json.gz)。
- MHW 来源：`/Users/bytedance/game/ps4/Monster Hunter World Iceborne 港版中文 CUSA09554 V15.23/` 的 9.00 整合版十四卷及 `DLC/DLC.rar`。未叠加 5.05–7.55 降级补丁。原 SFO **APP_VER 01.00、VERSION 15.23** 原样保留，不能仅据 APP_VER 写成旧版。真实成品为 `/Volumes/Macintosh HD - Data/Users/Shared/PS4-ZAR/CUSA09554.zar`；`/Users/bytedance/game/ps4/zar/CUSA09554.zar` 是指向它的软链接，备份时必须包含目标。SHA256 `50588595a5055009b06940e473e01a4854d31877b65184ca66394352c547c991`。2,723 文件完整比对。见[来源及内容清单](monster-hunter-20260921/CUSA09554-provenance.json)、[所有文件名及大小](monster-hunter-20260921/CUSA09554-files.json.gz)。

空间不足时向主卷额外复制 MHW 的容量检查拒绝了操作，未发布截断成品；改为在另一卷保留已验证的原 ZAR，建立索引链接。大小比较针对解包后的内容，不是对 RAR 压缩率的比较。

AYN 成品路径均为 `files/games/<TITLE>/<TITLE>.zar`。先传入唯一暂存目录的 `.partial`，确认大小和 SHA 后再改名发布；MHR/MHW 传输加设备哈希分别约 393 / 639 秒。Library 注册 `INSTALLED`，缓存 SFO 分别与更新 16.01 / 整合版 15.23 字节一致。未把资产展开到设备。来源 JSON 含设备最终路径、字节数及 SHA。

## libc 初始化根因及修复

MHR 原 APK `e44290a0`、PID29089 在 libc `abort+4` 的 `int 0x44` 退出。游戏 libc 基址 `0x100000000`，abort 偏移 `0x47c90`。

Guest RSP 观察默认堆 mutex 槽 `0x1001282f0` 首次初始化前为 0，随后相同槽再次初始化得到 `0x80020010`（EBUSY），直接 abort。调用链为 libc `+0x7a8 → _malloc_init(+0x48ec0) → +0x49330 → _malloc_init_lv2(+0x4d980) → scePthreadMutexInit`。Android `GuestRuntime::Run` 在 DT_INIT 前手动调用游戏 `libc.prx` 的 `_malloc_init`，而游戏构造函数自身还会再调用一次。

[修复](../../../src/core/host_runtime/guest_runtime.cpp)对齐[桌面 Linker](../../../src/core/linker.cpp)：只有系统 `libSceLibcInternal.sprx` 由宿主提前调用 allocator/mutex-enable；游戏 `libc.prx` 由自身 DT_INIT 初始化。没有放宽重复 mutex 初始化的 Busy 检查。MHW 原版也在 mutex EBUSY 后 libc `+0x56e24` 退出；修改后两款均越过该位置。

RSP 证据保留各次寄存器、栈和槽值。首次要求 programId 的连接被拒绝，因为目标没有提供；另一次重连 remote EOF，随后显式 pause 成功。成功会话以 caller 提供的 host PID/starttime 关联，目标未回传完整程序身份，不能声称工具完成了 programId/buildId 独立认证。[清理结果](monster-hunter-20260921/mhr-rsp-cleanup.json)确认拥有的断点、forward 和连接已释放。Reverse Study 对 analysis ELF 返回 functionCount=0，不能把空 callers 解释为无调用；因此使用原 PT_LOAD 字节/VA 不变的标准 ELF 和有界 LLVM 反汇编核对路径。

合成 provider 夹具现在分别模拟游戏自行初始化和系统预初始化。AYN shell UID 的生产 FEX/Linker runner 验证 fallback、系统优先、系统失败不回退、移除系统恢复 fallback、游戏构造失败、TLS bootstrap、构造内取消，7 种 × 3 轮通过。证据见[runtime-tests](monster-hunter-20260921/runtime-tests/manifest.json)。两次辅助判定器错误（返回文本换行、初始化异常为 BackendFailed）已修正，原失败日志保留；不是生产修复失败。这里没有宣称真实固件系统 libc 全面验收。

## 文件可达性查询

MHW V1 越过 libc 后停在 `sceKernelCheckReachability`。新增 Android 存储桥接，遵循桌面的 255 字节路径上限，区分不可读地址 EFAULT 和过长路径 ENAMETOOLONG；使用实际会话 Stat/挂载命名空间查询目录、ZAR 基础/更新、已准入设备，保留 ENOENT 和越界/符号链接访问错误。没有宿主输出指针，也不修改 POSIX errno。路径入口排除 socket fd 分流。

AYN 文件 I/O 定向测试 **593 checks / 0 failures**，包含 ZAR 更新覆盖、原生目录、设备、缺失路径、symlink/越界、255/256 边界及跨未映射页字符串。[结果](monster-hunter-20260921/reachability-tests.log)。未实现整个缺失文件接口族。

## 最终 APK 实机边界

最终 V2 APK SHA 前缀 `e55bd3e9`、host `af1caa91`、JNI `2bd9c2d7`，安装包完整 SHA 与设备一致；[完整身份](monster-hunter-20260921/apk-v2.json)。保持用户 Turnip / Render 0.5 / High / 原 SBS 和陀螺仪设置。

| 游戏 | 最终观察 | 结论 |
| --- | --- | --- |
| MHW PID2022/gen1 | 越过 libc 与 CheckReachability，停在 `sceAppContentTemporaryDataFormat`，op40；Failed/Faulted，没有图形初始化或场景画面 | 启动仍失败 |
| MHR PID3768/gen2 | libc 完成、Turnip 初始化，停在 `sceKernelSetFsstParam`，op571；Failed/Faulted，没有菜单画面 | 启动仍失败 |
| TMNT PID3768/gen1 | 启动动画、中文主菜单可见，随后正常 UI Stop → Stopped/user_stop，guest return2147614724 | 初始化改动的有界回归通过；不是新的关卡/通关验收 |

MHR 与 TMNT 最终复用同一个应用进程，debug_status 的 run_uuid 仍显示旧值；以 title_id、generation、对应导入清单和采集时点区分，不能仅凭该 UUID 关联。原版两款的 `guest return=0` 也属于 Faulted，不是成功。

完整导入和拒绝清单保存为各 run 的 `imports.json.gz`，[统计及库族](monster-hunter-20260921/import-summary.json)：MHW 最终 999 行 / 250 refused 行 / 248 唯一拒绝；MHR 1201 / 347 / 344；TMNT 1424 / 190 / 164。导入拒绝不等于所有条目已经执行或都会阻塞。录像记录的是启动与失败，不作为游戏画面验收；文件保留在本机 `build/validation/monster-hunter-20260921/*launch.mp4`。

## 下一处接口合同

- **MHW 临时数据区完整生命周期**：桌面 Format/Unmount 目前都是无参零返回 stub，不能照搬。已核对本机 11.00 `libSceAppContent.sprx`：`+0x1720` 旧 Mount 接收输出指针并固定 option=1；`+0x1740` Mount2 接收 option/output；`+0x1760/+0x1770` Unmount/Format 均转发首参数，公共体做非空检查及初始化状态检查。后续需基于实际挂载点结构、会话自有临时根、打开文件 lease、格式化/卸载失败和错误映射一起实现；本轮未擅自加入“Format 返回 0”。
- **MHR FSST 配置**：本地公开 Orbital 类型表给出 `(int prio, SceKernelCpumask mask)`。本机 libkernel `+0x156f0` 独立确认，将 mask/priority 放入 16 字节记录并调用 `sysctlbyname("kern.fsst_param", ...)`，失败转 Orbis errno。桌面没有实现。下一步需核对实际调用值/结果消费以及仿真内核是否有对应 worker 和调度域，不能直接把这两个参数当成当前 guest 线程调度或假成功。网页检索未得到可靠的详细合同，不据二手符号列表补语义。

固件只保留在用户原路径及本机 analysis 输出，未将其 ELF/代码内容加入仓库。[analysis 来源 SHA](monster-hunter-20260921/appcontent-analysis.json)及现有 `build/validation/psvr-followup-20260920/libkernel-analysis.json` 可复核。

## 来源清理与收尾

见[血源来源清理](zar-source-cleanup-20260921.md)：已删除八卷原本体 RAR 和比对过的原提取树，保留成品、来源 SHA、完整文件清单；未入包的金手指/本地化补丁保留。逻辑移除 62,894,961,470 字节，同期可用空间增加 62,471,094,272 字节（约 58.18 GiB）。TMNT 原包另含 backport/存档，本次先保留；已满足空间需求。

MHW/MHR 原 RAR 保留。已提取的分析模块复制为独立文件并核 SHA，成品再次核 SHA 后清理本轮临时 PKG/散文件及重复 ZAR，详见 `temporary-cleanup.json`。本地 `/Users/bytedance/game/ps4/zar/README.md` 和每款 provenance JSON 作为归档入口。**AYN 上血源/TMNT 是旧散文件安装，Swan 上才是已验证 ZAR；本轮没有转换 AYN 这两款。**

最终 Library PID7069、session:none、TracerPid0、无 adb forward，owned scrcpy 已正常停止、三段录像 finalized。debug RSP port/wait=0；设置和 host config 逐字节与任务开始一致。[设备收尾](monster-hunter-20260921/cleanup-final.json)。没有可玩、全游戏回归、性能提升或 commit/push 声明。
