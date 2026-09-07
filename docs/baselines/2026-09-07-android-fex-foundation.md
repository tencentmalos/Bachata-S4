# Android / FEX 基础版本状态：2026-09-07

本记录将 **`a712889343ccd2588b712988dae0a76a104e0dce` 固定为后续迭代的基础版本**。它是完成源码组织、参考版本固定与可行性审计的开发基线；NDK 原生整合和 Android 16 KiB 设备验收尚未完成。

本文件在后续文档提交中保存，不改变基础提交的内容。后续请引用“2026-09-07 基础版本 / `a7128893`”，不要用不断移动的分支 HEAD 或 Android app 小版本号代替它。

## 1. 主仓身份与范围

| 项目 | 固定状态 |
|---|---|
| 基础提交 | [`a712889343ccd2588b712988dae0a76a104e0dce`](https://github.com/tencentmalos/Bachata-S4/commit/a712889343ccd2588b712988dae0a76a104e0dce) |
| 提交时间 | 2026-09-07 17:29:56 +08:00 |
| 记录时所在分支 | `feature/fangfang/basic_vr` |
| 推送状态 | 已推送；记录时远端分支提交与基础提交一致 |
| 当前远端仓库 | `tencentmalos/Bachata-S4`；原 `tencentmalos/shadPS4` 地址已重定向到这里 |
| 上游 | `shadps4-emu/shadPS4` |
| 主仓核心 | 桌面 shadPS4，源码版本为 `0.18.1 WIP` |
| 这次基础提交的内容 | 研究文档、证据、分析脚本、AGENTS/CLAUDE 入口及 references gitlink；未修改主仓 `src/` 或 CMake 实现 |

**基础版本不含完整 Android 原生 backend，也没有对应的已验收 APK/设备 Build ID。** Android 工程位于参考子仓，尚未移入主仓的构建目标。

## 2. references 固定版本

以下七项全部由基础提交中的 Git gitlink（mode 160000）固定。远端来源和初始化说明见 [references 索引](../../references/README.md)。

| 路径 | 固定提交 | 角色 |
|---|---|---|
| `references/Bachata-S4-android` | `67dbf4e5b54b0467bf8a93de3de0a557a71f9f43` | Android 前端及配套历史 runtime/core |
| `references/shadps4-arm64` | `be6bc2e9c60799e071dd2fafa6216e8d80ec619c` | ARM64 guest/HLE/线程与回调移植参考 |
| `references/FEX` | `50e6eee95ae95d3257672727a9302a30b4a60a9a` | 上游 FEXCore/JIT/调试源码研究 |
| `references/Bachata-S4` | `e170f8005970ae416e0f39997d82b4fcc57214fe` | 既有 SG8275 bring-up 分支 |
| `references/dynarmic-citron` | `a593d9262388e3216985b982e400f19ef9ce9749` | citron 所用 Dynarmic 对照版本 |
| `references/dynarmic-azahar` | `96a803e921ba19bde0fbb315264971fd268fb2ed` | azahar 所用 Dynarmic 对照版本 |
| `references/ps4-pkg-tools` | `45baedaa29b8da42b3c6fe912800d2fa4119834b` | PS4 内容格式与导入参考 |

另一个必须保留的版本区别：Bachata/ARM64 参考的 runtime lock 使用 **FEX `f2b679f6028ce1c38875233aecfcf5d3f8ebecec`**。它不是独立 FEX 子仓的版本；当前不能直接替换链接。

Android 源码的版本配置仍为 `0.1.8`，虽然官方历史来源映射把该提交关联至 `v0.1.9`。本基线不宣称该源码与发布 APK 一致，也不以公开 `v0.2.1` 发布标签表示已取得对应 Android 源码。详细出处见 [Android 基线选择](../android-foundation-selection.md)与[来源锁](../data/android-foundation.lock.json)。

## 3. 功能与实现状态

| 方面 | 基础版本的真实状态 |
|---|---|
| Android UI、游戏库、导入、设置、普通手柄 | 参考工程已有实现，选定作为复用基础；未完成主仓集成 |
| CPU guest/host 分界 | ARM64 参考已有 FEXCore 执行 guest、HLE ABI adapter、反向回调与线程接入代码 |
| Android 启动方式 | 参考路径启动 ARM64 glibc shadPS4 子进程，FEXCore 链在其中；不是 NDK/bionic backend |
| 控制/输入/音频 | Android 与 C++ 协议已有源码对应；实际 service 使用逐行 `BACHATA/1` |
| 16 KiB | 当前参考 engine 要求 4096，其他 host page size 返回 `ENOTSUP`；尚未适配 |
| 原生 Vulkan Surface | 尚缺 Android Surface 创建与完整生命周期接入；旧路径通过 X/Vortek、AHB 和 CPU/Bitmap/Canvas 呈现 |
| 停止/重启、暂停与调试 | 参考进程管理不能直接替代 native engine 生命周期；guest 状态适配与安全点控制待实现 |
| LLDB | 已完成 host→guest 调试方案和局部 host 反汇编验证；没有交付完整 Android guest debugger |
| 游戏与 VR | 未完成本基线的商业游戏运行验收；Beat Saber 的 PSVR/Move/tracking/双眼呈现仍是后续工作 |

上述实现依据及具体接口位置见 [Android / ARM64 整合审计](../android-arm64-integration-audit.md)。未来进展应分别报告“源码已实现”“host 测试通过”“目标设备已通过”，不要把它们合并成一个“支持”。

## 4. 已完成的验证

| 验证 | 结果 | 证据边界 |
|---|---|---|
| C++ 控制、输入、音频传输测试 | **18/18 通过**，macOS ARM64 | 编译并运行真实参考 C++ 和既有测试；未运行 Android Kotlin/Java、FEX guest 或 Vulkan |
| Android runtime lock 校验 | **8 components / 30 inputs 通过** | 只验证锁文件定义，不是 runtime/APK 构建 |
| FEX/Dynarmic 源码统计复现 | **4 个 Git revision 的计量分组全部与原记录一致** | 源码文本规模，不表示二进制规模、性能或实现工作量 |
| references 完整性 | **7 个 gitlink 与工作区 HEAD、文档记录一致；对应远端提交可取得** | 不代表所有嵌套构建依赖都已下载 |
| 文档与脚本检查 | JSON 可解析、Python 语法检查、相对链接和 Git whitespace 检查通过 | 不替代模拟器运行验证 |

现有输出集中在 [docs/data](../data/README.md)。**未执行的验收**包括：完整 APK 构建/安装、NDK FEX 执行、Android 16 KiB 实机、GPU 呈现、设备音频、完整游戏和 VR。当前没有可以据此承诺的帧率、兼容率或稳定运行时长。

## 5. 不属于这个基础版本的本地状态

记录时主仓自身的已跟踪文件没有未提交改动，但仍有以下独立工作区内容。它们未被基础提交保存，fresh clone 不会恢复这些内容：

- `references/Bachata-S4` 有两处未提交修改：
  - `android/BachataS4/app/src/main/kotlin/com/bachatas4/android/service/EmulationService.kt`
  - `android/BachataS4/core/runtime/build.gradle.kts`
- `externals/dear_imgui/` 是未纳入主仓的遗留独立 checkout，HEAD 为 `f4d9359095eff3eb03f685921edc1cf0e37b1687`；当前主仓的 ImGui gitlink 是 `externals/imgui`。
- 完整 citron/azahar 工程、游戏文件、下载的 runtime、设备部署文件及临时构建产物不由本基础提交保存。

恢复或比较基础版本时，以 **Git 提交内容**为准。既有子仓工作区若有改动，应先保存其独立工作，不用强制 reset/clean 来制造“干净基线”。

## 6. 在新目录恢复基础源码

下面命令用于新 clone，不改动现有工作区。它只检出固定参考源码，不递归下载全部构建依赖：

```sh
git clone --no-checkout https://github.com/tencentmalos/Bachata-S4.git shadps4-baseline
cd shadps4-baseline
git checkout --detach a712889343ccd2588b712988dae0a76a104e0dce
git submodule update --init references/Bachata-S4-android references/shadps4-arm64 references/FEX references/Bachata-S4 references/dynarmic-citron references/dynarmic-azahar references/ps4-pkg-tools
```

GitHub HTTPS 无法连通时，本次已成功使用保留 `github.com` Host 配置、将实际连接切换到 `ssh.github.com:443` 的 SSH 方式推送；这是本地连接选择，不是基础源码的依赖。

复现 host 传输测试和源码计量，继续按 [references 初始化说明](../../references/README.md)执行。需要开发时在该基础提交上建立自己的分支。之后比较主仓实现与参考版本变化可使用：

```sh
git diff a712889343ccd2588b712988dae0a76a104e0dce HEAD -- src CMakeLists.txt externals/CMakeLists.txt
git diff --submodule=short a712889343ccd2588b712988dae0a76a104e0dce HEAD -- .gitmodules references
```

## 7. 下一阶段的验收目标

按 [总体方案](../fex-android16-native-guest-host-plan.md)推进：

1. 从参考前端抽出 backend 接口，明确 Surface generation、输入、事件与停止完成通知。
2. 在真正 16 KiB Android app 中跑通 NDK/bionic FEX harness，覆盖 HLE 回调、TLS、多线程、JIT 失效及停止/重启。
3. 跑通原生 Vulkan Surface/swapchain 重建、音频和普通手柄输入。
4. 按依赖关系迁入实际 shadPS4 guest/HLE 链，完成非 VR 游戏基线。
5. 建立可用的 host LLDB/guest 状态视图，再单独推进 Beat Saber 的 VR 服务与时序。

后续里程碑新建状态记录，引用本基础提交、实现提交、设备/页大小、部署 Build ID 和实际测试结果；本文件保留为初始状态，不随进展改写成“当时已支持”。
