// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_clock.h"
#include "core/libraries/rtc/rtc.h"
#include "core/libraries/rtc/rtc_error.h"
#include "core/libraries/rtc/rtc_text.h"

namespace Core::HostRuntime {
inline constexpr std::string_view RtcNids[]{
    "lPEBYdVX0XQ", "fNaZ4DbzHAE", "8Yr143yEnRo", "M1TvFst-jrM", "8SljQx6pDP8", "8lfvnRMqwEM",
    "ZPD1YOKI+Kw", "18B2NS1y9UU", "CyIK-i4XdgQ", "3O7Ln8AqJ1o", "E7AR4o7Ny7E", "8w-H19ip48I",
    "jMNwqYr4R-k", "BtqmpTRXHgk", "jfRO0uTjtzA", "LlodCMDbk3o", "Ug8pCwQvh0c", "aYPCd1cChyg",
    "ueega6v3GUw", "bDEVVP4bTjQ", "n5JiAJXsbcs", "NR1J0N7L2xY", "MDc5cd8HfCA", "XPIiw58C+GM",
    "mn-tf4QiFzk", "CL6y9q-XbuQ", "07O525HgICs", "AqVMssr52Rc", "gI4t194c2W8", "-5y2uJ62qS8",
    "LN3Zcb72Q0c", "Ot1DE3gif84", "zO9UL3qIINQ", "HWxHOdbM-Pg", "eiuobaF-hK4", "AxHBk3eat04",
    "WJ3rqFwymew", "DwuHIlLGW8I", "lja0nNPWojg", "tOZ6fwwHZOA", "NxEI1KByvCI", "99bMGglFW3I"};
inline bool IsRtcNid(std::string_view nid) {
    return std::find(std::begin(RtcNids), std::end(RtcNids), nid) != std::end(RtcNids);
}
// Scalar/structure RTC family. Dates/ticks use the real desktop conversion
// helpers on local copies; time and SDK come from this session. Its existing
// kernel/SystemService timezone policy is UTC. No native timezone globals,
// wall-clock mutation, callbacks or network-clock substitution are introduced.
inline u32 DispatchRtc(GuestCpu::GuestAddressSpace& space, GuestClock& clock, u32 sdk,
                       std::string_view nid, const std::array<u64, 6>& a) {
    using namespace GuestCpu;
    using namespace Libraries::Rtc;
    using Date = OrbisRtcDateTime;
    using Tick = OrbisRtcTick;
    static_assert(sizeof(Date) == 16 && sizeof(Tick) == 8 && sizeof(time_t) == 8);
    constexpr u64 max_tick = 315537897599999999ULL; // 9999-12-31T23:59:59.999999
    auto read = [&](u64 addr, auto& value) {
        return bool(space.Read(GuestAddress{addr}, std::as_writable_bytes(std::span{&value, 1})));
    };
    auto put = [&](u64 addr, const auto& value) -> u32 {
        auto pin = space.AcquirePinnedSpan({GuestAddress{addr}, sizeof(value)}, true);
        if (!pin)
            return u32(ORBIS_RTC_ERROR_INVALID_POINTER);
        std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
        return 0;
    };
    auto now = [&](Tick& tick) {
        Libraries::Kernel::OrbisKernelTimespec ts{};
        if (clock.Read(Libraries::Kernel::ORBIS_CLOCK_REALTIME, ts, false))
            return ORBIS_RTC_ERROR_NO_CLOCK;
        const __int128 value = __int128(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000 + UNIX_EPOCH_TICKS;
        if (value < 0 || value > max_tick)
            return ORBIS_RTC_ERROR_INVALID_VALUE;
        tick.tick = u64(value);
        return 0;
    };
    auto date_out = [&](Tick tick) -> u32 {
        if (tick.tick > max_tick)
            return u32(ORBIS_RTC_ERROR_INVALID_VALUE);
        Date date{};
        const int status = sceRtcSetTick(&date, &tick);
        return status ? u32(status) : put(a[0], date);
    };
    if (nid == "eiuobaF-hK4" || nid == "AxHBk3eat04" || nid == "WJ3rqFwymew" ||
        nid == "DwuHIlLGW8I" || nid == "lja0nNPWojg" || nid == "tOZ6fwwHZOA") {
        Tick tick{};
        if (a[1]) {
            if (!read(a[1], tick))
                return u32(ORBIS_RTC_ERROR_INVALID_POINTER);
        } else if (int status = now(tick)) {
            return u32(status);
        }
        const bool local = nid == "AxHBk3eat04" || nid == "DwuHIlLGW8I" || nid == "tOZ6fwwHZOA";
        std::string text;
        if (const int status = FormatRtcText(tick, local ? 0 : s32(a[2]),
                                             nid == "eiuobaF-hK4" || nid == "AxHBk3eat04", text))
            return u32(status);
        auto pin = space.AcquirePinnedSpan({GuestAddress{a[0]}, text.size() + 1}, true);
        if (!pin)
            return u32(ORBIS_RTC_ERROR_INVALID_POINTER);
        std::memcpy(pin.Value().WritableBytes().data(), text.c_str(), text.size() + 1);
        return 0;
    }
    if (nid == "NxEI1KByvCI" || nid == "99bMGglFW3I") {
        std::string text;
        for (u64 i = 0; i < 96; ++i) {
            char c{};
            if (a[1] > UINT64_MAX - i || !read(a[1] + i, c))
                return u32(ORBIS_RTC_ERROR_INVALID_POINTER);
            if (!c) {
                Tick tick{};
                const int status = ParseRtcText(text, nid == "99bMGglFW3I", tick);
                return status ? u32(status) : put(a[0], tick);
            }
            text += c;
        }
        return u32(ORBIS_RTC_ERROR_BAD_PARSE);
    }
    if (nid == "LlodCMDbk3o" || nid == "8SljQx6pDP8")
        return 0; // desktop's stateless init/end
    if (nid == "jMNwqYr4R-k")
        return sceRtcGetTickResolution();
    if (nid == "Ug8pCwQvh0c" || nid == "3O7Ln8AqJ1o" || nid == "CyIK-i4XdgQ") {
        const int year = s32(a[0]), month = s32(a[1]), day = s32(a[2]);
        if (year < 1 || year > 9999)
            return u32(ORBIS_RTC_ERROR_INVALID_YEAR);
        if (nid == "Ug8pCwQvh0c")
            return u32(sceRtcIsLeapYear(year));
        const int days = sceRtcGetDaysInMonth(year, month);
        if (nid == "3O7Ln8AqJ1o" || days < 0)
            return u32(days);
        if (day < 1 || day > days)
            return u32(ORBIS_RTC_ERROR_INVALID_DAY);
        // Desktop GetDayOfWeek reads the singleton Linker SDK. Use this session's
        // checked date and the same calendar calculation instead.
        return std::chrono::weekday{
            std::chrono::sys_days{std::chrono::year(year) / std::chrono::month(month) /
                                  std::chrono::day(day)}}
            .c_encoding();
    }
    if (nid == "LN3Zcb72Q0c" || nid == "Ot1DE3gif84" || nid == "zO9UL3qIINQ" ||
        nid == "HWxHOdbM-Pg") {
        if (!space.ValidateRange({GuestAddress{a[0]}, sizeof(Tick)}, GuestPermission::Write))
            return u32(ORBIS_RTC_ERROR_INVALID_POINTER);
        return u32(ORBIS_RTC_ERROR_NO_CLOCK); // no network clock provider in this session
    }
    if (nid == "18B2NS1y9UU" || nid == "8lfvnRMqwEM" || nid == "ZPD1YOKI+Kw") {
        if (!a[0])
            return u32(ORBIS_RTC_ERROR_DATETIME_UNINITIALIZED);
        Tick tick{};
        if (int status = now(tick))
            return u32(status);
        if (nid == "18B2NS1y9UU")
            return put(a[0], tick);
        const __int128 adjusted =
            __int128(tick.tick) + (nid == "8lfvnRMqwEM" ? __int128(s32(a[1])) * 60000000 : 0);
        if (adjusted < 0 || adjusted > max_tick)
            return u32(ORBIS_RTC_ERROR_INVALID_VALUE);
        return date_out(Tick{u64(adjusted)});
    }
    if (nid == "lPEBYdVX0XQ" || nid == "8w-H19ip48I" || nid == "BtqmpTRXHgk" ||
        nid == "jfRO0uTjtzA" || nid == "E7AR4o7Ny7E") {
        Date date{};
        if (!read(a[0], date))
            return u32(ORBIS_RTC_ERROR_INVALID_POINTER);
        int status = sceRtcCheckValid(&date);
        if (status || nid == "lPEBYdVX0XQ")
            return u32(status);
        if (nid == "E7AR4o7Ny7E") {
            u32 dos{};
            status = sceRtcGetDosTime(&date, &dos);
            return status ? u32(status) : put(a[1], dos);
        }
        Tick tick{};
        status = sceRtcGetTick(&date, &tick);
        if (status)
            return u32(status);
        if (nid == "8w-H19ip48I")
            return put(a[1], tick);
        const u64 epoch = nid == "BtqmpTRXHgk" ? UNIX_EPOCH_TICKS : WIN32_FILETIME_EPOCH_TICKS;
        const u64 value = tick.tick < epoch      ? 0
                          : nid == "BtqmpTRXHgk" ? (tick.tick - epoch) / 1000000
                                                 : (tick.tick - epoch) * 10;
        return put(a[1], value);
    }
    if (nid == "aYPCd1cChyg") {
        Date date{};
        const int status = sceRtcSetDosTime(&date, u32(a[1]));
        return status ? u32(status) : put(a[0], date);
    }
    if (nid == "bDEVVP4bTjQ" || nid == "n5JiAJXsbcs") {
        const s64 input = nid == "bDEVVP4bTjQ" && sdk < 0x3000000 ? u32(a[1]) : s64(a[1]);
        if (input < 0)
            return u32(ORBIS_RTC_ERROR_INVALID_VALUE);
        const __int128 tick = nid == "bDEVVP4bTjQ"
                                  ? __int128(input) * 1000000 + UNIX_EPOCH_TICKS
                                  : __int128(input) / 10 + WIN32_FILETIME_EPOCH_TICKS;
        if (tick > max_tick)
            return u32(ORBIS_RTC_ERROR_INVALID_VALUE);
        return date_out(Tick{u64(tick)});
    }
    if (nid == "ueega6v3GUw") {
        Tick tick{};
        return read(a[1], tick) ? date_out(tick) : u32(ORBIS_RTC_ERROR_INVALID_POINTER);
    }
    if (nid == "fNaZ4DbzHAE") {
        Tick lhs{}, rhs{};
        if (!read(a[0], lhs) || !read(a[1], rhs))
            return u32(ORBIS_RTC_ERROR_INVALID_POINTER);
        // Retain desktop's current comparison convention; do not silently
        // change compatibility while migrating the ABI boundary.
        return u32(sceRtcCompareTick(&lhs, &rhs));
    }
    if (nid == "8Yr143yEnRo" || nid == "M1TvFst-jrM") {
        Tick tick{};
        return read(a[0], tick) ? put(a[1], tick) : u32(ORBIS_RTC_ERROR_INVALID_POINTER);
    }
    Tick source{};
    if (!read(a[1], source))
        return u32(ORBIS_RTC_ERROR_INVALID_POINTER);
    if (nid == "CL6y9q-XbuQ" || nid == "-5y2uJ62qS8") {
        if (source.tick > max_tick)
            return u32(ORBIS_RTC_ERROR_INVALID_VALUE);
        Date date{};
        sceRtcSetTick(&date, &source);
        const s64 count = s32(a[2]);
        if (nid == "-5y2uJ62qS8") {
            const s64 year = date.year + count;
            if (year < 1 || year > 9999)
                return u32(ORBIS_RTC_ERROR_INVALID_YEAR);
            date.year = year;
        } else {
            const s64 month = (s64(date.year) - 1) * 12 + date.month - 1 + count;
            if (month < 0 || month >= 9999 * 12)
                return u32(ORBIS_RTC_ERROR_INVALID_YEAR);
            date.year = month / 12 + 1;
            date.month = month % 12 + 1;
            date.day = std::min<int>(date.day, sceRtcGetDaysInMonth(date.year, date.month));
        }
        // Desktop calendar add helpers accidentally read the destination tick.
        // Use the checked source, then the same date validation/conversion.
        Tick result{};
        const int status = sceRtcGetTick(&date, &result);
        return status ? u32(status) : put(a[0], result);
    }
    s64 scale = 1, count = s64(a[2]);
    if (nid == "NR1J0N7L2xY") {
        scale = 86400000000;
        count = s32(a[2]);
    } else if (nid == "MDc5cd8HfCA") {
        scale = 3600000000;
        count = s32(a[2]);
    } else if (nid == "mn-tf4QiFzk")
        scale = 60000000;
    else if (nid == "07O525HgICs")
        scale = 1000000;
    else if (nid == "gI4t194c2W8") {
        scale = 604800000000;
        count = s32(a[2]);
    } else if (nid != "XPIiw58C+GM" && nid != "AqVMssr52Rc")
        return u32(ORBIS_RTC_ERROR_NOT_SUPPORTED);
    const __int128 result = __int128(source.tick) + __int128(count) * scale;
    if (result < 0 || result > UINT64_MAX)
        return u32(ORBIS_RTC_ERROR_INVALID_VALUE);
    return put(a[0], Tick{u64(result)});
}
} // namespace Core::HostRuntime
