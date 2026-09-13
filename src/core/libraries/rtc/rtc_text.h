// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include "core/libraries/rtc/rtc.h"
#include "core/libraries/rtc/rtc_error.h"

namespace Libraries::Rtc {
inline constexpr u64 MaxCalendarTick = 315537897599999999ULL;
inline constexpr std::array<std::string_view, 12> RtcMonths{
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
inline constexpr std::array<std::string_view, 7> RtcWeekdays{"Sun", "Mon", "Tue", "Wed",
                                                             "Thu", "Fri", "Sat"};
// No Linker, timezone globals, guest addresses or mutable clock. The public
// desktop/guest adapters supply their own clock and timezone policy.
inline int FormatRtcText(OrbisRtcTick tick, int offset_minutes, bool rfc2822, std::string& out) {
    if (offset_minutes <= -1440 || offset_minutes >= 1440 || tick.tick > MaxCalendarTick)
        return ORBIS_RTC_ERROR_INVALID_VALUE;
    const __int128 adjusted = __int128(tick.tick) + __int128(offset_minutes) * 60000000;
    if (adjusted < 0 || adjusted > MaxCalendarTick)
        return ORBIS_RTC_ERROR_INVALID_VALUE;
    tick.tick = u64(adjusted);
    OrbisRtcDateTime date{};
    if (const int result = sceRtcSetTick(&date, &tick))
        return result;
    const int absolute = offset_minutes < 0 ? -offset_minutes : offset_minutes;
    const char sign = offset_minutes < 0 ? '-' : '+';
    std::array<char, 64> buffer{};
    int size{};
    if (rfc2822) {
        const auto weekday = std::chrono::weekday{
            std::chrono::sys_days{
                std::chrono::year(date.year) / std::chrono::month(date.month) /
                std::chrono::day(
                    date.day)}}.c_encoding();
        size = std::snprintf(
            buffer.data(), buffer.size(), "%s, %02u %s %04u %02u:%02u:%02u %c%02d%02d",
            RtcWeekdays[weekday].data(), unsigned(date.day), RtcMonths[date.month - 1].data(),
            unsigned(date.year), unsigned(date.hour), unsigned(date.minute), unsigned(date.second),
            sign, absolute / 60, absolute % 60);
    } else {
        // Preserve the existing public aliases' two fractional digits. Correct
        // their value/padding; do not infer a new Precise ABI from its name.
        size = std::snprintf(buffer.data(), buffer.size(), "%04u-%02u-%02uT%02u:%02u:%02u.%02u",
                             unsigned(date.year), unsigned(date.month), unsigned(date.day),
                             unsigned(date.hour), unsigned(date.minute), unsigned(date.second),
                             date.microsecond / 10000);
        if (size < 0 || size >= int(buffer.size()))
            return ORBIS_RTC_ERROR_INVALID_VALUE;
        size += offset_minutes == 0
                    ? std::snprintf(buffer.data() + size, buffer.size() - size, "Z")
                    : std::snprintf(buffer.data() + size, buffer.size() - size, "%c%02d:%02d", sign,
                                    absolute / 60, absolute % 60);
    }
    if (size < 0 || size >= int(buffer.size()))
        return ORBIS_RTC_ERROR_INVALID_VALUE;
    out.assign(buffer.data(), size);
    return 0;
}

// Fixed grammar, bounded input and no stoi exceptions. ParseDateTime supports
// the three forms previously selected by desktop: RFC3339, RFC2822 and asctime.
// Malformed fields never partially publish the output tick.
inline int ParseRtcText(std::string_view text, bool only_rfc3339, OrbisRtcTick& out) {
    if (text.empty() || text.size() >= 96)
        return ORBIS_RTC_ERROR_BAD_PARSE;
    auto number = [&](size_t at, size_t count) -> int {
        if (at > text.size() || count > text.size() - at)
            return -1;
        int value{};
        for (size_t i = at; i < at + count; ++i) {
            if (text[i] < '0' || text[i] > '9')
                return -1;
            value = value * 10 + text[i] - '0';
        }
        return value;
    };
    int year{}, month{}, day{}, hour{}, minute{}, second{}, micros{}, offset{};
    auto zone = [&](size_t at, bool colon) -> bool {
        if (at >= text.size())
            return false;
        if (text.substr(at) == "Z" || text.substr(at) == "z")
            return true;
        if (text.size() - at != (colon ? 6 : 5) || (text[at] != '+' && text[at] != '-') ||
            (colon && text[at + 3] != ':'))
            return false;
        const int h = number(at + 1, 2), m = number(at + (colon ? 4 : 3), 2);
        if (h < 0 || h > 23 || m < 0 || m > 59)
            return false;
        offset = (h * 60 + m) * (text[at] == '-' ? -1 : 1);
        return true;
    };
    if (text.size() >= 20 && text[4] == '-') {
        if (text[7] != '-' || (text[10] != 'T' && text[10] != 't') || text[13] != ':' ||
            text[16] != ':')
            return ORBIS_RTC_ERROR_BAD_PARSE;
        year = number(0, 4);
        month = number(5, 2);
        day = number(8, 2);
        hour = number(11, 2);
        minute = number(14, 2);
        second = number(17, 2);
        size_t end = 19;
        if (text[end] == '.') {
            const size_t start = ++end;
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
                if (end - start < 6)
                    micros = micros * 10 + text[end] - '0';
                ++end;
            }
            if (end == start)
                return ORBIS_RTC_ERROR_BAD_PARSE;
            for (size_t digits = end - start; digits < 6; ++digits)
                micros *= 10;
        }
        if (!zone(end, true))
            return ORBIS_RTC_ERROR_BAD_PARSE;
    } else if (!only_rfc3339 && text.size() == 31 && text[3] == ',') {
        if (std::find(RtcWeekdays.begin(), RtcWeekdays.end(), text.substr(0, 3)) ==
            RtcWeekdays.end())
            return ORBIS_RTC_ERROR_BAD_PARSE;
        if (text[4] != ' ' || text[7] != ' ' || text[11] != ' ' || text[16] != ' ' ||
            text[19] != ':' || text[22] != ':' || text[25] != ' ' || !zone(26, false))
            return ORBIS_RTC_ERROR_BAD_PARSE;
        for (int i = 0; i < 12; ++i)
            if (text.substr(8, 3) == RtcMonths[i])
                month = i + 1;
        year = number(12, 4);
        day = number(5, 2);
        hour = number(17, 2);
        minute = number(20, 2);
        second = number(23, 2);
    } else if (!only_rfc3339 && (text.size() == 24 || (text.size() == 25 && text.back() == '\n'))) {
        if (std::find(RtcWeekdays.begin(), RtcWeekdays.end(), text.substr(0, 3)) ==
            RtcWeekdays.end())
            return ORBIS_RTC_ERROR_BAD_PARSE;
        if (text[3] != ' ' || text[7] != ' ' || text[10] != ' ' || text[13] != ':' ||
            text[16] != ':' || text[19] != ' ')
            return ORBIS_RTC_ERROR_BAD_PARSE;
        for (int i = 0; i < 12; ++i)
            if (text.substr(4, 3) == RtcMonths[i])
                month = i + 1;
        year = number(20, 4);
        day = text[8] == ' ' ? number(9, 1) : number(8, 2);
        hour = number(11, 2);
        minute = number(14, 2);
        second = number(17, 2);
    } else {
        return ORBIS_RTC_ERROR_BAD_PARSE;
    }
    if (year < 1 || month < 1 || day < 1 || hour < 0 || minute < 0 || second < 0)
        return ORBIS_RTC_ERROR_BAD_PARSE;
    OrbisRtcDateTime date{u16(year),   u16(month),  u16(day),   u16(hour),
                          u16(minute), u16(second), u32(micros)};
    OrbisRtcTick result{};
    if (sceRtcGetTick(&date, &result))
        return ORBIS_RTC_ERROR_BAD_PARSE;
    // Local clock = UTC + offset; parsing must subtract, not add, the offset.
    const __int128 utc = __int128(result.tick) - __int128(offset) * 60000000;
    if (utc < 0 || utc > MaxCalendarTick)
        return ORBIS_RTC_ERROR_BAD_PARSE;
    out.tick = u64(utc);
    return 0;
}
} // namespace Libraries::Rtc
