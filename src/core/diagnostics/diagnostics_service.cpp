// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/diagnostics/diagnostics_service.h"

#include <chrono>
#include <mutex>

#include "spatial/debugbus/DebugCommandRegistry.h"
#include "spatial/debugbus/DumpsysBridge.h"

#include "core/diagnostics/diagnostics_commands.h"
#include "core/diagnostics/diagnostics_hub_registry.h"

namespace Core::Diagnostics {
namespace {

std::once_flag g_once;
// The process registry. Function-local static so it outlives every session and
// is never destroyed before process exit (the dumpsys bridge holds a pointer to
// it). Matches the SessionCore/DiagnosticsHub singleton style.
spatial::debugbus::DebugCommandRegistry& ProcessRegistry() {
    static spatial::debugbus::DebugCommandRegistry registry;
    return registry;
}

std::uint64_t MonotonicNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

void EnsureDiagnosticsRegistered() {
    std::call_once(g_once, [] {
        auto& registry = ProcessRegistry();
        RegisterDiagnosticsCommands(registry, DiagnosticsHub::Instance(), [] { return MonotonicNs(); });
        // Bind to the Android dumpsys bridge so `adb shell dumpsys` / Service.dump
        // reaches the same typed backend. The registry outlives the process, so the
        // bridge's raw pointer stays valid.
        spatial::debugbus::SetDumpsysRegistry(&registry);
    });
}

std::string HandleDebugCommand(std::string_view command) {
    EnsureDiagnosticsRegistered();
    return ProcessRegistry().Handle(command);
}

}  // namespace Core::Diagnostics
