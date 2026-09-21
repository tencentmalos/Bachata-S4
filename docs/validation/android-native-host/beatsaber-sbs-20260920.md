# Beat Saber：AYN SBS 传输、停止修复与批量逆向验证

2026-09-20，`feature/malos/beat_saber_fix`，基线 `63b6d560`。用户指定 AYN 和 SBS；本轮未用 Swan，未接 OpenXR，未 commit/push。

## 当前结果

**SBS 传输和停止阻塞已有实现与定向验证；Beat Saber 仍未出现有效双眼场景。**
真实 AYN 截图仍黑。不能将约 60 次/秒的空画面呈现称作游戏 60 FPS、菜单可用或可玩。
启动仍使用精确版本的临时 VR 偏好诊断 patch；没有恢复 managed 层自然选择 PSVR 的完整流程。

- `SubmitVrFrame` 原来丢弃传入眼图，采样登记的 reprojection 输出 buffer。现在直接交给 `PrepareVrFrame`，输入释放回调在 GPU 完成且对应 host submit 返回后执行，不再以 Present 当作读者完成。
- 原 `PrepareVrFrame` 固定 `sbs=0`。现在在既有一次 post-process draw 内处理独立眼视图／单层数组视图，基础层和预乘 alpha 叠加层分别选择共享 UV 或分屏 UV。没有增加眼图复制、warp pass、CPU 回读或正常运行的同步等待。
- 判断依据是完整 cache view，包含数组 slice 与 swizzle；相同分配的不同 slice 不会混为同眼。相同 view 保留原 UV，**不是断言共享图一定为双眼 atlas**。当前游戏两眼 descriptor 同址、Color2D、1920×1080，而画面全零，尚不能证明其布局内容。
- VR 路径补齐 Guest capture boundary；仅 capture 活动时 drain 已提交工作，与普通 VideoOut 同协议。
- 修复 Android EventFlag 不能被 session Stop 唤醒的问题，并与桌面共用状态实现，纠正 Clear keep-mask、Cancel 替换 pattern/实际 waiter 数量、Poll EBUSY 语义。

## 精确身份与导入盘点

AYN Thor `9c2841a4` / Adreno 740 / Android 13。ROM fingerprint：
`qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`。
Turnip Mesa 26.0.0-devel `5ac41be677`，driver SHA256
`fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09`。
保留 Render 0.5 / Texture High。没有改变驱动选择或执行 root 操作。

CUSA12878/01.00，原 SELF SHA `9ac1745407a97af986d5f6dfe246a7c0d695857f4049d7d717f896ed219778cf`；
解密 ELF SHA `71278f9c85ef93a9b2e8703299d6875020eb4d8f7b821bf98eb36ec73340592b`；
ET_DYN 分析副本 SHA `a0ea75288830ff4665c6ab65aa93210c1588155c2e543bddbac48e45a3c5d04a`。
只修改 e_type 两字节，PT_LOAD/IDA image base=0，运行模块基址=0x400000，不能混用。

[最终完整清单](beatsaber-sbs-20260920/final-imports.json)共 1164 条：636 runtime_bound、347 guest_export、167 refused、14 not_relocated。
实际 importer 为 eboot 702、Il2Cpp 282、libc 91、Fios2 81、PS4Util 8。
Hmd/Tracker/Camera/Move/VideoOut 共 70 条均 runtime_bound；这只证明绑定，不代表完整语义。
[按族清单](beatsaber-sbs-20260920/family-audit.json)保留所有拒绝项：包括 GNM 38、Posix 34、kernel 30、Audio3d/SocialScreen 各 7 等。
这些项没有在本轮观察窗口触发 Unsupported fault，**其后续可达性仍未知**；不能把尚未运行到当作全库已完成。

## EventFlag 停止阻塞：实栈与修复

基线 PID11548/gen1、UUID `f1cf2910855ade2448666e329f717088`，APK `82607da9…` / host `7b6345cd…`。
约 8014 guest flips 后 UI Stop 长时间处于 Stopping。实际 LLDB 栈：

- Guest-1 / TID13005 在 `GuestRuntime::Impl::~Impl` join workers（旧源码 guest_runtime.cpp:734）。
- Guest-14/15/16 / TID13048/13049/13050 在 EventFlag CV predicate wait（旧源码 :1856/:1915）。
- Liverpool GPU 命令线程和 timeline completion 线程分别闲置等待自己的队列；没有 GPU fence 卡死证据。

