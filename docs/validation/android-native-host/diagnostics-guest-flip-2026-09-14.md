# DiagnosticsHub guest_flip producer + live present + convenience pushes (Work Package A, step 7)

2026-09-14，主仓 `codex/android-fex-round2`,继续 [图形调试工具整包 spec](../../specs/android-graphics-debugging-toolkit.md) §3.1 的 producer 接线。上一步(df932bfd)只在 Run 返回时发布 HostPresent;本步接入 **guest_flip**(黑屏排查的关键信号)并让两信号逐 flip 实时更新。

## 交付

**hub 便捷 producer 接口**(diagnostics_hub_registry.{h,cpp}):
- `DiagnosticsHub::Advance(signal, delta=1)` 和 `PublishCount(signal, absolute)` —— 内部取 monotonic 时间戳,在 hub 锁下拷贝 active 发布器指针后**释放锁**再 push;无 active generation 时安全 no-op。producer(present/submit/flip/PM4 点)**无需持有发布器引用**,避免与 generation 的寿命耦合。

**guest_flip producer**(guest_graphics_hle.cpp,flip 提交成功处):
- `sceGnmSubmitAndFlipCommandBuffers` 返回 0(guest flip 被接受)时:`Advance(GuestFlip)` + `PublishCount(HostPresent, guest_presents)`。**GuestFlip 与 HostPresent 的差值就是黑屏的直接判据**:guest 请求了 flip,host 是否真的 present 了。两信号现在逐 flip 实时更新,而非仅 Run 返回。
- 保留原返回值语义;仅在 `flip_result == 0` 时计数,失败/拒绝不计。
- 锁序安全:此处持 `graphics.SubmissionMutex()`,hub 便捷 push 只取自身小锁、绝不取 submission mutex,无锁序反转。

backend 的 Run-返回 HostPresent 发布保留(`PublishCount` 仅前进时更新时间戳,故与实时发布幂等一致,`guest_presents` 单调不减)。

## 验证

- registry 测试扩到 **32 checks**(新增便捷 push 的空 hub no-op、active 落点、revoke 后再 no-op),本机 `clang++ -std=c++23 -Wall -Wextra` 通过,0 failures。
- **真实 NDK r29** `aarch64-linux-android33 -std=c++23 -Wall -DSHADPS4_TYPED_HLE_HOST=1` 全 host include 环境:`diagnostics_hub_registry.cpp` 与**改动的 `guest_graphics_hle.cpp`** 均 exit 0。
- hub/publisher/commands 既有单元测试(28/41)逻辑未变。

## 边界

- 仍未接:pm4_consumed、host_draw、queue_submit、overlay_redraw 的 producer;SessionCore→hub 的 phase/run_uuid;`Service.dump` 的 `SetDumpsysRegistry` app 接线。
- GuestFlip 计数在 flip HLE 成功分支;非图形路径/合成 flip 不计——这是有意的(只记真实 guest flip)。
- 未做完整 host/APK 链接或真机运行(本机不可跑);仅真实 NDK 逐 TU 编译 + 本机单测。未改 FEX/Foundation/Citron。
