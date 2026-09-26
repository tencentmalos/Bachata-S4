// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/diagnostics/diagnostics_service.h"

#include <chrono>
#include <memory>
#include <mutex>

#include "spatial/debugbus/DebugCommandRegistry.h"
#if defined(__ANDROID__)
#include "spatial/debugbus/DumpsysBridge.h"
#endif
#if defined(SHADPS4_DEBUGBUS_TCP)
#include "spatial/debugbus/TcpServer.h"
#endif

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

#if defined(SHADPS4_DEBUGBUS_TCP)
// First used after ProcessRegistry(), so at exit it is destroyed (and its network
// thread joined) before the registry it serves. The emulator stops it explicitly
// before shutting down the subsystems the handlers reach.
struct DebugBusTransport {
    std::mutex mutex;
    std::unique_ptr<spatial::debugbus::TcpServer> server;
};

DebugBusTransport& Transport() {
    static DebugBusTransport transport;
    return transport;
}
#endif

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
#if defined(__ANDROID__)
        // Bind to the Android dumpsys bridge so `adb shell dumpsys` / Service.dump
        // reaches the same typed backend. The registry outlives the process, so the
        // bridge's raw pointer stays valid.
        spatial::debugbus::SetDumpsysRegistry(&registry);
#endif
    });
}

std::string HandleDebugCommand(std::string_view command) {
    EnsureDiagnosticsRegistered();
    return ProcessRegistry().Handle(command);
}

std::uint16_t StartDebugBusServer(std::uint16_t port) {
#if defined(SHADPS4_DEBUGBUS_TCP)
    // The registry has no concurrent-modification protection: register everything
    // before a client can reach it.
    EnsureDiagnosticsRegistered();
    auto& transport = Transport();
    std::scoped_lock lock{transport.mutex};
    transport.server.reset();
    // Same greeting shape and end marker as Azahar's DebugDump service, so its
    // clients (which skip two greeting lines and read until "--END--") work as is.
    spatial::debugbus::TcpServerOptions options{
        .greeting = "shadPS4 DebugBus\r\nSend 'help' for available requests.\r\n",
        .response_end_marker = "--END--\n",
    };
    auto server =
        std::make_unique<spatial::debugbus::TcpServer>(port, ProcessRegistry(), std::move(options));
    if (!server->IsListening()) {
        return 0;
    }
    transport.server = std::move(server);
    return transport.server->Port();
#else
    (void)port;
    return 0;
#endif
}

void StopDebugBusServer() {
#if defined(SHADPS4_DEBUGBUS_TCP)
    auto& transport = Transport();
    std::scoped_lock lock{transport.mutex};
    transport.server.reset();
#endif
}

}  // namespace Core::Diagnostics
