// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <charconv>
#include <optional>
#include <string_view>
#include "common/types.h"
#include "core/guest_cpu/hle/call_adapter.h"

namespace Core::HostRuntime {
// Constructed per entry only when explicitly enabled. Success traffic cannot
// exhaust the independent error budget; try-lock EBUSY is recorded as a result,
// not treated as proof of an emulation failure.
class GuestRwlockDiagnostics {
public:
    struct Record {
        std::array<u64, 6> args{};
        u64 call{}, result{}, rsp{}, caller{};
        bool adapter_ok{}, caller_readable{};
    };
    static bool Matches(std::string_view name) {
        return name.starts_with("scePthreadRwlock") || name.starts_with("pthread_rwlock");
    }
    static std::optional<u64> ParseSlot(std::string_view text) {
        if (text.empty())
            return u64{0};
        if (text.starts_with("0x") || text.starts_with("0X"))
            text.remove_prefix(2);
        u64 value{};
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
        if (error != std::errc{} || end != text.data() + text.size())
            return {};
        return value;
    }
    explicit GuestRwlockDiagnostics(u64 slot = 0) : slot(slot) {}

    template <class Invoke, class Emit>
    GuestCpu::Status Call(GuestCpu::Hle::HleCallFrame& frame, Invoke&& invoke, Emit&& emit) {
        using namespace GuestCpu;
        if (slot && frame.registers.Get(Gpr::Rdi) != slot)
            return invoke(frame);
        Record r;
        r.call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        r.rsp = frame.registers.Get(Gpr::Rsp);
        for (unsigned i = 0; i < r.args.size(); ++i)
            r.args[i] = frame.registers.Get(Hle::kSysVIntegerOrder[i]);
        // No pin, observer, or diagnostic lock may span the real wait.
        auto status = invoke(frame);
        r.adapter_ok = bool(status);
        r.result = frame.registers.Get(Gpr::Rax);
        auto& budget = !r.adapter_ok || u32(r.result) ? errors : successes;
        const u32 limit = !r.adapter_ok || u32(r.result) ? 64 : 32;
        if (!Claim(budget, limit))
            return status;
        try {
            if (frame.space && r.rsp)
                r.caller_readable = bool(frame.space->ReadData(
                    GuestAddress{r.rsp}, std::as_writable_bytes(std::span{&r.caller, 1})));
            emit(r);
        } catch (...) {
            // Even a failed diagnostic read/sink must preserve the real result.
        }
        return status;
    }

private:
    static bool Claim(std::atomic<u32>& used, u32 limit) {
        auto value = used.load(std::memory_order_relaxed);
        while (value < limit) {
            if (used.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                return true;
        }
        return false;
    }
    const u64 slot;
    std::atomic<u64> calls{};
    std::atomic<u32> successes{}, errors{};
};
} // namespace Core::HostRuntime
