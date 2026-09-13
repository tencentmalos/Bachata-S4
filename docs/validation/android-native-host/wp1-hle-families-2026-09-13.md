# WP1 HLE 函数族扩展与 WP1→WP2 边界

日期：2026-09-13。基点 `60b29d3e`（libc/startup 修复之后）。本轮直接在生产 GuestRuntime 上继续补 Orbis 函数族，未改 FEX/Foundation 子仓。二进制在该基点加工作区修改时构建，保留其实际 SHA/Build ID。

## 结论

真实 TMNT 选取内容（本体 v1.00 + 更新 v1.08 的 executable/modules/param.sfo，无完整 assets）现已在 FEX 上跑完整个**非图形**guest 启动：guest libc bootstrap → 六个依赖模块 DT_INIT → sysmodule → UserService → SystemService → 进程身份 → GnmDriver owner 注册，并在 `sceVideoOutOpen`（显示输出）具名结构化 Faulted。这是精确的 **WP1→WP2 交接点**：其后是渲染/Turnip/Surface（WP2）。

**这不是游戏可玩、渲染或 Swan 验收。** 仍是 AYN Thor/API33/ARM64/4KiB 的 shell runner，只部署选取的 executable/modules，没有完整 assets、UI 安装版本 lease、Turnip 画面或十分钟运行。Swan/API36 **NOT_RUN**。

## 沿最早确证失败的边界推进（每步真机复现）

| 顺序 | fault import | 函数 | 处理 |
|---|---|---|---|
| 1 | `g8cM39EUZ6o#libSceSysmodule` op61 | sceSysmoduleLoadModule | session-owned sysmodule（非 desktop 全局表）：LookupSysmodule 读静态表；GuestRuntime 保存 per-id refcount + provider readiness（已加载 guest 模块或 hle_modules 里的系统库）。缺 provider → 具名失败 ORBIS_SYSMODULE_LOCK_FAILED，绝不假成功。u16/u32 两套 id API 都接 Load/Unload/IsLoaded/GetHandle |
| 2 | `j3YMu1MVNNo#libSceUserService` op20 | sceUserServiceInitialize | UserService 启动族（Initialize/Terminate/GetInitialUser/GetForegroundUser/GetLoginUserIdList/GetUserName/GetEvent）：校验 guest 输出指针、host 本地结构体调用真实实现、写回 |
| 3 | `fZo48un7LK4#libSceSystemService` op86 | sceSystemServiceParamGetInt | SystemService 启动族（ParamGetInt/GetStatus/ReceiveEvent[~8KiB 堆本地]/HideSplashScreen）：同 validated-copy 策略 |
| 4 | `WslcK1FQcGI#libkernel` op19 | sceKernelIsNeoMode | 进程身份标量族（IsInSandbox/IsNeoMode/HasNeoMode/IsDevkit/IsProspero/IsCEX/GetMainSocId/GetCpumode/GetCurrentCpu）：无指针，直接分发到真实实现，值与其余模拟器一致 |
| 5 | `ZFqKFl23aMc#libSceGnmDriver` op255 | sceGnmRegisterOwner | 仅接此一个（零售固件返回 FAILURE，guest 容忍）：校验 name 字符串，返回真实 ORBIS_GNM_ERROR_FAILURE。GPU 提交/翻页路径不接 |
| — | `Up36PTk687E#libSceVideoOut` op156 | **sceVideoOutOpen（当前边界）** | VideoOutDriver 全局（HLE 模块，driver 为 null）→ 需 Presenter/renderer/Turnip/Surface 绑定 Session generation。**WP2** |

## 准入门控（Bind gate）

每个库一个显式 NID allow-set；只有本 runtime 真正实现的函数按对应库后缀准入，其余函数仍具名 fault。绝不因描述符存在就调用未实现函数或裸指针。

## 验证与身份

- **回归**：真实 FEX `guest_execution_tests` 242/0、`veneer_allocator_tests` 19/0（build/v0-fex-test，AYN 真机）。
- **三轮同进程**：CLI `fault` 模式 PASS×3（三轮到达同一 sceVideoOutOpen 边界，generation 2/4/6、context 1/3/5，每 generation 干净重置）；`cancel` 模式 PASS×3（启动中途干净取消）。sysmodule refcount/mutex 是 per-Impl 成员（非 static），无跨轮泄漏；仅 NID allow-set 是 static const。
- 源码修改：`src/core/host_runtime/guest_runtime.cpp`、`src/core/libraries/kernel/process.h`、`src/core/libraries/sysmodule/sysmodule_internal.{h,cpp}`。提交 `d736540c`、`d31a919b`（分支 codex/android-fex-round2）。
- 游戏字节、重建 ELF、maps/内存 dump 不入仓。设备临时部署目录已清理。

## 下一实际边界（WP2）

`sceVideoOutOpen` 起是图形输出子系统：把 Session generation 的 ANativeWindow/AndroidWindow/Presenter 与 guest renderer 绑定，接通 GnmDriver 命令提交、VideoOut 端口/缓冲/翻页 IRQ、Turnip/Surface 寿命。生产输入/用户/平台服务应在放行首次 guest 调用前就绪。不能伪造 sceVideoOutOpen 或在无 WP2 渲染接线时绑定 GnmDriver 提交/VideoOut。继续沿 [整版 TMNT spec](../../specs/android-native-host-tmnt-after-runtime.md)。
