# Android 菜单与实际实现核对及精简（2026-09-19）

## 结果与范围

Settings 改为 **Graphics / Controller Buttons / Touch Layout** 三个入口。原运行时目录有 88 项 shadPS4 参数和 118 项 Box64 参数；当前保留的运行时设置只有 Internal Scale、Shading Quality 两项。不是将未实现选项置灰，也没有顺带实现这些功能。

全局及单游戏 Settings 共用同一入口和有效设置目录。移除首次启动、设置页、游戏启动失败页中的旧驱动管理路由，Play 与 F-Droid 都不再展示旧驱动选择器。游戏库的导入、更新/DLC、启动、移除、方向切换，以及会话的触屏显隐、系统内存显示、退出和诊断报告，沿用现有实际实现。

[206 项旧目录逐项核对表](evidence/android-menu-prune-20260919/catalog-audit.csv) 记录每一项的处理结果。判断标准是 Android UI → 配置存储 → 当前会话消费者的真实调用链，不能因为 C++ 桌面核心里有同名字段就宣称 Android 菜单生效。

## 保留的设置及消费者

| 保留项 | 实际链路 | 边界 |
|---|---|---|
| Internal Scale：0.5 / 0.75 / 1.0 | `InternalScale.resolve` → `FexSessionService` → `nativeSetInternalScalePercent` | 默认 0.5，重启游戏生效；全局/游戏覆盖；资源限制见此前 internal scale 报告 |
| Shading Quality：Low / Medium / High | `GuestShadingQuality.resolve` → `nativeSetGuestShadingQuality` → guest fragment shading rate | 默认 High / 1×1，设备不支持时回退；FDM 仍关闭；不承诺每次 draw 都适用 |
| 控制器 1 按键映射、重置、面键翻转 | `SessionScreen` → `NativePadBridge.configureProfiles` → `NativeButtonMapping` | 支持数字按钮和 D-pad HAT；普通模拟轴不走映射；下次游戏启动读取 |
| 触屏位置、大小、可见性、层级、透明度、整体比例、摇杆固定中心 | `TouchLayoutRepository` → `FixedControllerOverlay` / `TouchControllerState` | 由实际布局渲染和触摸命中读取；编辑后保存，下次启动读取 |

设置编辑现在显示真正的全局继承值，单游戏取消覆盖后也随全局设置刷新。控制器和触屏编辑器打开继承配置时，同样先读取全局值；触屏的“继承全局”只在单游戏范围出现。

“Map All Buttons”只收集原生适配器支持的数字目标；不再把模拟摇杆/扳机轴列入队列。原生映射与 UI 校验共用 `NativeButtonMapping.supports`，遇到未支持的轴不会写入一个运行时无效的映射。离开页面会停止按键捕获。

## 移除的菜单族

| 菜单/选项 | 移除依据 |
|---|---|
| CPU、FEX/Box64 选择、全部 118 项 Box64 参数 | Service 固定执行原生 FEXCore 会话，没有应用 `guestBackend` 或 Box64 环境目录 |
| SDL/OpenAL 后端及各音频设备选择 | 当前 Android 音频后端不读取这些 profile 项 |
| 桌面窗口、全屏模式、窗口宽高、内部宽高、FSR/RCAS、HDR、Present Mode、VBlank 等旧 GPU 参数 | Android profile 未应用到核心；窗口跟随 Surface；保留的 Internal Scale 有独立显式 setter |
| Direct Memory Access | 它是 **GPU/着色器核心设置，不是 FEX CPU 开关**。桌面消费在 SPIR-V emit / shader info collection；Android 菜单值未传到该设置 |
| Neo/DevKit、网络/UPnP、Discord、目录、奖杯弹窗、旧 FPS/音量参数 | 当前 Android 会话未消费对应 profile 项；目录和会话叠层另有真实管理链 |
| 旧 Input、Log、Vulkan validation/RenderDoc/pipeline cache 等目录项 | 菜单保存值不等于原生控制生效；已有 DebugBus/诊断路径保留 |
| RAW 配置编辑、运行时配置导入/导出 | 旧通用配置暴露和搬运大量失效字段，已删除对应编辑页面/模型与入口 |
| Drivers：下载/ZIP 导入/切换、Vortek、Mali 优化 | 页面操作 `vulkan-drivers/installed` 与 profile.driverId；实际启动调用 `AndroidTurnip.prepare` 的独立 bionic 包；系统/Turnip 诊断选择由 JNI 的 `debug.shadps4.vulkan_driver` 决定。旧页面选择不生效 |
| 控制器 Dead Zone、Trigger Threshold、Invert Axes、Vibration、Motion Controls | 当前 `NativePadBridge` 未读取这些配置字段；模拟轴走 Foundation/native 标准化，传感器与震动有独立链路，不能用无效 UI 宣称可配置 |
| 触屏 Vibration Feedback | overlay 未消费 `layout.vibrationEnabled`；移除开关 |
| 旧运行时资产下载 | 原生 FEX 已随 APK 构建；删除不可达的 glibc ZIP 下载/解压动作及注入开关 |

## 配置和代码处理

- 新增 Android 专用 `runtime-settings/android.json`，Settings 不再自动展示桌面/Box64 全量目录。增加菜单项必须同时核对当前消费者。
- 删除旧 RAW/ProfileTransfer 页面逻辑、旧分类导航和下载注入；取消 App 到旧驱动 UI 的全部路由。
- 未删除用户已有 JSON 中的旧字段、驱动包或手柄配置。编辑支持项只更新指定字段，不进行破坏性迁移。历史目录和通用解析仍保留，旧驱动模块源码暂未整体删除，但没有可达 UI 入口。
- 新目录中的着色项 `nativeKey` 使用 `GPU.guest_shading_quality`，不再沿用容易误解为 FDM 控制的旧描述。实际消费者仍为现有独立 setter。
- 前一轮内存诊断的未提交修改保留，本轮不修改 GPU 内存分配策略、FEX 执行或游戏保存数据。

## 验证

构建与测试结果、APK 身份见同目录 evidence 中的 `validation.json`。验证重点是 Android Settings 配置继承/保存、拒绝旧目录和非法值、原生数字按键映射，以及两种 App 变体的导航编译。未做全游戏或性能回归。

设备检查时，AYN Thor `9c2841a4` 正在 TMNT 实战（PID 9572 / generation 1，Turnip，Surface 1920×1080 / x0.5）。为保留正在运行的游戏，本轮先生成 APK，不默认结束游戏或覆盖安装；未将编译通过描述成新菜单已在设备验收。
