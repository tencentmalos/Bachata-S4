// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string_view>
#include "core/guest_cpu/hle/call_adapter.h"

namespace Core::HostRuntime {
// libkernel 11.00 takes a text prefix and walks x86 frame records. Walking the
// native ARM stack here would report the adapter, not the guest caller. Keep
// every guest read checked and label incomplete traces instead of following
// corrupt frame pointers or manufacturing a successful unwind.
struct GuestBacktrace {
    std::array<char, 1024> prefix{};
    size_t prefix_size{};
    std::string_view prefix_status{"complete"};
    std::array<u64, 256> addresses{};
    size_t count{};
    std::string_view stop{"complete"};
    u64 stopped_at{};
};

inline GuestBacktrace CaptureGuestBacktrace(GuestCpu::GuestAddressSpace& space,
                                           const GuestCpu::RegisterFile& registers) {
    using namespace GuestCpu;
    GuestBacktrace trace;
    if (!HasAll(registers.validity, RegisterValidity::Gpr)) {
        trace.stop = "registers_unavailable";
        return trace;
    }
    const u64 prefix = registers.Get(Gpr::Rdi);
    if (!prefix) {
        trace.prefix_status = "null";
    } else {
        for (; trace.prefix_size < trace.prefix.size(); ++trace.prefix_size) {
            char c{};
            if (prefix > UINT64_MAX - trace.prefix_size ||
                !space.ReadData({prefix + trace.prefix_size},
                                std::as_writable_bytes(std::span{&c, 1}))) {
                trace.prefix_status = "unreadable";
                break;
            }
            if (!c) break;
            trace.prefix[trace.prefix_size] = c;
        }
        if (trace.prefix_size == trace.prefix.size()) trace.prefix_status = "truncated";
    }
    const u64 rsp = registers.Get(Gpr::Rsp);
    u64 caller{};
    if (!rsp || !space.ReadData({rsp}, std::as_writable_bytes(std::span{&caller, 1}))) {
        trace.stop = "unreadable_return";
        trace.stopped_at = rsp;
        return trace;
    }
    trace.addresses[trace.count++] = caller;
    u64 frame = registers.Get(Gpr::Rbp);
    while (frame) {
        trace.stopped_at = frame;
        if (trace.count == trace.addresses.size()) {
            trace.stop = "frame_limit";
            return trace;
        }
        if (frame < rsp || (frame & 7)) {
            trace.stop = "invalid_frame";
            return trace;
        }
        std::array<u64, 2> record{};
        if (frame > UINT64_MAX - 15) {
            trace.stop = "unreadable_frame";
            return trace;
        }
        // The two aligned slots can belong to different stack mappings. Admit
        // both together so retirement cannot replace one between the reads.
        const std::array<GuestAddressSpace::DataRequest, 2> requests{
            GuestAddressSpace::DataRequest{{{frame}, 8}},
            GuestAddressSpace::DataRequest{{{frame + 8}, 8}}};
        {
            auto pins = space.AcquireDataBatch(requests);
            if (!pins) {
                trace.stop = "unreadable_frame";
                return trace;
            }
            for (size_t i = 0; i < record.size(); ++i)
                std::memcpy(&record[i], pins.Value()[i].Bytes().data(), 8);
        }
        trace.addresses[trace.count++] = record[1];
        if (record[0] && record[0] <= frame) {
            trace.stop = "nonascending_frame";
            return trace;
        }
        frame = record[0];
    }
    trace.stopped_at = 0;
    return trace;
}
} // namespace Core::HostRuntime
