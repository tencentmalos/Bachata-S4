// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "spatial/platform/OsConsoleLogger.hpp"

#include "common/logging/log.h"

namespace spatial::platform {

void OsConsoleLogger::outputMessage(LogLevel level, std::string_view tag,
                                    std::string_view message) {
    // Foundation's FDM helpers are embedded as a small static target. Keep
    // their diagnostics in shadPS4's main Render.Vulkan sink instead of
    // pulling in Foundation's complete platform/logger implementation.
    const auto logger = Common::Log::ALL_LOGGERS[Common::Log::Class::Render_Vulkan];
    if (!logger) {
        return;
    }
    const auto severity = static_cast<spdlog::level>(static_cast<int>(level));
    if (logger->should_log(severity)) {
        logger->log(severity, "[Foundation:{}] {}", tag, message);
    }
}

} // namespace spatial::platform
