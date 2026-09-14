// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <string>

#include "spatial/debugbus/DumpsysBridge.h"

#include "core/diagnostics/diagnostics_hub_registry.h"
#include "core/diagnostics/diagnostics_service.h"

using namespace Core::Diagnostics;

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
    // First command builds the registry, registers commands, binds dumpsys.
    {
        const std::string r = HandleDebugCommand("debug_status");
        CHECK(Has(r, "session: none"));  // no session registered in this test process
    }

    // Idempotent: a second EnsureDiagnosticsRegistered / command does not double-register
    // or crash, and dispatch still works.
    {
        EnsureDiagnosticsRegistered();
        EnsureDiagnosticsRegistered();
        const std::string r = HandleDebugCommand("renderdoc_status");
        CHECK(Has(r, "renderdoc_api_loaded:"));
    }

    // The dumpsys bridge now routes to the same backend (SetDumpsysRegistry was
    // called by EnsureDiagnosticsRegistered).
    {
        const std::string viaBridge = spatial::debugbus::HandleDumpsysRequest("debug_status", {});
        CHECK(Has(viaBridge, "session: none"));
        CHECK(!Has(viaBridge, "no registry bound"));
    }

    // A registered session is visible through the service and the bridge.
    {
        auto pub = DiagnosticsHub::Instance().Register(88, 99);
        pub->SetStage("Running");
        const std::string direct = HandleDebugCommand("debug_status");
        CHECK(Has(direct, "session: active"));
        CHECK(Has(direct, "generation: 88"));
        const std::string bridge = spatial::debugbus::HandleDumpsysRequest("debug_status", {});
        CHECK(Has(bridge, "generation: 88"));
        (void)DiagnosticsHub::Instance().Revoke(88);
    }

    // Empty command returns help (registry behavior).
    {
        const std::string help = HandleDebugCommand("");
        CHECK(Has(help, "debug_status"));
    }

    std::printf("diagnostics_service: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
