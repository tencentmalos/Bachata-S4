# V0 子仓归属与开发分支

检查日期：2026-09-07。先检查 GitHub fork 网络、提交可达性及远端 ref，再开始依赖改动。
本次用已登录 `tencentmalos` 的 `gh` 创建 FEX fork、查询仓库和创建/验证 refs；
GitHub commit API 能读到但 ref API 不能直接引用的跨 fork 提交，通过 Git push 写入目标仓库后再次用 `gh api` 验证。

## 已准备的可写分支

| 本地目录 / 用途 | tencentmalos 仓库 | 开发分支 | 起点 |
|---|---|---|---|
| 主仓 | [Bachata-S4](https://github.com/tencentmalos/Bachata-S4) | `feature/fangfang/basic_vr`；V0 实现另建 `codex/android-fex-v0` | 本次准备提交；比较基线仍为 `a7128893` |
| `references/Bachata-S4-android` | 同上 | `codex/android-fex-v0-frontend` | `67dbf4e5b54b0467bf8a93de3de0a557a71f9f43` |
| `references/shadps4-arm64` | 同上 | `codex/android-fex-v0-arm64` | `be6bc2e9c60799e071dd2fafa6216e8d80ec619c` |
| `references/Bachata-S4` | 同上 | 既有 `feature/fangfang/sg8275-bringup` | `e170f8005970ae416e0f39997d82b4fcc57214fe` |
| FEX V0 依赖开发 | [FEX](https://github.com/tencentmalos/FEX)（本次新 fork） | `codex/android-fex-v0` | `f2b679f6028ce1c38875233aecfcf5d3f8ebecec` |
| `references/FEX` 审计版本 | 同上 | `codex/upstream-audit-20260907` | `50e6eee95ae95d3257672727a9302a30b4a60a9a` |
| `foundation/` | [foundation](https://github.com/tencentmalos/foundation)（既有自有仓） | `codex/shadps4-android-fex-v0` | azahar 所用 `b753063273a73ff6664c567f62cfa5070f5d60a5`，已叠加最小构建开关 |

Android、ARM64 和当前主仓都属于 `shadps4-emu/shadPS4` 的 fork 网络，已有
`tencentmalos/Bachata-S4` 可承载三条独立开发线，无需再建同源 fork。主仓旧 remote
`tencentmalos/shadPS4` 会重定向到这个名字。分支之间不表示已经合并或接口已经兼容。

**七个 reference gitlink 均保持原 SHA。** `.gitmodules` 中 Android、ARM64、FEX 的
URL 已改为自有仓，branch 指向相应远端分支；FEX reference 仍指向新审计版本。
构建所用旧 FEX 应另建 `externals/` 依赖 checkout，不把 reference 隐式降级。
Foundation 的最终版本由本次主仓 gitlink 固定，变更范围见 [接入记录](foundation-integration.md)。

## 只读依赖的处理

- 两份 Dynarmic 已在 `tencentmalos/dynarmic` 下，现有开发分支包括
  `feature/azahar-memory-watchpoints`、`feature/malos/submodule-remotes`；V0 只作 API/规模对照，
  无需为它们创建新的实现分支。两个 reference 仍各固定自己的 SHA。
- `references/ps4-pkg-tools` 仍来自 `xXJSONDeruloXx/ps4-pkg-tools`，本次没有创建自有 fork。
  它不在 V0 的修改范围；后续真要改包格式/导入工具时，先 fork、建分支、验证提交再修改。
- 普通 `externals/` 第三方依赖本次不修改。将来若确需改其中源码，也必须先完成同样的归属检查。
- FEX fork/分支的存在不改变其贡献规则。遵守仓内 `AGENTS.md` / `CLAUDE.md`；本次未修改 FEX 代码。
- SG8275 子仓的已有未提交改动、遗留 `externals/dear_imgui` 没有纳入提交。

### FEX 修改记录（2026-09-07 追加）

上一条"本次未修改 FEX 代码"描述的是归属检查当时的状态。此后经用户明确授权，
在自有 fork 上修改了 FEXCore 的 host 页大小处理：

| 项 | 值 |
|---|---|
| 分支 | `feature/malos/host-page-size` |
| 提交 | `6e862ccd7` |
| fork 起点 | `dcfbe49cf`（`tencentmalos/FEX`） |
| upstream base | `50e6eee95`（`FEX-Emu/FEX`） |
| 范围 | 7 个文件，+84/-12。见 [适配记录](fex-host-page-size-adaptation.md) |

两点必须记住：

1. **上游规则依然存在。** fork 的 `dcfbe49cf` 提交删掉了 upstream `AGENTS.md` / `CLAUDE.md`
   中"AI must not be used to generate code for contributions"那一行，但删除声明不等于规则不存在。
   授权范围是"自有 fork 且不回流上游"。若将来要向 `FEX-Emu/FEX` 提 PR，
   必须由人类工程师重写，不能直接拿这次的改动提交。
2. **主仓 gitlink 已推进**到 `6e862ccd7`，子仓分支已 push 到 `tencentmalos/FEX`，
   `.gitmodules` 的 `branch` 也随之改为 `feature/malos/host-page-size`。
   注意这次推进**同时纳入了 fork 的 `dcfbe49cf`**（gitlink 原本停在审计版 `50e6eee95`），
   也就是上面第 1 点提到的那个删除声明的提交。审计版 `codex/upstream-audit-20260907` 仍在远端保留。

## 后续工作约定

1. 先 `git status` 检查子仓，不切换带未提交改动的工作区。fresh clone 的子模块通常是 detached HEAD。
2. `git submodule sync -- <path>` 同步新 URL；`git -C <path> fetch origin` 后显式切到上表分支。
   不用 `git submodule update --remote` 顺便更新源码。
3. 通用基础设施改动提交在 foundation 分支；shadPS4 的 guest 状态、Orbis ABI、生命周期 adapter 留在主仓。
4. 子仓先 commit/push 并通过远端 ref 检查，再提交主仓 gitlink 与说明。fork 存在、分支存在和提交可重建是三个独立检查项。
5. 远端开发分支会前进，历史状态以主仓 gitlink 和本文件的起点 SHA 为准。

例如检查远端实际指向：

```sh
gh api repos/tencentmalos/Bachata-S4/git/ref/heads/codex/android-fex-v0-frontend --jq .object.sha
gh api repos/tencentmalos/Bachata-S4/git/ref/heads/codex/android-fex-v0-arm64 --jq .object.sha
gh api repos/tencentmalos/FEX/git/ref/heads/codex/android-fex-v0 --jq .object.sha
gh api repos/tencentmalos/foundation/git/ref/heads/codex/shadps4-android-fex-v0 --jq .object.sha
```
