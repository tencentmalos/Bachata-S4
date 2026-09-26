#include "core/emulator_settings.h"
#include "core/diagnostics/overlay_control.h"
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/diagnostics/diagnostics_commands.h"
#include "common/profiler.h"
#include "common/gpu_timing.h"
#include "common/logging/log_stats.h"
#if defined(SHADPS4_TYPED_HLE_HOST)
#include "core/host_runtime/guest_patch.h"
#include "core/host_runtime/orbis_pad_adapter.h"
#include "core/host_runtime/guest_auto_tag.h"
#include "core/host_runtime/guest_sync_metrics.h"
#include "core/diagnostics/executable_export.h"
#endif

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
#include "core/memory.h"
#include "core/guest_write_watch.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/upload_diagnostics.h"
#include "video_core/amdgpu/pm4_stats.h"
#include "video_core/amdgpu/pm4_trace.h"
#include "video_core/renderer_vulkan/vk_command_recorder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/memory_diagnostics.h"
#include "video_core/renderdoc_capture.h"
#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
#include "video_core/renderer_vulkan/capture_recorder.h"
#endif

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
    out << "first_guest_flip: " << r.first_guest_flip << "\n";
    out << "last_guest_flip: " << r.last_guest_flip << "\n";
    out << "state: " << VideoCore::ToString(r.state) << "\n";
    out << "requested_frames: " << r.requested_frames << "\n";
    if (r.delay_boundaries)
        out << "delay: " << r.skipped_boundaries << "/" << r.delay_boundaries << "\n";
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
// Like ParseId, but zero is a valid count (e.g. "no delay").
bool ParseCount(std::string_view text, u64& result) {
    if (text.empty()) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
    return ec == std::errc{} && end == text.data() + text.size();
}
std::string BadArguments() { return "status: invalid_arguments\n"; }

std::uint64_t NowNs(const MonotonicClockNs& clock) {
    return clock ? clock() : 0;
}

}  // namespace

