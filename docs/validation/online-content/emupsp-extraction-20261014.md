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

## 首版验证（AYN 续测前）

以下为首版发布时的检查，后续 AYN 下载、APK 重建与完整导入结果见下一节。

- shadPS4：launch-only C++ 源文件 clang++ 语法检查通过；Android `:feature:library:compileDebugKotlin` 通过。Kotlin 验证跳过 `:core:runtime:packageSourceTurnip`，因本机旧驱动产物与主干 pin 不同；不是完整 APK 构建。
- EmuPSP：Rust workspace 76 项测试通过，Clippy all-targets `-D warnings` 通过；两组 Go race 测试通过；worker/C ABI 隔离与关闭协议通过。
- Mac arm64 `.app` 自包含打包、临时签名和五类 native owner open/snapshot/close 冒烟通过；Android arm64 APK、2 项发布路径单测、lint 通过（0 errors / 17 warnings）。摘要在独立仓 `docs/artifacts-20261014.json`。
- Mac 可访问性树确认首页三入口、平台切换及目录/Title 设置。Ghost 真实目录请求取得 16,365 条、341 页，第一页 48 个 icon 和 48 个 BG 缓存；未启动游戏下载。
- ScreenCaptureKit 持续返回 -3811/-3812，未完成像素截图验收。Swan 布局/控制器、真实账号下载与长时恢复、文件授权和模拟器导入、Windows/Linux 运行均未验收。本仓完整桌面模拟器/APK 构建未执行。

## AYN 后续实机检查（下载 → 安装通过）

AYN Thor（Android 13、1920×1080）实际完成用户提供分享的 10 卷 37.131 GiB 下载、校验、解压与发布，生成 36.036 GiB PKG。通过系统 SAF 选择该文件，shadPS4 直接读取可 seek FD，不另存一份 PKG 缓存；2026-10-14 19:52 安装成功登记 **CUSA01740 / God of War III Remastered**，输出 49,513,778,622 字节，日志 `completed=true`，游戏库真实封面已显示。原有 10 个游戏目录均保留，仅新增这一项，临时导入目录清理完成。未做该游戏运行验收。

本仓导入修复 `5fc3fc2ce`：

- 单包/批量 PKG 导入复制可定位 FD，立即关闭 ContentResolver 包装对象，释放稳定 provider 引用。前两次因 EmuPSP 被 LMKD 回收，shadPS4 被连带 `DEPENDENCY DIED`；修后 fixture 提供器死亡设备测试 1/0，真实提取中主动结束 EmuPSP 提供器仍可继续读取。
- 第三次暴露直接 LMKD 回收：总可用内存约 12 GiB，但 Normal zone 接近水位、文件缓存约 9 GiB。解包改为每线程 16 MiB 写回窗口，释放已读/已写缓存，应用常驻内存没有显著增加，Normal zone 恢复约 6 GiB；完整成功轮未见同类回收。
- 文件长度、区块索引和尾块写入使用 64 位，区分逻辑长度与分配填充。写盘/同步失败显式报错；2 / 4 / 9 GiB 边界和尾块 UBSan 测试通过。中间 fwrite 版本触发过 32 位截断回归，严格区块等量检查又误拒绝合法填充，均已修正，未把中间包记为完成版本。
- 导入确认文案不再声称所有 PKG 都先复制一份。

完整 native host、源码匹配的 Turnip 和 F-Droid debug APK 均重建通过，没有跳过驱动身份检查；7 项 app 单测在两种 flavor 通过。最终 shadPS4 APK SHA-256 `e08f6b1035b6385dfb4607d62b728371fbdad346086d257cf292db00abb4753d`，与设备安装文件一致。测试前 APK 已在本地忽略目录备份，覆盖更新保留应用数据。

EmuPSP 最终设备源码 `be45bdd`，APK SHA-256 `628c2f31a5d34ed4528230ca93b8e1fbb54056680c831c5d54ee22801a2cf658`。首页三平台同排、目录折叠筛选、三档 Title 卡片、Icon 最大 144 dp、BG 开关、真实 Ghost/Gamer520 图片与中外标题已查。修复未知大小显示、临时身份检查误报未登录、准备阶段空白计数；SAF 提供器独立进程。新下载重连仅通过 fixture/race 测试，实际下载用旧 APK 和人工重试完成，不能声称新策略已通过真实 CDN 验收。

本轮还修复独立核心 owner 锁释放和并发测试目录冲突，Rust workspace 77 项、Clippy、Go race、Android APK/2 单测/lint、Mac 构建和五 owner 冒烟通过。下载链接、提取码和凭证不写入仓库。

边界：Swan 头显、Windows/Linux 运行、Citron/Azahar 原工程切换仍未验收。初始安装空间仍按压缩 PFS 估算，并行提取进度存在回退，需继续完善。19:45 在覆盖安装后、未运行导入时系统服务自行重启，原因未定位；恢复后完成最终导入。详细时间线、全部反例和截图索引见独立仓 [AYN 验证记录](https://github.com/tencentmalos/EmuPSP/blob/main/docs/validation-ayn-20261014.md)，包摘要见 [AYN 产物清单](https://github.com/tencentmalos/EmuPSP/blob/main/docs/artifacts-ayn-20261014.json)。

## 2026-10-15《战神 3》运行补测

[运行记录与完整导入绑定清单](gow3-launch-20261015.md)：AYN 上复用已安装的 CUSA01740，无重新下载或导入。两个进程均在 libc 初始化调用 `getargc` 时因 Android bridge 未接入而停止，`graphics=not-created`，未出首帧；下载/安装成功不等于游戏兼容。实际加载三模块 508 条导入、30 个不同未接入符号已归档，未伪造兼容实现。
