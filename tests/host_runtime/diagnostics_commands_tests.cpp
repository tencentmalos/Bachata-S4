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

    // --- renderdoc_status: reports API-loaded and capture-not-implemented ---
    {
        const std::string r = registry.Handle("renderdoc_status");
        CHECK(Has(r, "renderdoc_api_loaded:"));
        CHECK(Has(r, "capture_backend: not-implemented"));
    }

    // --- overlay status: honest not-implemented + redraw counter ---
    {
        const std::string r = registry.Handle("overlay status");
        CHECK(Has(r, "overlay: not-implemented"));
        CHECK(Has(r, "overlay_redraw:"));
        // overlay show is not-implemented, never faked.
        const std::string show = registry.Handle("overlay show");
        CHECK(Has(show, "status: not-implemented"));
    }

    // --- pending commands never fake success ---
    {
        for (const char* name : {"renderdoc_capture", "profiler_ring", "profiler_capture",
                                 "performance_capture", "guest_command_trace",
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
