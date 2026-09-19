# 血源 Android 接入：完整缺口盘点与修复（2026-09-17）

进行中，尚未验收到可操作游戏。设备 AYN Thor / API 33 / ARM64 / 4 KiB，高通系统驱动。
主 ELF 为 CUSA03023；主程序、libc.prx、libSceFios2.prx 共 861 条导入记录。
25 版原始清单有 150 个具名拒绝；清单见 [完整拒绝项](bloodborne-20260917/refused-before-batch.json)。
25 版的非拒绝分类不是完整重定位证据；新版本由 Linker 直接记录实际重定位结果，未重定位单独列出。

## 当前批次

- 此前已经批量接入旧 GNM、PlayGo、Matching2 本地离线控制、NetEpoll、奖杯准备/生命周期/查询。
- 25 版真实失败是无片元着色器的 pipeline：辅助细分阶段从残留 PS 配置构建输入，要求 VS 未输出的 location 1；驱动拒绝链接并触发断言。不是游戏接口 Unsupported。
- 修复辅助细分阶段从实际 VS exports 构造接口，刷新缺失 PS 时仍需要的当前运行时状态；PipelineKeyVersion 4→5，淘汰旧辅助 SPIR-V 缓存。
- 高通独立 GPU pipeline 测试：空/单个/稀疏输出 × Rect/Quad，12 checks / 0 failures。仅证明这些 pipeline 可创建，不代替游戏画面验收。
- 存档五入口作为一组补齐：Setup/Get/SetSaveDataMemory、DirNameSearch、SaveIcon。共用桌面 Store/目录搜索，缓存归属会话；输出在 I/O 后以完整映射身份重新校验。Android 原生定向测试 172/0，覆盖三轮独立 Store、持久化、局部覆盖与坏指针；尚非实际游戏存档验收。
- 发现 Android 元数据未设置 SYSTEM_VER，导致桌面 helper 误入固件 1.7 以前的兼容分支；现从 param.sfo 同步原始版本及桌面掩码，独立于编译 SDK。

## 批量处理范围与剩余项

下表是实际加载时的全量拒绝项，不代表全部都会被当前离线流程调用。
在线库的拒绝不能直接认定为“不阻塞”；必须结合 sysmodule 失败处理和后续实际流程。
本批优先图形崩溃、存档本地闭环，然后继续当前游戏主流程；不扩展真实网络/SSL。

