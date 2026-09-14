# DiagnosticsHub registry — revocable per-generation publishers (Work Package A, step 4)

2026-09-14，主仓 `codex/android-fex-round2`,实施 [图形调试工具整包 spec](../../specs/android-graphics-debugging-toolkit.md) §3.1「Session generation 通过可撤销引用挂入」。这是 DiagnosticsHub 的 **owner/容器** 半边;上一步 [e29f875d](diagnostics-hub-2026-09-14.md) 是发布器本身。**仍未接 producer、未接 `Service.dump`、未接命令 registry。**

## 交付

新增 `src/core/diagnostics/diagnostics_hub_registry.{h,cpp}`:进程内唯一的 `DiagnosticsHub`(function-local static,沿用 JNI `SessionCore& Session()` 单例风格,与 `std::atomic<shared_ptr>` 无关——本仓未用该特性,为 NDK libc++ 可移植性回避)。

- **`Register(generation, pid)`** —— 建新 `DiagnosticsPublisher`,设为 active,返回给该 generation 用于推进。`generation==0` 拒绝。
- **`Revoke(generation)`** —— 仅当 active 仍属于该 generation 时清除;**旧 generation 的迟到 teardown 是 no-op**,不会抹掉更新 session 的发布器。
- **`QuerySnapshot(out, now_ns)`** —— 在 hub 锁下拷贝 `shared_ptr`,**释放锁后**再调无锁 `CopyInto`。reader 绝不跨 hub 锁做快照拷贝;并发 Revoke 也不会在 reader 脚下析构发布器——reader 自己的 `shared_ptr` 保活。
- hub 锁只用于指针记账(copy/compare/swap shared_ptr),从不跨 `CopyInto` / Session mutex / VM drain / GPU fence / 导出,不可能与 runtime 锁死锁。hub 从不回调 SessionCore/GuestRuntime:producer 推,reader 拉。

## 验证

新增 `tests/host_runtime/diagnostics_hub_registry_tests.cpp`(26 checks),注册进 CMake `HOST_BUILD_PROBES`,链接 `shadps4_host`;`diagnostics_hub_registry.cpp` 加入 `if(BUILD_HOST_CORE)` 的 `target_sources`。

- 本机 `clang++ -std=c++20 -Wall -Wextra`:**26 checks / 0 failures**,三次运行确定性一致,无警告。
- 覆盖:空 hub(no session,`generation==0` 拒绝);register→query→revoke 生命周期;**stale generation 不能 revoke 更新的**(gen5 迟到 teardown 在 gen7 active 后为 no-op);revoke 后快照 `has_session=false`;**并发 use-after-free 守护**:一个 churner 线程 30000 次 Register/Advance/Revoke(generation 严格递增),一个 reader 线程持续 QuerySnapshot——数千次读全部 identity 自洽(`pid==generation*10`),0 次撕裂/损坏,churner 跑完无崩溃;进程单例 `Instance()` 同一性。
- clangd 0 diagnostics;CMake if/endif 平衡未变(59/59)。
- Sanitizer 同前:本机 Apple clang 17 TSan/ASan 环境损坏(非本代码),以无损坏并发读 + shared_ptr 保活 + 锁只记账证明安全。

## 边界

- 未接 producer:generation 尚未在启动/present/submit 点调用 `Register`/`Advance`/`Revoke`;`Service.dump` 尚未调 `QuerySnapshot`。这些是下一步(hub producer 接线 + 命令 registry)。
- `diagnostics_hub_registry.cpp` 仅编入 host DSO(`BUILD_HOST_CORE`);桌面暂无引用。
- 未改 FEX/Foundation/Citron。
