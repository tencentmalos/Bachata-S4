// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <charconv>
#include <chrono>
#include <mutex>
#include <sstream>
#include "core/diagnostics/diagnostics_hub.h"
#include "core/diagnostics/pipeline_handoff.h"
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Core::Diagnostics::Handoff {
std::atomic<Id> enabled_generation{};
namespace {
struct Row {
    Id time, thread, object, token, value;
    const char* kind;
    const char* name;
    bool wait;
};
struct Recording {
    Id generation{}, pid{}, capture{}, start{}, deadline{};
    std::string run_uuid;
    const char* reason = "not_started";
    std::vector<Row> rows;
};
std::mutex mutex;
Recording recording;
std::atomic<Id> next_id{};
constexpr std::size_t capacity = 100000;
Id Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
Id Thread() {
#ifdef _WIN32
    return GetCurrentThreadId();
#elif defined(__APPLE__)
    uint64_t id{};
    pthread_threadid_np(nullptr, &id);
    return id;
#else
    return syscall(SYS_gettid);
#endif
}
void Stop(const char* reason) {
    recording.reason = reason;
    enabled_generation.store(0, std::memory_order_release);
}
bool Expire() {
    if (enabled_generation.load(std::memory_order_relaxed) && Now() >= recording.deadline)
        Stop("duration");
    return enabled_generation.load(std::memory_order_relaxed) != 0;
}
void Push(Id generation, Id capture, Row row) {
    if (!Enabled(generation))
        return;
    std::lock_guard lock{mutex};
    if (!Enabled(generation) || (capture && capture != recording.capture))
        return;
    if (row.time >= recording.deadline) {
        Stop("duration");
        return;
    }
    if (row.time < recording.start)
        return;
    if (recording.rows.size() == capacity) {
        Stop("capacity");
        return;
    }
    recording.rows.push_back(row);
}
std::string StatusLocked() {
    return "state=" + std::string(Enabled(recording.generation) ? "recording" : "stopped") +
           "\ncapture_id=" + std::to_string(recording.capture) +
           "\ngeneration=" + std::to_string(recording.generation) +
           "\nevents=" + std::to_string(recording.rows.size()) + "\nreason=" + recording.reason +
           "\n";
}
} // namespace
Id NextId() noexcept {
    return next_id.fetch_add(1, std::memory_order_relaxed) + 1;
}
void Event(Id generation, const char* kind, Id object, Id token, Id value) {
    if (Enabled(generation))
        Push(generation, 0, {Now(), Thread(), object, token, value, kind, "", false});
}
Scope::Scope(const char* name_, Id generation_, Id object_, Id related_, bool wait_)
    : profiler_scope{name_}, name{name_}, generation{generation_}, object{object_}, related{related_}, wait{wait_} {
    if (!Enabled(generation))
        return;
    const auto time = Now();
    const auto thread = Thread();
    std::lock_guard lock{mutex};
    if (!Enabled(generation) || !Expire() || time < recording.start)
        return;
    if (recording.rows.size() == capacity) {
        Stop("capacity");
        return;
    }
    capture = recording.capture;
    token = NextId();
    recording.rows.push_back({time, thread, object, token, related, "begin", name, wait});
}
Scope::~Scope() {
    if (token && Enabled(generation))
        Push(generation, capture, {Now(), Thread(), object, token, related, "end", name, wait});
}
void EndGeneration(Id generation) {
    std::lock_guard lock{mutex};
    if (Enabled(generation) && Expire())
        Stop("session_ended");
}
std::string Control(const std::vector<std::string>& args, const DiagnosticsSnapshot& snapshot) {
    const std::string action = args.empty() ? "status" : args[0];
    std::unique_lock lock{mutex};
    Expire();
    if (action == "start" && args.size() <= 2) {
        Id milliseconds{2000};
        if (args.size() == 2) {
            const auto& value = args[1];
            auto [end, ec] =
                std::from_chars(value.data(), value.data() + value.size(), milliseconds);
            if (ec != std::errc{} || end != value.data() + value.size() || milliseconds < 100 ||
                milliseconds > 10000)
                return "error=invalid_duration\n";
        }
        if (!snapshot.has_session || !snapshot.generation)
            return "error=no_session\n";
        if (enabled_generation.load())
            return "error=already_recording\n";
        recording = {};
        recording.rows.reserve(capacity);
        recording.generation = snapshot.generation;
        recording.pid = snapshot.pid;
        recording.run_uuid = snapshot.run_uuid;
        recording.capture = NextId();
        recording.start = Now();
        recording.deadline = recording.start + milliseconds * 1000000;
        recording.reason = "recording";
        enabled_generation.store(snapshot.generation, std::memory_order_release);
    } else if (action == "stop" && args.size() == 1) {
        if (enabled_generation.load())
            Stop("manual");
    } else if (action == "dump" && args.size() == 1) {
        if (enabled_generation.load())
            return "error=still_recording\n";
        if (!recording.capture)
            return "error=no_capture\n";
        const auto copy = recording;
        lock.unlock(); // No producer lock across serialization/Java or file I/O.
        std::ostringstream out;
        out << "{\"schema\":\"shadps4.pipeline-handoff.v1\",\"generation\":" << copy.generation
            << ",\"pid\":" << copy.pid << ",\"capture_id\":" << copy.capture << ",\"run_uuid\":\""
            << copy.run_uuid << "\",\"clock\":\"steady_ns\",\"start_ns\":" << copy.start
            << ",\"deadline_ns\":" << copy.deadline << ",\"stop_reason\":\"" << copy.reason
            << "\",\"complete\":"
            << (std::string_view(copy.reason) == "duration" ? "true" : "false")
            << ",\"columns\":[\"time_ns\",\"thread\",\"object\",\"token\",\"value\",\"kind\","
               "\"name\",\"wait\"],\"events\":[";
        bool first = true;
        for (const auto& r : copy.rows) {
            if (!first)
                out << ',';
            first = false;
            out << '[' << r.time << ',' << r.thread << ',' << r.object << ',' << r.token << ','
                << r.value << ",\"" << r.kind << "\",\"" << r.name << "\","
                << (r.wait ? "true" : "false") << ']';
        }
        out << "]}\n";
        return out.str();
    } else if (action != "status" || args.size() > 1) {
        return "error=invalid_arguments\n";
    }
    return StatusLocked();
}
} // namespace Core::Diagnostics::Handoff