| 库 | 数量 | 完整函数族/入口 |
|---|---:|---|
| libSceAjm | 3 | sceAjmFinalize, sceAjmBatchErrorDump, sceAjmModuleUnregister |
| libSceAudioIn | 3 | sceAudioInOpen, sceAudioInClose, sceAudioInInput |
| libSceGnmDriver | 5 | sceRazorCaptureSinceLastFlip, sceRazorCaptureCommandBuffersOnlySinceLastFlip, sceRazorIsLoaded, sceRazorCaptureImmediate, sceRazorCaptureCommandBuffersOnlyImmediate |
| libSceImeDialog | 5 | sceImeDialogGetStatus, sceImeDialogInit, sceImeDialogTerm, sceImeDialogAbort, sceImeDialogGetResult |
| libSceMouse | 4 | sceMouseInit, sceMouseOpen, sceMouseClose, sceMouseRead |
| libSceMsgDialog | 4 | sceMsgDialogUpdateStatus, sceMsgDialogOpen, sceMsgDialogTerminate, sceMsgDialogInitialize |
| libSceNet | 1 | sceNetResolverStartAton |
| libSceNpAuth | 4 | sceNpAuthDeleteRequest, sceNpAuthGetAuthorizationCode, sceNpAuthCreateAsyncRequest, sceNpAuthPollAsync |
| libSceNpCommerce | 4 | sceNpCommerceDialogInitialize, sceNpCommerceDialogOpen, sceNpCommerceDialogUpdateStatus, sceNpCommerceDialogTerminate |
| libSceNpCommon | 2 | sceNpCmpOnlineId, sceNpCmpNpId |
| libSceNpManager | 1 | sceNpNotifyPlusFeature |
| libSceNpMatching2 | 15 | sceNpMatching2KickoutRoomMember, sceNpMatching2LeaveLobby, sceNpMatching2LeaveRoom, sceNpMatching2JoinRoom, sceNpMatching2SetRoomMemberDataInternal, sceNpMatching2GrantRoomOwner, sceNpMatching2SetRoomDataInternal, sceNpMatching2SearchRoom, sceNpMatching2JoinLobby, sceNpMatching2SetRoomDataExternal, sceNpMatching2GetWorldInfoList, sceNpMatching2SignalingGetConnectionStatus, sceNpMatching2SignalingGetPingInfo, sceNpMatching2GetLobbyInfoList, sceNpMatching2CreateJoinRoom |
| libSceNpProfileDialog | 5 | sceNpProfileDialogTerminate, sceNpProfileDialogGetResult, sceNpProfileDialogInitialize, sceNpProfileDialogUpdateStatus, sceNpProfileDialogOpen |
| libSceNpScore | 14 | sceNpScoreAbortRequest, sceNpScoreCensorComment, sceNpScoreGetRankingByNpIdPcId, sceNpScoreDeleteNpTitleCtx, sceNpScoreGetRankingByRange, sceNpScoreCreateNpTitleCtx, sceNpScoreGetBoardInfo, sceNpScoreRecordGameData, sceNpScoreSetPlayerCharacterId, sceNpScoreDeleteRequest, sceNpScoreCreateRequest, sceNpScoreSanitizeComment, sceNpScoreGetGameData, sceNpScoreRecordScore |
| libSceNpSignaling | 7 | sceNpSignalingActivateConnection, sceNpSignalingInitialize, sceNpSignalingCreateContext, sceNpSignalingDeactivateConnection, sceNpSignalingTerminate, sceNpSignalingGetConnectionStatus, sceNpSignalingDeleteContext |
| libSceNpUtility | 7 | sceNpLookupCreateTitleCtx, sceNpLookupCreateAsyncRequest, sceNpLookupNpId, sceNpLookupPollAsync, sceNpLookupAbortRequest, sceNpLookupDeleteTitleCtx, sceNpLookupDeleteRequest |
| libSceNpWebApi | 17 | sceNpWebApiGetHttpResponseHeaderValueLength, sceNpWebApiReadData, sceNpWebApiInitialize, sceNpWebApiAbortRequest, sceNpWebApiRegisterPushEventCallback, sceNpWebApiGetHttpResponseHeaderValue, sceNpWebApiDeleteContext, sceNpWebApiTerminate, sceNpWebApiGetHttpStatusCode, sceNpWebApiSendRequest, sceNpWebApiDeleteRequest, sceNpWebApiUtilityParseNpId, sceNpWebApiUnregisterPushEventCallback, sceNpWebApiCreateRequest, sceNpWebApiCreateContext, sceNpWebApiCreatePushEventFilter, sceNpWebApiDeletePushEventFilter |
| libScePosix | 2 | rmdir, getpagesize |
| libSceSaveData | 5 | sceSaveDataGetSaveDataMemory, sceSaveDataSaveIcon, sceSaveDataDirNameSearch, sceSaveDataSetSaveDataMemory, sceSaveDataSetupSaveDataMemory |
| libSceSystemService | 1 | sceSystemServiceLaunchWebBrowser |
| libSceVoice | 11 | sceVoiceStart, sceVoiceInit, sceVoiceStop, sceVoiceGetPortInfo, sceVoiceEnd, sceVoiceWriteToIPort, sceVoiceDisconnectIPortFromOPort, sceVoiceDeletePort, sceVoiceReadFromOPort, sceVoiceCreatePort, sceVoiceConnectIPortToOPort |
| libkernel | 26 | sceKernelMlock, _exit, mmap, sysctl, __elf_phdr_match_addr, scePthreadMutexTimedlock, sceKernelDebugRaiseException, __Ux86_64_setcontext, __stack_chk_fail, munmap, signal, sigfillset, sceKernelPrintBacktraceWithModuleInfo, sceKernelTruncate, sigprocmask, _is_signal_return, sceKernelGetModuleInfoFromAddr, getrusage, getpagesize, sceKernelGettimezone, __pthread_cxa_finalize, sigreturn, sceKernelRmdir, msync, sceKernelReleaseFlexibleMemory, sceKernelDebugRaiseExceptionOnReleaseMode |

