// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cstdio>
#include <future>
#include <thread>
#include <unistd.h>
#include "core/file_sys/fs.h"
#include "core/host_runtime/guest_posix_network.h"
#include "core/host_runtime/guest_random.h"
#include "core/host_runtime/guest_storage.h"
#include "core/libraries/kernel/kernel.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace std::chrono_literals;
static std::atomic<unsigned> checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    for (u64 offset = 0; offset < 0x10000; offset += 0x4000)
        CHECK(space->Map({GuestAddress{base + offset}, 0x4000},
                         GuestPermission::Read | GuestPermission::Write));
    auto write = [&](u64 at, const auto& value) {
        CHECK(space->WriteData(GuestAddress{at}, std::as_bytes(std::span{&value, 1})));
    };
    auto read = [&]<class T>(u64 at) {
        T value{};
        CHECK(space->ReadData(GuestAddress{at}, std::as_writable_bytes(std::span{&value, 1})));
        return value;
    };
    auto ids = std::make_shared<GuestDescriptorIds>();
    auto root =
        std::filesystem::temp_directory_path() / ("shad-socket-tests-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    Core::FileSys::MntPoints mounts;
    GuestStorage storage(mounts, root, "CUSA99991", 1000, ids);
    const auto file = storage.Open("/dev/zero", 0, 0);
    CHECK(!file.error);
    GuestNetwork net(false, ids);
    auto ready = [&](s32 fd) {
        auto r = storage.PollReady(fd);
        return r.error ? -Libraries::Kernel::NativeToPosixErrno(r.error) : int(r.value);
    };
    auto call = [&](std::string_view nid, std::array<u64, 6> a = {}, std::stop_token stop = {}) {
        return s64(GuestPosixNetwork::Dispatch(
            net, *space, nid, a, base + 128,
            [&](int e) {
                write(base + 128, s32(e));
                return UINT64_MAX;
            },
            ready, stop));
    };
    auto sc = [&](std::string_view nid, std::array<u64, 6> a = {}) {
        return net.Dispatch(*space, nid, a, base + 132);
    };
    auto error = [&] { return read.operator()<s32>(base + 128); };
    auto wait_for_block = [&](size_t n = 1) {
        auto end = std::chrono::steady_clock::now() + 2s;
        while (net.Sockets().WaiterCount() < n && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        CHECK(net.Sockets().WaiterCount() >= n);
    };
    CHECK(sc("Nlev7Lg8k3A") == 0);
    CHECK(call("TU-d9PfIHPM", {9, 1, 0}) == -1 && error() == POSIX_EAFNOSUPPORT);
    CHECK(call("TU-d9PfIHPM", {2, 1, 17}) == -1 && error() == POSIX_EPROTONOSUPPORT);
    CHECK(call("TU-d9PfIHPM", {2, 9, 0}) == -1 && error() == POSIX_EPROTOTYPE);
    CHECK(call("pxnCmagrtao", {u64(file.value), 1}) == -1 && error() == POSIX_ENOTSOCK);
    CHECK(call("pxnCmagrtao", {999, 1}) == -1 && error() == POSIX_EBADF);
    CHECK(call("8nY19bKoiZk", {u64(file.value), 3}) == -1 && error() == POSIX_ENOSYS);
    const u64 addr = base + 256, len = base + 320, data = base + 512, out = base + 1024,
              set = base + 2048, tv = base + 2200;
    std::array<u8, 16> ipv4{0, 2, 0, 0, 127, 0, 0, 1};
    auto pair = [&](bool ipv6 = false) {
        const u64 listener = call("TU-d9PfIHPM", {ipv6 ? 28ull : 2ull, 1, 0});
        CHECK(listener != u64(file.value) && s64(listener) >= 0);
        if (ipv6) {
            std::array<u8, 28> ip6{};
            ip6[1] = 28;
            ip6[23] = 1;
            write(addr, ip6);
        } else
            write(addr, ipv4);
        CHECK(sc("bErx49PgxyY", {listener, addr, ipv6 ? 28ull : 16ull}) == 0);
        CHECK(call("pxnCmagrtao", {listener, 2}) == 0);
        write(len, u32{28});
        CHECK(sc("hoOAofhhRvE", {listener, addr, len}) == 0);
        CHECK(read.operator()<u32>(len) == (ipv6 ? 28 : 16));
        auto address = read.operator()<std::array<u8, 28>>(addr);
        CHECK(address[0] == (ipv6 ? 28 : 16));
        CHECK(address[2] || address[3]);
        const u64 client = sc("Q4qBuN-c0ZM", {0, ipv6 ? 28ull : 2ull, 1, 0});
        CHECK(s64(client) >= 0 && client != listener);
        CHECK(call("8nY19bKoiZk", {client, 4, 4}) == 0);
        auto connected = call("XVL8So3QJUk", {client, addr, ipv6 ? 28ull : 16ull});
        CHECK(connected == 0 || (connected == -1 && error() == POSIX_EINPROGRESS));
        CHECK(call("8nY19bKoiZk", {client, 4, 0}) == 0);
        write(len, u32{2});
        write(out, std::array<u8, 32>{});
        const u64 server = call("3e+4Iv7IJ8U", {listener, out, len});
        CHECK(s64(server) >= 0);
        CHECK(read.operator()<u32>(len) == (ipv6 ? 28 : 16));
        auto truncated = read.operator()<std::array<u8, 32>>(out);
        CHECK(truncated[0] == (ipv6 ? 28 : 16) && truncated[1] == (ipv6 ? 28 : 2) &&
              truncated[2] == 0);
        write(data, s32{1});
        CHECK(sc("2mKX2Spso7I", {client, 6, 1, data, 4}) == 0);
        write(len, u32{4});
        CHECK(call("6O8EwYOgH9Y", {client, 6, 1, out, len}) == 0 && read.operator()<s32>(out) == 1);
        CHECK(sc("45ggEzakPJQ", {listener}) == 0);
        return std::array<u64, 2>{client, server};
    };
    std::printf("stage TCP pair\n");
    auto [client, server] = pair();
    write(data, std::array<char, 8>{'h', 'e', 'l', 'l', 'o'});
    write(base + 132, s32{77});
    CHECK(call("8nY19bKoiZk", {server, 4, 4}) == 0);
    CHECK(call("8nY19bKoiZk", {server, 3}) == 6);
    CHECK(call("Ez8xjo9UF4E", {server, out, 8, 0}) == -1 && error() == POSIX_EAGAIN);
    CHECK(read.operator()<s32>(base + 132) == 77); // POSIX errno is separate.
    CHECK(sc("9wO9XrMsNhc", {server, out, 8, 0}) == u32(ORBIS_NET_ERROR_BASE | POSIX_EAGAIN));
    CHECK(read.operator()<s32>(base + 132) == POSIX_EAGAIN);
    CHECK(call("8nY19bKoiZk", {server, 4, 0}) == 0);
    CHECK(call("fZOeZIOEmLw", {client, data, 5, 0}) == 5);
    CHECK(sc("9wO9XrMsNhc", {server, out, 8, 2}) == 5); // PEEK
    CHECK(call("Ez8xjo9UF4E", {server, out, 8, 0}) == 5);
    CHECK(read.operator()<std::array<char, 5>>(out) ==
          (std::array<char, 5>{'h', 'e', 'l', 'l', 'o'}));
    std::printf("stage Both set bits count\n");
    // Both set bits count; regular files cause immediate return while sockets poll.
    struct Timeval {
        s64 sec, usec;
    };
    write(tv, Timeval{0, 0});
    write(set, u64{1} << client);
    write(set + 128, u64{1} << client);
    CHECK(call("T8fER+tIGgk", {client + 1, set, set + 128, 0, tv}) == 1);
    CHECK(read.operator()<u64>(set) == 0 && read.operator()<u64>(set + 128) == (u64{1} << client));
    write(set, u64{1} << server);
    auto pending = std::async(std::launch::async, [&] {
        return call("T8fER+tIGgk", {server + 1, set, 0, 0, 0});
    });
    wait_for_block();
    CHECK(sc("beRjXBn-z+o", {client, data, 5}) == 5);
    CHECK(pending.wait_for(2s) == std::future_status::ready && pending.get() == 1);
    CHECK(call("Ez8xjo9UF4E", {server, out, 8, 0}) == 5);
    std::printf("stage WAITALL accumulates\n");
    // WAITALL accumulates multiple writes; other guest work can run while waiting.
    pending = std::async(std::launch::async, [&] {
        return call("Ez8xjo9UF4E", {server, out, 10, 0x40});
    });
    wait_for_block();
    CHECK(call("fZOeZIOEmLw", {client, data, 5}) == 5);
    CHECK(pending.wait_for(5ms) == std::future_status::timeout);
    CHECK(call("fZOeZIOEmLw", {client, data, 5}) == 5);
    CHECK(pending.wait_for(2s) == std::future_status::ready && pending.get() == 10);
    std::printf("stage epoll preserves\n");
    // epoll preserves opaque u64 user data and maps readiness back to guest ids.
    const auto epoll = sc("SF47kB2MNTo");
    Libraries::Net::OrbisNetEpollEvent event{1, 0, 0, {.data_u64 = 0xabcdef0123456789}};
    write(data, event);
    CHECK(sc("ZVw46bsasAk", {epoll, 1, server, data}) == 0);
    CHECK(sc("ZVw46bsasAk", {epoll, 1, server, data}) == u32(ORBIS_NET_ERROR_BASE | POSIX_EEXIST));
    auto epwait = std::async(std::launch::async, [&] {
        return sc("drjIbDbA7UQ", {epoll, out, 1, u32(-1)});
    });
    wait_for_block();
    CHECK(call("fZOeZIOEmLw", {client, data, 4}) == 4);
    CHECK(epwait.wait_for(2s) == std::future_status::ready && epwait.get() == 1);
    auto received = read.operator()<Libraries::Net::OrbisNetEpollEvent>(out);
    CHECK(received.ident == server && received.events == 1 &&
          received.data.data_u64 == event.data.data_u64);
    CHECK(call("Ez8xjo9UF4E", {server, out, 4}) == 4);
    epwait = std::async(std::launch::async, [&] {
        return sc("drjIbDbA7UQ", {epoll, out, 1, u32(-1)});
    });
    wait_for_block();
    CHECK(sc("w21YgGGNtBk", {epoll}) == 0);
    CHECK(epwait.wait_for(2s) == std::future_status::ready &&
          epwait.get() == u32(ORBIS_NET_ERROR_BASE | POSIX_ECANCELED));
    CHECK(sc("ZVw46bsasAk", {epoll, 3, server, 0}) == 0);
    CHECK(sc("Inp1lfL+Jdw", {epoll}) == 0);
    std::printf("stage Short receive\n");
    // Short receive never overwrites the unused tail; stale output mappings fail.
    write(out, std::array<u8, 32>{0xa5});
    CHECK(call("fZOeZIOEmLw", {client, data, 1}) == 1);
    CHECK(call("Ez8xjo9UF4E", {server, 1, 1}) == -1 && error() == POSIX_EFAULT);
    CHECK(call("Ez8xjo9UF4E", {server, out, 1}) == 1); // invalid output did not consume
    pending = std::async(std::launch::async, [&] {
        return call("Ez8xjo9UF4E", {server, base + 0x8000, 8});
    });
    wait_for_block();
    CHECK(space->Unmap({GuestAddress{base + 0x8000}, 0x4000}));
    CHECK(space->Map({GuestAddress{base + 0x8000}, 0x4000},
                     GuestPermission::Read | GuestPermission::Write));
    write(base + 0x8000, u64{0xfeed});
    CHECK(call("fZOeZIOEmLw", {client, data, 1}) == 1);
    CHECK(pending.wait_for(2s) == std::future_status::ready && pending.get() == -1 &&
          error() == POSIX_EFAULT);
    CHECK(read.operator()<u64>(base + 0x8000) == 0xfeed);
    std::printf("stage Orbis integer\n");
    // Orbis integer-us timeout and BSD timeval option, not Android int timeouts.
    write(data, s32{3000});
    CHECK(sc("2mKX2Spso7I", {server, 0xffff, 0x1106, data, 4}) == 0);
    auto before = std::chrono::steady_clock::now();
    CHECK(call("Ez8xjo9UF4E", {server, out, 1}) == -1 && error() == POSIX_EAGAIN);
    CHECK(std::chrono::steady_clock::now() - before >= 3ms);
    write(len, u32{16});
    CHECK(call("6O8EwYOgH9Y", {server, 0xffff, 0x1006, out, len}) == 0);
    CHECK(read.operator()<Timeval>(out).usec == 3000);
    write(data, s32{0});
    CHECK(sc("2mKX2Spso7I", {server, 0xffff, 0x1106, data, 4}) == 0);
    std::printf("stage Individual cancellation\n");
    // Individual cancellation, abort, close and shutdown are distinct.
    std::stop_source cancel;
    pending = std::async(std::launch::async, [&] {
        return call("Ez8xjo9UF4E", {server, out, 1}, cancel.get_token());
    });
    wait_for_block();
    cancel.request_stop();
    CHECK(pending.wait_for(2s) == std::future_status::ready && pending.get() == -1 &&
          error() == POSIX_EINTR);
    pending = std::async(std::launch::async, [&] { return call("Ez8xjo9UF4E", {server, out, 1}); });
    wait_for_block();
    CHECK(sc("zJGf8xjFnQE", {server, 0}) == 0);
    CHECK(pending.wait_for(2s) == std::future_status::ready && pending.get() == -1 &&
          error() == POSIX_ECANCELED);
    CHECK(call("TUuiYS2kE8s", {client, 1}) == 0);
    CHECK(call("Ez8xjo9UF4E", {server, out, 1}) == 0); // EOF
    CHECK(call("socket.close", {client}) == 0);
    CHECK(sc("45ggEzakPJQ", {server}) == 0);
    CHECK(call("Ez8xjo9UF4E", {server, out, 1}) == -1 && error() == POSIX_EBADF);
    std::printf("stage IPv6 conversion\n");
    // IPv6 conversion, getsockname and accepted addresses.
    auto v6 = pair(true);
    CHECK(call("fZOeZIOEmLw", {v6[0], data, 1}) == 1);
    CHECK(call("Ez8xjo9UF4E", {v6[1], out, 1}) == 1);
    for (auto fd : v6)
        CHECK(call("socket.close", {fd}) == 0);
    std::printf("stage Datagram boundaries\n");
    // An idle epoll registration must not keep a closed native socket alive.
    auto watched_pair = pair();
    const auto idle_epoll = sc("SF47kB2MNTo");
    write(data, event);
    CHECK(sc("ZVw46bsasAk", {idle_epoll, 1, watched_pair[0], data}) == 0);
    CHECK(call("socket.close", {watched_pair[0]}) == 0);
    write(data, s32{100000});
    CHECK(sc("2mKX2Spso7I", {watched_pair[1], 0xffff, 0x1106, data, 4}) == 0);
    CHECK(call("Ez8xjo9UF4E", {watched_pair[1], out, 1}) == 0);
    CHECK(sc("drjIbDbA7UQ", {idle_epoll, out, 1, 0}) == 0);
    CHECK(sc("Inp1lfL+Jdw", {idle_epoll}) == 0);
    CHECK(call("socket.close", {watched_pair[1]}) == 0);
    // Datagram boundaries, scatter/gather and truncation flag.
    const u64 udp = call("TU-d9PfIHPM", {2, 2, 17});
    write(addr, ipv4);
    CHECK(call("KuOmgKoqCdY", {udp, addr, 16}) == 0);
    write(len, u32{16});
    CHECK(call("RenI1lL1WFk", {udp, addr, len}) == 0);
    const u64 sender = call("TU-d9PfIHPM", {2, 2, 0});
    CHECK(call("oBr313PppNE", {sender, data, 8, 0, addr, 16}) == 8);
    struct Iov {
        u64 at, size;
    };
    struct Message {
        u64 name;
        u32 name_size, pad;
        u64 iov;
        s32 count;
        u32 pad2;
        u64 control;
        u32 control_size;
        s32 flags;
    };
    write(base + 3000, Iov{out, 3});
    write(base + 3100, Message{0, 0, 0, base + 3000, 1, 0, 0, 0, 0});
    CHECK(call("hI7oVeOluPM", {udp, base + 3100, 0}) == 3);
    CHECK(read.operator()<Message>(base + 3100).flags == 0x10);
    write(base + 3000, std::array<Iov, 2>{{{data, 2}, {data + 2, 3}}});
    write(base + 3100, Message{addr, 16, 0, base + 3000, 2, 0, 0, 0, 0});
    CHECK(sc("2eKbgcboJso", {sender, base + 3100, 0}) == 5);
    write(len, u32{16});
    CHECK(call("lUk6wrGXyMw", {udp, out, 8, 0, base + 3500, len}) == 5);
    CHECK(read.operator()<std::array<u8, 16>>(base + 3500)[1] == 2);
    auto external = ipv4;
    external[4] = 192;
    external[5] = 0;
    external[6] = 2;
    external[7] = 1;
    write(addr, external);
    CHECK(call("XVL8So3QJUk", {sender, addr, 16}) == -1 && error() == POSIX_ENETDOWN);
    CHECK(call("oBr313PppNE", {sender, data, 1, 0, addr, 16}) == -1 && error() == POSIX_ENETDOWN);
    for (auto fd : {udp, sender})
        CHECK(call("socket.close", {fd}) == 0);
    std::printf("stage Select cancellation\n");
    // Orbis fd_set word boundaries; file readiness uses the same adapter callback.
    write(tv, Timeval{0, 0});
    for (int fd : {63, 64, 1023}) {
        std::array<u64, 16> bits{};
        bits[fd / 64] = u64{1} << (fd % 64);
        write(set, bits);
        write(set + 128, bits);
        auto rc = net.Sockets().Dispatch(
            *space, "T8fER+tIGgk", {u64(fd + 1), set, set + 128, 0, tv},
            [&](int e) {
                write(base + 128, s32(e));
                return UINT64_MAX;
            },
            [fd](s32 id) { return id == fd ? 3 : -POSIX_EBADF; });
        CHECK(rc == 2);
        CHECK(read.operator()<decltype(bits)>(set) == bits);
        CHECK(read.operator()<decltype(bits)>(set + 128) == bits);
    }
    // Select cancellation with an empty set is not fabricated readiness.
    std::stop_source select_cancel;
    pending = std::async(std::launch::async,
                         [&] { return call("T8fER+tIGgk", {0}, select_cancel.get_token()); });
    wait_for_block();
    select_cancel.request_stop();
    CHECK(pending.wait_for(2s) == std::future_status::ready && pending.get() == -1 &&
          error() == POSIX_EINTR);
    CHECK(call("T8fER+tIGgk", {1025}) == -1 && error() == POSIX_EINVAL);
    write(tv, Timeval{-1, 0});
    CHECK(call("T8fER+tIGgk", {0, 0, 0, 0, tv}) == -1 && error() == POSIX_EINVAL);
    auto last = pair();
    const u64 filler = call("TU-d9PfIHPM", {2, 2, 0});
    pending = std::async(std::launch::async, [&] {
        return call("Ez8xjo9UF4E", {last[1], out, 1});
    });
    wait_for_block();
    CHECK(call("socket.close", {last[1]}) == 0);
    const u64 reused = call("TU-d9PfIHPM", {2, 2, 0});
    CHECK(reused == last[1]);
    CHECK(pending.wait_for(2s) == std::future_status::ready && pending.get() == -1 &&
          error() == POSIX_EBADF);
    CHECK(call("socket.close", {reused}) == 0);
    CHECK(call("socket.close", {last[0]}) == 0);
    CHECK(call("socket.close", {filler}) == 0);
    pending = std::async(std::launch::async, [&] { return call("T8fER+tIGgk", {0}); });
    wait_for_block();
    net.RequestStop();
    CHECK(pending.wait_for(2s) == std::future_status::ready && pending.get() == -1 &&
          error() == POSIX_EINTR);
    CHECK(net.Sockets().Empty());
    CHECK(storage.Close(file.value).error == 0);
    CHECK(GuestRandom(*space, data, 64) == 0);
    CHECK(GuestRandom(*space, data, 65) == u32(SCE_RANDOM_ERROR_INVALID));
    std::filesystem::remove_all(root);
    std::printf("socket/posix/random: %u checks, %u failures\n", checks.load(), failures.load());
    return failures ? 1 : 0;
}
