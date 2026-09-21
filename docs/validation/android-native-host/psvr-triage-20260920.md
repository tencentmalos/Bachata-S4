# AYN 三款 PSVR：启动修复、画面对比与推进建议

2026-09-20，`feature/malos/beat_saber_fix` / 基线 `63b6d560`，未 commit/push。目标设备是 AYN Thor `9c2841a4`，保持 Turnip `5ac41be677`、Render 0.5、Texture High、SBS 开启。两款新游戏继续直接读已核 SHA 的 ZAR，没有重新散文件安装或修改游戏内容。

**建议下一轮先修 Beat Saber 的双眼分离和 Move 指向。** 本轮已找到并修复其画面被 Alpha 遮黑的原因，最终版能持续看到正向背景、安全提示和光剑，但仍双眼重影、无法完成 Continue 交互。另两款还停在启动阶段。三款均未达到可玩验收，屏幕 FPS 不是性能 A/B。

| 游戏 | 最终 APK 实测 | 当前具体边界 | 建议下一批范围 |
|---|---|---|---|
| Beat Saber，CUSA12878 / 01.00 | PID9732/gen1，背景、HEALTH AND SAFETY WARNING、Continue 和光剑可见；上下方向正常 | 双眼图像叠加，尚非正确 SBS；触屏 Cross 和 R2 被输入桥接受，但未离开提示页 | 同帧追踪 guest 两眼 viewport/scissor、投影、RT/slice/atlas 与 HMD 提交的关系；随后验证 Move 射线命中与 trigger/confirm |
| AllInOneSports，CUSA36289 / 01.02 | PID8079，越过原 SaveData 空参数初始化；仍在首帧前 SIGSEGV | 实际 guest `fread+0x54` / libc+0xa9f4，访问地址 1；调用者和参数来源尚未证明 | 从实际调用点核对 fread 四参数、fopen 返回、数据路径和模块初始化次序；不能把本次新崩溃归因 Turnip 或 ZAR 损坏 |
| Tetris Effect: Connected，CUSA13427 / VERSION02.05、APP_VER01.00 | PID8895/gen2，EOS 解析成功、Prepare ready，进入 Run 后具名拒绝 | `sceKernelAllocateMainDirectMemory` 首次调用未接入，无场景 | 成组处理 DirectMemory 分配/查询/checked release，以及 MemoryPool reserve/expand/commit/decommit；先复用桌面语义与现有 VM 退休/映射协议 |

[最终 Beat Saber 截图](psvr-triage-20260920/beat-final-screen.png)；[Tetris 最终黑色输出](psvr-triage-20260920/tetris-final-screen.png)。AllInOneSports 在绘制前崩溃，没有可供比较的游戏场景。

## 加载器改动及边界

### AllInOneSports：区分链接依赖与初始化依赖

旧 PID23598/23735 的 FEX fatal 记录一致指向 `0x1056850ca`。实际 SaveData 基址为 `0x105684000`，对应 SaveData+0x10ca；exact ELF SHA `c1cc801715cbd09a52498754321d6458da3f50fa730b7231773a6789e7d37810`。Reverse Study 证明 +0x10c0 是 Unity 插件参数版本检查，进入时先解引用第二参数；唯一静态直接 caller 是 +0x2b60。运行日志证明调用发生在启动 DT_INIT 序列，未等 Unity 的 LoadStartModule 参数。

`Media/Plugins` 模块现在始终等待实际 LoadStartModule 参数，即使它已经被 Il2Cpp 的 DT_NEEDED 引用。完整链接图仍用于预映射、TLS 和重定位；初始化图则排除通向这些插件的自动启动边，并重新计算真正的启动根顺序。避免只从启动列表删除插件、却被父模块依赖递归再次用空参数启动。普通依赖仍按序初始化；重复、并发、取消、等待环及 pRes 原有语义保留。

