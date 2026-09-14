// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/diagnostics/diagnostics_hub.h"

namespace Core::Diagnostics {

void DiagnosticsPublisher::SetRunUuid(std::string_view uuid) {
    std::lock_guard lock(strings_mtx_);
    run_uuid_.assign(uuid);
}

void DiagnosticsPublisher::SetStage(std::string_view stage) {
    std::lock_guard lock(strings_mtx_);
    stage_.assign(stage);
}

void DiagnosticsPublisher::SetStopReason(std::string_view reason) {
    std::lock_guard lock(strings_mtx_);
    stop_reason_.assign(reason);
}

void DiagnosticsPublisher::SetDriverIdentity(std::string_view identity) {
    std::lock_guard lock(strings_mtx_);
    driver_identity_.assign(identity.substr(0, 4096));
}
void DiagnosticsPublisher::SetTerminalDetail(std::string_view detail) {
    std::lock_guard lock(strings_mtx_);
    terminal_detail_.assign(detail.substr(0, 4096));
}
void DiagnosticsPublisher::CopyInto(DiagnosticsSnapshot& out, u64 now_ns) const {
    // Numeric identity from atomics.
    const u64 gen = generation_.load(std::memory_order_acquire);
    out.has_session = gen != 0 && active_.load(std::memory_order_acquire);
    out.generation = gen;
    out.pid = pid_.load(std::memory_order_relaxed);
    out.phase = phase_.load(std::memory_order_relaxed);
    out.snapshot_ns = now_ns;

    for (u32 i = 0; i < kAdvanceSignalCount; ++i) {
        const AtomicCounter& c = counters_[i];
        AdvanceCounter& d = out.counters[i];
        d.count = c.count.load(std::memory_order_relaxed);
        d.last_advance_ns = c.last_advance_ns.load(std::memory_order_relaxed);
        d.available = c.available.load(std::memory_order_relaxed);
    }

    // Strings under the dedicated lock -- held only for these copies.
    {
        std::lock_guard lock(strings_mtx_);
        out.run_uuid = run_uuid_;
        out.stage = stage_;
        out.stop_reason = stop_reason_;
        out.driver_identity = driver_identity_;
        out.terminal_detail = terminal_detail_;
    }
}

}  // namespace Core::Diagnostics
