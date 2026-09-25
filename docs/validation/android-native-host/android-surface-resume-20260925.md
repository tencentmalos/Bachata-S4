# Android Surface 后台恢复（AYN Thor，2026-09-25）

## 故障与修复

TMNT（CUSA50828）运行中按 Home 再返回，旧包的 Guest session 会因 `Android swapchain present: Surface lost`、`Android swapchain acquire: ErrorSurfaceLostKHR` 或 `Android swapchain create: ErrorSurfaceLostKHR` 进入 BackendFailure，画面和输入均不能恢复。问题不在 DebugBus 输入焦点：Android `SurfaceView` 退出前台时撤销原 `Surface`，而 `FexSessionService` 把任何 Surface 替换视为整代游戏终止；Vulkan swapchain 也将 Android surface loss 当作不可恢复异常。

同一 Guest generation 现在接收 Surface 撤销和替换。JNI 按 generation 将新 `ANativeWindow` 交给 `AndroidWindow`；swapchain 持有旧窗口引用，待 GPU 提交排空、旧 swapchain 与 `VkSurfaceKHR` 销毁后，再用新窗口建立 surface/swapchain。撤销期间 presenter 跳过 acquire/present 并归还帧，Guest 不因窗口生命周期被停止。非 surface-loss 的 Vulkan 错误仍按原路径报告。入口的 generation 校验阻止旧 Activity/Surface 更新下一代游戏。

## 构建与设备验证

主仓修复从 `malos/main` 的 `d74cb779b` 独立工作树制作。为保留 Game Agent 正在实测的 SCAP2 协议，把同一生命周期补丁同步应用到其隔离的 SDK 快照（起点 `a562e8100` 加现有未提交 SCAP2 代码）后构建、安装；SCAP2 代码不包含在本次主仓提交中。

- Android host 增量构建通过；`libshadps4_host.so` SHA-256 `36ada4424694fc4df1f9abac9fc91f97f306876850028fd32f9cb806e554d09f`。
- `:app:assembleFdroidDebug --offline` 通过，APK SHA-256 `694bed42a67ebd579c3690517341f23bd8234920c6f1bafc75b96d876bc601df`；安装后设备回读 SHA 一致。包名 `com.shadps4.android`、versionCode `26081400`、签名证书 SHA-256 `ce9d5b4d6524b793e48e00658f4d6c9d2ccfa1b1059dbb1fb34244c68cd1f8eb` 未变。APK 内 host DSO SHA 与上述构建产物一致；JNI DSO SHA-256 `c63977bb4500ddb90b554feac49da5b79e096dd4bd3dc1cdbd17c5398af3567f`，包含新的 JNI 入口。
- AYN Thor `9c2841a4` 上，TMNT 使用同一 PID `3918`、generation `1` 连续完成三次 Home→返回；状态持续 `Running`，Guest flip 和 host present 恢复递增。随后 DebugBus 十字键按键在 Guest poll 中可见，进入实际关卡。关卡中再次 Home→返回：`guest_flip` 从 `13043` 到 `13406`、`host_present` 从 `12615` 到 `12972`；左摇杆输入有 20 次 Guest poll，截图中角色随之移动。
- SCAP2 live capture 期间再次 Home→返回：采集到返回前 51、后台期间 16、返回后 149 个媒体帧；219 个帧带 source frame 关联，ID 单调，EOS 到达、reader error 为零。PID/generation 保持不变，`host_present` 从 `22894` 到 `23436`。结束后 capture 状态为 `stopped`/`eos:true`，临时 ADB forward 已移除、手柄 owner 已释放。
- 夜间模式切换再还原后同一 generation 仍运行，但 Android ActivityRecord ID 未变；这只算配置变更 smoke，不算 Activity 重建验证。

本轮未清理游戏数据或存档，系统夜间模式恢复为原先的 `no`。安装包保留 SCAP2 和 DebugBus，供 Game Agent 继续实测。设备级验证覆盖 TMNT/Turnip 的后台恢复；未声称所有游戏、系统驱动或任意 Activity 重建均已回归。
