// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <string>

#include "spatial/debugbus/DebugCommandRegistry.h"

#include "core/diagnostics/diagnostics_commands.h"
#include "core/diagnostics/diagnostics_hub.h"
#include "core/diagnostics/diagnostics_hub_registry.h"

using namespace Core::Diagnostics;
using spatial::debugbus::DebugCommandRegistry;

static unsigned checks{}, failures{};
#define CHECK(x)                                                                                    \
    do {                                                                                            \
        ++checks;                                                                                   \
        if (!(x)) {                                                                                 \
            ++failures;                                                                             \
            std::printf("FAIL line %d: %s\n", __LINE__, #x);                                        \
        }                                                                                           \
    } while (0)

static bool Has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int main() {
    DiagnosticsHub hub;
    DebugCommandRegistry registry;
    u64 fake_now = 5000;
    RegisterDiagnosticsCommands(registry, hub, [&] { return fake_now; });

    // --- debug_status with no session ---
    {
        const std::string r = registry.Handle("debug_status");
        CHECK(Has(r, "session: none"));
        CHECK(Has(r, "snapshot_ns: 5000"));
    }

    // --- debug_status with an active session, unavailable vs zero rendering ---
    {
        auto pub = hub.Register(11, 22);
        pub->SetStage("Running");
        pub->Advance(AdvanceSignal::HostPresent, 100, 3);
        pub->MarkAvailable(AdvanceSignal::GpuRetire, false);
        fake_now = 9999;

        const std::string r = registry.Handle("debug_status");
        CHECK(Has(r, "session: active"));
        CHECK(Has(r, "generation: 11"));
        CHECK(Has(r, "pid: 22"));
        CHECK(Has(r, "stage: Running"));
        CHECK(Has(r, "snapshot_ns: 9999"));
        CHECK(Has(r, "host_present: 3"));              // real count
        CHECK(Has(r, "gpu_retire: unavailable"));      // NOT "0" (spec §3.1)
        CHECK(Has(r, "guest_flip: 0 (never)"));        // available and genuinely 0
    }

    {
        auto pub = hub.Acquire(11);
        pub->SetPhase(5);
        pub->SetStage("Stopped");
        pub->SetStopReason("user_stop");
        pub->SetTerminalDetail("guest_presents=3");
        pub->Complete();
        pub->Advance(AdvanceSignal::HostPresent, 200, 1); // retired lease cannot advance
        const auto r = registry.Handle("debug_status");
        CHECK(Has(r, "session: none"));
        CHECK(Has(r, "phase: 5"));
        CHECK(Has(r, "host_present: 3 (age_ns=9899)"));
        CHECK(Has(r, "terminal_detail: guest_presents=3"));
        auto next = hub.Register(12, 22);
        CHECK(next->Generation() == 12);
        CHECK(hub.Acquire(11) == nullptr);

    }

    // --- renderdoc_status: reports API-loaded and capture-not-implemented ---
    {
        const std::string r = registry.Handle("renderdoc_status");
        CHECK(Has(r, "renderdoc_api_loaded:"));
        CHECK(Has(r, "capture_backend: coordinator"));
    }

    // --- overlay status: honest not-implemented + redraw counter ---
    {
        const std::string r = registry.Handle("overlay status");
        CHECK(Has(r, "overlay: hidden"));
        CHECK(Has(r, "overlay_redraw:"));
        // Visibility is actual control state; it does not claim a renderer exists.
        const std::string show = registry.Handle("overlay show");
        CHECK(Has(show, "overlay: shown"));
        CHECK(Has(registry.Handle("overlay hide"), "overlay: hidden"));
        CHECK(Has(registry.Handle("overlay invalid"), "invalid_arguments"));
    }

    // --- renderdoc_capture is now a real coordinator command; with RenderDoc
    //     absent (stub IsRenderDocLoaded=false) it arms then fails, never fakes success ---
    {
        const std::string r = registry.Handle("renderdoc_capture 2");
        CHECK(Has(r, "state:"));

        // RenderDoc not loaded in this test -> failed with a reason, not ready.
        CHECK(Has(r, "status: no_matching_renderer"));
        CHECK(!Has(r, "state: ready"));
        const std::string st = registry.Handle("renderdoc_capture_status");
        CHECK(Has(st, "state:"));
        const std::string cancel = registry.Handle("renderdoc_capture_cancel");
        CHECK(Has(cancel, "status: invalid_arguments"));
        for (const char* arg : {"-1", "0", "9", "1junk", "1 2", "18446744073709551616"})
            CHECK(Has(registry.Handle(std::string("renderdoc_capture ") + arg), "invalid_arguments"));
        CHECK(Has(registry.Handle("renderdoc_capture_cancel 1 22 extra"), "invalid_arguments"));
    }

    // --- still-pending commands never fake success ---
    {
        CHECK(Has(registry.Handle("profiler_ring"), "profiler_ring_not_built"));
        CHECK(Has(registry.Handle("profiler_capture"), "profiler_ring_not_built"));
        for (const auto* arg : {"file 0", "file 1025", "file 1 0", "file 1 3601",
                                "file -1", "file 1x", "file 1 1 extra", "status extra",
                                "stop extra", "invalid"})
            CHECK(Has(registry.Handle(std::string("profiler_capture ") + arg), "invalid_arguments"));
        for (const char* name : {"performance_capture", "guest_command_trace",
                                 "gpu_command_trace", "guest_screenshot"}) {
            const std::string r = registry.Handle(name);
            CHECK(Has(r, "status: not-implemented"));
            CHECK(Has(r, std::string("command: ") + name));
            // must not claim a ready/receipt
            CHECK(!Has(r, "ready"));
        }
    }

    // --- help lists the real registered commands ---
    {
        const std::string help = registry.HelpText();
        CHECK(Has(help, "debug_status"));
        CHECK(Has(help, "renderdoc_status"));
        CHECK(Has(help, "profiler_ring"));
        CHECK(Has(help, "gpu_command_trace"));
    }

    // --- unknown command handled gracefully by the registry ---
    {
        const std::string r = registry.Handle("totally_unknown_cmd");
        CHECK(Has(r, "unknown command: totally_unknown_cmd"));
    }

    std::printf("diagnostics_commands: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