## 验收与限制

工作区包含之前多轮未提交修改，本轮继续保留。没有新 spec、提交、推送或完整回归。
构建/运行日志暂存 `build/bloodborne-runtime-20260917/`，最终结果在本文件更新。
翻页、进程存活、未调用在线接口均不等于游戏已可玩。

## 28 版普通 APK 实际观察

PID 20130 / generation 1 已越过旧 pipeline 必现崩溃，持续翻页，黑屏时约 30 FPS。之后出现灰白画面（用户同时确认），约 18–19 FPS。仍无可操作游戏图像，不能宣称游戏跑通。UI Stop 得到真实 Cancelled 终态。

[Linker 实际重定位清单](bloodborne-20260917/imports-after-save-batch.json)：861 行中 runtime_bound 474、guest_export 227、refused 145、not_relocated 15；拒绝符号去重 141。`runtime_bound` 只说明桥接/回退解析成功，不表示该接口已被游戏实际执行。

存档测试最初误把 CREATE 与 CREATE2 同时设置，Mount 拒绝后测试又直接 file_size 导致测试程序退出；修正测试输入并保留失败日志，生产存档未因此修改。SYSTEM_VER 测试同步真实 PSF 策略后完整 172/0。

## 黑屏/灰白帧的 RenderDoc 定位

同一普通 APK 会话 PID 23158 / generation 1 / UUID 93f480bf16fe452b3caa33e925e9dce7 捕获两个完整 guest flip：黑屏 1327→1328（38,790,508 bytes），Options 后 3576→3577（49,558,427 bytes）。SHA 和 receipt 在 `build/bloodborne-runtime-20260917/`。正常 Stop 后关闭 Android debug layer；在同一 Adreno 740 / 高通 0676.53 上远程回放，未用 macOS GPU 回放。

第二帧事件 247 的纹理 695 已有完整 Bloodborne 标题图。事件 307 的片元程序 688 读取它及三层 1D LUT 350，输出到显示纹理 333；事件 318 将 333 合成到 Frame image 322，之后进入带 StatusLayer 的 swapchain。原事件 307 输出全黑。该 RDC 没有复现普通运行中用户所见的灰白色，不能称两者像素等价。

捕获 XML 证明 draw 为 3 vertices × 1 instance、RectList 对应 patchControlPoints 3、scissor 1920×1080、viewport 16384×16384、无 cull/discard，color write RGBA。原生 MCP 的 action 数量字段错误报告 0，pixel_history 也曾返回零修改；这两个结果不能作为“没绘制”的证据。真实片元常量红替换证明目标像素可正常写红。

逐项替换原 SPIR-V，并在每次替换后切换事件再执行 307：

- 原码：中心标题像素 (0,0,0,1)。
- 仅移除 `DenormFlushToZero 32` 的 execution mode / capability：恢复 (1,1,1,1)，完整标题图 PNG SHA `7c1759cf486d8a592abf7c2139453cae43699ffacd58ea206f32c1bf51435aeb`，已实际看图。
- 仅移除 `SignedZeroInfNanPreserve`、改成整体 vec4 store、改 signed coordinates、移除可选 None operands：仍全黑。
- 回到原码：再次全黑。原图全黑 PNG SHA `e320cebac9889dbcf22003cd5435c75b522b3a3f6a16b0db8a052fe3d6e471c6`。
- 辅助 TCS/TES 去掉未写 BuiltIn 的实验无效，未应用到生产代码。

[完整替换矩阵](bloodborne-20260917/float-controls-rdc-matrix.json)；原/仅移除 FTZ 两个 SPIR-V 一并归档。所有替换均恢复，capture/debug session 关闭。回放连接有 25 秒空闲超时；失败调用保留，没有把失败当作正常空结果。

