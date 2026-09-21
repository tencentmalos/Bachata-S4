# PSVR 分支集成交付

2026-09-21，用户要求将 `feature/malos/beat_saber_fix` 合入并推送 `malos/main`。

实现提交 `490057a0fd0b15bf3c9a9f6dd06e96c22e03491e` 归档当前分支积累的 Android PSVR 工作：Guest 插件/模块生命周期、离线 NP/Signin/HTTP/SSL、共享 socket、对话框、内存/事件及 AIO、HMD 提交和 SBS、DebugBus 可执行文件导出、MSAA 策略和 gyro 转轴。验证记录与公开参考另作归档提交，保持实现 diff 可审阅。

本次合入前 origin/malos/main 与分支基线同为 `63b6d560`。主分支约定继续为 `malos/main`，采用保留 feature 历史的 merge commit。原有 gitlink 未变化，所有已登记子模块工作区检查无修改；无额外子仓提交。未纳入两份独立本地 checkout：`externals/dear_imgui/`、`references/mesa-turnip-xr-fdm2/`。APK、游戏内容和大型运行捕获保留本机，归档仅含源码、公开资料、说明和有界验证证据。

最新实测见 [MSAA / gyro](msaa-policy-gyro-20260921.md)：双驱动 × 两模式各 26,154 checks/0 failures、48 SPIR-V、Kotlin 120+14 tests 通过；最终 APK e727bef6 已装 AYN，Beat Saber 安全提示左右分离，用户确认三轴正常。源码/测试的 `git diff --check` 通过；原始日志/网页快照保留原字节和 SHA，不为消除其中的空白告警改写证据。

合入不是完整可玩认证。Beat Saber Continue/歌曲、Sports 后续流程、Tetris 确认后的黑屏及 PC0 故障仍按各自报告保留；没有进行全游戏、桌面完整回归或严格性能 A/B。AGENTS.md 下方“未提交”“待修”的历史条目需按日期及最新报告解释，不能因为归档而消除验收缺口。
