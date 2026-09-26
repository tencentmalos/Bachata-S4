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
    // LogLevel follows spdlog's order, which is also Common::Log::Level's.
    const auto severity = static_cast<Common::Log::Level>(static_cast<int>(level));
    if (severity >= Common::Log::Level::Off ||
        !Common::Log::ShouldLog(Common::Log::Class::Render_Vulkan, severity)) {
        return;
    }
    Common::Log::Log(Common::Log::Class::Render_Vulkan, severity, __FILE__, __LINE__, __func__,
                     "[Foundation:{}] {}", tag, message);
}

} // namespace spatial::platform
