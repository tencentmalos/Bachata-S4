// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_np.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::Np;
using namespace Libraries::Np::NpManager;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__);                             \
        }                                                                                          \
    } while (0)
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    CHECK(space->Map({GuestAddress{base + 0x4000}, 0x4000}, GuestPermission::Read));
    constexpr u64 invalid_user = u32(-1);
    const u64 output = base + 256;
    std::array<u8, 64> sentinel;
    sentinel.fill(0xa5);
    auto reset = [&] {
        CHECK(space->Write(GuestAddress{output}, std::as_bytes(std::span{sentinel})));
    };
    auto bytes = [&] {
        std::array<u8, 64> value{};
        CHECK(space->Read(GuestAddress{output}, std::as_writable_bytes(std::span{value})));
        return value;
    };
    auto unchanged = [&] { CHECK(bytes() == sentinel); };
    auto prefix = [&](const auto& expected) {
        auto want = sentinel;
        std::memcpy(want.data(), &expected, sizeof(expected));
        CHECK(bytes() == want); // also checks surrounding bytes and structure padding
    };
    GuestNpOffline current(Common::ElfInfo::FW_900, {{1000, false}, {1001, true}});
    auto call = [&](std::string_view nid, u64 user = 1000, u64 out = 0) {
        return current.Dispatch(*space, nid, {user, out ? out : output});
    };
    constexpr u32 bad = ORBIS_NP_ERROR_INVALID_ARGUMENT, offline = ORBIS_NP_ERROR_SIGNED_OUT;
    for (auto nid : NpOfflineNids) {
        CHECK(AdmitsNpOffline(nid, "#libSceNpManager#1#libSceNpManager#Function", true));
        CHECK(!AdmitsNpOffline(nid, "#libSceNpManager#1#libSceNpManager#Function", false));
        CHECK(!AdmitsNpOffline(nid, "#libkernel#1#libkernel#Function", true));
        CHECK(!AdmitsNpOffline(nid, "#libSceNpManager#2#libSceNpManager#Function", true));
        if (nid == "3Zl8BePTh9Y" || nid == "JELHf4xPufo")
            continue;
        for (auto ptr : {u64{0}, u64{1}, base + 0x4000, base + 0x8000, UINT64_MAX})
            CHECK(current.Dispatch(*space, nid, {1000, ptr}) == bad);
    }
    CHECK(!IsNpOfflineNid("qQJfO8HAiaY")); // no callbacks, online requests or fabricated success
    CHECK(!IsNpOfflineNid("8Z2Jc5GvGDI"));
    CHECK(
        !AdmitsNpOffline("rbknaUjpqWo", "#libSceNpManagerCompat#1#libSceNpManager#Function", true));
    CHECK(
        AdmitsNpOffline("a8R9-75u4iM", "#libSceNpManagerCompat#1#libSceNpManager#Function", true));
    CHECK(call("3Zl8BePTh9Y") == 0 && call("JELHf4xPufo") == 0);
    reset();
    CHECK(call("rbknaUjpqWo") == offline);
    prefix(u64{0});
    reset();
    CHECK(call("rbknaUjpqWo", invalid_user) == bad);
    unchanged();
    reset();
    CHECK(call("rbknaUjpqWo", 0x12345678) == offline);
    prefix(u64{0}); // no existence check
    const u64 tail = base + 0x4000 - 4;
    CHECK(space->Write(GuestAddress{tail}, std::as_bytes(std::span{sentinel}).first(4)));
    CHECK(call("rbknaUjpqWo", 1000, tail) == bad);
    u32 tail_value{};
    CHECK(space->Read(GuestAddress{tail}, std::as_writable_bytes(std::span{&tail_value, 1})));
    CHECK(tail_value == 0xa5a5a5a5); // no partial clear across a read-only boundary
    for (auto nid : {"p-o74CnoNzY", "XDncXQIJUSk"}) {
        reset();
        CHECK(call(nid) == offline);
        unchanged();
        CHECK(call(nid, invalid_user) == bad);
        unchanged();
    }
    reset();
    CHECK(call("eQH7nWPcAgc") == 0);
    prefix(u32{1});
    reset();
    CHECK(call("e-ZuhGEoeC4") == 0);
    prefix(u32{0});
    reset();
    CHECK(call("oPO9U42YpgI") == 0);
    prefix(u32{0});
    for (auto nid : {"eQH7nWPcAgc", "e-ZuhGEoeC4", "oPO9U42YpgI"}) {
        reset();
        CHECK(call(nid, invalid_user) == bad);
        unchanged();
    }
    reset();
    CHECK(call("VgYczPGB5ss", 1) == offline);
    unchanged();
    CHECK(call("VgYczPGB5ss", 0) == bad);
    unchanged();
    OrbisNpOnlineId online{};
    std::memcpy(online.data, "test-user", 9);
    CHECK(space->Write(GuestAddress{base}, std::as_bytes(std::span{&online, 1})));
    reset();
    CHECK(call("F6E4ycq9Dbg", base) == offline);
    unchanged();
    CHECK(call("F6E4ycq9Dbg", base + 0x8000 - 10) == bad);
    unchanged();
    reset();
    CHECK(call("a8R9-75u4iM", base) == u32(ORBIS_NP_ERROR_USER_NOT_FOUND));
    prefix(u64{0});
    reset();
    CHECK(call("IPb1hd1wAGc", base) == 0);
    prefix(u32{0});
    reset();
    CHECK(call("Oad3rvY-NJQ", 1000) == 0);
    prefix(false);
    reset();
    CHECK(call("Oad3rvY-NJQ", 1001) == 0);
    prefix(true);
    reset();
    CHECK(call("Oad3rvY-NJQ", 2000) == u32(ORBIS_NP_ERROR_USER_NOT_FOUND));
    prefix(false);
    reset();
    CHECK(call("Oad3rvY-NJQ", invalid_user) == bad);
    prefix(false);
    for (s32 sdk : {-1, 0, s32(Common::ElfInfo::FW_400 - 1), s32(Common::ElfInfo::FW_400),
                    s32(Common::ElfInfo::FW_900 - 1), s32(Common::ElfInfo::FW_900)}) {
        GuestNpOffline np(sdk, {});
        const bool modern9 = sdk < 0 || sdk >= s32(Common::ElfInfo::FW_900);
        const bool modern4 = sdk < 0 || sdk >= s32(Common::ElfInfo::FW_400);
        reset();
        CHECK(np.Dispatch(*space, "eQH7nWPcAgc", {invalid_user, output}) == (modern9 ? bad : 0));
        if (modern9)
            unchanged();
        else
            prefix(u32{1});
        reset();
        CHECK(np.Dispatch(*space, "e-ZuhGEoeC4", {invalid_user, output}) == (modern4 ? bad : 0));
        if (modern4)
            unchanged();
        else
            prefix(u32{0});
        for (auto nid : {"p-o74CnoNzY", "XDncXQIJUSk"}) {
            reset();
            CHECK(np.Dispatch(*space, nid, {invalid_user, output}) ==
                  (modern9 ? bad : u32(ORBIS_NP_ERROR_USER_NOT_FOUND)));
            unchanged();
            CHECK(np.Dispatch(*space, nid, {invalid_user, 0}) ==
                  (modern9 ? bad : u32(ORBIS_NP_ERROR_USER_NOT_FOUND)));
        }
        reset();
        CHECK(np.Dispatch(*space, "Oad3rvY-NJQ", {1001, output}) ==
              u32(ORBIS_NP_ERROR_USER_NOT_FOUND));
        prefix(false); // session isolation
    }
    std::printf("NP offline: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