生产修复在 `vk_pipeline_cache.cpp`：仅 Qualcomm proprietary driver 的 `support_fp32_denorm_flush` 置 false，其他浮点控制保留；Turnip 和其他驱动不变。参考 Citron `vulkan_device.cpp` 对同驱动禁用全部 float controls 的既有兼容，本次收窄到实际确证模式。已有 Shader::Profile 比较使含旧模式的缓存不再复用，不需要删除用户缓存。该路径使用默认 FP32 denorm 行为，不声明严格 subnormal/IEEE 行为验收。

29 版已构建安装：host SHA `50acf7b888416ff1cb105319ecea9cca6924e4b63c0abc70c269d8b9578df3cd`，APK SHA `872100535544782d69d1268db01d2dac5215e819b054f1bc7acc719668cd265c`；Host/JNI RelWithDebInfo、FEX Release、playstoreDebug。普通运行 PID 20454 / generation 1 / UUID 3f056920ce7c57cce8bc20b1994c75d4 已显示完整标题图约 30 FPS；Circle 后仍出现灰白约 18 FPS。因此只验收标题合成修复，灰白及菜单文字仍未修复。UI Stop 确认 Stopped/user_stop。

第三帧 `bloodborne-grey29.rdc`（SHA `3b0f5c22f13a5af4f6f045a3eec5b4d470bc40125d81eadf8142094f4878707f`）来自 PID 22298 / generation 1，flip 1073→1074。文件名和 warmup label 含 grey，但实际检查 event 322/resource 819 及 event 347/resource 195 都是完整标题图，没有菜单文字；它不是灰白现场证据。保存原始 label 并在这里纠正。

同 PID generation 2 再次启动观察到灰白；捕获请求 2 在 flip 1226→1227 结束后进入 writing，随后超时 cancelling/cleanup_pending。线程 27023 停在 adreno_drawctxt_wait，27029/27031 在 dma_fence_default_wait；Android 截图与正常停止也不再返回。记录 `rdc-grey-receipt30*.txt`、`threads30.txt`、`status30-stuck.txt` 后强制停止进程恢复。没有完整新 RDC，不把该抓帧后 GPU 停顿归因于原始白屏。

## 切 Turnip 的同版本普通运行

用户要求先切 Turnip。保持上述 APK 不变，完整重启进程，`debug.shadps4.vulkan_driver=turnip`，清除 RenderDoc debug layer，guest debugger 端口/等待均为 0。实际 PID 1142 / generation 1 / UUID d39662b1752504fd79d1bf85148dca4b，驱动 Mesa 26.0.0-devel (git-5ac41be677)、shaderInt64=1、SHA `fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`。`renderdoc_status` 确认 API 未加载。

截图 `screen-turnip31b.png` 确认 Play Online / Play Offline 菜单文字出现，约 30 FPS；`screen-turnip31e.png` 已推进到开场人物剧情及英文字幕，约 16–17 FPS。之前高通同版只见背景/灰白不能代表流程停在标题。当前仍未验收角色创建、可操作场景或存档闭环。


`screen-turnip31f.png` / `screen-turnip31j.png` 确认进入 Contract 角色创建：人物、姓名/性别/年龄/出身/外观菜单和属性均可见，约 10–11 FPS。Cross 能打开返回标题确认，Circle 能取消确认，证明输入实际被游戏消费。选 Enter Name 后 `SysmoduleLoad id=0x96 name=libSceImeDialog result=0x805a10ff`；当前缺少 IME provider，游戏未调用被拒绝的导入即返回角色创建，因此没有 Faulted 终态也不代表输入名字成功。下一项应按完整 IME 对话框族处理 Init/GetStatus/GetResult/Abort/Term、文本输出、Android UI 与 session 取消，不应仅让 Load 返回 0。

[Turnip 实测证据清单](bloodborne-20260917/turnip31-artifacts.json)保留普通 APK 身份、截图及上述日志。测试结束时保留 PID 1142 / generation 1 角色创建界面供用户查看，Turnip 选择保持；自动输入已结束，TracerPid=0，RenderDoc API 未加载，未作新抓帧。还没有填入姓名、创建存档或进入可操作游戏场景，不能宣称全流程完成。
