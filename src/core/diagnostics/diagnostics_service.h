// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DiagnosticsService: the process-owned command entry that binds the toolkit's
// DebugCommandRegistry (spec §3.1) to the reachable surfaces -- Android
// Service.dump / dumpsys via Foundation's DumpsysBridge, and a direct JNI string
// command. One registry per process, populated once, reading the DiagnosticsHub
// singleton.
//
// This exists so the JNI/app layer has a single trivial call
// (HandleDebugCommand) and does not itself own the registry, the command set, or
// the hub. Registration is idempotent and lazy: the first HandleDebugCommand (or
// an explicit EnsureRegistered) builds the registry, registers the commands, and
// calls SetDumpsysRegistry so adb `dumpsys` reaches the same backend.

#pragma once

#include <string>
#include <string_view>

namespace Core::Diagnostics {

// Ensures the process registry is built, the toolkit commands are registered
// against the DiagnosticsHub singleton, and the registry is bound to the
// Foundation dumpsys bridge. Idempotent and thread-safe; safe to call from app
// init or lazily on first command. §3.1 limits this to debug-reachable control.
void EnsureDiagnosticsRegistered();

// Dispatches one command line (e.g. "debug_status" or "overlay status") through
// the process registry, building it first if needed. Never blocks on runtime
// work: status commands read a non-blocking DiagnosticsHub snapshot.
[[nodiscard]] std::string HandleDebugCommand(std::string_view command);

}  // namespace Core::Diagnostics
