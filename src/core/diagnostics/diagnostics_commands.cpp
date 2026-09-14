#include "core/diagnostics/overlay_control.h"
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/diagnostics/diagnostics_commands.h"

#include <charconv>
#include <sstream>
#include <string>
#include <vector>

#include "spatial/debugbus/DebugCommandRegistry.h"

#include "core/diagnostics/diagnostics_hub.h"
#include "core/diagnostics/pipeline_handoff.h"
#include "core/diagnostics/diagnostics_hub_registry.h"
#include "video_core/renderdoc.h"
#include "video_core/gpu_reshape_status.h"
#include "video_core/renderdoc_capture.h"

namespace Core::Diagnostics {
namespace {

// Formats "<count>" or "unavailable" for a signal, so a build that cannot measure
// a signal never prints a misleading 0 (spec §3.1).
std::string FormatCounter(const AdvanceCounter& c, u64 now) {
    if (!c.available) {
        return "unavailable";
    }
    std::ostringstream out;
    out << c.count;
    if (c.count > 0) {
        out << " (age_ns=" << (now >= c.last_advance_ns ? now - c.last_advance_ns : 0) << ")";
    } else {
        out << " (never)";
    }
    return out.str();
}

std::string FormatStatus(const DiagnosticsSnapshot& snap) {
    std::ostringstream out;
    out << "session: " << (snap.has_session ? "active" : "none") << "\n";
    if (!snap.generation) { out << "snapshot_ns: " << snap.snapshot_ns << "\n"; return out.str(); }
    out << "generation: " << snap.generation << "\n";
    out << "pid: " << snap.pid << "\n";
    out << "phase: " << snap.phase << "\n";
    out << "run_uuid: " << (snap.run_uuid.empty() ? "unknown" : snap.run_uuid) << "\n";
    out << "stage: " << (snap.stage.empty() ? "unknown" : snap.stage) << "\n";
    if (!snap.stop_reason.empty()) {
        out << "stop_reason: " << snap.stop_reason << "\n";
    }
    out << "driver: " << (snap.driver_identity.empty() ? "unavailable" : snap.driver_identity) << "\n";
    if (!snap.terminal_detail.empty()) out << "terminal_detail: " << snap.terminal_detail << "\n";
    out << "snapshot_ns: " << snap.snapshot_ns << "\n";
    for (u32 i = 0; i < kAdvanceSignalCount; ++i) {
        const auto sig = static_cast<AdvanceSignal>(i);
        out << ToString(sig) << ": " << FormatCounter(snap.Counter(sig), snap.snapshot_ns) << "\n";
    }
    return out.str();
}

// Every not-yet-implemented command routes here so the reply is honest and
// uniform. §3.1: never fake success or a receipt.
std::string NotImplemented(std::string_view command) {
    std::ostringstream out;
    out << "status: not-implemented\n";
    out << "command: " << command << "\n";
    out << "detail: backend not wired in this build; no capture/receipt produced\n";
    return out.str();
}

// Formats a RenderDoc capture receipt (spec §3.2). Ready always includes the
// stable file path; failure includes the reason. Never claims Ready without a path.
std::string FormatReceipt(const VideoCore::CaptureReceipt& r) {
    std::ostringstream out;
    out << "status: " << r.command_status << "\n";
    out << "request_id: " << r.request_id << "\n";
    out << "generation: " << r.generation << "\n";
    out << "completed_frames: " << r.completed_frames << "\n";
    out << "cleanup_pending: " << (r.cleanup_pending ? "true" : "false") << "\n";
    out << "coverage: " << r.coverage << "\n";
    out << "state: " << VideoCore::ToString(r.state) << "\n";
    out << "requested_frames: " << r.requested_frames << "\n";
    out << "captures_before: " << r.num_captures_before << "\n";
    out << "captures_after: " << r.num_captures_after << "\n";
    if (!r.run_uuid.empty()) {
        out << "run_uuid: " << r.run_uuid << "\n";
    }
    if (!r.capture_uuid.empty()) {
        out << "capture_uuid: " << r.capture_uuid << "\n";
    }
    if (!r.file_path.empty()) {
        out << "file: " << r.file_path << "\n";
        out << "timestamp: " << r.capture_timestamp << "\n";
        out << "file_size: " << r.file_size << "\n";
        out << "sha256: " << r.file_sha256 << "\n";
        out << "sidecar: " << r.sidecar_path << "\n";
        out << "first_present: " << r.first_present << "\n";
        out << "last_present: " << r.last_present << "\n";
    }
    if (!r.failure_reason.empty()) {
        out << "failure_reason: " << r.failure_reason << "\n";
    }
    return out.str();
}

bool ParseId(std::string_view text, u64& result) {
    if (text.empty()) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
    return ec == std::errc{} && end == text.data() + text.size() && result != 0;
}
std::string BadArguments() { return "status: invalid_arguments\n"; }

std::uint64_t NowNs(const MonotonicClockNs& clock) {
    return clock ? clock() : 0;
}

}  // namespace

void RegisterDiagnosticsCommands(spatial::debugbus::DebugCommandRegistry& registry,
                                 DiagnosticsHub& hub, MonotonicClockNs clock) {
    // debug_status: non-blocking hub snapshot.
    registry.Register(
        "debug_status", "Show session/advance status (non-blocking)",
        [&hub, clock](const std::vector<std::string>&) {
            DiagnosticsSnapshot snap;
            hub.QuerySnapshot(snap, clock ? clock() : 0);
            return FormatStatus(snap);
        });

    registry.Register("gpu_reshape_status", "Sampled GPU Reshape status (no SDK/GPU waits)",
        [](const std::vector<std::string>&) { return GpuReshape::StatusSnapshot(); });

    registry.Register("pipeline_handoff", "Bounded concurrency evidence: start [100..10000 ms] | status | stop | dump",
        [&hub, clock](const std::vector<std::string>& args) {
            DiagnosticsSnapshot snapshot;
            hub.QuerySnapshot(snapshot, NowNs(clock));
            return Handoff::Control(args, snapshot);
        });

    // renderdoc_status: whether the RenderDoc API is loaded (see the loader
    // repair). Capture itself is a later backend.
    registry.Register(
        "renderdoc_status", "Show whether the RenderDoc API is loaded",
        [](const std::vector<std::string>&) {
            std::ostringstream out;
            out << "renderdoc_api_loaded: " << (VideoCore::IsRenderDocLoaded() ? "true" : "false")
                << "\n";
            out << "capture_backend: coordinator\n";
            return out.str();
        });

    // renderdoc_capture [frames]: arm a request/receipt capture (spec §3.2). The
    // coordinator advances at real present boundaries; this returns the initial
    // receipt (armed, or failed if RenderDoc is absent).
    registry.Register(
        "renderdoc_capture", "renderdoc_capture [frames] -- arm a frame capture",
        [&hub, clock](const std::vector<std::string>& args) {
            u64 frames = 1;
            if (args.size() > 1 || (!args.empty() && !ParseId(args[0], frames)) || frames > 8)
                return BadArguments();
            DiagnosticsSnapshot snap;
            hub.QuerySnapshot(snap, NowNs(clock));
            const auto r = VideoCore::GetCaptureCoordinator().Arm(
                static_cast<u32>(frames), snap.has_session ? snap.generation : 0,
                snap.run_uuid, NowNs(clock));
            return FormatReceipt(r);
        });

    // renderdoc_capture_status [request_id]: query the current or a specific
    // request's receipt without blocking.
    registry.Register(
        "renderdoc_capture_status", "renderdoc_capture_status [request_id]",
        [clock](const std::vector<std::string>& args) {
            auto& coord = VideoCore::GetCaptureCoordinator();
            if (args.empty()) {
                return FormatReceipt(coord.Query(NowNs(clock)));
            }
            u64 id{};
            if (args.size() != 1 || !ParseId(args[0], id)) return BadArguments();
            return FormatReceipt(coord.Query(id, NowNs(clock)));
        });

    registry.Register(
        "renderdoc_capture_cancel", "renderdoc_capture_cancel <request_id> [generation]",
        [&hub, clock](const std::vector<std::string>& args) {
            u64 id{}, generation{};
            if (args.empty() || args.size() > 2 || !ParseId(args[0], id)) return BadArguments();
            DiagnosticsSnapshot snap;
            hub.QuerySnapshot(snap, NowNs(clock));
            generation = snap.generation;
            if (args.size() == 2 && !ParseId(args[1], generation)) return BadArguments();
            return FormatReceipt(VideoCore::GetCaptureCoordinator().Cancel(id, generation));
        });

    registry.Register("overlay", "overlay status | show | hide",
        [&hub, clock](const std::vector<std::string>& args) {
            if (args.size() > 1) return BadArguments();
            const auto sub = args.empty() ? "status" : args[0];
            if (sub == "show") status_overlay_enabled.store(true, std::memory_order_relaxed);
            else if (sub == "hide") status_overlay_enabled.store(false, std::memory_order_relaxed);
            else if (sub != "status") return BadArguments();
            DiagnosticsSnapshot snap;
            hub.QuerySnapshot(snap, NowNs(clock));
            std::ostringstream out;
            out << "overlay: " << (status_overlay_enabled.load() ? "shown" : "hidden") << "\n";
            out << "overlay_redraw: " << FormatCounter(snap.Counter(AdvanceSignal::OverlayRedraw), snap.snapshot_ns) << "\n";
            out << "session: " << (snap.has_session ? "session_active" : "no_session") << "\n";
            return out.str();
        });

    // Commands whose backends are not built yet. Registered so help/schema is
    // complete and callers get an honest, uniform reply -- never a faked success.
    static constexpr const char* kPending[] = {
        "guest_screenshot",
        "guest_screenshot_status",
        "profiler_ring",
        "profiler_capture",
        "performance_capture",
        "performance_capture_status",
        "performance_capture_cancel",
        "guest_command_trace",
        "gpu_command_trace",
    };
    for (const char* name : kPending) {
        const std::string command{name};
        registry.Register(command, "(backend not implemented yet)",
                          [command](const std::vector<std::string>&) {
                              return NotImplemented(command);
                          });
    }
}

}  // namespace Core::Diagnostics
