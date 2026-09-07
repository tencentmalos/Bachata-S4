# 研究证据索引

以下均为 2026-09-07 的有界记录，不是持续更新的设备验证状态。

| 文件 | 内容与限制 |
|---|---|
| [android-foundation.lock.json](android-foundation.lock.json) | Android 基线提交、出处、工具配置与未验证项 |
| [android-baseline-sources-2026-09-07.json](android-baseline-sources-2026-09-07.json) | 发布信息、公开源码映射和仓库目录快照 |
| [android-arm64-interface-comparison.json](android-arm64-interface-comparison.json) | 指定目录的 tracked 文件逐字节比较；未声称整个 fork 相同 |
| [android-arm64-runtime-contract-tests-2026-09-07.txt](android-arm64-runtime-contract-tests-2026-09-07.txt) | 真实 C++ 控制/输入/音频实现的 18 个 host 测试及命令；没有运行 Android/FEX/Vulkan |
| [fex-dynarmic-source-size-2026-09-07.json](fex-dynarmic-source-size-2026-09-07.json) | 固定提交源码文本规模，含精确计量范围；不代表性能 |
| [lldb-cross-disassembly-2026-09-07.txt](lldb-cross-disassembly-2026-09-07.txt) | macOS LLDB 对 x86 字节的交叉反汇编；不是 Android guest 调试验收 |

原始运行记录中的绝对路径、临时目录和机器版本保留为历史证据。新机器请使用 `scripts/analysis/` 与 references 初始化说明，不应照搬历史临时路径。更新实验时保存新的日期和精确版本，不把旧输出改写为新平台成功。
