// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <fmt/format.h>

#include <spatial/core/imodules/ILogger.hpp>
#include <spatial/log/LogWriterFactory.h>

#include "common/logging/log.h"
#include "common/logging/log_stats.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "common/types.h"
#include "core/emulator_settings.h"

namespace Common::Log {
namespace {

static_assert(static_cast<int>(Level::Trace) == static_cast<int>(spatial::LogLevel::Trace) &&
              static_cast<int>(Level::Warning) == static_cast<int>(spatial::LogLevel::Warn) &&
              static_cast<int>(Level::Off) == static_cast<int>(spatial::LogLevel::Off));

constexpr spatial::LogLevel ToSpatial(Level level) {
    return static_cast<spatial::LogLevel>(level);
}

constexpr auto Channel = spatial::LogChannel::LOG_CHANNEL_NORMAL;
// Rotated files kept next to the current one (<name>_1.log is the previous run or chunk).
constexpr u32 KeptLogFiles = 2;

// A Foundation logger kind per Class, named after the class. Its level is the class filter, and
// Foundation's kind switches (ILogger::SetLogLevelByName) reach it like any other kind.
class ClassLogger final : public spatial::ILogger {
public:
    void Register(std::string_view name) {
        this->name = name;
        AddLoggerToMap(name, this);
    }

protected:
    void LogMessageImpl(spatial::LogMessageConfig config, std::string_view message) override {
        GLOG.traceMessage(Channel, name, config, message);
    }

private:
    std::string_view name; // NameOf literal: the log thread reads it after the call returns.
};

std::array<ClassLogger, NUM_LOG_CLASSES>& Loggers() {
    static auto& loggers = []() -> std::array<ClassLogger, NUM_LOG_CLASSES>& {
        static std::array<ClassLogger, NUM_LOG_CLASSES> array;
        for (int i = 0; i < NUM_LOG_CLASSES; ++i) {
            array[i].Register(NameOf(static_cast<Class>(i)));
            array[i].SetLevel(spatial::LogLevel::Info);
        }
        return array;
    }();
    return loggers;
}

std::mutex g_state_mutex;
spatial::ILogWritter* g_file_writer{}; // Owned; the current file of the main channel.
// Guest instrumentation has its own module (channel thread and file), outside the kinds.
spatial::LogModule g_guest_patch;
bool g_guest_patch_running{};
std::string g_active_filter;
std::atomic<Level> g_flush_level{Level::Off};

std::string_view Basename(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// Foundation file writers add ".log" and rotate <name>_<n>.log, so the name is used without its
// extension. Returns null when the file cannot be opened; logging then continues elsewhere.
spatial::ILogWritter* MakeFileWriter(std::string_view filename, bool append) {
    const auto dir = FS::GetUserPath(FS::PathType::LogDir);
    const auto size_limit = EmulatorSettings.GetLogSizeLimit();
    spatial::FileWriterConfig config{
        .logFilePath = FS::PathToUTF8String(dir),
        .logName = FS::PathToUTF8String(std::filesystem::path(filename).stem()),
        // 0 means unlimited; the writer takes MiB and ignores sizes below 1 MiB.
        .maxFileSizeM = size_limit == 0 ? (size_t{1} << 20) : std::max<size_t>(1, size_limit >> 20),
        .maxFileNum = KeptLogFiles,
        .isAppend = append,
        .isRotateByTime = false,
    };
    return spatial::modules::LogWriterFactory::queryFileWriter(config);
}

std::optional<spatial::LogLevel> ParseLevel(std::string_view text) {
    std::string lower{text};
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // Foundation names plus the spellings older configs use.
    static constexpr std::pair<std::string_view, spatial::LogLevel> names[] = {
        {"trace", spatial::LogLevel::Trace},  {"debug", spatial::LogLevel::Debug},
        {"info", spatial::LogLevel::Info},    {"warn", spatial::LogLevel::Warn},
        {"warning", spatial::LogLevel::Warn}, {"err", spatial::LogLevel::Error},
        {"error", spatial::LogLevel::Error},  {"critical", spatial::LogLevel::Critical},
        {"off", spatial::LogLevel::Off},
    };
    for (const auto& [name, level] : names) {
        if (lower == name) {
            return level;
        }
    }
    return std::nullopt;
}

struct Filter {
    spatial::LogLevel fallback{spatial::LogLevel::Info};
    std::vector<std::pair<std::string, spatial::LogLevel>> kinds;
};

// Parses "<kind>:<level> ... *:<level>". Kinds are checked against the registered loggers when
// `strict`; otherwise unknown entries are skipped like before.
std::optional<Filter> ParseFilter(std::string_view text, bool strict, std::string* error) {
    Filter filter;
    const auto known = spatial::ILogger::GetAllLoggerState();
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto end = std::min(text.find(' ', pos), text.size());
        const auto token = text.substr(pos, end - pos);
        pos = end + 1;
        if (token.empty()) {
            continue;
        }
        const auto colon = token.rfind(':');
        const auto level =
            colon == std::string_view::npos ? std::nullopt : ParseLevel(token.substr(colon + 1));
        const auto name = colon == std::string_view::npos ? token : token.substr(0, colon);
        if (!level || (name != "*" && !known.contains(name))) {
            if (strict) {
                *error = !level ? fmt::format("bad level in '{}' (trace|debug|info|warning|"
                                              "error|critical|off)",
                                              token)
                                : fmt::format("unknown logger kind '{}'", name);
                return std::nullopt;
            }
            continue;
        }
        if (name == "*") {
            filter.fallback = *level;
        } else {
            filter.kinds.emplace_back(name, *level);
        }
    }
    return filter;
}

void ApplyFilter(const Filter& filter, std::string_view text) {
    const bool enabled = EmulatorSettings.IsLogEnable();
    for (const auto& [name, level] : spatial::ILogger::GetAllLoggerState()) {
        spatial::ILogger::SetLogLevelByName(name,
                                            enabled ? filter.fallback : spatial::LogLevel::Off);
    }
    if (enabled) {
        for (const auto& [name, level] : filter.kinds) {
            spatial::ILogger::SetLogLevelByName(name, level);
        }
    }
    std::scoped_lock lock{g_state_mutex};
    g_active_filter = text;
}

} // namespace

bool ShouldLog(Class log_class, Level level) noexcept {
    return Loggers()[static_cast<std::size_t>(log_class)].ShouldLog(ToSpatial(level));
}

void VLog(Class log_class, Level level, const char* file, int line, const char* func,
          fmt::string_view format, fmt::format_args args) {
    fmt::memory_buffer msg;
    const std::string_view fn = std::string_view(func) == "operator()" ? "lambda" : func;
    // Time, level, thread id and class come from the Foundation writers.
    fmt::format_to(fmt::appender(msg), "({}) {}:{} {}: ", Common::GetCurrentThreadName(),
                   Basename(file), line, fn);
    const auto header = msg.size();
    fmt::vformat_to(fmt::appender(msg), format, args);
    Loggers()[static_cast<std::size_t>(log_class)].Log({ToSpatial(level), EMPTY_LOG_MASK},
                                                       std::string_view(msg.data(), msg.size()));
    Stats::Record(log_class, file, line, func, msg.size() - header);
    Stats::Tick();
    // Critical lines usually precede an abort; the channel writes asynchronously.
    if (level >= std::min(g_flush_level.load(std::memory_order_relaxed), Level::Critical)) {
        Flush();
    }
}

namespace {
// Moves the main channel to `filename`; the current file stays in use if the new one cannot be
// opened.
void UseFile(std::string_view filename, bool append) {
    auto* writer = MakeFileWriter(filename, append);
    if (!writer) {
        return;
    }
    std::scoped_lock lock{g_state_mutex};
    GLOG.addChannelWriter(Channel, writer);
    if (g_file_writer) {
        // The channel stops using it before removeChannelWriter returns.
        GLOG.removeChannelWriter(Channel, g_file_writer);
        g_file_writer->flush();
        delete g_file_writer;
    }
    g_file_writer = writer;
}
} // namespace

void Setup(std::string_view shadps4_filename) {
    // The channels live for the whole process; later calls only move the main file.
    static std::once_flag started;
    std::call_once(started, [] {
        std::atexit(Shutdown);
        std::at_quick_exit(Flush);
        Loggers();

        spatial::LogInitParm parm{spatial::LogLevel::Trace};
        auto& channel =
            parm.channelCfg.emplace(Channel, spatial::LogInitParm::ChannelCfg{"shadPS4:Log"})
                .first->second;
#ifndef __ANDROID__
        // Android keeps logcat quiet; its logs are in the app's log directory.
        channel.consoleWriter = spatial::modules::LogWriterFactory::queryConsoleWriter(true);
#endif
        GLOG.initLogger(parm);

        spatial::LogInitParm guest_parm{spatial::LogLevel::Trace};
        auto& guest_channel =
            guest_parm.channelCfg
                .emplace(Channel, spatial::LogInitParm::ChannelCfg{"shadPS4:GuestLog"})
                .first->second;
        if (auto* writer = MakeFileWriter("guest-patch", false)) {
            guest_channel.writers.push_back(writer);
            g_guest_patch.initLogger(guest_parm);
            g_guest_patch_running = true;
        }
    });
    UseFile(shadps4_filename, false);
    UpdateLogLevels(EmulatorSettings.GetLogFilter());
}

void Switch(std::string_view game_filename, bool append_log) {
    UpdateLogLevels(EmulatorSettings.GetLogFilter());
    UpdateLogFlushLevel(EmulatorSettings.GetLogFlushLevel());
    UseFile(game_filename, append_log || EmulatorSettings.IsLogAppend());
}

void Shutdown() {
    std::scoped_lock lock{g_state_mutex};
    // release() joins the channel threads and deletes their writers.
    GLOG.release();
    g_file_writer = nullptr;
    if (g_guest_patch_running) {
        g_guest_patch.release();
        g_guest_patch_running = false;
    }
}

void Flush() {
    GLOG.flushChannel(Channel);
    if (g_guest_patch_running) {
        g_guest_patch.flushChannel(Channel);
    }
}

void WriteOverlayEvent(std::string_view message) noexcept {
    WriteGuestPatch(message);
    try {
        LOG_INFO(ImGui, "{}", message);
    } catch (...) {
    }
}

void WriteGuestPatch(std::string_view message) noexcept {
    try {
        if (g_guest_patch_running) {
            g_guest_patch.traceMessage(Channel, "GuestPatch",
                                       {spatial::LogLevel::Info, EMPTY_LOG_MASK}, message);
        }
    } catch (...) {
        // Diagnostics must never affect guest execution or HLE return values.
    }
}

void UpdateLogLevels(std::string_view log_filter) {
    Loggers();
    std::string error;
    if (const auto filter = ParseFilter(log_filter, false, &error)) {
        ApplyFilter(*filter, log_filter);
    }
}

void UpdateLogFlushLevel(std::string_view log_flush_level) {
    const auto level = log_flush_level.empty() ? std::nullopt : ParseLevel(log_flush_level);
    g_flush_level.store(level ? static_cast<Level>(*level) : Level::Off, std::memory_order_relaxed);
}

std::string FilterCommand(const std::vector<std::string>& args) {
    Loggers();
    if (!args.empty()) {
        std::string text;
        for (const auto& token : args) {
            text += text.empty() ? token : ' ' + token;
        }
        std::string error;
        const auto filter = ParseFilter(text, true, &error);
        if (!filter) {
            return "error=" + error + "\n";
        }
        ApplyFilter(*filter, text);
    }
    std::string out;
    {
        std::scoped_lock lock{g_state_mutex};
        out = "filter: " + g_active_filter + "\nkinds:\n";
    }
    const auto states = spatial::ILogger::GetAllLoggerState();
    const std::map<std::string_view, spatial::LogLevel> sorted{states.begin(), states.end()};
    for (const auto& [name, level] : sorted) {
        out += fmt::format("  {} {}\n", name, spatial::LogModule::getLogLevelName(level));
    }
    return out;
}

} // namespace Common::Log
