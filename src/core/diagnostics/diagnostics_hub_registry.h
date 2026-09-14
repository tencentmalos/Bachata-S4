// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DiagnosticsHub container: the process-owned owner of the active generation's
// DiagnosticsPublisher (spec docs/specs/android-graphics-debugging-toolkit.md
// §3.1, "Session generation 通过可撤销引用挂入").
//
// One hub lives for the process. Each session generation registers a publisher
// and revokes it at teardown. A reader (Android Service.dump on the main thread,
// or later ImGui) queries a snapshot without ever blocking on runtime work:
//
//   * The hub holds shared_ptr<DiagnosticsPublisher>. QuerySnapshot copies that
//     shared_ptr under a short hub mutex, releases the mutex, THEN calls the
//     lock-free DiagnosticsPublisher::CopyInto. So the reader never holds the hub
//     lock across the snapshot copy, and the publisher cannot be freed mid-read
//     even if the owning generation revokes concurrently -- the reader's
//     shared_ptr keeps it alive.
//   * Revoke is generation-checked: a stale generation clearing after a newer one
//     already registered is a no-op, so a late teardown cannot wipe a live
//     session's publisher.
//   * The hub mutex is only ever held for pointer bookkeeping (copy/compare/swap
//     a shared_ptr), never across CopyInto, a Session mutex, VM drain, GPU fence,
//     or export. It cannot deadlock against the runtime's locks.
//
// The hub deliberately does NOT reach into SessionCore/GuestRuntime; producers
// push into the publisher, readers pull snapshots. This keeps Service.dump off
// every runtime lock (the whole point of §3.1).

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "core/diagnostics/diagnostics_hub.h"

namespace Core::Diagnostics {

class DiagnosticsHub final {
public:
    // Process-wide instance (function-local static, matching the SessionCore
    // Session() pattern). Never destroyed before process exit.
    [[nodiscard]] static DiagnosticsHub& Instance();

    DiagnosticsHub() = default;
    DiagnosticsHub(const DiagnosticsHub&) = delete;
    DiagnosticsHub& operator=(const DiagnosticsHub&) = delete;

    // Registers a fresh publisher for `generation` and makes it the active one,
    // replacing any previous. Returns the publisher so the owning generation can
    // push advances into it. generation must be non-zero.
    [[nodiscard]] std::shared_ptr<DiagnosticsPublisher> Register(std::uint64_t generation,
                                                                 std::uint64_t pid);

    // Clears the active publisher IF it still belongs to `generation`. A stale
    // generation (an older teardown arriving after a newer Register) is a no-op.
    // Returns true if this call actually cleared it.
    bool Revoke(std::uint64_t generation);

    // Non-blocking snapshot of the active generation. Returns false with
    // out.has_session == false when no generation is registered. now_ns stamps
    // the snapshot; callers pass a monotonic clock reading.
    bool QuerySnapshot(DiagnosticsSnapshot& out, std::uint64_t now_ns) const;

    // The generation currently registered, or 0 if none.
    [[nodiscard]] std::uint64_t ActiveGeneration() const;

private:
    mutable std::mutex mtx_;
    std::shared_ptr<DiagnosticsPublisher> active_;
    std::uint64_t active_generation_{0};
};

}  // namespace Core::Diagnostics
