// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_ssl.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
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
    GuestSslOffline ssl;
    auto call = [&](std::string_view nid, u64 id, u64 ptr = 0) {
        return ssl.Dispatch(*space, nid, {id, ptr});
    };
    CHECK(call("hdpVEUDFW3s", 0) == GuestSslOffline::InvalidArgument);
    CHECK(call("hdpVEUDFW3s", UINT64_MAX) == GuestSslOffline::NoMemory);
    const u32 id = call("hdpVEUDFW3s", 0x4c000);
    CHECK(id == 1);
    const u32 second = call("hdpVEUDFW3s", 0x4000);
    CHECK(second == 2);
    std::array<u64, 5> out{};
    auto reset = [&] {
        out.fill(0xa5a5a5a5a5a5a5a5);
        CHECK(space->WriteData(GuestAddress{base}, std::as_bytes(std::span{out})));
    };
    auto read = [&] {
        CHECK(space->ReadData(GuestAddress{base}, std::as_writable_bytes(std::span{out})));
    };
    for (auto nid : {"TDfQqO-gMbY", "-PoIzr3PEk0"}) {
        reset();
        for (auto ptr : {u64{0}, u64{1}, UINT64_MAX, base + 0x4000 - 8, base + 0x4000})
            CHECK(call(nid, id, ptr) == GuestSslOffline::InvalidArgument);
        read();
        CHECK(std::ranges::all_of(out, [](auto v) { return v == 0xa5a5a5a5a5a5a5a5; }));
        CHECK(call(nid, 0, base) == GuestSslOffline::InvalidId);
        CHECK(call(nid, 128, base) == GuestSslOffline::NotFound);
        CHECK(call(nid, id, base + 8) == 0);
        read();
        CHECK(out[0] == 0xa5a5a5a5a5a5a5a5 && out[4] == 0xa5a5a5a5a5a5a5a5);
        if (std::string_view(nid) == "TDfQqO-gMbY")
            CHECK(out[1] == 0 && out[2] == 0 && out[3] == 0);
        else
            CHECK(out[1] == 0x4c000 && out[2] == 16 && out[3] == 16);
    }
    CHECK(call("TDfQqO-gMbY", id, base + 8) == 0);
    CHECK(call("qIvLs0gYxi0", id, base + 8) == 0);
    CHECK(call("qIvLs0gYxi0", id, base + 8) == 0); // no allocation to double-free
    for (size_t i = 0; i < 3; ++i) {
        GuestSslOffline::CaCerts foreign{};
        if (i == 0)
            foreign.certs = base + 100;
        if (i == 1)
            foreign.count = 1;
        if (i == 2)
            foreign.pool = UINT64_MAX;
        CHECK(space->WriteData(GuestAddress{base}, std::as_bytes(std::span{&foreign, 1})));
        CHECK(call("qIvLs0gYxi0", id, base) == GuestSslOffline::InvalidArgument);
    }
    CHECK(call("qIvLs0gYxi0", id, 0) == GuestSslOffline::InvalidArgument);
    CHECK(call("qIvLs0gYxi0", id, UINT64_MAX) == GuestSslOffline::InvalidArgument);
    CHECK(call("0K1yQ6Lv-Yc", id) == 0);
    CHECK(call("0K1yQ6Lv-Yc", id) == GuestSslOffline::InvalidId);
    CHECK(call("TDfQqO-gMbY", id, base) == GuestSslOffline::InvalidId);
    CHECK(call("-PoIzr3PEk0", second, base + 8) == 0);
    read();
    CHECK(out[1] == 0x4000 && out[2] == 16 && out[3] == 16);
    CHECK(call("0K1yQ6Lv-Yc", second) == 0);
    for (auto nid : GuestSslOffline::Nids)
        CHECK(GuestSslOffline::IsNid(nid));
    for (auto nid : {"zXvd6iNyfgc", "E4a-ahM57QQ", "p5bM5PPufFY", "hi0veU3L2pU"})
        CHECK(!GuestSslOffline::IsNid(nid)); // TLS/connect/callback/transport still refused
    GuestSslOffline bounded;
    for (u32 i = 1; i <= 128; ++i)
        CHECK(bounded.Dispatch(*space, "hdpVEUDFW3s", {16}) == i);
    CHECK(bounded.Dispatch(*space, "hdpVEUDFW3s", {16}) == GuestSslOffline::TooMany);
    CHECK(bounded.Dispatch(*space, "0K1yQ6Lv-Yc", {1}) == 0);
    CHECK(bounded.Dispatch(*space, "hdpVEUDFW3s", {16}) == 1);
    std::printf("ssl-offline: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