新 PID1982、最终 PID8079 的 Fios/libc/PS4Util/Il2Cpp 启动成功，不再在 SaveData 检查处崩溃；两次均在 `0x1043d29f4` 发生新的 SIGSEGV。真实绑定清单中的 fread 地址为 `0x1043d29a0`，静态导出范围覆盖故障地址。**这只证明越过旧失败点，不证明 SaveData 已实际收到描述符或存档流程完成。** fread/fopen 均绑定游戏 libc 导出，尚无证据支持混用 host FILE 的结论。libc 在当前 Reverse Study 导入中未生成函数索引；保留 NOT_IN_FUNCTION，未猜调用链。

### Tetris：有约束的模块名兼容

游戏导入 `EOSSDK-PS4-Shipping.debug_prx`，ZAR 中实际文件为 `prx/eossdk-ps4-shipping.prx`。该 ELF 导出模块身份为 `EOSSDK-PS4-Shipping`；不是打包漏文件。

Prepare 增加 `prx` 搜索目录；优先精确文件名，失败后只做 ASCII 大小写与 `.debug_prx`→`.prx` 规范化。候选必须唯一、位于安装根内，加载后必须核对导出的模块名，否则失败；没有模糊子串匹配。动态 LoadStartModule 的同目录已准备模块使用同样的身份规则。挂载视图下 `prx` 的相关 Wwise 模块也预备进完整图，未被启动根引用者延迟初始化；没有任意 live TLS 或动态卸载实现。

最终 2,603 条完整绑定记录含 EOS 的真实 guest_export；Prepare ready 后的首次实际失败为 `B+vc2AO2Zrc`，不再是缺文件。该失败发生于 Run 的早期，不能声称 EOS 初始化、Wwise 或游戏启动完成。完整内存族缺口已列入清单，未采取“只补第一个函数再跑”的办法。

## Beat Saber：Alpha 根因与画面证据

基线 RDC SHA `5bfe8050da75d0dedef655aa832b32df68d89a898b8e4f074951893a6ab71c85`，PID14211/gen1、guest flip3177→3178。复用已成功在 AYN Turnip 回放的离线库，没有在 macOS 本地回放 Adreno RDC。

- event1497/resource346 实际名为 **Frame image #1**，是 host 后处理输出。离线库自动标记 `guest_target` 不准确，本报告没有把它当成原始眼图。RGB 含背景、提示面板和光剑，Alpha 的绝大部分为零。
- event1522/resource195 是 Swapchain Image 3，已主要变黑，仅剩与 Alpha 非零区吻合的局部元素及 host UI。两张 PNG 均经 RenderDoc review_image 实际查看，RGB/Alpha 分开核对。
- 代码提供独立机制证据：PrepareVrFrame 保留基底 Alpha，post_process 再原样写出；Presenter 的 ImGui::Image 随后使用 SrcAlpha/OneMinusSrcAlpha。普通 VideoOut 已将采样视图 Alpha 设为 1，VR 路径遗漏了这一步语义。
- 修复为 VR 专用 opaque bit：先保持原覆盖层混合，再将最终输出 Alpha 置 1；普通输出语义不变。PID5953 普通运行实际恢复背景及提示，证明改善不限于合成测试。
- 最终版再对直接 HMD 纹理做一次 Y 翻转。PID9732 的安全提示、Continue 和光剑方向正常；但双眼叠加仍然存在，不能据此称正确 SBS。

完整选择器/身份见 [渲染证据](psvr-triage-20260920/render-evidence.json)，[基线后处理帧](psvr-triage-20260920/baseline-postprocess-frame.png) 与 [基线交换链](psvr-triage-20260920/baseline-swapchain.png)。离线 deep_pipeline/pixel_history 仍 pending，没有当作“无异常”；没有完整 HDR 或外部合成显示等价性声明。

下一步需围绕实际 guest 眼图重新取证。当前基线候选 resource1535 为单层 2688×1512 guest 图（host 1344×756），双眼/atlas 身份尚未证明。不能用“把同一张图左右复制”掩盖重影，也不能假定 shared view 就是完整 SBS atlas。

## 陀螺仪及已知服务问题

