# EmuPSP 独立在线内容应用 · 2026-10-14

## 分支和独立仓库

按用户要求，从维护主干 `origin/malos/main` 的 `e6b1e0f27a6e0882befc1f1d3cf6a2e8b85f9f06` 切出 `feature/online_shop_psp`。应用最终命名 **EmuPSP**，PSP 指 Pico Space Pro（Swan），不是 Sony PSP。

独立 private 仓库：[tencentmalos/EmuPSP](https://github.com/tencentmalos/EmuPSP)，首版源码 [aa62ce4e0ad941953e0de314171c41f02ba26b85](https://github.com/tencentmalos/EmuPSP/tree/aa62ce4e0ad941953e0de314171c41f02ba26b85)。Rust/Go、C ABI、JNI、桌面与 Android 客户端由该仓持有，无模拟器或 Foundation 构建依赖。

## 本仓变更

- 桌面大屏页变为 EmuPSP 外部入口；`--online-store` 通过 `emupsp://browse?platform=ps4` 启动独立应用，缺少处理器时报告错误。
- 移除桌面在线内容 SDK 链接、Go 库拷贝和旧 Mac 独立商店打包脚本；登录助手移入 EmuPSP。
- Android 移除 `foundation-online-content` Gradle 模块与未使用的 `OnlineContentStore`。导入面板通过通用 `io.github.tencentmalos.content.BROWSE` action，携 `protocol_version=1`、`platform=ps4` 打开内容应用。
- 既有系统文件选择器负责从 EmuPSP DocumentsProvider 选取完成文件；仍由 shadPS4 校验、安装和执行。未把内容凭证、安装路径、固件或密钥交给其他应用。
- Foundation 和 Mesa 的 gitlink 未变动；本地干净子仓已对齐所选主干的固定提交。其他 reference 子仓及 `references/Madeira/` 原有工作区内容保留，未纳入本次提交。

## 独立应用能力

Avalonia PC 与 Compose Android 的首页均先选 PS4 / 3DS / Switch，包含原创平台图标、独立 BG 和新 EmuPSP 图标。Title 卡片参考 Qt：图标、中文与原名、背景轮播、小/中/大卡片、独立 Icon 尺寸及持久显示偏好；在线目录和完成内容库复用。

迁入的 online 来源：PS4 Baidu；Switch Ghost 与 Gamer520/Baidu；3DS 商店与百度合集。原 Rust/Go 任务、范围/摘要校验、关联更新/DLC 与汉化、恢复和准备流程保留，UI 接入对应来源操作。不同平台/来源有独立 owner 和根目录，切换页面不结束下载。

Android 采用用户启动的前台服务、只读 SAF URI 和可 seek FD。来源凭证、暂存、缓存及任务 journal 不导出。更丰富的跨应用导入回执、能力发现与文件保留协议仍是后续设计，尚未实现。

Citron/Azahar 的 online 功能已迁入独立仓，但其原工程入口切换和旧实现删除尚未执行；两个工作区原有变更未修改。详细来源和功能差异在独立仓的 [迁移清单](https://github.com/tencentmalos/EmuPSP/blob/aa62ce4e0ad941953e0de314171c41f02ba26b85/docs/migration.md)。

## 已执行验证与边界

- shadPS4：launch-only C++ 源文件 clang++ 语法检查通过；Android `:feature:library:compileDebugKotlin` 通过。Kotlin 验证跳过 `:core:runtime:packageSourceTurnip`，因本机旧驱动产物与主干 pin 不同；不是完整 APK 构建。
- EmuPSP：Rust workspace 76 项测试通过，Clippy all-targets `-D warnings` 通过；两组 Go race 测试通过；worker/C ABI 隔离与关闭协议通过。
- Mac arm64 `.app` 自包含打包、临时签名和五类 native owner open/snapshot/close 冒烟通过；Android arm64 APK、2 项发布路径单测、lint 通过（0 errors / 17 warnings）。摘要在独立仓 `docs/artifacts-20261014.json`。
- Mac 可访问性树确认首页三入口、平台切换及目录/Title 设置。Ghost 真实目录请求取得 16,365 条、341 页，第一页 48 个 icon 和 48 个 BG 缓存；未启动游戏下载。
- ScreenCaptureKit 持续返回 -3811/-3812，未完成像素截图验收。Swan 布局/控制器、真实账号下载与长时恢复、文件授权和模拟器导入、Windows/Linux 运行均未验收。本仓完整桌面模拟器/APK 构建未执行。
