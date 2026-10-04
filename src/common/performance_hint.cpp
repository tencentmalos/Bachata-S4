// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/performance_hint.h"

#if defined(__ANDROID__)
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <android/performance_hint.h>
#include <fmt/format.h>
#endif

namespace Common::PerformanceHint {

#if defined(__ANDROID__)
namespace {

using Clock = std::chrono::steady_clock;

std::mutex mutex;
std::atomic<bool> active{false};
APerformanceHintSession* session{};
std::vector<int32_t> thread_ids;
int64_t target_ns{};
Clock::time_point last_frame{};
uint64_t reports{};
int last_error{};

// Threads named Guest-* (game threads) plus the emulator threads that feed the GPU.
std::vector<int32_t> GuestThreads() {
    std::vector<int32_t> ids;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", error)) {
        std::ifstream comm(entry.path() / "comm");
        std::string name;
        std::getline(comm, name);
        if (name.starts_with("Guest-") || name.starts_with("shadPS4:GpuComm") ||
            name.starts_with("shadPS4:VkRecor")) {
            int32_t id{};
            const std::string tid = entry.path().filename().string();
            if (std::from_chars(tid.data(), tid.data() + tid.size(), id).ec == std::errc{}) {
                ids.push_back(id);
            }
        }
    }
    return ids;
}

std::vector<int32_t> ParseIds(const std::string& text) {
    std::vector<int32_t> ids;
    size_t begin = 0;
    while (begin < text.size()) {
        const size_t end = std::min(text.find(',', begin), text.size());
        int32_t id{};
        if (std::from_chars(text.data() + begin, text.data() + end, id).ec != std::errc{}) {
            return {};
        }
        ids.push_back(id);
        begin = end + 1;
    }
    return ids;
}

void Close() {
    active.store(false, std::memory_order_relaxed);
    if (session != nullptr) {
        APerformanceHint_closeSession(session);
        session = nullptr;
    }
}

} // namespace

std::string Command(const std::vector<std::string>& args) {
    const std::string sub = args.empty() ? "status" : args[0];
    std::scoped_lock lock{mutex};
    if (sub == "on" && (args.size() == 2 || args.size() == 3)) {
        Close();
        thread_ids = args[1] == "guest" ? GuestThreads() : ParseIds(args[1]);
        double target_ms = 33.333;
        if (args.size() == 3) {
            char* end = nullptr;
            target_ms = std::strtod(args[2].c_str(), &end);
            if (end == args[2].c_str() || *end != '\0' || !(target_ms > 0.0)) {
                return "status: invalid_arguments\n";
            }
        }
        if (thread_ids.empty()) {
            return "status: no_threads\n";
        }
        target_ns = static_cast<int64_t>(target_ms * 1e6);
        APerformanceHintManager* manager = APerformanceHint_getManager();
        if (manager == nullptr) {
            return "status: unsupported\n";
        }
        session = APerformanceHint_createSession(manager, thread_ids.data(), thread_ids.size(),
                                                 target_ns);
        if (session == nullptr) {
            return "status: session_failed\n";
        }
        reports = 0;
        last_error = 0;
        last_frame = {};
        active.store(true, std::memory_order_relaxed);
    } else if (sub == "off" && args.size() == 1) {
        Close();
    } else if (sub != "status" || args.size() != 1) {
        return "status: invalid_arguments\n";
    }
    std::string out = fmt::format("perf_hint: {}\n", session != nullptr ? "on" : "off");
    if (session != nullptr) {
        out += fmt::format("threads: {}\ntarget_ns: {}\nreports: {}\nlast_error: {}\n",
                           thread_ids.size(), target_ns, reports, last_error);
        if (APerformanceHintManager* manager = APerformanceHint_getManager()) {
            out += fmt::format("preferred_update_rate_ns: {}\n",
                               APerformanceHint_getPreferredUpdateRateNanos(manager));
        }
    }
    return out;
}

void NoteGameFrame() {
    if (!active.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (session == nullptr) {
        return;
    }
    const auto now = Clock::now();
    if (last_frame != Clock::time_point{}) {
        const int64_t duration =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_frame).count();
        last_error = APerformanceHint_reportActualWorkDuration(session, duration);
        ++reports;
    }
    last_frame = now;
}

#else

std::string Command(const std::vector<std::string>&) {
    return "status: unsupported\n";
}

void NoteGameFrame() {}

#endif

} // namespace Common::PerformanceHint
