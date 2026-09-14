# Trace identity vocabulary (Work Package A, step 2)

2026-09-14，主仓 `codex/android-fex-round2`,实施 [图形调试工具整包 spec](../../specs/android-graphics-debugging-toolkit.md) §4.1 的身份结构。按 spec §6,身份结构必须在工作包 A 开始时定下,避免工作包 B 返工记录接口。这是纯数据 + 序列化层,**不接入任何 runtime、不产生 trace、不代表任何工具已可用**。

## 交付

新增 header-only `src/core/diagnostics/trace_identity.h`(namespace `Core::Diagnostics`),定义所有诊断记录共用的:

- **`CommonHeader`** —— spec §4.1 公共头:schema/version、run UUID、PID、session generation、capture UUID、clock 域/单位、主仓/FEX/Foundation/SDK revisions、host/JNI Build IDs、title/version/guest 模块 hash、Turnip/RenderDoc 身份、config hash、截断原因。`ToJsonObject()` 产出确定顺序、全部带引号的 JSON 对象。
- **`CorrelationKeys`** —— spec §4.1 全套关联 ID(run/session/capture、context/thread/invocation、submission/command_buffer/queue/queue_generation、packet/offset/parent_ib/action、accepted_guest_flip/host_submit/host_present、resource/backing/shader_hash/pipeline_hash)。`ToFields()` 只输出已 set 或带显式 unknown 原因的 key。
- **`CorrelationId`** —— 一个 64 位 ID 要么是已知值,要么是「unknown + 原因」。

强制两条 spec 不变量,由类型而非调用者保证:

1. **64 位 ID 序列化为十进制字符串。** JSON importer 若把数字存成 IEEE double,会静默截断 2^53 以上的值(submission id、guest VA、shader hash 会丢低位)。`CorrelationId::Serialize()` 恒发引号字符串。
2. **不可得的字段是「unknown + 原因」,绝不是 0。** 0 是合法 ID,拿它当「未追踪」会伪造出假匹配。`CorrelationId` 默认 unknown;`CorrelationKeys::ToFields()` 省略纯默认-unknown 的 key,使「present but unknown」与「与本记录无关」可区分。

PS4/Session 专属身份(title/version、guest 模块 hash、Turnip/RenderDoc 身份、Orbis submission id)按 spec §2.3 留在主仓,不下沉 Foundation。

## 验证

新增 `tests/host_runtime/trace_identity_tests.cpp`,沿用既有 `CHECK` 宏 harness,注册进 CMake `HOST_BUILD_PROBES` 块(header-only,只需 `common/types.h`,不链接 host DSO)。

- 本机 macOS 直编直跑:`clang++ -std=c++20 -Wall -Wextra -Isrc`,**23 checks / 0 failures**,run exit 0。
- 覆盖:`0x5A5A5A5A5A5A5A5A` 和 `u64::max` 十进制字符串保真(证明非 double 截断);值 0 与 unknown 可区分;`CorrelationKeys` 只输出 set/显式-unknown key,默认-unknown 和空字符串被省略;`CommonHeader` JSON 全值带引号、id 为字符串、引号/tab/换行转义正确、空 `truncation_reason` 如实输出为 `""`;`FieldsToJsonObject` 空列表 `{}` 与确定顺序。
- CMake if/endif 平衡未变(59/59),仅在既有 probes 块内新增一个 `add_executable`。

## 边界

- 未接入任何 producer:runtime 尚未填充这些字段,也未产出记录。字段的实际来源(§4.1 要求的 minting、generation 纪律、同 VA remap 不复用完整键)属于后续 producer 增量。
- 未实现 §4.4 的离线 decoder、`foundation.gpu-action-index.v1` 规范化输出或容器适配。
- 未改 FEX/Foundation/Citron。
