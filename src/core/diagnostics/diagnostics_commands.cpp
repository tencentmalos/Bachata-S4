// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/diagnostics/diagnostics_commands.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "spatial/debugbus/DebugCommandRegistry.h"

#include "core/diagnostics/diagnostics_hub.h"
#include "core/diagnostics/diagnostics_hub_registry.h"
#include "video_core/renderdoc.h"
#include "video_core/renderdoc_capture.h"

namespace Core::Diagnostics {
namespace {

// Formats "<count>" or "unavailable" for a signal, so a build that cannot measure
// a signal never prints a misleading 0 (spec §3.1).
std::string FormatCounter(const AdvanceCounter& c) {
    if (!c.available) {
        return "unavailable";
    }
    std::ostringstream out;
    out << c.count;
    if (c.count > 0) {
        out << " (last +" << c.last_advance_ns << "ns)";
    } else {
        out << " (never)";
    }
    return out.str();
}

std::string FormatStatus(const DiagnosticsSnapshot& snap) {
    std::ostringstream out;
    if (!snap.has_session) {
        out << "session: none\n";
        out << "snapshot_ns: " << snap.snapshot_ns << "\n";
        return out.str();
    }
    out << "session: active\n";
    out << "generation: " << snap.generation << "\n";
    out << "pid: " << snap.pid << "\n";
    out << "phase: " << snap.phase << "\n";
    out << "run_uuid: " << (snap.run_uuid.empty() ? "unknown" : snap.run_uuid) << "\n";
    out << "stage: " << (snap.stage.empty() ? "unknown" : snap.stage) << "\n";
    if (!snap.stop_reason.empty()) {
        out << "stop_reason: " << snap.stop_reason << "\n";
    }
    out << "snapshot_ns: " << snap.snapshot_ns << "\n";
    for (u32 i = 0; i < kAdvanceSignalCount; ++i) {
        const auto sig = static_cast<AdvanceSignal>(i);
        out << ToString(sig) << ": " << FormatCounter(snap.Counter(sig)) << "\n";
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
    out << "request_id: " << r.request_id << "\n";
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
    if (r.state == VideoCore::CaptureRequestState::Ready) {
        out << "file: " << r.file_path << "\n";
        out << "timestamp: " << r.capture_timestamp << "\n";
    }
    if (!r.failure_reason.empty()) {
        out << "failure_reason: " << r.failure_reason << "\n";
    }
    return out.str();
}

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
            u32 frames = 1;
            if (!args.empty()) {
                frames = static_cast<u32>(std::strtoul(args.front().c_str(), nullptr, 10));
                if (frames < 1) {
                    frames = 1;
                }
            }
            DiagnosticsSnapshot snap;
            hub.QuerySnapshot(snap, NowNs(clock));
            const auto r = VideoCore::GetCaptureCoordinator().Arm(
                frames, snap.run_uuid, snap.run_uuid.empty() ? std::string{} : (snap.run_uuid + ":cap"),
                NowNs(clock));
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
            const auto id = std::strtoull(args.front().c_str(), nullptr, 10);
            return FormatReceipt(coord.Query(id, NowNs(clock)));
        });

    // renderdoc_capture_cancel: cancel the active request.
    registry.Register(
        "renderdoc_capture_cancel", "renderdoc_capture_cancel -- cancel the active capture",
        [clock](const std::vector<std::string>&) {
            auto& coord = VideoCore::GetCaptureCoordinator();
            coord.Cancel();
            return FormatReceipt(coord.Query(NowNs(clock)));
        });

    // overlay status: overlay redraw counter from the hub. show/hide need the
    // Layer, which is a later step.
    registry.Register(
        "overlay", "overlay status | show | hide",
        [&hub, clock](const std::vector<std::string>& args) {
            const std::string sub = args.empty() ? "status" : args.front();
            if (sub == "status") {
                DiagnosticsSnapshot snap;
                hub.QuerySnapshot(snap, clock ? clock() : 0);
                std::ostringstream out;
                out << "overlay: not-implemented\n";
                out << "overlay_redraw: "
                    << FormatCounter(snap.Counter(AdvanceSignal::OverlayRedraw)) << "\n";
                return out.str();
            }
            return NotImplemented("overlay " + sub);
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
