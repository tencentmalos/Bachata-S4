// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DiagnosticsCommands: registers the graphics/perf debugging toolkit's control
// commands into a Foundation DebugCommandRegistry (spec
// docs/specs/android-graphics-debugging-toolkit.md §3.1). Both Android
// Service.dump (via DumpsysBridge) and, later, the ImGui panel call through this
// same registry, so there is one typed backend and one help/schema source.
//
// Only the commands whose backend actually exists are implemented now:
//   * debug_status      -- formats a non-blocking DiagnosticsHub snapshot.
//   * renderdoc_status  -- whether the RenderDoc API is loaded.
//   * overlay status    -- overlay redraw counter (the overlay itself is later).
//
// The remaining spec commands (renderdoc_capture, guest_screenshot,
// profiler_ring/profiler_capture, performance_capture, guest_command_trace,
// gpu_command_trace, overlay show/hide) are registered but return an explicit
// "not-implemented" status. §3.1 forbids faking success; a command must never
// report a capture/receipt it did not produce. As each backend lands, its
// handler is replaced -- the command name and help stay stable.
//
// Reading a status command never blocks on runtime work: debug_status calls
// DiagnosticsHub::QuerySnapshot, which copies atomics + short strings only.

#pragma once

#include <cstdint>
#include <functional>

namespace spatial::debugbus {
class DebugCommandRegistry;
}

namespace Core::Diagnostics {

class DiagnosticsHub;

// A monotonic clock reading in nanoseconds, injected so tests are deterministic
// and the module has no hidden clock dependency.
using MonotonicClockNs = std::function<std::uint64_t()>;

// Registers all §3.1 toolkit commands into `registry`, backed by `hub`. The hub
// and clock must outlive the registry. Safe to call once during app init on a
// debug build; §3.1 limits registration to debug builds / adb-reachable control.
void RegisterDiagnosticsCommands(spatial::debugbus::DebugCommandRegistry& registry,
                                 DiagnosticsHub& hub, MonotonicClockNs clock);

}  // namespace Core::Diagnostics
