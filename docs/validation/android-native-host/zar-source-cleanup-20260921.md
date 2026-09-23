# 已验证 ZAR 的来源清理（2026-09-21）

用户授权空间不足时清理已制作 ZAR 且验证可用游戏的原始 RAR 和散文件，保留成品及来源文档。

## 血源诅咒：老猎人

- 游戏：Bloodborne™ The Old Hunters Edition，ID `CUSA03023`。
- 成品：`/Users/bytedance/game/ps4/zar/CUSA03023.zar`，31,148,441,550 字节；SHA256 `911be85bac3f2a23f391068da65e67d3098e715985a789ef498e7cf55599817c`。
- 内容：老猎人版本体，SFO APP_VER `01.00`，28,831 文件；未另叠加四个 CHEAT V1–V4 的 1.09 PKG，也未合入简体化补丁。原文件夹名称不作为额外内容已入包的证明。
- 来源：原八卷“《血源诅咒：老猎人》港版中文版游戏本体”RAR，内含 `HP9000-CUSA03023_00-BLOODBORNE0000AS-A0100-V0100.pkg`。完整路径、每卷大小/SHA 及删除状态见[来源清单](zar-source-cleanup-20260921/bloodborne.json)，另保留[RAR 原目录](zar-source-cleanup-20260921/bloodborne-rar-listing.txt)。随包两个说明/URL 小文件提取至原目录 `ZAR-来源保留`。
- 清理前重新核对成品 SHA，并将全部 28,831 文件与原提取目录逐字节对比：[结果](zar-source-cleanup-20260921/bloodborne-roundtrip.txt)、[文件名/大小完整清单](zar-source-cleanup-20260921/bloodborne-source-files.json.gz)。
- 实机依据：[Swan 诊所人物、HUD、移动和 Stop 验证](resource-policy-remaining-20260920.md)。这是有界场景验证，不代表通关。
- 清理范围：上述八卷原始 RAR 及 `build/validation/swan-zar-20260920/bloodborne-extracted/CUSA03023`；逻辑字节合计 62,894,961,470。未打入 ZAR 的金手指、简体化补丁和存档保留。磁盘可用空间变化单独记录，不把逻辑大小直接当实际释放空间。

AYN 当前血源/TMNT 是旧散文件安装；Swan 已用 ZAR 验证。本次只清理本机来源，不改 AYN 这两款游戏及其存档。MHW/MHR 已打包安装，但启动适配未完成，原 RAR 暂保留。TMNT 的原包包含另外的 backport 和存档，先保留；血源来源清理已足够本次空间需求。