void RegisterDiagnosticsCommands(spatial::debugbus::DebugCommandRegistry& registry,
                                 DiagnosticsHub& hub, MonotonicClockNs clock) {
#if defined(SHADPS4_TYPED_HLE_HOST)
    registry.Register("pad", "Session-bound controller: capabilities | status | state | release_all",
        [](const std::vector<std::string>& args) {
            return HostRuntime::GlobalPadAdapter().DebugCommand(args);
        });
    registry.Register("guest_executable_export",
        "Export disk executables asynchronously: start [current|absolute_path] | start path_hex HEX | status [id] | cancel id",
        [](const std::vector<std::string>& args) { return ExecutableExport::Command(args); });
    registry.Register("hle_sync", "HLE synchronization metrics: status | dump | start/detail/stop <context>",
        [](const std::vector<std::string>& args) { return HostRuntime::SyncMetrics::Command(args); });
    registry.Register("guest_auto_tag", "Guest IR auto tag status | enable/disable <context>",
        [](const std::vector<std::string>& args) { return GuestAutoTag::Command(args); });
    registry.Register("guest_patch", "Guest patch status | enable/disable <context ID>",
        [](const std::vector<std::string>& args) { return GuestPatch::Command(args); });
#endif
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

    registry.Register("internal_scale", "Configured physical image scale (restart required to change)",
        [](const std::vector<std::string>& args) {
            if (!args.empty() && (args.size() != 1 || args[0] != "status")) return BadArguments();
            return VideoCore::MemoryDiagnostics::PolicyStatus();
        });

    registry.Register("srt_batch",
        "Batched SRT (shader user-data) guest reads: status | on | off | verify on|off",
        [](const std::vector<std::string>& args) { return Core::MemoryManager::SrtReadBatch::Command(args); });
    registry.Register("pass_log",
        "Render pass instance log (default off): start [passes] | status | dump | stop",
        [](const std::vector<std::string>& args) { return Vulkan::Scheduler::PassLogCommand(args); });
    registry.Register("vk_recorder",
        "Deferred Vulkan command recording thread: status | on | off (applies at the next submission)",
        [](const std::vector<std::string>& args) { return Vulkan::CommandRecorder::Command(args); });
    registry.Register("pm4_stats",
        "Guest command buffer structure census (default off): start [detailed_frames] | status | stop",
        [](const std::vector<std::string>& args) { return AmdGpu::Pm4Stats::Command(args); });
    registry.Register("upload_diag",
        "Texture re-upload diagnostics (default off): start [log_lines] | status | stop | ignore_storage_dirty on|off | fill_clear on|off",
        [](const std::vector<std::string>& args) { return VideoCore::UploadDiagnostics::Command(args); });

    registry.Register("log_stats",
        "Written log lines per call site and class (nothing is dropped): status [top]",
        [](const std::vector<std::string>& args) { return Common::Log::Stats::Command(args); });
    registry.Register("log_filter",
        "Per-class log levels for this session: no args shows them | "
        "<class>:<level> ... (e.g. *:info Kernel.Vmm:warning)",
        [](const std::vector<std::string>& args) { return Common::Log::FilterCommand(args); });

    registry.Register("hle_guest_copy",
        "Copy-shader HLE also writes its exact destination regions to guest memory: "
        "status | on | off",
        [](const std::vector<std::string>& args) { return Vulkan::HleGuestCopyCommand(args); });

    registry.Register("guest_write_watch",
        "GPU-produced writes into guest memory matching a 64-bit pattern (default four FP16 "
        "NaNs) or overlapping a range (default off): start [pattern_hex] | range <address> "
        "<bytes> | status | dump [count] | stop",
        [](const std::vector<std::string>& args) { return Core::GuestWriteWatch::Command(args); });

    registry.Register("memory_describe",
        "Guest address: mapping, physical address, aliases, recent mapping changes and bytes: "
        "<address> [bytes<=256]",
        [](const std::vector<std::string>& args) -> std::string {
            if (args.empty() || args.size() > 2) return BadArguments();
            u64 address = 0;
            u64 count = 64;
            const auto parse = [](const std::string& text, u64& out) {
                const bool hex = text.starts_with("0x") || text.starts_with("0X");
                const char* begin = text.data() + (hex ? 2 : 0);
                const auto [end, ec] = std::from_chars(begin, text.data() + text.size(), out,
                                                       hex ? 16 : 10);
                return ec == std::errc{} && end == text.data() + text.size();
            };
            if (!parse(args[0], address) || (args.size() == 2 && !parse(args[1], count)) ||
                count == 0 || count > 256) {
                return BadArguments();
            }
            auto* memory = Core::Memory::Instance();
            std::string out = fmt::format("address {:#x}: {}\n", address,
                                          memory->DescribeForCrash(address));
            std::vector<u8> bytes(count);
            if (memory->TryCopySparseMemory(address, bytes.data(), count)) {
                for (u64 row = 0; row < count; row += 16) {
                    out += fmt::format("  +{:#04x}:", row);
                    for (u64 i = row; i < std::min(row + 16, count); ++i) {
                        out += fmt::format(" {:02x}", bytes[i]);
                    }
                    out += "\n";
                }
            } else {
                out += "  <not readable>\n";
            }
            const VAddr addresses[] = {address};
            for (const auto& line : memory->DescribeMappingHistoryForCrash(addresses)) {
                out += fmt::format("  mapping {}\n", line);
            }
            return out;
        });

    registry.Register("gpu_memory", "GPU allocation snapshot: request | status (no GPU waits)",
        [](const std::vector<std::string>& args) {
            if (args.size() > 1 || (!args.empty() && args[0] != "request" && args[0] != "status"))
                return BadArguments();
            return VideoCore::MemoryDiagnostics::Read(args.empty() || args[0] == "request");
        });

    registry.Register("shading_quality", "Transient guest shading quality: low | medium | high | status (FDM stays off)",
        [](const std::vector<std::string>& args) {
            if (args.size() > 1) return BadArguments();
            const auto sub = args.empty() ? "status" : args[0];
            if (sub == "low") EmulatorSettings.SetGuestShadingQuality(0);
            else if (sub == "medium") EmulatorSettings.SetGuestShadingQuality(1);
            else if (sub == "high") EmulatorSettings.SetGuestShadingQuality(2);
            else if (sub != "status") return BadArguments();
            return std::string("guest_shading_quality=") +
                   std::to_string(EmulatorSettings.GetGuestShadingQuality()) +
                   " FDM=off (requested quality; per-pipeline fallback may apply)\n";
        });

    registry.Register("gpu_timing", "Litep device timestamp queries: start (coarse) | detail (render passes) | stop | status",
        [](const auto& args) { return Common::Profiler::GpuTimingControl(args); });
    registry.Register("profiler_ring_gpu", "GPU timestamps in the shared ring: start | detail | stop | status (alias of gpu_timing)",
        [](const auto& args) { return Common::Profiler::GpuTimingControl(args); });
    registry.Register("profiler_ring", "LiteTrace ring: status | start | stop | dump [frames, 0=all]",
        [](const std::vector<std::string>& args) { return Common::Profiler::Control(args); });
    registry.Register("profiler_capture", "LiteTrace streaming: status | file/socket [max_mib] [seconds] | stop",
        [](const std::vector<std::string>& args) { return Common::Profiler::CaptureControl(args); });

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
        "renderdoc_capture", "renderdoc_capture [frames] [delay] -- arm a frame capture, starting after delay presents",
        [&hub, clock](const std::vector<std::string>& args) {
            u64 frames = 1, delay = 0;
            if (args.size() > 2 || (!args.empty() && !ParseId(args[0], frames)) || frames > 8 ||
                (args.size() == 2 && !ParseCount(args[1], delay)))
                return BadArguments();
            DiagnosticsSnapshot snap;
            hub.QuerySnapshot(snap, NowNs(clock));
            const auto r = VideoCore::GetCaptureCoordinator().Arm(
                static_cast<u32>(frames), snap.has_session ? snap.generation : 0,
                snap.run_uuid, NowNs(clock), VideoCore::CaptureBoundary::HostPresent,
                static_cast<u32>(std::min<u64>(delay, 1'000'000)));
            return FormatReceipt(r);
        });

    registry.Register("renderdoc_guest_capture", "renderdoc_guest_capture [frames] [delay] -- capture complete guest flip intervals, starting after delay flips",
        [&hub, clock](const std::vector<std::string>& args) {
            u64 frames = 1, delay = 0;
            if (args.size() > 2 || (!args.empty() && !ParseId(args[0], frames)) || frames > 8 ||
                (args.size() == 2 && !ParseCount(args[1], delay)))
                return BadArguments();
            DiagnosticsSnapshot snap;
            hub.QuerySnapshot(snap, NowNs(clock));
            return FormatReceipt(VideoCore::GetCaptureCoordinator().Arm(
                static_cast<u32>(frames), snap.has_session ? snap.generation : 0,
                snap.run_uuid, NowNs(clock), VideoCore::CaptureBoundary::GuestFlip,
                static_cast<u32>(std::min<u64>(delay, 1'000'000))));
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

    registry.Register("overlay", "overlay status | show | hide | simple | summary | detail | controls | text small|medium|large",
        [&hub, clock](const std::vector<std::string>& args) {
            const auto sub = args.empty() ? "status" : args[0];
            if (sub == "text" && args.size() == 2 &&
                (args[1] == "small" || args[1] == "medium" || args[1] == "large")) {
                StatusOverlayMailbox().Request("text " + args[1]);
            } else if (args.size() <= 1 && (sub == "show" || sub == "hide" || sub == "simple" ||
                       sub == "summary" || sub == "detail" || sub == "controls")) {
                StatusOverlayMailbox().Request(sub);
            } else if (sub != "status" || args.size() > 1) return BadArguments();
            DiagnosticsSnapshot snap;
            hub.QuerySnapshot(snap, NowNs(clock));
            std::ostringstream out;
            if (sub != "status") out << "request: queued\n";
            out << "overlay: " << (status_overlay_enabled.load() ? "shown" : "hidden") << "\n";
            out << "overlay_redraw: " << FormatCounter(snap.Counter(AdvanceSignal::OverlayRedraw), snap.snapshot_ns) << "\n";
            out << "session: " << (snap.has_session ? "session_active" : "no_session") << "\n";
            return out.str();
        });

    // Guest GPU command trace: one bounded capture of PM4 packets, decoded actions and
    // host consequences (video_core/amdgpu/pm4_trace.h); both names drive it.
    for (const char* name : {"gpu_command_trace", "guest_command_trace"}) {
        registry.Register(
            name,
            "status | arm [frames=2] [delay=0] [max_mib=64] | save | cancel -- PM4 + guest "
            "action trace, written to CapturesDir/gpu-trace",
            [&hub, clock](const std::vector<std::string>& args) {
                DiagnosticsSnapshot snap;
                hub.QuerySnapshot(snap, NowNs(clock));
                AmdGpu::Pm4Trace::Identity id{snap.run_uuid, snap.pid, snap.generation,
                                              snap.driver_identity};
                return AmdGpu::Pm4Trace::Command(args, id, NowNs(clock));
            });
    }

#if defined(SHADPS4_HAS_SCRCPY_CAPTURE_SDK)
    registry.Register("capture_video", "capture_video [start|start_live TOKEN|stop|status] -- embedded final RT H.264",
        [](const std::vector<std::string>& args) {
            return Vulkan::HandleEmbeddedCaptureCommand(args);
        });
#endif

    // Commands whose backends are not built yet. Registered so help/schema is
    // complete and callers get an honest, uniform reply -- never a faked success.
    static constexpr const char* kPending[] = {
        "guest_screenshot",
        "guest_screenshot_status",
        "performance_capture",
        "performance_capture_status",
        "performance_capture_cancel",
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
