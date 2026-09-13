// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_rtc.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::Rtc;
static unsigned checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL line %d: %s\n", __LINE__, #x);                                       \
        }                                                                                          \
    } while (0)
int main() {
    AddressSpaceConfig cfg{};
    cfg.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(cfg);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    auto set = [&](u64 addr, const auto& value) {
        CHECK(space->Write(GuestAddress{addr}, std::as_bytes(std::span{&value, 1})));
    };
    auto get = [&]<typename T>(u64 addr) {
        T value{};
        if (!space->Read(GuestAddress{addr}, std::as_writable_bytes(std::span{&value, 1})))
            std::abort();
        return value;
    };
    GuestClock clock;
    auto call = [&](std::string_view nid, std::array<u64, 6> a, u32 sdk = 0x9000000) {
        CHECK(IsRtcNid(nid));
        return DispatchRtc(*space, clock, sdk, nid, a);
    };
    CHECK(call("LlodCMDbk3o", {}) == 0 && call("8SljQx6pDP8", {}) == 0);
    CHECK(call("jMNwqYr4R-k", {}) == 1000000);
    CHECK(call("Ug8pCwQvh0c", {2000}) == 1 && call("Ug8pCwQvh0c", {1900}) == 0);
    CHECK(call("3O7Ln8AqJ1o", {2024, 2}) == 29);
    CHECK(call("CyIK-i4XdgQ", {1970, 1, 1}) == 4);
    CHECK(call("CyIK-i4XdgQ", {2023, 2, 29}) == u32(ORBIS_RTC_ERROR_INVALID_DAY));
    OrbisRtcDateTime date{1970, 1, 1, 23, 59, 59, 999999};
    set(base, date);
    CHECK(call("lPEBYdVX0XQ", {base}) == 0);
    CHECK(call("8w-H19ip48I", {base, base + 32}) == 0);
    CHECK(get.operator()<OrbisRtcTick>(base + 32).tick == u64(UNIX_EPOCH_TICKS) + 86399999999ULL);
    auto original = get.operator()<OrbisRtcDateTime>(base);
    CHECK(std::memcmp(&original, &date, sizeof(date)) == 0);
    CHECK(call("ueega6v3GUw", {base + 64, base + 32}) == 0);
    auto roundtrip = get.operator()<OrbisRtcDateTime>(base + 64);
    CHECK(std::memcmp(&roundtrip, &date, sizeof(date)) == 0);
    CHECK(call("BtqmpTRXHgk", {base, base + 96}) == 0 && get.operator()<u64>(base + 96) == 86399);
    CHECK(call("jfRO0uTjtzA", {base, base + 96}) == 0);
    CHECK(call("n5JiAJXsbcs", {base + 64, get.operator()<u64>(base + 96)}) == 0);
    roundtrip = get.operator()<OrbisRtcDateTime>(base + 64);
    CHECK(std::memcmp(&roundtrip, &date, sizeof(date)) == 0);
    CHECK(call("bDEVVP4bTjQ", {base + 64, 0}) == 0 &&
          get.operator()<OrbisRtcDateTime>(base + 64).year == 1970);
    CHECK(call("bDEVVP4bTjQ", {base + 64, UINT64_MAX}) == u32(ORBIS_RTC_ERROR_INVALID_VALUE));
    CHECK(call("bDEVVP4bTjQ", {base + 64, UINT64_MAX}, 0x2000000) == 0 &&
          get.operator()<OrbisRtcDateTime>(base + 64).year == 2106);
    date = {2024, 2, 29, 17, 59, 58, 0};
    set(base, date);
    CHECK(call("E7AR4o7Ny7E", {base, base + 96}) == 0);
    CHECK(call("aYPCd1cChyg", {base + 64, get.operator()<u32>(base + 96)}) == 0);
    roundtrip = get.operator()<OrbisRtcDateTime>(base + 64);
    CHECK(std::memcmp(&roundtrip, &date, sizeof(date)) == 0);
    CHECK(call("8w-H19ip48I", {base, base + 32}) == 0);
    auto tick = get.operator()<OrbisRtcTick>(base + 32);
    const std::array<std::pair<std::string_view, u64>, 7> scales{
        {{"NR1J0N7L2xY", 86400000000ULL},
         {"MDc5cd8HfCA", 3600000000ULL},
         {"XPIiw58C+GM", 1},
         {"mn-tf4QiFzk", 60000000},
         {"07O525HgICs", 1000000},
         {"AqVMssr52Rc", 1},
         {"gI4t194c2W8", 604800000000ULL}}};
    for (auto [nid, scale] : scales) {
        CHECK(call(nid, {base + 96, base + 32, 2}) == 0 &&
              get.operator()<u64>(base + 96) == tick.tick + 2 * scale);
        CHECK(call(nid, {base + 96, base + 32, UINT64_MAX}) == 0 &&
              get.operator()<u64>(base + 96) == tick.tick - scale);
    }
    CHECK(call("CL6y9q-XbuQ", {base + 96, base + 32, 12}) == 0);
    CHECK(call("ueega6v3GUw", {base + 64, base + 96}) == 0);
    roundtrip = get.operator()<OrbisRtcDateTime>(base + 64);
    CHECK(roundtrip.year == 2025 && roundtrip.month == 2 && roundtrip.day == 28);
    CHECK(call("-5y2uJ62qS8", {base + 96, base + 32, 1}) == u32(ORBIS_RTC_ERROR_INVALID_DAY));
    CHECK(call("-5y2uJ62qS8", {base + 96, base + 32, 4}) == 0);
    for (auto nid : {"8Yr143yEnRo", "M1TvFst-jrM"})
        CHECK(call(nid, {base + 32, base + 96}) == 0 &&
              get.operator()<u64>(base + 96) == tick.tick);
    CHECK(call("fNaZ4DbzHAE", {base + 32, base + 96}) == 1); // existing desktop convention
    CHECK(call("18B2NS1y9UU", {base + 128}) == 0 &&
          get.operator()<u64>(base + 128) > u64(UNIX_EPOCH_TICKS));
    CHECK(call("8lfvnRMqwEM", {base + 64, 0}) == 0 &&
          get.operator()<OrbisRtcDateTime>(base + 64).year >= 2026);
    CHECK(call("ZPD1YOKI+Kw", {base + 64}) == 0);
    for (auto nid : {"LN3Zcb72Q0c", "Ot1DE3gif84", "zO9UL3qIINQ", "HWxHOdbM-Pg"}) {
        set(base + 128, u64{123});
        CHECK(call(nid, {base + 128}) == u32(ORBIS_RTC_ERROR_NO_CLOCK) &&
              get.operator()<u64>(base + 128) == 123);
    }
    CHECK(call("18B2NS1y9UU", {1}) == u32(ORBIS_RTC_ERROR_INVALID_POINTER));
    CHECK(call("8w-H19ip48I", {1, base + 96}) == u32(ORBIS_RTC_ERROR_INVALID_POINTER));
    CHECK(call("8w-H19ip48I", {base, 1}) == u32(ORBIS_RTC_ERROR_INVALID_POINTER));
    set(base + 32, u64{1});
    set(base + 96, u64{123});
    CHECK(call("07O525HgICs", {base + 96, base + 32, UINT64_MAX}) ==
              u32(ORBIS_RTC_ERROR_INVALID_VALUE) &&
          get.operator()<u64>(base + 96) == 123);
    CHECK(call("07O525HgICs", {base + 96, base + 32, INT64_MAX}) ==
          u32(ORBIS_RTC_ERROR_INVALID_VALUE));
    set(base + 32, u64{315537897599999999ULL});
    CHECK(call("ueega6v3GUw", {base + 64, base + 32}) == 0);
    CHECK(get.operator()<OrbisRtcDateTime>(base + 64).year == 9999);
    set(base + 32, UINT64_MAX);
    CHECK(call("ueega6v3GUw", {base + 64, base + 32}) == u32(ORBIS_RTC_ERROR_INVALID_VALUE));
    // The shared desktop helper also retains its input and handles 59 minutes.
    auto copy = date;
    OrbisRtcTick desktop{};
    CHECK(sceRtcGetTick(&copy, &desktop) == 0 && std::memcmp(&copy, &date, sizeof(date)) == 0);
    CHECK(desktop.tick == tick.tick);
    date = {2000, 2, 29, 0, 0, 0, 123456};
    CHECK(sceRtcGetTick(&date, &desktop) == 0);
    set(base + 32, desktop);
    const auto string_at = [&](u64 at) {
        const auto chars = get.operator()<std::array<char, 64>>(at);
        return std::string(chars.data());
    };
    for (auto nid : {"eiuobaF-hK4", "AxHBk3eat04"}) {
        CHECK(call(nid, {base + 256, base + 32, 0}) == 0);
        CHECK(string_at(base + 256) == "Tue, 29 Feb 2000 00:00:00 +0000");
    }
    CHECK(call("eiuobaF-hK4", {base + 256, base + 32, u64(-30LL)}) == 0);
    CHECK(string_at(base + 256) == "Mon, 28 Feb 2000 23:30:00 -0030");
    for (auto nid : {"WJ3rqFwymew", "DwuHIlLGW8I", "lja0nNPWojg", "tOZ6fwwHZOA"}) {
        CHECK(call(nid, {base + 256, base + 32, 0}) == 0);
        CHECK(string_at(base + 256) == "2000-02-29T00:00:00.12Z");
    }
    CHECK(call("WJ3rqFwymew", {base + 256, base + 32, 65}) == 0);
    CHECK(string_at(base + 256) == "2000-02-29T01:05:00.12+01:05");
    CHECK(call("AxHBk3eat04", {base + 256, 0}) == 0); // session clock when tick is null
    CHECK(call("WJ3rqFwymew", {base + 256, base + 32, UINT32_MAX / 2}) ==
          u32(ORBIS_RTC_ERROR_INVALID_VALUE));
    CHECK(call("AxHBk3eat04", {base + 256, 1}) == u32(ORBIS_RTC_ERROR_INVALID_POINTER));
    const std::array<char, 8> sentinel{'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    set(base + 0x3ff8, sentinel);
    CHECK(call("AxHBk3eat04", {base + 0x3ff8, base + 32}) == u32(ORBIS_RTC_ERROR_INVALID_POINTER));
    CHECK((get.operator()<std::array<char, 8>>(base + 0x3ff8) == sentinel));
    auto parse = [&](std::string_view text, std::string_view nid = "99bMGglFW3I") {
        std::array<char, 96> input{};
        std::memcpy(input.data(), text.data(), std::min(text.size(), input.size()));
        set(base + 512, input);
        return call(nid, {base + 96, base + 512});
    };
    CHECK(parse("2000-02-29T01:30:00.123456+01:30") == 0);
    CHECK(get.operator()<u64>(base + 96) == desktop.tick);
    CHECK(parse("2000-02-28T23:30:00.123456789-00:30") == 0);
    CHECK(get.operator()<u64>(base + 96) == desktop.tick);
    CHECK(parse("2000-02-29T00:00:00.1Z") == 0);
    CHECK(get.operator()<u64>(base + 96) == desktop.tick - 23456);
    CHECK(parse("Tue, 29 Feb 2000 01:30:00 +0130", "NxEI1KByvCI") == 0);
    CHECK(get.operator()<u64>(base + 96) == desktop.tick - 123456);
    CHECK(parse("Tue Feb 29 00:00:00 2000\n", "NxEI1KByvCI") == 0);
    CHECK(get.operator()<u64>(base + 96) == desktop.tick - 123456);
    CHECK(parse("Tue Feb  1 00:00:00 2000", "NxEI1KByvCI") == 0);
    const std::string valid = "2000-02-29T01:30:00.123456+01:30";
    for (size_t i = 0; i < valid.size(); ++i) {
        set(base + 96, u64{123});
        CHECK(parse(std::string_view(valid).substr(0, i)) == u32(ORBIS_RTC_ERROR_BAD_PARSE));
        CHECK(get.operator()<u64>(base + 96) == 123);
    }
    for (auto bad : {"2001-02-29T00:00:00Z", "2000-13-01T00:00:00Z", "2000-02-29T24:00:00Z",
                     "2000-02-29T00:60:00Z", "2000-02-29T00:00:60Z", "2000-02-29T00:00:00.Z",
                     "2000-02-29T00:00:00+24:00", "2000-02-29T00:00:00+00:60",
                     "0001-01-01T00:00:00+01:00", "9999-12-31T23:59:59-01:00",
                     "Tue, 29 Abc 2000 00:00:00 +0000", "??? Feb 29 00:00:00 2000"})
        CHECK(parse(bad, "NxEI1KByvCI") == u32(ORBIS_RTC_ERROR_BAD_PARSE));
    CHECK(parse(std::string(96, '1')) == u32(ORBIS_RTC_ERROR_BAD_PARSE));
    CHECK(call("99bMGglFW3I", {base + 96, base + 0x3ff8}) == u32(ORBIS_RTC_ERROR_INVALID_POINTER));
    CHECK(parse(valid) == 0);
    CHECK(call("99bMGglFW3I", {1, base + 512}) == u32(ORBIS_RTC_ERROR_INVALID_POINTER));
    std::array<char, 64> native_text{};
    CHECK(sceRtcFormatRFC2822(native_text.data(), &desktop, -30) == 0 &&
          std::string(native_text.data()) == "Mon, 28 Feb 2000 23:30:00 -0030");
    CHECK(sceRtcParseRFC3339(&desktop, valid.c_str()) == 0);
    OrbisRtcTick expected{};
    CHECK(sceRtcGetTick(&date, &expected) == 0 && desktop.tick == expected.tick);
    desktop.tick = 123;
    CHECK(sceRtcParseDateTime(&desktop, "x") == ORBIS_RTC_ERROR_BAD_PARSE && desktop.tick == 123);
    std::printf("GUEST_RTC checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
