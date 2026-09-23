// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <chrono>
#include <future>
#include <thread>
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
    {
        unsigned calls{};
        const GuestNetwork::MacAddress expected{0x02, 1, 2, 3, 4, 5}; // synthetic provider only
        GuestNetwork identity(false, std::make_shared<GuestDescriptorIds>(), [&] {
            ++calls;
            return std::optional{expected};
        });
        auto query = [&](u64 address, u64 flags = 0) {
            return identity.Dispatch(*space, "6Oc0bLsIYe0", {address, flags}, base + 128);
        };
        CHECK(query(0, 1) == error(ORBIS_NET_ENOTINIT) && calls == 0);
        CHECK(identity.Dispatch(*space, "Nlev7Lg8k3A", {}, base + 128) == 0);
        CHECK(query(0) == error(ORBIS_NET_EINVAL) && calls == 0);
        CHECK(query(base + 512, 1) == error(ORBIS_NET_EINVAL) && calls == 0);
        CHECK(query(1) == error(ORBIS_NET_EFAULT) && calls == 0);
        CHECK(query(base + 0x4000) == error(ORBIS_NET_EFAULT) && calls == 0);
        write(base + 512, std::array<u8, 8>{9,9,9,9,9,9,9,9});
        CHECK(query(base + 512) == 0 && calls == 1);
        CHECK(read.operator()<std::array<u8, 8>>(base + 512) ==
              (std::array<u8, 8>{2,1,2,3,4,5,9,9}));
        identity.RequestStop();
        CHECK(query(base + 512) == error(ORBIS_NET_ECANCELED) && calls == 1);
        GuestNetwork absent(false, std::make_shared<GuestDescriptorIds>(), [] {
            return std::optional<GuestNetwork::MacAddress>{};
        });
        CHECK(absent.Dispatch(*space, "Nlev7Lg8k3A", {}, base + 128) == 0);
        CHECK(absent.Dispatch(*space, "6Oc0bLsIYe0", {base + 512}, base + 128) == error(ORBIS_NET_ENODEV));
        CHECK(read.operator()<std::array<u8, 8>>(base + 512) ==
              (std::array<u8, 8>{2,1,2,3,4,5,9,9}));
        CHECK(!space->Counts().live_pins);
    }
    write(base, std::array<char, 8>{'p', 'o', 'o', 'l', 0});
    {
        // Production module publication performs Initialize, without a guest
        // sceNetInit call. Term must still return it to the cold state.
        GuestNetwork published(false);
        CHECK(published.Initialize() == 0);
        CHECK(published.Initialize() == 0);
        auto dispatch = [&](std::string_view nid, std::array<u64, 6> args = {}) {
            return published.Dispatch(*space, nid, args, base + 128);
        };
        const auto pool = dispatch("dgJBaeJnGpo", {base, 65536, 0});
        CHECK(s32(pool) > 0);
        CHECK(dispatch("cTGkc6-TBlI") == error(ORBIS_NET_EBUSY));
        CHECK(dispatch("K7RlrTkI-mw", {pool}) == 0);
        CHECK(dispatch("cTGkc6-TBlI") == 0);
        CHECK(dispatch("dgJBaeJnGpo", {base, 65536, 0}) == error(ORBIS_NET_ENOTINIT));
        CHECK(dispatch("Nlev7Lg8k3A") == 0);
        published.RequestStop();
        CHECK(published.Initialize() == error(ORBIS_NET_ECANCELED));
        CHECK(dispatch("Nlev7Lg8k3A") == error(ORBIS_NET_ECANCELED));
        GuestNetwork online_provider(true);
        CHECK(online_provider.Initialize() == error(ORBIS_NET_ENETDOWN));
        CHECK(online_provider.Dispatch(*space, "dgJBaeJnGpo", {base, 65536, 0}, base + 128)
              == error(ORBIS_NET_ENOTINIT));
    }
    CHECK(call("dgJBaeJnGpo", {base, 4096, 0}) == error(ORBIS_NET_ENOTINIT));
    CHECK(call("Nlev7Lg8k3A") == 0);
    CHECK(call("Nlev7Lg8k3A") == 0);
    CHECK(call("SF47kB2MNTo", {base,1}) == error(ORBIS_NET_EINVAL));
    CHECK(call("SF47kB2MNTo", {1,0}) == error(ORBIS_NET_EFAULT));
    const auto epoll=call("SF47kB2MNTo",{base,0}); CHECK(epoll==0x20000);
    write(base+256,u64{0xfeed});
    CHECK(call("drjIbDbA7UQ",{epoll,base+256,1,0})==0);
    using namespace std::chrono_literals;
    const auto before = std::chrono::steady_clock::now();
    CHECK(call("drjIbDbA7UQ", {epoll, base+256, 1, 33000}) == 0);
    CHECK(std::chrono::steady_clock::now() - before >= 30ms);
    auto wait = std::async(std::launch::async, [&] {
        return net.Dispatch(*space, "drjIbDbA7UQ", {epoll, base+256, 1, u32(-1)}, base+144);
    });
    // Repeated aborts cover a worker that has not entered the wait yet.
    for (int i = 0; i < 100 && wait.wait_for(1ms) != std::future_status::ready; ++i)
        CHECK(call("w21YgGGNtBk", {epoll}) == 0);
    if (wait.wait_for(1ms) != std::future_status::ready) net.RequestStop();
    CHECK(wait.get() == error(ORBIS_NET_ECANCELED));
    CHECK(call("drjIbDbA7UQ", {epoll, base+256, 1, 0}) == 0); // abort is not permanent
    const auto doomed = call("SF47kB2MNTo", {base, 0});
    auto destroyed_wait = std::async(std::launch::async, [&] {
        return net.Dispatch(*space, "drjIbDbA7UQ", {doomed, base+256, 1, u32(-1)}, base+144);
    });
    // Destroy must also be safe before waiter lookup/registration.
    CHECK(call("Inp1lfL+Jdw", {doomed}) == 0);
    CHECK(destroyed_wait.wait_for(1s) == std::future_status::ready);
    CHECK(destroyed_wait.get() == error(ORBIS_NET_EBADF));
    CHECK(read.operator()<u64>(base+256)==0xfeed);
    CHECK(call("drjIbDbA7UQ",{epoll,base+256,UINT64_MAX,0})==error(ORBIS_NET_EINVAL));
    CHECK(call("drjIbDbA7UQ",{epoll,1,1,0})==error(ORBIS_NET_EFAULT));
    write(base+256, Libraries::Net::OrbisNetEpollEvent{1});
    CHECK(call("ZVw46bsasAk",{epoll,1,1,base+256})==error(ORBIS_NET_EBADF));
    CHECK(call("ZVw46bsasAk",{epoll,2,1,0})==error(ORBIS_NET_EFAULT));
    CHECK(call("w21YgGGNtBk",{epoll})==0);
    CHECK(call("cTGkc6-TBlI")==error(ORBIS_NET_EBUSY));
    CHECK(call("Inp1lfL+Jdw",{epoll})==0);
    CHECK(call("Inp1lfL+Jdw",{epoll})==error(ORBIS_NET_EBADF));
    CHECK(call("drjIbDbA7UQ",{epoll,base+256,1,0})==error(ORBIS_NET_EBADF));
    const auto socket = call("Q4qBuN-c0ZM", {base, 2, 1, 0});
    CHECK(s64(socket) >= 0 && socket < 1024);
    {
        using namespace Libraries::Net;
        CHECK(call("hLuXdjHnhiI", {u32(-1), 0, 0, 0}) == 1);
        CHECK(call("hLuXdjHnhiI", {socket, 0, 1, 0}) == error(ORBIS_NET_EINVAL));
        CHECK(call("hLuXdjHnhiI", {socket, base+1024, 0, 0}) == error(ORBIS_NET_EINVAL));
        CHECK(call("hLuXdjHnhiI", {socket, 1, 1, 0}) == error(ORBIS_NET_EFAULT));
        write(base+128, s32{123});
        CHECK(call("hLuXdjHnhiI", {socket, base+1024, 1, 0x1000}) == error(ORBIS_NET_EINVAL));
        CHECK(read.operator()<s32>(base+128) == 123); // wrapper's direct return
        CHECK(call("hLuXdjHnhiI", {socket, base+1024, 1, 0x40}) == error(ORBIS_NET_EOPNOTSUPP));
        CHECK(call("hLuXdjHnhiI", {socket, base+1024, 1, 0}) == 1);
        auto info = read.operator()<OrbisNetSockInfo>(base+1024);
        CHECK(info.s == socket && info.socket_type == 1 && std::string_view(info.name) == "pool");
        CHECK(info.flags == ORBIS_NET_SOCKINFO_F_SELF); // host fd is always nonblocking
        struct Addr { u8 size, family; u16 port; u32 addr; u64 zero; };
        write(base+512, Addr{16, 2, 0, htonl(0x7f000001), 0});
        CHECK(call("bErx49PgxyY", {socket, base+512, 16}) == 0);
        CHECK(call("kOj1HiAGE54", {socket, 4}) == 0);
        CHECK(call("hLuXdjHnhiI", {socket, base+1024, 1, 0}) == 1);
        info = read.operator()<OrbisNetSockInfo>(base+1024);
        CHECK(info.state == ORBIS_NET_SOCKINFO_STATE_LISTEN && info.local_port &&
              info.local_adr.inaddr_addr == htonl(0x7f000001));
        write(base+512, Addr{16, 2, info.local_port, info.local_adr.inaddr_addr, 0});
        const auto client = call("Q4qBuN-c0ZM", {base, 2, 1, 0});
        CHECK(client < 1024 && call("OXXX4mUk3uk", {client, base+512, 16}) == 0);
        CHECK(call("hLuXdjHnhiI", {client, base+1024, 1, 0}) == 1);
        info = read.operator()<OrbisNetSockInfo>(base+1024);
        CHECK(info.state == ORBIS_NET_SOCKINFO_STATE_ESTABLISHED && info.remote_port &&
              info.recv_buffer_size > 0 && info.send_buffer_size > 0);
        CHECK(call("hLuXdjHnhiI", {u32(-1), base+1024, 1, 0}) == 1);
        CHECK(call("hLuXdjHnhiI", {u32(-1), base+1024, 1000000, 0}) == 2);
        CHECK(call("45ggEzakPJQ", {client}) == 0);
        CHECK(call("hLuXdjHnhiI", {client, base+1024, 1, 0}) == error(ORBIS_NET_EBADF));
        const auto ipv6 = call("Q4qBuN-c0ZM", {base, 28, 2, 0});
        CHECK(ipv6 < 1024 && call("hLuXdjHnhiI", {ipv6, base+1024, 1, 0}) == error(ORBIS_NET_EAFNOSUPPORT));
        CHECK(call("hLuXdjHnhiI", {u32(-1), 0, 0, 0}) == 1); // IPv4 ABI only
        CHECK(call("45ggEzakPJQ", {ipv6}) == 0);
        CHECK(!space->Counts().live_pins);
    }
    CHECK(call("45ggEzakPJQ", {socket}) == 0);
    CHECK(call("Q4qBuN-c0ZM", {1, 2, 1, 0}) == error(ORBIS_NET_EFAULT));
    CHECK(call("45ggEzakPJQ", {1}) == error(ORBIS_NET_EBADF));
    CHECK(call("9wO9XrMsNhc", {1, base, 128}) == error(ORBIS_NET_EBADF));
    CHECK(read.operator()<u32>(base) == 0x6c6f6f70); // no fabricated received bytes
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
    CHECK(call("wIsKy+TfeLs",{base,123,base+512})==u32(ORBIS_NET_CTL_ERROR_INVALID_ADDR));
    CHECK(call("wIsKy+TfeLs",{base+0x4000,123,base+512})==0);
    const auto toolkit_id=read.operator()<s32>(base+512);
    auto toolkit_events=net.BeginCallbacks(true);
    CHECK(toolkit_events && toolkit_events->empty()); net.EndCallbacks();
    CHECK(call("2oUqKR5odGc",{u64(toolkit_id)})==0);
    CHECK(call("2oUqKR5odGc",{u64(toolkit_id)})==u32(ORBIS_NET_CTL_ERROR_ID_NOT_FOUND));
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
    CHECK(online.InitializeControl() == u32(ORBIS_NET_CTL_ERROR_NOT_AVAIL));
    CHECK(online.Dispatch(*space, "Nlev7Lg8k3A", {}, base + 132) == error(ORBIS_NET_ENETDOWN));
    CHECK(read.operator()<s32>(base + 132) == ORBIS_NET_ENETDOWN);
    for (unsigned generation = 0; generation < 3; ++generation) {
        GuestNetwork next(false);
        // Published control provider works before application Net init, stays
        // disconnected, and a repeated init preserves live registrations.
        CHECK(next.InitializeControl() == 0);
        CHECK(next.Dispatch(*space, "wIsKy+TfeLs", {base+0x4000,7,base+256}, base+132) == 0);
        CHECK(next.InitializeControl() == 0);
        CHECK(next.Dispatch(*space, "2oUqKR5odGc", {0}, base+132) == 0);
        CHECK(next.Dispatch(*space, "uBPlr0lbuiI", {base+256}, base+132) == 0);
        CHECK(read.operator()<s32>(base+256) == ORBIS_NET_CTL_STATE_DISCONNECTED);
        CHECK(next.Dispatch(*space, "Z4wwCFiBELQ", {}, base+132) == 0);
        CHECK(!next.BeginCallbacks(true));
        CHECK(next.Dispatch(*space, "gky0+oaNM4k", {}, base+132) == 0);
        CHECK(next.Dispatch(*space, "Nlev7Lg8k3A", {}, base + 132) == 0);
        CHECK(next.Dispatch(*space, "dgJBaeJnGpo", {base, 4096, 0}, base + 132) == 1);
    }
    {
        GuestNetwork stopping(false);
        CHECK(stopping.Dispatch(*space, "Nlev7Lg8k3A", {}, base+128) == 0);
        const auto id = stopping.Dispatch(*space, "SF47kB2MNTo", {0,0}, base+128);
        auto parked = std::async(std::launch::async, [&] {
            return stopping.Dispatch(*space, "drjIbDbA7UQ", {id, base+256, 1, u32(-1)}, base+144);
        });
        stopping.RequestStop();
        CHECK(parked.wait_for(1s) == std::future_status::ready);
        CHECK(parked.get() == error(ORBIS_NET_ECANCELED));
        CHECK(stopping.Dispatch(*space, "SF47kB2MNTo", {0,0}, base+128) == error(ORBIS_NET_ECANCELED));
    }
    std::printf("GUEST_NETWORK checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