旧 EventFlag wait 只等待 pattern 命中，没有 HLE cancellation token，因此 owner join 无法结束。
新 `EventFlagState` 从桌面提取同一套位条件、FIFO/priority 和清除语义，用 stop-aware CV 唤醒取消者。
Android 的 guest 指针仍由桥接检查并复制；不传入本地状态类。metadata 锁只管理共享对象引用，不跨 wait。
Delete 先从 handle map 移除再唤醒持引用的 waiter。早期拒绝不覆写 result pattern。
本次没有重定义桌面 priority 规则或补齐 EventFlag 所有扩展接口。

原生 debugger 会话 `be3d72abe5664a0d8990087412901d5f` 已清理，TracerPid=0。
证据位于 `build/validation/beatsaber-sbs-20260920/baseline-stop-debug/`，manifest SHA
`b0de310d31714868777865e94cfefa01f0d29921f470bd4d77c419d282d8d718`。
首次 LLDB 自动版本发现失败，后指定现有 AGDE LLDB21 才成功；没有把失败 attach 作为栈证据。

## 黑眼图：RenderDoc 的实际证据

直接眼图版 PID21992/gen1，Host interval capture SHA
`7385441befb2108c2aa6e5c8a049734598a0f871a810884c4de124c57ad71614`，33,274,956 bytes。
这版尚未补 VR Guest capture boundary，因此首次 guest capture 明确以 `capture_deadline_exceeded` 失败；随后 host interval 成功。

最初回放错误是 Qualcomm ICD 缺少 `VK_KHR_workgroup_memory_explicit_layout`。
使用设备已有私有 Turnip replay loader，并校验同一 driver SHA 后，AYN 远程回放成功；没有在 macOS 回放 Adreno RDC。
`eyes.rdc.db` 物化 41 actions、119 resources、13 textures、8 pipeline outputs；深层 pipeline 使用后续显式远程会话读取。

- resource **516**：guest VA `0x229c90900`，RGBA8 sRGB，guest1920×1080 / host960×540。
- Guest draws **98、109** 将 516 作为颜色附件。draw109 的输入 HDR texture 为 resource495。
- Host draw **120** 的四个 sampler bindings 均为 516，输出 frame image **532**。
- `resource_usage(516)` 同时证明 98/109 写入、120 读取；没有依赖纹理名字猜测链路。
- `review_texture(109,516,mip0,slice0)` 和 `(120,516,mip0,slice0)` 均完整导出并查看 image blocks：RGB 全零、alpha 全零；原 PNG SHA 相同为 `3826f4fa…`。532 也为全零。

因此在这帧内，黑色输入已存在于 Host 合成之前；修改分屏坐标不能产生缺失的场景。
这还不能区分 Unity 没有提交有效场景、Guest shader/状态生成错误或更上游 HLE 条件缺失。
Host interval 跨异步生产/呈现，不宣称 532 与该次 Android present 完全等价。
原有 scrcpy 会话在远程 replay 切换后曾保留旧帧，已与 adb screencap 对照并关闭，最终截图使用实际设备 screencap。

## 验证与最终设备状态

| 项目 | 结果与覆盖 |
| --- | --- |
| EventFlag，本机 / AYN | 各 **1205 / 0**：AND/OR、keep-mask、clear bits/all、poll/timeout、FIFO、Cancel、Delete、Single 拒绝、输出写回、64轮×8 waiter 的 Stop 注册竞态 |
| Reprojection transport，本机 | **31 / 0**：修复测试夹具缺失 set_active，覆盖取得/释放呈现权、延迟退休、Stop 和重新初始化 |
| SBS production shader，AYN Qualcomm / Turnip | 各 **6144 / 0**：独立数组 slice、共享双彩图、基础/overlay 混合布局、预乘 alpha、Y 翻转，GPU copy/readback逐像素核验 |
| Host / APK | Android RelWithDebInfo 构建成功 |
| 最终 VR Guest capture | PID592/gen1，guest flips **1806→1807**，Ready / completed1 / cleanup_pending=false；SHA `bee2e79a54724f072d0715fea4cfc7dbd37dcf7e3d56dfef7ab43682fd726abb` |
| 普通运行（无 RenderDoc） | PID2598/gen1 / UUID `d7d73c9320509b49dbfb983ba08b59c1`，3764 presents 时检查仍黑，尚无菜单/操作验收 |
| 最终 UI Stop | **Stopped / user_stop**，点击到观测约 **0.522s**；guest return=`0x80020004`（EINTR），不能记为 return0 |

