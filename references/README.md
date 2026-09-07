# Reference source checkouts

这些目录均以 **Git submodule / gitlink（mode 160000）**加入主仓；不是复制进主仓历史的源码目录，也不会自动加入当前 CMake 构建。

## 固定版本与用途

| 路径 | 精确提交与来源 | 用途 |
|---|---|---|
| `Bachata-S4-android` | [67dbf4e5b54b0467bf8a93de3de0a557a71f9f43](https://github.com/zFitness/Bachata-S4/tree/67dbf4e5b54b0467bf8a93de3de0a557a71f9f43) | 本次选定的 Android 前端、session、输入、打包与配套历史核心 |
| `shadps4-arm64` | [be6bc2e9c60799e071dd2fafa6216e8d80ec619c](https://github.com/zenithblue-oss/shadps4-arm64/tree/be6bc2e9c60799e071dd2fafa6216e8d80ec619c) | ARM64/FEX guest/HLE/线程回调移植参考；没有 Android Gradle 工程 |
| `FEX` | [50e6eee95ae95d3257672727a9302a30b4a60a9a](https://github.com/FEX-Emu/FEX/tree/50e6eee95ae95d3257672727a9302a30b4a60a9a) | 上游 FEXCore、JIT、调试和 16 KiB 源码审计 |
| `Bachata-S4` | [e170f8005970ae416e0f39997d82b4fcc57214fe](https://github.com/tencentmalos/Bachata-S4/tree/e170f8005970ae416e0f39997d82b4fcc57214fe) | 既有 SG8275 bring-up 分支；upstream 是 The412Banner/Bachata-S4 |
| `dynarmic-citron` | [a593d9262388e3216985b982e400f19ef9ce9749](https://github.com/tencentmalos/dynarmic/tree/a593d9262388e3216985b982e400f19ef9ce9749) | citron 所用 Dynarmic 的源码计量/API 对照 |
| `dynarmic-azahar` | [96a803e921ba19bde0fbb315264971fd268fb2ed](https://github.com/tencentmalos/dynarmic/tree/96a803e921ba19bde0fbb315264971fd268fb2ed) | azahar 所用 Dynarmic 的源码计量/API 对照 |
| `ps4-pkg-tools` | [45baedaa29b8da42b3c6fe912800d2fa4119834b](https://github.com/xXJSONDeruloXx/ps4-pkg-tools/tree/45baedaa29b8da42b3c6fe912800d2fa4119834b) | 既有 PS4 内容格式/导入参考 |

版本事实截至 2026-09-07。实际 checkout 以主仓提交中的 gitlink 为准；更新时同步本表。Android 的官方来源映射及版本不一致见 [基线选择](../docs/android-foundation-selection.md)。上述七个提交已通过对应 GitHub 仓库 commit API 检查可取得。

两份模拟器参考使用的 FEX runtime pin 仍是 `f2b679f6028ce1c38875233aecfcf5d3f8ebecec`，与独立 FEX 子仓不同。这是有意保留的比较维度，不能直接交叉替换。

## 初始化与复现

在新 clone 中只检出参考源码，不递归下载全部外部依赖：

```sh
git submodule update --init references/Bachata-S4-android references/shadps4-arm64 references/FEX references/Bachata-S4 references/dynarmic-citron references/dynarmic-azahar references/ps4-pkg-tools
```

已有工作区先检查子仓状态；上面的命令可能切换子仓 HEAD。修改 `.gitmodules` 的来源后，可按需运行 `git submodule sync -- references/Bachata-S4`，注意它会同步本地 origin URL。

复现 host C++ 传输测试只额外需要 GoogleTest 所在的现有依赖：

```sh
git submodule update --init externals/cpp-httplib
python3 scripts/analysis/run_android_arm64_contract_tests.py
```

源码计量不需要 FEX/Android 的完整编译依赖。若浅克隆缺少旧 FEX 对象，先取得该提交：

```sh
git -C references/FEX fetch origin f2b679f6028ce1c38875233aecfcf5d3f8ebecec
python3 scripts/analysis/compare_cpu_core_size.py --output /tmp/fex-dynarmic-source-size.json
```

构建某个参考项目时，再按其构建说明初始化嵌套依赖。FEX 的递归子模块可能包含较大的预编译测试数据；单纯源码阅读不需要下载这些数据。

## 边界与维护

- Android 子仓本地开发分支 `codex/android-foundation` 只是当前工作区的便利设置；fresh clone 默认 detached HEAD。需要改子仓时，在该子仓创建自己的分支，先提交/推送子仓，再更新主仓 gitlink。
- 主仓无法保存子仓未提交的文件。既有 SG8275 子仓中的独立本地改动未纳入这次参考基线提交，也未被覆盖。
- `externals/dear_imgui` 是遗留的本地独立 checkout，不在当前主仓构建与 gitlink 清单内；本次未收编、删除或修改它。当前 ImGui 依赖是 `.gitmodules` 中的 `externals/imgui`。
- 完整 citron/azahar 工程不是本仓子模块；本仓固定了用于规模对比的两个 Dynarmic 版本，Android/XR 架构对照使用相应外部源码引用。
- 阅读和遵守各子仓自己的贡献说明；尤其 FEX 源码用于审计，不生成其禁止的 AI 代码贡献。

总体入口：[开发资料](../docs/README.md)、[AGENTS.md](../AGENTS.md)。
