# 可复用 DebugBus 可执行文件导出（2026-09-21）

实现和调用方式见 [使用说明](../../guest-executable-export.md)。本地未提交，保留当前分支原有修改。

`guest_executable_export start/status/cancel` 使用 Foundation 现有 registry；MainActivity 的 debug-only `dump ... debugbus` 入口让 Library 状态也能使用。后台线程持有独立文件后端快照，扫描生产有效挂载视图、DLC 和显式外部模块；原文件、ELF、全部扫描 inventory 与 manifest 分开保存。Ready 仅在完成复制、逐段 ELF 回读和 SHA 后原子发布；取消/失败不伪装成功。无 Guest 内存转储、暂停、VM/GPU idle 或 patch 修改。

AYN 定向测试最终 **63 checks / 0 failures**：目录、普通 ZAR、更新覆盖/大小写、空文件遮盖、all-in-one/DLC、SELF 还原及段字节、原 ELF、加密/压缩/未知文件、原文件哈希、损坏/越界拒绝、符号链接、空格路径、无效 UTF-8/参数、快照退休/替换、过期 ID、单任务 busy，以及阻塞读期间 status/cancel。初轮 57/0 保留。未做完整游戏回归或性能结论。

最终 APK `cab079da171be78ea262e54358a1cab1a4b63df15ae96e76b060b1f970426171`，Host `7df85a1f04eea7d2458b985e897ed8c06160fb078f240ca8a74932a59ccf3178`，JNI `cf1a8c1ba607bcde3b58b0db1865415019d0d1de27b0220399aba350b9dea326`。构建成功、安装后提取 APK 核对 Host/JNI SHA 一致。安装前没有活动 FexSessionService；未打断游戏。

最终 APK 在 Library 通过真实 dumpsys 路由完成：

- Beat Saber 当前目录，request `9aa52924fa90c1bd34e3433e4fe5bdc6`：18 原文件、17 ELF，1 个加密 right.sprx 保留原文件。首版实际导出的 18 原文件/17 ELF 与前一轮独立 Python/设备 SHA 导出逐一相同；最终版同路径重新导出并由脚本拉回、校验成功。版本确为 AYN 1.00，不混称本机 2.04。
- Tetris all-in-one ZAR，request `1f5bf8a6ee3554f0cab3b1feca1d7d47`（[最终 receipt](executable-export-debugbus-20260921/tetris-final-receipt.json)）：扫描 166 文件，45 原始候选、43 ELF，另有 1 个空占位和 1 个加密 SELF。最终 manifest 的全部原文件/ELF SHA 与初版真实导出一致。主 ELF SHA `157845ba4297a000ac9cfeb26b7b78925d7d903a2ee34575c74d0f5ede6e9677`，Toolkit ELF SHA `f9204cc565a9c3d78a432ff34e0a0da47304901eae94bdc1a108694d787cf13c`，与此前静态/运行调试使用的模块相同。

当前会话 `start current` 的生命周期和挂载快照已做生产类定向测试；本轮真实 app 导出使用显式目录/ZAR，没有为了验收导出功能启动游戏。报告不把这些导出当作 Tetris 可运行证明。

[证据 manifest](executable-export-debugbus-20260921/manifest.json) 只包含日志、导入元数据和哈希，不纳入游戏二进制。完整二进制保留在用户 Downloads 与 app-private 导出目录。后续 Tetris 工作以这些精确模块继续分析 SigninDialog/离线生命周期。
