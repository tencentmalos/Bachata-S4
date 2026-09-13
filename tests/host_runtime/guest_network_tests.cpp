// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_network.h"
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
    CHECK(space->Map({GuestAddress{base + 0x4000}, 0x4000},
                     GuestPermission::Read | GuestPermission::Execute));
    auto write = [&](u64 address, const auto& value) {
        CHECK(space->Write(GuestAddress{address}, std::as_bytes(std::span{&value, 1})));
    };
    auto read = [&]<class T>(u64 address) {
        T value{};
        CHECK(space->Read(GuestAddress{address}, std::as_writable_bytes(std::span{&value, 1})));
        return value;
    };
    GuestNetwork net(false);
    auto call = [&](std::string_view nid, std::array<u64, 6> a = {}) {
        CHECK(IsNetNid(nid) || IsNetCtlNid(nid));
        return net.Dispatch(*space, nid, a, base + 128);
    };
    const auto error = [](int e) { return u32(ORBIS_NET_ERROR_BASE | e); };
    write(base, std::array<char, 8>{'p', 'o', 'o', 'l', 0});
    CHECK(call("dgJBaeJnGpo", {base, 4096, 0}) == error(ORBIS_NET_ENOTINIT));
    CHECK(call("Nlev7Lg8k3A") == 0);
    CHECK(call("Nlev7Lg8k3A") == 0);
    CHECK(call("dgJBaeJnGpo", {1, 4096, 0}) == error(ORBIS_NET_EFAULT));
    CHECK(call("dgJBaeJnGpo", {base, 0, 0}) == error(ORBIS_NET_EINVAL));
    CHECK(call("dgJBaeJnGpo", {base, 32 << 20, 0}) == error(ORBIS_NET_ENOMEM));
    CHECK(call("dgJBaeJnGpo", {base, 4096, 1}) == error(ORBIS_NET_EINVAL));
    const u64 pool = call("dgJBaeJnGpo", {base, 4096, 0});
    CHECK(pool == 1);
    CHECK(call("C4UgDHHPvdw", {base, 99, 0}) == error(ORBIS_NET_EBADF));
    CHECK(call("C4UgDHHPvdw", {base, pool, 1}) == error(ORBIS_NET_EINVAL));
    const auto resolver = call("C4UgDHHPvdw", {base, pool, 0});
    CHECK(resolver == 0x10000);
    CHECK(call("K7RlrTkI-mw", {pool}) == error(ORBIS_NET_EBUSY));
    CHECK(call("J5i3hiLJMPk", {resolver, base + 512}) == 0 &&
          read.operator()<u32>(base + 512) == 0);
    write(base + 256, u32{123});
    CHECK(call("Nd91WaWmG2w", {resolver, 1, base + 256, 1, 1, 0}) == error(ORBIS_NET_EFAULT));
    CHECK(call("Nd91WaWmG2w", {resolver, base, 1, 1, 1, 0}) == error(ORBIS_NET_EFAULT));
    CHECK(call("Nd91WaWmG2w", {resolver, base, base + 256, UINT64_MAX, 1, 0}) ==
          error(ORBIS_NET_EINVAL));
    CHECK(call("Nd91WaWmG2w", {resolver, base, base + 256, 1, 1, 1}) ==
          u32(ORBIS_NET_ERROR_RESOLVER_ENODNS));
    CHECK(read.operator()<u32>(base + 256) == 123);
    CHECK(call("J5i3hiLJMPk", {resolver, base + 512}) == 0 &&
          read.operator()<u32>(base + 512) == u32(ORBIS_NET_ERROR_RESOLVER_ENODNS));
    CHECK(call("AzqoBha7js4", {resolver, 0}) == 0);
    CHECK(call("J5i3hiLJMPk", {resolver, base + 512}) == 0 &&
          read.operator()<u32>(base + 512) == error(ORBIS_NET_ECANCELED));
    CHECK(call("kJlYH5uMAWI", {resolver}) == 0);
    CHECK(call("J5i3hiLJMPk", {resolver, base + 512}) == error(ORBIS_NET_EBADF));
    CHECK(call("kJlYH5uMAWI", {resolver}) == error(ORBIS_NET_EBADF));
    CHECK(call("cTGkc6-TBlI") == error(ORBIS_NET_EBUSY));
    CHECK(call("K7RlrTkI-mw", {pool}) == 0);
    CHECK(call("K7RlrTkI-mw", {pool}) == error(ORBIS_NET_EBADF));
    CHECK(call("dgJBaeJnGpo", {base, 4096, 0}) == 2);
    CHECK(call("K7RlrTkI-mw", {2}) == 0);
    CHECK(call("HQOwnfMGipQ") == base + 128);
    write(base + 132, s32{77});
    CHECK(call("K7RlrTkI-mw", {0}) == error(ORBIS_NET_EBADF));
    CHECK(read.operator()<s32>(base + 128) == ORBIS_NET_EBADF &&
          read.operator()<s32>(base + 132) == 77);
    for (auto nid : {"9T2pDF2Ryqg", "pQGpHYopAIY"})
        CHECK(call(nid, {0x12345678}) == 0x78563412);
    for (auto nid : {"3CHi1K1wsCQ", "tOrRi-v3AOM"})
        CHECK(call(nid, {0x123456789abcdef0ULL}) == 0xf0debc9a78563412ULL);
    for (auto nid : {"iWQWrwiSt8A", "Rbvt+5Y2iEw"})
        CHECK(call(nid, {0x1234}) == 0x3412);
    std::array<char, 48> text{};
    for (const auto& [family, address] :
         std::array<std::pair<u64, const char*>, 2>{{{2, "127.0.0.1"}, {28, "2001:db8::1"}}}) {
        text = {};
        std::strcpy(text.data(), address);
        write(base + 256, text);
        CHECK(call("8Kcp5d-q1Uo", {family, base + 256, base + 512}) == 1);
        CHECK(call("9vA2aW+CHuA", {family, base + 512, base + 768, 48}) == base + 768);
        CHECK(std::string(read.operator()<std::array<char, 48>>(base + 768).data()) == address);
    }
    CHECK(call("9vA2aW+CHuA", {28, base + 512, base + 768, 3}) == 0 &&
          read.operator()<s32>(base + 128) == ORBIS_NET_ENOSPC);
    CHECK(call("8Kcp5d-q1Uo", {0, base + 256, base + 512}) == UINT64_MAX &&
          read.operator()<s32>(base + 128) == ORBIS_NET_EAFNOSUPPORT);
    CHECK(call("8Kcp5d-q1Uo", {2, 1, base + 512}) == UINT64_MAX);
    CHECK(call("Xn2TA2QhxHc", {28, base + 256, base + 512, 0}) == 1);
    CHECK(call("Xn2TA2QhxHc", {28, base + 256, base + 512, 1}) == UINT64_MAX);
    text = {};
    std::strcpy(text.data(), "999.9.9.9");
    write(base + 256, text);
    write(base + 512, u32{123});
    CHECK(call("8Kcp5d-q1Uo", {2, base + 256, base + 512}) == 0 &&
          read.operator()<u32>(base + 512) == 123);
    CHECK(call("gky0+oaNM4k") == 0);
    CHECK(call("uBPlr0lbuiI", {1}) == u32(ORBIS_NET_CTL_ERROR_INVALID_ADDR));
    CHECK(call("uBPlr0lbuiI", {base + 256}) == 0 && read.operator()<s32>(base + 256) == 0);
    CHECK(call("obuxdTiwkF8", {14, base + 256}) == u32(ORBIS_NET_CTL_ERROR_NOT_CONNECTED));
    CHECK(call("0cBgduPRR+M", {1, base + 256}) == 0 &&
          read.operator()<u32>(base + 256) == u32(ORBIS_NET_CTL_ERROR_NETWORK_DISABLED));
    CHECK(call("UJ+Z7Q+4ck0", {1, 0, base + 256}) == u32(ORBIS_NET_CTL_ERROR_INVALID_ADDR));
    CHECK(call("UJ+Z7Q+4ck0", {base + 0x4000, 7, 1}) == u32(ORBIS_NET_CTL_ERROR_INVALID_ADDR));
    for (u64 i = 0; i < 8; ++i) {
        CHECK(call("UJ+Z7Q+4ck0", {base + 0x4000, i, base + 256}) == 0);
        CHECK(read.operator()<s32>(base + 256) == s32(i));
    }
    CHECK(call("UJ+Z7Q+4ck0", {base + 0x4000, 9, base + 256}) ==
          u32(ORBIS_NET_CTL_ERROR_CALLBACK_MAX));
    auto batch = net.BeginCallbacks();
    CHECK(batch && batch->size() == 8);
    CHECK(!net.BeginCallbacks());         // recursive/concurrent dispatch is bounded
    CHECK(net.IsCurrent((*batch)[0]));
    CHECK(call("Rqm2OnZMCz0", {0}) == 0); // callback may unregister itself
    CHECK(!net.IsCurrent((*batch)[0]));
    CHECK(call("UJ+Z7Q+4ck0", {base + 0x4000, 99, base + 256}) == 0);
    CHECK(!net.IsCurrent((*batch)[0])); // same slot, different registration
    CHECK(call("Rqm2OnZMCz0", {8}) == u32(ORBIS_NET_CTL_ERROR_INVALID_ID));
    CHECK(call("Z4wwCFiBELQ") == 0);
    CHECK(!net.IsCurrent((*batch)[1]));
    net.EndCallbacks();
    CHECK(!net.BeginCallbacks());
    CHECK(call("gky0+oaNM4k") == 0);
    batch = net.BeginCallbacks();
    CHECK(batch && batch->empty());
    net.EndCallbacks();
    CHECK(call("Z4wwCFiBELQ") == 0);
    CHECK(call("cTGkc6-TBlI") == 0);
    CHECK(call("uBPlr0lbuiI", {base + 256}) == u32(ORBIS_NET_CTL_ERROR_NOT_AVAIL));
    GuestNetwork online(true);
    CHECK(online.Dispatch(*space, "Nlev7Lg8k3A", {}, base + 132) == error(ORBIS_NET_ENETDOWN));
    CHECK(read.operator()<s32>(base + 132) == ORBIS_NET_ENETDOWN);
    for (unsigned generation = 0; generation < 3; ++generation) {
        GuestNetwork next(false);
        CHECK(next.Dispatch(*space, "Nlev7Lg8k3A", {}, base + 132) == 0);
        CHECK(next.Dispatch(*space, "dgJBaeJnGpo", {base, 4096, 0}, base + 132) == 1);
    }
    std::printf("GUEST_NETWORK checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
