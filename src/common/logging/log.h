// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <fmt/base.h>

#include "common/logging/classes.h"

namespace Common::Log {

// Same order as Foundation's spatial::LogLevel.
enum class Level : std::uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
    Off,
};

// Logging runs on Foundation's LogModule: every Class is a Foundation logger kind named after the
// class (see classes.cpp), so its level is the kind switch. Setup starts the log channel with the
// main file; Switch moves the channel to the game's file.
void Setup(std::string_view shadps4_filename);
void Switch(std::string_view game_filename, bool append_log);
void Shutdown();
void Flush();

// Writes a diagnostic record to the dedicated guest-patch file. It has its own log channel so
// guest instrumentation cannot change the user's main log filters or reach the console.
void WriteGuestPatch(std::string_view message) noexcept;
// One host-side entry point, also callable from the JNI DSO without exporting the log backend.
void WriteOverlayEvent(std::string_view message) noexcept;

// `<class>:<level> ...` with `*:<level>` as the default; applies to every Foundation logger kind.
void UpdateLogLevels(std::string_view log_filter);
void UpdateLogFlushLevel(std::string_view log_flush_level);

[[nodiscard]] bool ShouldLog(Class log_class, Level level) noexcept;

void VLog(Class log_class, Level level, const char* file, int line, const char* func,
          fmt::string_view format, fmt::format_args args);

template <typename... Args>
void Log(Class log_class, Level level, const char* file, int line, const char* func,
         fmt::format_string<Args...> format, Args&&... args) {
    VLog(log_class, level, file, line, func, format, fmt::make_format_args(args...));
}

} // namespace Common::Log

// Define the fmt lib macros
#define LOG_GENERIC_AT(log_class, log_level, func, ...)                                            \
    do {                                                                                           \
        if (Common::Log::ShouldLog(Common::Log::Class::log_class, log_level)) {                    \
            Common::Log::Log(Common::Log::Class::log_class, log_level, __FILE__, __LINE__, func,   \
                             __VA_ARGS__);                                                         \
        }                                                                                          \
    } while (false)

#define LOG_GENERIC(log_class, log_level, ...)                                                     \
    LOG_GENERIC_AT(log_class, log_level, __func__, __VA_ARGS__)

#ifdef NDEBUG
#define LOG_TRACE(log_class, ...) (void(0))
#else
#define LOG_TRACE(log_class, ...) LOG_GENERIC(log_class, Common::Log::Level::Trace, __VA_ARGS__)
#endif

#define LOG_DEBUG(log_class, ...) LOG_GENERIC(log_class, Common::Log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(log_class, ...) LOG_GENERIC(log_class, Common::Log::Level::Info, __VA_ARGS__)
#define LOG_WARNING(log_class, ...) LOG_GENERIC(log_class, Common::Log::Level::Warning, __VA_ARGS__)
#define LOG_ERROR(log_class, ...) LOG_GENERIC(log_class, Common::Log::Level::Error, __VA_ARGS__)
#define LOG_CRITICAL(log_class, ...)                                                               \
    LOG_GENERIC(log_class, Common::Log::Level::Critical, __VA_ARGS__)