GPU 像素用例使用 RGBA8 UNORM、线性输出（HDR 分支）检验布局与混合，不替代真实游戏的 sRGB/HDR 画质验收。共享视图用例只检验保留 UV，不证明游戏的共享眼图是 atlas。

最终 APK SHA `45101e2f1602020e5f530cfaf87965543150add97727655db46d1c57689cf3ca`，
host `9259c10f8da0f92f806c43eeb51d2e6bdc740d9f16970d2aeec46d24d0cd15ff`，
JNI `6de7a2ee4fe25ffa4e30fa9ce6454f512d5ecc60f729d5d6ad60cb09513ae0da`。
此前 EventFlag 首个修复包 PID20943 Stop 返回0；最终包保留实际 EINTR，不混用验收结果。

最终无活动游戏、输入注入、scrcpy、replay 或 debugger；TracerPid0，RenderDoc API未加载，四项层设置恢复 null，replay loader property 清空。
临时 VR 偏好 patch SHA `82f6aeb2…` 在测试中启用，结束后清空 next-session property；包与证据文件保留。
没有修改用户驱动、画质偏好；未做全游戏回归或性能结论。

## Reverse Study 新批处理的实用边界

实际使用 `0.3.0-local.20260920.batch1`，本地实现 `025ee978`。
初次 manifest 写成 ELF64 导致 loader identity gate 失败，未导出；核实 capabilities 为 ELF 后重建项目。

| job | 结果 |
| --- | --- |
| `06a3f97…` F1 | 97 requested，40 completed，57 `FUNCTION_CHUNKS_UNSUPPORTED`，4 shards；frontier237，snapshot40 |
| `e7df351…` F2 | 四个根全部导出，4 native exports，snapshot44；frontier273，所以总状态仍 partial |
| `ece1501…` 相同 F2 热缓存 | 4 hits，**0 native exports**，snapshot48；没有将重复 revision 当作新增函数 |

97 次 F1 native attempt 含失败，不是97个成功分析函数。57个非连续 PLT/thunk 函数不是“没有调用者”。
本轮验证了 exact identity、分片、持久化和热缓存；没有验证崩溃后恢复、跨模块动态绑定图或批量类型写回。
工具仍只有单活动 IDB，不能并发占用互动 study 与 batch。

既有 v2 symbol index 升到 revision2，新增静态 correlated 名称：
`eboot+0xd6c400 — UnityPSVR_CreateEyeRenderTextures`、`eboot+0xd6ae40 — UnityPSVR_ReprojectionThread`。
13 symbols、2 shards，exact source/runtime SHA 的 `symbols_prepare(dryRun=true)` 通过；未知 ABI 未编造类型。
[批处理状态与事实 SHA](beatsaber-sbs-20260920/batch-evidence.json)保留失败、frontier 和具体 offsets。

## 下一批的明确入口

1. 在精确 Il2Cpp 模块中批量标定 VR 选择、初始化完成、场景/相机启用与首个有效场景提交的链路；将本轮两个 eboot 根作为跨模块连接点。诊断偏好重排不能成为无证据的正式激活逻辑。
2. 在已可用的 Guest capture boundary 上，从 516 的上游 HDR495 和场景 submission 开始定位零输出；补足 shader/常量/视口与 guest 调用链证据后再修改游戏/HLE。
3. 恢复 layer.root20、共享图 atlas 布局和可选 overlay 的确切 guest 合同，并在有实际场景时验左右眼视差。当前虚拟 HMD/固定 Camera/Move 不是完整 VR 控制或光学追踪。
4. 继续按完整拒绝清单逐功能族闭环；不能因为本轮没有触发便删除或宣称无依赖。

全部证据入口：[manifest](beatsaber-sbs-20260920/manifest.json)。大文件与原生调试导出留在 manifest 指向的本机 build 目录。
