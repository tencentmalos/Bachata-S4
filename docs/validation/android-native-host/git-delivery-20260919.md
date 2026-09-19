# 2026-09-19 当前修改交付

用户要求先 commit/push，随后明确所有未提交源码均需按所属仓库妥善提交，并把 Android internal scale 默认设为 0.5。卡死/无声问题暂缓，内存读数对比没有完成；本轮不新增设备回归。

## 子仓提交与顺序

先发布最内层 profiler SDK，再发布 Foundation / FEX / Bachata-S4，核对远端分支包含 SHA 后更新主仓 gitlink。

| 仓库 | 分支 | 提交 | 范围 |
| --- | --- | --- | --- |
| profiler_sdk | codex/shadps4-stage-profiler | 0ef631cd7b0859aa0eea6beb0e8739bd43e75129 | 推送已有 startup/stage 采集提交，未新改 SDK |
| Foundation | codex/shadps4-internal-scale | 6da584c12c0a0c05656f1b86be3b40d10e8ec545 | 原有 audio/input、SDK pin，以及通用 Vulkan ASTC codec |
| FEX | feature/malos/host-page-size | 3f1f30a060b633980ed8e7674eb8d8997457edad | 原有 opt-in memory/profile probes、FXRSTOR TOP 修复与测试 |
| Bachata-S4 | feature/fangfang/sg8275-bringup | 16e850602a9b9e87c936f885a757001b1886228e | 保存原有 X11 emulated libraries 与 NDK 29 两处改动 |

这些均推送既有自有仓；未向 FEX 上游贡献，未合并 Foundation main。Bachata-S4 是独立参考工程，本轮只保存原有源码并检查 diff，未构建、安装或宣称其验证通过。`externals/dear_imgui` 是原有独立、干净的 checkout，没有未提交源码；它不在主仓依赖清单中，不重复导入第三方源码。

## 主仓内容与验证

主仓分支 `codex/android-fex-round2` 包含 guest shading rate、关闭 FDM、普通上屏路径修正、0.5/0.75/1.0 internal scale、压缩贴图物理缩小和 Foundation codec 接入；Android 默认 0.5、Settings 三档选择、状态栏倍率、定向测试与中文证据一起交付。

- [Internal scale](internal-scale-20260919.md)：真实 GPU 两套驱动各 1,563,042 checks / 0 failures，23 项最新配置/Settings 定向测试通过，APK 构建成功；压缩质量限制和未完成游戏对照均明确保留。
- [Shading rate](guest-shading-rate-20260919.md)：两套驱动各 35,630 / 0，按用户要求恢复默认 1×1，不把微弱收益当作稳定加速。
- FEX 现有验证复用 [guest auto tag](guest-auto-tag-2026-09-16.md)、[入口拦截](frame-interception-2026-09-16.md)、[memory watch](guest-debugger-mixed-watch-2026-09-15.md)，本次仅提交已有实现，不重复全量回归。
- 已提交原始失败/未验证 manifest：0.5 timeout、过期 review，以及用户要求停止后的 `STOPPED_UNVERIFIED`。Controller disconnected/卡死/无声与 0.5/1.0 总内存差值仍未定位。

未提交游戏内容、APK/ELF、生成的 SPIR-V 或凭据。APK/二进制留在本地 build，提交 SHA256 与定向验证日志。Git 中的文本日志仅清理行尾空格与末尾空行，原始文本留在本地 build/validation/git-delivery-20260919/raw-text；计数、错误、标识和图片内容未改写。当前运行游戏未被本次提交动作重启，自动输入保持关闭。
