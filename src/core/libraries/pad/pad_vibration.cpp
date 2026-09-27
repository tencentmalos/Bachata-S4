// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <charconv>
#include <string_view>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/pad/pad_vibration.h"

namespace Libraries::Pad::Vibration {

namespace {

constexpr std::array OutcomeNames = {"sent",         "unknown handle", "not connected",
                                     "no controller motor", "host rejected", "invalid arguments"};

std::atomic<u64> calls{};
std::atomic<u64> nonzero_calls{};
std::array<std::atomic<u64>, u32(Outcome::Count)> outcomes{};
std::atomic<u32> last_value{};   ///< large << 8 | small
std::atomic<s32> last_handle{-1};
std::atomic<u32> last_outcome{u32(Outcome::Count)};

bool ParseU32(std::string_view text, u32& value) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size();
}

} // namespace

void Record(s32 handle, u8 large, u8 small, Outcome outcome, const char* detail) {
    const u64 call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (large || small) {
        nonzero_calls.fetch_add(1, std::memory_order_relaxed);
    }
    outcomes[u32(outcome)].fetch_add(1, std::memory_order_relaxed);
    last_value.store(u32(large) << 8 | small, std::memory_order_relaxed);
    last_handle.store(handle, std::memory_order_relaxed);
    const u32 previous = last_outcome.exchange(u32(outcome), std::memory_order_relaxed);
    if (call <= 8 || previous != u32(outcome)) {
        LOG_INFO(Lib_Pad, "scePadSetVibration #{} handle {} large {} small {}: {}{}{}", call,
                 handle, large, small, OutcomeNames[u32(outcome)], detail ? " - " : "",
                 detail ? detail : "");
    }
}

std::string Command(const std::vector<std::string>& args) {
    if (!args.empty() && args[0] == "test") {
        u32 large{}, small{}, handle{1};
        if (args.size() < 3 || args.size() > 4 || !ParseU32(args[1], large) ||
            !ParseU32(args[2], small) || large > 255 || small > 255 ||
            (args.size() == 4 && !ParseU32(args[3], handle))) {
            return "usage: pad_vibration test LARGE SMALL [HANDLE]   (motor levels 0..255)\n";
        }
        const OrbisPadVibrationParam param{.largeMotor = u8(large), .smallMotor = u8(small)};
        const int result = scePadSetVibration(s32(handle), &param);
        return fmt::format("scePadSetVibration(handle {}, large {}, small {}) = {:#x}\n", handle,
                           large, small, u32(result));
    }
    if (!args.empty() && (args.size() != 1 || args[0] != "status")) {
        return "usage: pad_vibration [status] | test LARGE SMALL [HANDLE]\n";
    }
    const u32 value = last_value.load();
    std::string out = fmt::format("scePadSetVibration calls {} ({} with a motor on); last: handle "
                                  "{}, large {}, small {}, {}\n",
                                  calls.load(), nonzero_calls.load(), last_handle.load(),
                                  value >> 8, value & 0xff,
                                  last_outcome.load() < u32(Outcome::Count)
                                      ? OutcomeNames[last_outcome.load()]
                                      : "none");
    out += "  outcomes:";
    for (u32 i = 0; i < u32(Outcome::Count); ++i) {
        out += fmt::format(" {} {}{}", OutcomeNames[i], outcomes[i].load(),
                           i + 1 < u32(Outcome::Count) ? "," : "\n");
    }
    return out;
}

} // namespace Libraries::Pad::Vibration
