# 给执行 AI 的任务书：实现 Android / FEX V0

将下面任务交给执行 AI，并提供此仓库访问权限即可。本文不代表已启动任何其他 AI。

## 执行任务

请在当前 shadPS4 仓库实现第一个 Android/FEX 验证版，按照下面文档执行，不要仅返回一份计划或接口空壳：

1. 读取根目录 `AGENTS.md`，了解固定源码、子仓贡献规则和本地独立改动。
2. 读取 [V0 总 spec](android-fex-v0.md)、[CPU API 契约](android-fex-v0-api.md)、[验收矩阵](android-fex-v0-acceptance.md)。三份文档共同定义范围和完成条件；验收矩阵是逐项结果清单。
3. 读取 [a7128893 基础状态](../baselines/2026-09-07-android-fex-foundation.md)。以包含本 spec 的提交为实现起点创建 `codex/android-fex-v0`，将 `a7128893` 保留为比较基线。
4. 读取 [子仓归属](../subrepository-ownership.md) 与 [Foundation 接入](../foundation-integration.md)。先核对自有 remote/分支；直接复用现有基础设施构建入口，完成反射/packing 的依赖接入。TCP 可后置，启用时复用 foundation 网络设施。

目标是 Android 16 / ARM64 / 真实 16 KiB 页的普通 app，NDK/bionic 构建、FEXCore 执行自有 x86-64 fixture。公开 CPU API 不暴露 FEX 私有类型，覆盖 Run/有限 Step/暂停停止/寄存器/失效/HLE callback；原生 Vulkan Surface 完成最小显示和生命周期验证。

按 M0→M5 推进。先落实工具链和依赖锁、FEX Android 构建闭包，再做 CPU/VM/ABI/控制；Android app/Surface 可在依赖允许时推进。阶段性提交，更新任务进度和已知问题。按事实区分代码完成、host 验证、16 KiB 实机验收。

首版采用与参考 bridge 一致的 FEX `f2b679f6…`；独立 `references/FEX` 的 `50e6eee9…` 是审计/比较版本，不直接替换。需要改版本时先形成简短技术决定及迁移测试，不把“升级到最新”当作默认修复。

本轮必须执行 spec 中实际允许完成的开发、构建、测试和证据整理。缺少设备时继续完成其余工作，明确标 NOT_RUN；不要虚报 V0_ACCEPTED。若适用贡献规则或外部依赖阻碍必须修改的 FEX 代码，准确说明阻断，并继续可独立完成的部分，不绕过规则。

## 实现时不能偏离的重点

- 不用 glibc 子进程、FEXLoader、Box64、rootfs 或伪造页面大小满足 NDK/16 KiB 要求。
- 不全局改 4096→16384；实际 host mapping、逻辑代码索引、PS4 ABI 粒度分别处理。
- 不把 Dynarmic 风格 API 等同于完整软件 MMU；V0 DirectMapped 模式的边界必须显式。
- V0 允许 ExplicitPublication SMC，但必须用真实 guest store→发布→跨线程失效测试，并拒绝请求 TransparentSMC 的 caller。不得据此声称任意游戏 SMC 已兼容。
- 无 HLE 的死循环也能中断；不以 kill/force-stop 代替正常停止。
- 有限 Step 真正执行一个受支持 guest 指令；未支持类别在执行前拒绝，不用“一个 block”代替。
- LLDB 读取稳定的安全点快照；异步 host stop 的旧 CPUState 不得伪装成精确 guest 寄存器。
- 不提交商业游戏、下载的 runtime 或无关本地工作，不修改已有基础状态记录。
- 不重复实现通用网络/反射/序列化框架；不要把 foundation 尚未验证的 allocator/TLS/module 配置直接带入 ART/FEX 进程。API 36 工具链必须检查真实 sysroot，不能只信本地 NDK 目录标签。

## 最终交付

- 实现分支/提交和依赖版本；明确各模块从哪里迁入。
- 可重建 debug APK、SHA-256、native Build IDs、匹配符号及获取位置。
- 构建/产物检查/设备测试/证据收集脚本与 README。
- 验收矩阵的逐项结果 JSON，含真实环境、fixture hash、失败/未运行原因。
- `docs/validation/v0/` 下的环境锁、页大小/依赖审计、技术决定、实施报告与下一阶段建议。
- 最终状态只能根据事实选择 V0_ACCEPTED、V0_IMPLEMENTED_DEVICE_PENDING、V0_BLOCKED 或 V0_IN_PROGRESS。

## 需要深入时再查的资料

- [Android / ARM64 整合审计](../android-arm64-integration-audit.md)
- [整体原生化方案](../fex-android16-native-guest-host-plan.md)
- [FEX / Dynarmic 比较](../fexcore-dynarmic-source-comparison.md)
- [guest debugger 分析](../fex-guest-debugger-feasibility.md)、[host LLDB 工作流](../fex-lldb-host-guest-workflow.md)
- [references 固定版本与初始化](../../references/README.md)

V0 对音频、完整 PS4 loader/HLE、VR 和透明 SMC 的范围，以本 spec 的明确规定为准；整体研究方案中更远期的目标不自动扩成此次交付条件。
