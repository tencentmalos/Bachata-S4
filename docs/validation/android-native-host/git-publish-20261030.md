# Android 修复与 GoW guest patch 主干归档（2026-10-30）

用户要求将当前工作提交并合入主干。本仓约定主干为 `malos/main`，不是官方同步用的
`main`。本轮起点为 `feature/malos/swan_performance` 的 `22bd5a9d76fa32b726cf14831a00373795e200ac`；
检查时 `origin/malos/main=bb8175cb3501aac0c3d9a4b62ba31f11f673cc69` 是其祖先，已有三个归档提交尚未合入。
本次源码提交为 `6e59205ed4623779aa6188ed5c06d96a5d6f530d`，随后补齐本轮与历史验证文档，
以快进方式集成，保留原提交历史。

## 归档范围

- Android 会话恢复、图层生命周期、后台前台服务、扣除暂停时间的 guest 时钟。
- AJM 实例编号循环复用、自适应同步对象地址窗口、地址映射索引、音频和慢帧诊断。
- 游戏库中的存档查看和独立备注，共用 Foundation PinyinIME 和构建内置词库。
- GoW4 启动接口、跨相邻映射 PlayGo、完整文件读取、较大 VA 范围及分块 GPU 上传。
- 单 wave ordered-count/TG_SIZE、纹理视图掩码、CMASK 兼容和 CPU 脏区/目录摘要。
- Status Detail 恢复平铺；成功上屏帧采集、GPU 输入快照及对应测试。
- 精确版本绑定的 GoW4 日志地址、720p、体积雾和实验 TAA guest recipes；GoW3 纹理区容量候选。
- Turnip 静态隐藏 C++ 运行时打包检查，以及 KGSL 校准时间戳能力宣告修正。

实验选项及验证边界不因提交而改变：GoW3 容量候选仍未完成实机验收；公开 TAA 标志未移除
resolve，不默认选择；GoW4 关雾的9.11FPS仅是已记录的标题场景结果，仍不可玩。
图形 pass 拆分与 GPU timing detail 崩溃尚未解决。被撤回的上传 hoist 候选未混入生产代码。

## 依赖先行发布

| 仓库 | 固定提交 | 已发布分支 |
|---|---|---|
| `tencentmalos/PinyinIME` | `229ef17373d7d12cefc968b03419d2075dfb2f57` | `codex/bounded-milestone-extension` |
| `tencentmalos/foundation` | `1f8699223f065c0a1aac6662598c8167dceae819` | `codex/profiler-slow-frame-owner` |
| `tencentmalos/mesa-mirror` | `6fc75c0620bc4bf4d1a52f67535fbfb4276f74ff` | `codex/shadps4-kgsl-calibrated-timestamps` |

按 PinyinIME → Foundation 顺序发布，Mesa 独立提交能力修正，然后才更新父仓 gitlink。
三个远端分支均通过 `git ls-remote` 核对完整 SHA。父仓 `.gitmodules` 的 Foundation/Mesa
branch 同步到发布分支；实际构建版本仍由 gitlink 固定，不随远端分支自动推进。
新检出需初始化 Foundation 的 `third_party/pinyinime` 子模块。没有向 Mesa 上游提交。

## 本轮重新验证

| 检查 | 结果 |
|---|---|
| Android `shadps4_host` | NDK29/API33 当前源码增量编译链接通过 |
| Android app | `:app:compileFdroidDebugKotlin` 通过 |
| Kotlin 单测 | core:data94、feature:library20、feature:session18，共132项，0失败/错误/跳过 |
| 图层生命周期 | 当前源码重建、CTest1/1通过 |
| 共用拼音 | 当前 Foundation 引擎重建、CTest1/1通过 |
| 成功呈现节奏 | CTest1/1，11 checks /0 failures |
| GoW4 guest handlers | 当前生产 handler 编译运行：720p9项、体积雾7项通过 |
| Turnip 运行时依赖检查 | Python9项通过 |
| Git | 显式路径暂存，差异空白检查通过，无游戏二进制/构建产物入仓 |

首次 Gradle 检查使用默认旧 `build/android-host-api33/native`，其驱动仍依赖
`libc++_shared.so`，被打包守卫正确拒绝。随后显式指定本轮已经重建的
`build/android-host-capture-api33/native/shadps4-host-loader.cmake`，全部检查通过。
没有跳过守卫或修改用户的 `local.properties`。Foundation 模板和 PinyinIME 调试输出的
已有编译警告仍存在；不记为零警告构建。

构建/测试日志在本地 `build/validation/git-publish-20261030/`。本轮未安装 APK、未操作设备、
未复测游戏，不把历史实机结果当作本轮验收；完整桌面构建和全量回归未重新执行。

## 保留的本地状态

`references/Bachata-S4-android` 和 `references/shadps4-arm64` 中的外部依赖检出差异未纳入
本次提交，两者父仓 gitlink 保持原值；没有 reset、clean 或递归更新这些参考工作区。
本地游戏录制、截图、原始 ELF/SELF、诊断数据和 APK 仍在各自验证目录，未加入 Git。
旧文档的“本地未提交/未推送”保留为采集时点记录，当前发布状态以本条及 Git 历史为准。
