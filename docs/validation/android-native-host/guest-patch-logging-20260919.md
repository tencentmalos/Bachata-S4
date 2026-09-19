# Guest patch 独立日志验证（2026-09-19）

## 目的

为 custom SDK 的 `shad_sdk_log(tag, value)` 提供独立、可检索的落盘路径，避免 guest
标定事件淹没 shadPS4 主日志，同时保留主日志已有的初始化、刷新和关闭生命周期。

## 实现边界

- `Common::Log::Setup()` 创建主 `shadps4.log` sink 后，再创建独立的
  `guest-patch.log` sink 和 `GuestPatch` logger。
- `Common::GuestPatchLog::Write()` 只负责生成固定格式的
  `[PATCH_LOG] package=... tag=... value=... context=... thread=... generation=... invocation=...`
  记录，并交给 `Common::Log::WriteGuestPatch()`。
- `GuestPatch` logger 不加入 `ALL_LOGGERS`，因此不会进入控制台、主 logger 的过滤规则、
  去重 sink 或异步主 sink。日志事件在 info 级别立即 flush；guest 日志文件也在全局
  `Common::Log::Flush()` 中刷新。
- 写日志和格式化均是 best effort；文件不可写、logger 尚未初始化或格式化失败时，
  只丢弃诊断事件，不改变 guest patch 的返回值和执行流程。
- 当前没有把 Foundation 的完整 log module 依赖拉进 shadPS4 host profile。Foundation
  profiler/litep ring 仍用于计数器，tagged log 单独写入 shadPS4 日志目录中的文件。

## SDK 语义

`shad_sdk_query(1)` 的 bit2 表示 tagged log 能力。recipe 的 `logs` 数组最多声明 64 个
数字 tag；未声明 tag 返回 `-22`，声明成功后在状态中累计样本数和最后值。guest 不传字符串、
host 指针或任意 symbol，宿主根据 package 中的 tag 名称补齐上下文。

## 验证

- custom SDK fixture 重新生成通过：`GUEST_FUNCTIONS_BUILD_PASS`，输出目录为
  `build/guest-patch-log-test-current`。
- NDK 29 Android host 目标通过增量编译：

  ```text
  cmake --build build/android-host-api33-sbs-20260919-r29/native \
    --target shadps4_host -j 6
  [7/7] Linking CXX shared library libshadps4_host.so
  ```

- 旧 NDK 28 目录仍会在既有 `std::stop_token` / `std::jthread` 代码处失败，该失败与本次
  日志改动无关；本轮不改动旧构建配置。

## Beat Saber 标定结果（本日更新）

静态工具已通过只修正 ELF e_type 的分析副本恢复工作；原 SELF/ELF/分析副本分别记录 SHA。
现已部署精确构建绑定的 11 入口观察器及 typed HLE 返回码 wrapper，实际 guest 日志先后定位
设备选择、HMD provider、owner user 与 Reprojection 初始化失败。
详见 [Beat Saber VR 初始化验证](beatsaber-vr-init-20260919.md) 与
[guest 源码说明](../../../guest/games/CUSA12878/01.00/README.md)。
独立日志已被真实 Beat Saber guest SDK 调用验证；这不等于整帧 C/C++ 重编译或游戏可玩验收。
