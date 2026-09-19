# 2026-09-20 跨仓提交与分支切换

本轮按用户要求先保存全部既有源码、测试和文档，再切到 `feature/malos/hle_vr` 继续 Buffer Cache 拆分。游戏二进制、APK/ELF、构建目录、原始性能采样二进制不入库；独立且干净的 `externals/dear_imgui` 不重复导入。

## 已发布检查点

| 仓库 | 分支 | 已推送提交 | 内容 |
|---|---|---|---|
| profiler SDK | `codex/shadps4-stage-profiler` | `e00321ea40c4c608aff74a787200bc59493eea19` | 发布原有 stage/async flow 提交 |
| Foundation | `codex/shadps4-internal-scale` | `39a8b9412b27d52d68fcf420f0431432beaac575` | LiteTrace async flow、ring 修订及 SDK pin |
| profiler analysis engine | `codex/litep-analysis-engine` | `4f5a316aaaf2dc226ff37db58239ae26b117e03f` | 发布已有分析引擎提交 |
| dev_tools | `codex/litep-profiler` | `e2444774` | Litep 帧贡献、异步流、KGSL sidecar 与测试 |
| dev_tools | 同上 | `5d7a8234` | guest workset/重编译与重建符号工具 |
| dev_tools | 同上 | `01e8233bc3c60518985bccdd76b62777828197f9` | 合并远端已有 17 个提交后推送，没有改写远端历史 |
| shadPS4 | `codex/android-fex-round2` | `f8d264e527ec589c418699d0a4f67b80641bff8b` | Android Settings/语言/离线 epoll、内存诊断、profiling、区域锁、中文文档及历史证据 |

先推送内层依赖，确认远端可达后更新父仓 pin。检查点的各仓 HEAD/upstream 均为 0/0，记录见 [delivery.json](evidence/buffer-upload-unlock-20260920/delivery.json)。主仓 origin 推送被 GitHub 正常重定向到 `tencentmalos/Bachata-S4`，本轮没有私自替换 remote。

旧报告中的“本地未提交/未推送”描述的是其记录时刻；上述检查点已经交付这些工作。此前已经发布的 FEX、Oboe 等依赖没有新增修改。

## 新分支实现

从 `f8d264e5` 创建、切换并发布 `feature/malos/hle_vr`，随后完成 [上传与脏页锁拆分](buffer-upload-unlock-20260920.md)。该分支的新提交包含实现、58 项定向检查、实际 APK 短时设备证据及本记录。没有新增 guest 二进制 patch、修改 FEX/Foundation 实现或进行完整游戏回归。

交付前检查主仓和子仓状态，按路径显式暂存；新证据以无损 gzip 保存原始文本，避免格式清理改变源 SHA。旧四组原始文本证据继续使用限定目录的 `.gitattributes -whitespace`，不影响源码 whitespace 检查。工具仓的 `_out/` 仅为排除的构建产物。
