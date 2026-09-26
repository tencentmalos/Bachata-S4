// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <fmt/format.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include "common/logging/log.h"
#include "core/guest_write_watch.h"

namespace Core::GuestWriteWatch {

std::atomic<bool> armed{false};

namespace {

thread_local const char* t_writer = "unattributed";
thread_local u64 t_detail = 0;

constexpr u64 DefaultPattern = 0x7e007e007e007e00ULL; // four FP16 NaNs

std::atomic<bool> pattern_enabled{false};
std::atomic<u64> pattern{DefaultPattern};
std::atomic<VAddr> range_begin{0};
std::atomic<VAddr> range_end{0};

struct Event {
    u64 seq;
    s64 time_ms;
    const char* writer;
    u64 detail;
    VAddr destination;
    u64 size;
    u64 matches;
    VAddr first_match;
    bool in_range;
};

constexpr std::size_t RingSize = 4096;
std::mutex ring_mutex;
std::array<Event, RingSize> ring{};
u64 next_seq = 0;
u64 total_writes_checked = 0;
const auto start_time = std::chrono::steady_clock::now();

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start_time)
        .count();
}

bool ParseU64(const std::string& text, u64& out) {
    const bool hex = text.starts_with("0x") || text.starts_with("0X");
    const char* begin = text.data() + (hex ? 2 : 0);
    const auto [end, ec] = std::from_chars(begin, text.data() + text.size(), out, hex ? 16 : 10);
    return ec == std::errc{} && end == text.data() + text.size();
}

std::string Describe(const Event& e) {
    return fmt::format("#{} t={}ms {} detail={:#x} dst=[{:#x},{:#x}) matches={} first={:#x}{}",
                       e.seq, e.time_ms, e.writer, e.detail, e.destination,
                       e.destination + e.size, e.matches, e.first_match,
                       e.in_range ? " IN_RANGE" : "");
}

std::string Status() {
    std::scoped_lock lock{ring_mutex};
    return fmt::format("armed={} pattern={} {:#018x} range=[{:#x},{:#x}) checked_writes={} "
                       "events={}\n",
                       armed.load() ? 1 : 0, pattern_enabled.load() ? "on" : "off", pattern.load(),
                       range_begin.load(), range_end.load(), total_writes_checked, next_seq);
}

} // namespace

Scope::Scope(const char* writer, u64 detail) noexcept
    : previous_writer{t_writer}, previous_detail{t_detail} {
    t_writer = writer;
    t_detail = detail;
}

Scope::~Scope() {
    t_writer = previous_writer;
    t_detail = previous_detail;
}

void CheckSlow(VAddr destination, const void* data, u64 size) {
    if (!size || !data) {
        return;
    }
    u64 matches = 0;
    VAddr first_match = 0;
    if (pattern_enabled.load(std::memory_order_relaxed)) {
        const u64 value = pattern.load(std::memory_order_relaxed);
        const auto* bytes = static_cast<const u8*>(data);
        // Words at 8-byte aligned guest addresses, where the corrupted pointers live.
        for (u64 offset = (8 - (destination & 7)) & 7; offset + 8 <= size; offset += 8) {
            u64 word;
            std::memcpy(&word, bytes + offset, sizeof(word));
            if (word == value) {
                if (!matches) {
                    first_match = destination + offset;
                }
                ++matches;
            }
        }
    }
    const VAddr watch_begin = range_begin.load(std::memory_order_relaxed);
    const VAddr watch_end = range_end.load(std::memory_order_relaxed);
    const bool in_range = watch_begin < watch_end && destination < watch_end &&
                          destination + size > watch_begin;

    Event event{};
    {
        std::scoped_lock lock{ring_mutex};
        ++total_writes_checked;
        if (!matches && !in_range) {
            return;
        }
        event = Event{next_seq++, NowMs(), t_writer, t_detail, destination,
                      size,       matches, first_match, in_range};
        ring[event.seq % RingSize] = event;
    }
    LOG_WARNING(Core, "guest write watch {}", Describe(event));
}

std::string Command(const std::vector<std::string>& args) {
    const std::string sub = args.empty() ? "status" : args[0];
    if (sub == "start") {
        u64 value = DefaultPattern;
        if (args.size() > 2 || (args.size() == 2 && !ParseU64(args[1], value))) {
            return "usage: start [pattern_hex]\n";
        }
        {
            std::scoped_lock lock{ring_mutex};
            next_seq = 0;
            total_writes_checked = 0;
        }
        pattern.store(value);
        pattern_enabled.store(true);
        armed.store(true);
        return Status();
    }
    if (sub == "range") {
        u64 address = 0, bytes = 0;
        if (args.size() != 3 || !ParseU64(args[1], address) || !ParseU64(args[2], bytes) ||
            bytes == 0 || address > ~u64{0} - bytes) {
            return "usage: range <address> <bytes>\n";
        }
        range_begin.store(address);
        range_end.store(address + bytes);
        armed.store(true);
        return Status();
    }
    if (sub == "stop") {
        armed.store(false);
        pattern_enabled.store(false);
        range_begin.store(0);
        range_end.store(0);
        return Status();
    }
    if (sub == "status") {
        return Status();
    }
    if (sub == "dump") {
        u64 count = 64;
        if (args.size() > 2 || (args.size() == 2 && !ParseU64(args[1], count))) {
            return "usage: dump [count]\n";
        }
        std::string out = Status();
        std::scoped_lock lock{ring_mutex};
        const u64 available = std::min<u64>({next_seq, RingSize, count});
        for (u64 i = next_seq - available; i < next_seq; ++i) {
            out += Describe(ring[i % RingSize]) + "\n";
        }
        return out;
    }
    return "usage: start [pattern_hex] | range <address> <bytes> | status | dump [count] | stop\n";
}

void ArmFromEnvironment() {
    std::string value;
    if (const char* env = std::getenv("SHADPS4_GUEST_WRITE_WATCH")) {
        value = env;
    }
#ifdef __ANDROID__
    char property[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.shadps4.guest_write_watch", property) > 0) {
        value = property;
    }
#endif
    if (value.empty() || value == "0") {
        return;
    }
    const auto result = Command(value == "1" ? std::vector<std::string>{"start"}
                                             : std::vector<std::string>{"start", value});
    LOG_WARNING(Core, "guest write watch armed at startup: {}", result);
}

} // namespace Core::GuestWriteWatch
