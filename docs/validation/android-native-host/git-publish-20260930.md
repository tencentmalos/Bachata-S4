# Swan XR source delivery — 2026-09-30

本轮按仓库既有约定，以 `origin/malos/main` 为主干。两次 fetch 后其 HEAD 均为
`60ec46d05`，当前 feature 已包含它，`git rebase origin/malos/main` 返回 up to date，
没有产生冲突或改写旧提交。本地 `malos/main` 同步该远端；本地 `main` 独立快进到
官方 `upstream/main` 的 `94e217781`。官方分支另有 86 个未合入 fork 的提交，
本轮没有将它们整合进 `malos/main` 或 feature。

## 保存的实现

- 主仓 `12e8d00a2`：Android OpenXR/PSVR 与影院、真实硬件输入、默认 runtime IPD、
  Move 虚拟球心候选、Launch 平铺配置、独立 ImGui 状态/错误层、影院 PSV、
  源码 Turnip 构建和一致性校验、Beat Saber guest 修复、PS4 dynamic 分析输出。
- 主仓 `c74a08ec6`：9 月 29–30 日验证报告、截图、负对照及未解决问题证据。
- Foundation [`82b09a1302cc499f1fc8e085e3c500e550afb743`](https://github.com/tencentmalos/foundation/commit/82b09a1302cc499f1fc8e085e3c500e550afb743)，
  `codex/shadps4-xr-foundation`：原生指标和 JniHelper、FSR1/SGSR1/FDM、XR 控制器和图层。
- Mesa [`351a4847a04dbcc9e18de3b609adacae6d3c796a`](https://github.com/tencentmalos/mesa-mirror/commit/351a4847a04dbcc9e18de3b609adacae6d3c796a)，
  `codex/shadps4-xr-turnip`：Swan 编码 Surface 布局、FDM 分块/缩放、默认关闭的 CP 诊断。

两子仓均先 push，再用 `ls-remote` 核对完整 SHA，最后提交父仓 gitlink。
`.gitmodules` 的对应 branch 与已发布源码分支一致。主仓推送目标为
`origin/feature/malos/swan_performance`；无需强制推送。
下面各历史报告中的“未提交”“无 commit/push”描述当时状态，本轮已归档其实现和证据。

## 本轮验证

- Android host 和 `openxr_pose_tests` 编译成功；本轮未在设备执行 probe。
- Playstore Debug APK 构建成功；APK 内 host/driver/identity/binding SHA 一致，
  Turnip 源码状态 `dirty=False`、source.patch 为空。
- Kotlin：app 7、core/data 82、core/runtime 140、feature/library 11，合计 **240/0**。
  Gradle 增量运行保留有效的 up-to-date 结果，不声称全部重新执行。
- Foundation JNI CTest **1/1**（重新检查构建后运行）。
- 生产 FDM 分块 **121/0**；PSV 模型契约、超分内嵌 SPIR-V 一致性、PS4 工具 selftest 通过。
- 自有源码/文档 diff whitespace 检查通过。原始日志证据与原封保留的 SGSR 上游文件
  不做格式改写；后者保留原始 trailing whitespace。

产物身份见 [package-identity.json](evidence/git-publish-20260930/package-identity.json)，
测试明细见 [kotlin-results.json](evidence/git-publish-20260930/kotlin-results.json)。
本机 APK/ELF 和构建日志保留在 build/，不纳入 Git；已审查归档内容，不包含游戏 ELF、
下载运行库、完整 GPU snapshot 或凭据。git rebase 前保留本地安全分支
`codex/swan-before-rebase-20260930`，未跟踪文件备份也保留在 build/。

## 边界

本轮仅提交、同步和构建，未安装新包或操作设备。新驱动的构建 SHA 因 Git 版本身份变化而变化；
这不增加实机验证结论。GPU hang/DeviceLost 仍未解决，Move 75 mm 仍是待物理标定候选。
旧设备 CleanupPending 继续按原故障报告处理。
