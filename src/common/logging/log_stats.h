// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/logging/classes.h"

// Log volume accounting. Nothing is dropped here: every written line is counted per class and
// per call site (file:line), and cumulative counters (Log.Lines, Log.MessageBytes,
// Log.Class.<name>) go to the Litep ring. A noisy class is then turned down with the regular
// per-class filter (`log_filter` on DebugBus, or the Log.filter setting).
namespace Common::Log::Stats {

// Accounts one written line of `message_bytes` formatted message bytes.
void Record(Class log_class, const char* file, int line, const char* func,
            std::size_t message_bytes) noexcept;

// Publishes the counters at most every 250 ms; called from the logging path, so no timer thread.
void Tick() noexcept;

// DebugBus `log_stats`: status [top].
std::string Command(const std::vector<std::string>& args);

} // namespace Common::Log::Stats

namespace Common::Log {
// DebugBus `log_filter`: without arguments shows the active filter and every class level; with
// `<class>:<level>` / `*:<level>` tokens replaces the filter for the running session (the
// Log.filter setting is not changed).
std::string FilterCommand(const std::vector<std::string>& args);
} // namespace Common::Log