按用户后续要求恢复 HMD 陀螺仪模拟，覆盖此前固定正前方策略：每次会话从单位旋转起步，积分 display-aligned gyro，精确轴角增量、归一化，拒绝 NaN/Inf 和重复/倒序时间戳，超过 100ms 的间隔只重建时间基点。ResetOrientation 清空角速度/时间；双眼 63mm IPD 随头部旋转进入同一跟踪空间，camera 参考旋转仍为单位四元数。Move 位置仍为固定测试姿态，未把陀螺仪冒充位置跟踪。Android 陀螺仪使用设备自然坐标，已补屏幕旋转映射（[Android 官方传感器文档](https://developer.android.com/develop/sensors-and-location/sensors/sensors_overview)）。

数学与 Android 接入测试通过；设备已安装该实现，但本轮没有受控物理转动/长期漂移验收。Cross/R2 的 4 个触屏 press/release 事件被 native 输入桥接受、最终 buttons=0；这不等于 guest UI 已点击成功。

NP 初始化旧错误仍是 ENOTINIT `0x804101c8`，此前已静态追到 UnityNpToolkit2 Core::init 链，尚未改变网络初始化合同。LoginDialog `0xe2` 在最终运行仍因无 provider 返回 `0x805a10ff`。本轮没有伪造 provider 或在线成功；Alpha 根因能独立解释已确认的画面丢失，这些服务问题并未因此被标记完成。

## 全量盘点、验证与交付

静态清单覆盖两款 eboot、所有提取 PRX/SPRX、完整 NID、库/模块及导出。有效 Sports 使用 update 覆盖 app；未进入运行图的系统可选模块与零字节 `libSceSmart.prx` 明确保留，不作已加载模块。真实 Linker 清单另外记录实际绑定，不能把静态声明当成功。

| 最终游戏清单 | 行数 | guest_export | runtime_bound | refused 行 / 唯一 symbol | not_relocated |
|---|---:|---:|---:|---:|---:|
| Beat Saber | 1939 | 780 | 792 | 353 / 328 | 14 |
| AllInOneSports | 2303 | 969 | 907 | 416 / 365 | 11 |
| Tetris | 2603 | 1146 | 907 | 533 / 411 | 17 |

`runtime_bound` 包含经检查的 guest libc 转发，不全是 host HLE。本轮批量准入十项在两游戏真实 libc 中存在的函数：finite/inf/nan、ldexp/_FSin/hypotf、strtoll、bad_function_call、qsort、snprintf_s；仍要求唯一 provider 和 executable ELF 范围。拒绝项按完整功能族记录，含内存、SaveData2、NP/HTTP/SSL、平台/对话框、音视频、线程等；尚未完成这些族，未统一零返回。后续应先做族合同审计和桌面实现复用，而非逐个消除 Unsupported 列表。

证据目录包含三款 `*-final-imports.json` 与 `*-final-family-audit.json`、静态 [module-inventory.json](psvr-triage-20260920/module-inventory.json)。本地游戏 ELF、反编译内容和大型 RDC 留在 build/validation，不提交游戏代码。

- 生命周期测试 host/AYN 各 **245/0**；VR sensor/IPD host/AYN 各 **36/0**；Kotlin 旋转映射 **1/0**。
- 真 FEX/生产 Linker 既有模块 **9 轮**通过。GPU 生产 post-process shader，独立 slice/shared atlas、覆盖层、Y 翻转及 Alpha0/0.5：Qualcomm、Turnip 各 **12,288/0**。
- 最终 APK **1d939bc3…**，host **0c520ad8…**，JNI **bd04d977…**；三者设备安装文件与本地 SHA 一致，见 [完整身份](psvr-triage-20260920/final-binaries.json)。host/JNI RelWithDebInfo；保留既有提取 native 库安装方式，未宣称 APK 直接 mmap 打包检查完成。
- 三款最终 APK 已短测；没有全游戏回归、性能提升、长期稳定或可玩声明。旧 SaveData 崩溃、两次 fread 崩溃、Tetris 原缺依赖/新内存接口边界、最初两次 probe 路径参数错误均保留。

最终 PID9732/gen1 正常 UI Stop，Stopped/user_stop，5548 presents；guest return=2147614724（不是 0）。输入 token 归零、buttons=0，无活动游戏/采集/调试器，RenderDoc API 未加载、replay loader 属性空、guest patch/auto-tag 属性空（停止后状态为 session_retired）、TracerPid=0。保持用户的驱动、倍率、材质和 SBS 设置。
