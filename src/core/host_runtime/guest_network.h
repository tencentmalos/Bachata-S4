// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/network/net_ctl_codes.h"
#include "core/libraries/network/net_error.h"
#include "core/libraries/network/netctl.h"
#include "core/libraries/network/net.h"

namespace Core::HostRuntime {
inline constexpr std::string_view NetNids[]{
    "SF47kB2MNTo", "ZVw46bsasAk", "Inp1lfL+Jdw", "drjIbDbA7UQ", "w21YgGGNtBk",
    "Nlev7Lg8k3A", "cTGkc6-TBlI", "dgJBaeJnGpo", "K7RlrTkI-mw", "HQOwnfMGipQ",
    "9T2pDF2Ryqg", "3CHi1K1wsCQ", "iWQWrwiSt8A", "pQGpHYopAIY", "tOrRi-v3AOM",
    "Rbvt+5Y2iEw", "9vA2aW+CHuA", "8Kcp5d-q1Uo", "Xn2TA2QhxHc", "C4UgDHHPvdw",
    "kJlYH5uMAWI", "J5i3hiLJMPk", "Nd91WaWmG2w", "AzqoBha7js4",
    "Q4qBuN-c0ZM", "45ggEzakPJQ", "OXXX4mUk3uk", "bErx49PgxyY", "kOj1HiAGE54",
    "9wO9XrMsNhc", "beRjXBn-z+o", "PIWqhn9oSxc", "2mKX2Spso7I", "TSM6whtekok",
    "gvD1greCu0A", "hoOAofhhRvE", "304ooNZxWDY", "xphrZusl78E", "zJGf8xjFnQE"};
inline constexpr std::string_view NetCtlNids[]{"gky0+oaNM4k", "Z4wwCFiBELQ", "uBPlr0lbuiI",
                                               "obuxdTiwkF8", "JO4yuTuMoKI", "0cBgduPRR+M",
                                               "UJ+Z7Q+4ck0", "Rqm2OnZMCz0", "iQw3iQPhvUQ"};
inline bool IsNetNid(std::string_view nid) {
    return std::ranges::find(NetNids, nid) != std::end(NetNids);
}
inline bool IsNetCtlNid(std::string_view nid) {
    return std::ranges::find(NetCtlNids, nid) != std::end(NetCtlNids);
}
// Control plane for the existing desktop offline policy. This is not an online
// socket/HTTP provider. An online-configured Session fails init explicitly.
// Guest ABI, resource ids and callbacks belong here, not to Foundation's host
// networking or desktop's global HandleTable/NetCtlInternal.
class GuestNetwork {
public:
    explicit GuestNetwork(bool online_requested) : online_requested(online_requested) {}
    ~GuestNetwork() { RequestStop(); }
    void RequestStop() {
        std::lock_guard lock(mutex);
        stopping = true;
        for (const auto& [id, epoll] : epolls) {
            std::lock_guard epoll_lock(epoll->mutex);
            epoll->stopped = true;
            epoll->changed.notify_all();
        }
    }
    struct Callback {
        u64 function{}, argument{}, revision{};
        size_t id{};
    };
    std::optional<std::vector<Callback>> BeginCallbacks() {
        std::lock_guard lock(mutex);
        if (!ctl_initialized || dispatching)
            return {};
        dispatching = true;
        std::vector<Callback> result;
        try {
            for (size_t i = 0; i < callbacks.size(); ++i)
                if (callbacks[i].function)
                    result.push_back(callbacks[i]);
        } catch (...) {
            dispatching = false;
            throw;
        }
        return result;
    }
    bool IsCurrent(const Callback& callback) {
        std::lock_guard lock(mutex);
        return ctl_initialized && callback.id < callbacks.size() &&
               callbacks[callback.id].revision == callback.revision &&
               callbacks[callback.id].function == callback.function;
    }
    void EndCallbacks() {
        std::lock_guard lock(mutex);
        dispatching = false;
    }
    u64 Dispatch(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                 const std::array<u64, 6>& a, u64 errno_address) {
        using namespace GuestCpu;
        using namespace Libraries::NetCtl;
        auto read = [&](u64 address, auto& value) {
            return bool(space.ReadData(GuestAddress{address},
                                       std::as_writable_bytes(std::span{&value, 1})));
        };
        auto put = [&](u64 address, const auto& value) {
            auto pin = space.AcquireDataSpan({GuestAddress{address}, sizeof(value)}, true);
            if (!pin)
                return false;
            std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
            return true;
        };
        auto failure = [&](int error) -> u64 {
            if (!put(errno_address, s32(error)))
                return u32(ORBIS_NET_ERROR_BASE | ORBIS_NET_EFAULT);
            return u32(ORBIS_NET_ERROR_BASE | error);
        };
        auto string = [&](u64 address, size_t limit) -> std::optional<std::string> {
            std::string result;
            for (u64 i = 0; i < limit; ++i) {
                char c{};
                if (!address || address > UINT64_MAX - i || !read(address + i, c))
                    return {};
                if (!c)
                    return result;
                result += c;
            }
            return {};
        };
        if (nid == "HQOwnfMGipQ")
            return errno_address;
        if (nid == "9T2pDF2Ryqg" || nid == "pQGpHYopAIY")
            return std::byteswap(u32(a[0]));
        if (nid == "3CHi1K1wsCQ" || nid == "tOrRi-v3AOM")
            return std::byteswap(a[0]);
        if (nid == "iWQWrwiSt8A" || nid == "Rbvt+5Y2iEw")
            return std::byteswap(u16(a[0]));
        if (nid == "8Kcp5d-q1Uo" || nid == "Xn2TA2QhxHc" || nid == "9vA2aW+CHuA") {
            const bool ntop = nid == "9vA2aW+CHuA";
            auto fail = [&](int e) -> u64 {
                (void)failure(e);
                return ntop ? 0 : UINT64_MAX;
            };
            const int family = u32(a[0]) == 2 ? AF_INET : u32(a[0]) == 28 ? AF_INET6 : -1;
            if (family < 0)
                return fail(ORBIS_NET_EAFNOSUPPORT);
            if (nid == "Xn2TA2QhxHc" && u32(a[3]))
                return fail(ORBIS_NET_EINVAL);
            const size_t bytes = family == AF_INET ? 4 : 16;
            std::array<u8, 16> address{};
            if (ntop) {
                if (!space.ReadData(GuestAddress{a[1]},
                                    std::as_writable_bytes(std::span{address}).first(bytes)))
                    return fail(ORBIS_NET_EFAULT);
                std::array<char, INET6_ADDRSTRLEN> text{};
                if (!::inet_ntop(family, address.data(), text.data(), text.size()))
                    return fail(ORBIS_NET_EINVAL);
                const size_t length = std::strlen(text.data()) + 1;
                if (u32(a[3]) < length)
                    return fail(ORBIS_NET_ENOSPC);
                auto pin = space.AcquireDataSpan({GuestAddress{a[2]}, length}, true);
                if (!pin)
                    return fail(ORBIS_NET_EFAULT);
                std::memcpy(pin.Value().WritableBytes().data(), text.data(), length);
                return a[2]; // guest address, never inet_ntop's native pointer
            }
            const auto text = string(a[1], INET6_ADDRSTRLEN);
            if (!text)
                return fail(ORBIS_NET_EFAULT);
            auto pin = space.AcquireDataSpan({GuestAddress{a[2]}, bytes}, true);
            if (!pin)
                return fail(ORBIS_NET_EFAULT);
            const int result = ::inet_pton(family, text->c_str(), address.data());
            if (result == 1)
                std::memcpy(pin.Value().WritableBytes().data(), address.data(), bytes);
            return u64(result); // invalid text is 0, not a fabricated address
        }
        // Offline epolls own no host sockets. Preserve timeout and cancellation
        // semantics without entering desktop epoll/logging on every guest poll.
        if (nid == "drjIbDbA7UQ") {
            std::shared_ptr<OfflineEpoll> epoll;
            std::unique_lock domain_lock(mutex);
            if (!initialized) return failure(ORBIS_NET_ENOTINIT);
            const auto it = epolls.find(s32(a[0]));
            if (it == epolls.end()) return failure(ORBIS_NET_EBADF);
            epoll = it->second;
            domain_lock.unlock();
            if (s32(a[2]) <= 0 || a[2] > 65536) return failure(ORBIS_NET_EINVAL);
            if (!space.ValidateRange({GuestAddress{a[1]}, a[2] * sizeof(Libraries::Net::OrbisNetEpollEvent)},
                                     GuestPermission::Write))
                return failure(ORBIS_NET_EFAULT);
            // No output buffer pin and no domain/VM lock survives the wait.
            std::unique_lock wait_lock(epoll->mutex);
            const auto epoch = epoll->abort_epoch;
            const auto interrupted = [&] {
                return epoll->destroyed || epoll->stopped || epoll->abort_epoch != epoch;
            };
            const s32 timeout_us = s32(a[3]);
            if (timeout_us < 0) epoll->changed.wait(wait_lock, interrupted);
            else if (timeout_us > 0)
                epoll->changed.wait_for(wait_lock, std::chrono::microseconds(timeout_us), interrupted);
            const int error = epoll->destroyed ? ORBIS_NET_EBADF :
                              interrupted() ? ORBIS_NET_ECANCELED : 0;
            wait_lock.unlock();
            return error ? failure(error) : 0; // timeout, no invented readiness
        }
        std::lock_guard lock(mutex);
        if (nid == "SF47kB2MNTo") {
            if (!initialized) return failure(ORBIS_NET_ENOTINIT);
            if (stopping) return failure(ORBIS_NET_ECANCELED);
            if (u32(a[1])) return failure(ORBIS_NET_EINVAL);
            auto name = a[0] ? string(a[0], 64) : std::optional<std::string>{"anon"};
            if (!name) return failure(ORBIS_NET_EFAULT);
            if (epolls.size() >= 64 || next_epoll == INT32_MAX) return failure(ORBIS_NET_ENOMEM);
            const s32 id = next_epoll++;
            epolls.emplace(id, std::make_shared<OfflineEpoll>());
            return id;
        }
        if (nid == "ZVw46bsasAk" || nid == "Inp1lfL+Jdw" || nid == "w21YgGGNtBk") {
            if (!initialized) return failure(ORBIS_NET_ENOTINIT);
            const auto it = epolls.find(s32(a[0]));
            if (it == epolls.end()) return failure(ORBIS_NET_EBADF);
            const auto epoll = it->second;
            if (nid == "Inp1lfL+Jdw") {
                {
                    std::lock_guard epoll_lock(epoll->mutex);
                    epoll->destroyed = true;
                    epoll->changed.notify_all();
                }
                epolls.erase(it);
                return 0;
            }
            if (nid == "w21YgGGNtBk") {
                std::lock_guard epoll_lock(epoll->mutex);
                ++epoll->abort_epoch;
                epoll->changed.notify_all();
                return 0;
            }
            // No successful socket/async resolver exists in this offline domain.
            // Never let a guest id alias a desktop file descriptor by coincidence.
            if (u32(a[1]) < 1 || u32(a[1]) > 3) return failure(ORBIS_NET_EINVAL);
            if (u32(a[1]) != 2 && !a[3]) return failure(ORBIS_NET_EINVAL);
            return failure(u32(a[1]) == 1 ? ORBIS_NET_EBADF : ORBIS_NET_ENOENT);
        }
        if (nid == "Q4qBuN-c0ZM") {
            if (!initialized) return failure(ORBIS_NET_ENOTINIT);
            if (a[0] && !string(a[0], 33)) return failure(ORBIS_NET_EFAULT);
            // This offline provider has no network transport. Return a real
            // socket-creation failure; never invent a successful descriptor.
            return failure(ORBIS_NET_ENETDOWN);
        }
        if (nid == "45ggEzakPJQ" || nid == "OXXX4mUk3uk" || nid == "bErx49PgxyY" ||
            nid == "kOj1HiAGE54" || nid == "9wO9XrMsNhc" || nid == "beRjXBn-z+o" ||
            nid == "PIWqhn9oSxc" || nid == "2mKX2Spso7I" || nid == "TSM6whtekok" ||
            nid == "gvD1greCu0A" || nid == "hoOAofhhRvE" || nid == "304ooNZxWDY" ||
            nid == "xphrZusl78E" || nid == "zJGf8xjFnQE") {
            // No socket could be created in this Session. As in sys_net, reject
            // the descriptor before accessing any caller payload/output pointer.
            return failure(initialized ? ORBIS_NET_EBADF : ORBIS_NET_ENOTINIT);
        }
        if (nid == "Nlev7Lg8k3A") {
            if (online_requested)
                return failure(ORBIS_NET_ENETDOWN);
            initialized = true;
            return 0;
        }
        if (nid == "cTGkc6-TBlI") {
            if (!initialized)
                return failure(ORBIS_NET_ENOTINIT);
            if (!pools.empty() || !resolvers.empty() || !epolls.empty() || ctl_initialized)
                return failure(ORBIS_NET_EBUSY);
            initialized = false;
            return 0;
        }
        if (nid == "dgJBaeJnGpo" || nid == "K7RlrTkI-mw") {
            if (!initialized)
                return failure(ORBIS_NET_ENOTINIT);
            if (nid == "K7RlrTkI-mw") {
                auto it = pools.find(s32(a[0]));
                if (it == pools.end())
                    return failure(ORBIS_NET_EBADF);
                if (std::ranges::any_of(resolvers, [&](const auto& entry) {
                        return entry.second.pool == it->first;
                    }))
                    return failure(ORBIS_NET_EBUSY);
                pool_bytes -= it->second.size();
                pools.erase(it);
                return 0;
            }
            const auto name = string(a[0], 64);
            if (!name)
                return failure(ORBIS_NET_EFAULT);
            if (name->empty() || s32(a[1]) <= 0 || u32(a[2]))
                return failure(ORBIS_NET_EINVAL);
            if (u32(a[1]) > 16 * 1024 * 1024 || pool_bytes + u32(a[1]) > 64 * 1024 * 1024 ||
                pools.size() == 32 || next_pool == INT32_MAX)
                return failure(ORBIS_NET_ENOMEM);
            try {
                pools.emplace(next_pool, std::vector<u8>(u32(a[1])));
            } catch (const std::bad_alloc&) {
                return failure(ORBIS_NET_ENOMEM);
            }
            pool_bytes += u32(a[1]);
            return next_pool++;
        }
        if (nid == "C4UgDHHPvdw") {
            if (!initialized)
                return failure(ORBIS_NET_ENOTINIT);
            if (a[0] && !string(a[0], 64))
                return failure(ORBIS_NET_EFAULT);
            if (u32(a[2]))
                return failure(ORBIS_NET_EINVAL);
            if (!pools.contains(s32(a[1])))
                return failure(ORBIS_NET_EBADF);
            if (resolvers.size() >= 32 || next_resolver == INT32_MAX)
                return failure(ORBIS_NET_ENOMEM);
            try {
                resolvers.emplace(next_resolver, Resolver{s32(a[1]), 0});
            } catch (const std::bad_alloc&) {
                return failure(ORBIS_NET_ENOMEM);
            }
            return next_resolver++;
        }
        if (nid == "kJlYH5uMAWI" || nid == "J5i3hiLJMPk" || nid == "Nd91WaWmG2w" ||
            nid == "AzqoBha7js4") {
            if (!initialized)
                return failure(ORBIS_NET_ENOTINIT);
            const auto it = resolvers.find(s32(a[0]));
            if (it == resolvers.end())
                return failure(ORBIS_NET_EBADF);
            if (nid == "kJlYH5uMAWI") {
                resolvers.erase(it);
                return 0;
            }
            if (nid == "J5i3hiLJMPk")
                return put(a[1], it->second.status) ? 0 : failure(ORBIS_NET_EFAULT);
            if (nid == "AzqoBha7js4") {
                if (u32(a[1]))
                    return failure(ORBIS_NET_EINVAL);
                it->second.status = u32(ORBIS_NET_ERROR_BASE | ORBIS_NET_ECANCELED);
                return 0;
            }
            const auto hostname = string(a[1], 254);
            if (!hostname)
                return failure(ORBIS_NET_EFAULT);
            if (hostname->empty() || s32(a[3]) < 0 || s32(a[4]) < 0 || (u32(a[5]) & ~1u))
                return failure(ORBIS_NET_EINVAL);
            if (!space.ValidateRange({GuestAddress{a[2]}, 4}, GuestPermission::Write))
                return failure(ORBIS_NET_EFAULT);
            // Same offline decision as desktop: no DNS request, fake IP, or
            // asynchronous completion. Preserve the failure for GetError.
            it->second.status = u32(ORBIS_NET_ERROR_RESOLVER_ENODNS);
            return failure(ORBIS_NET_RESOLVER_ENODNS);
        }
        if (nid == "gky0+oaNM4k") {
            if (!initialized || online_requested)
                return u32(ORBIS_NET_CTL_ERROR_NOT_AVAIL);
            ctl_initialized = true;
            return 0;
        }
        if (nid == "Z4wwCFiBELQ") {
            callbacks = {};
            ctl_initialized = false;
            return 0;
        }
        if (!ctl_initialized)
            return u32(ORBIS_NET_CTL_ERROR_NOT_AVAIL);
        if (nid == "uBPlr0lbuiI")
            return put(a[0], s32(ORBIS_NET_CTL_STATE_DISCONNECTED))
                       ? 0
                       : u32(ORBIS_NET_CTL_ERROR_INVALID_ADDR);
        if (nid == "obuxdTiwkF8") {
            if (!space.ValidateRange({GuestAddress{a[1]}, sizeof(OrbisNetCtlInfo)},
                                     GuestPermission::Write))
                return u32(ORBIS_NET_CTL_ERROR_INVALID_ADDR);
            return u32(ORBIS_NET_CTL_ERROR_NOT_CONNECTED);
        }
        if (nid == "JO4yuTuMoKI") {
            OrbisNetCtlNatInfo info{};
            if (!read(a[0], info) ||
                !space.ValidateRange({GuestAddress{a[0]}, sizeof(info)}, GuestPermission::Write))
                return u32(ORBIS_NET_CTL_ERROR_INVALID_ADDR);
            if (info.size != sizeof(info))
                return u32(ORBIS_NET_CTL_ERROR_INVALID_SIZE);
            return u32(ORBIS_NET_CTL_ERROR_NOT_CONNECTED); // no invented mapped address/STUN
        }
        if (nid == "0cBgduPRR+M") {
            if (u32(a[0]) < 1 || u32(a[0]) > 3)
                return u32(ORBIS_NET_CTL_ERROR_INVALID_ID);
            return put(a[1], s32(ORBIS_NET_CTL_ERROR_NETWORK_DISABLED))
                       ? 0
                       : u32(ORBIS_NET_CTL_ERROR_INVALID_ADDR);
        }
        if (nid == "UJ+Z7Q+4ck0") {
            if (!space.ValidateRange({GuestAddress{a[0]}, 1}, GuestPermission::Execute))
                return u32(ORBIS_NET_CTL_ERROR_INVALID_ADDR);
            auto pin = space.AcquireDataSpan({GuestAddress{a[2]}, 4}, true);
            if (!pin)
                return u32(ORBIS_NET_CTL_ERROR_INVALID_ADDR);
            for (size_t i = 0; i < callbacks.size(); ++i) {
                if (callbacks[i].function)
                    continue;
                callbacks[i] = {a[0], a[1], ++revision, i};
                const s32 id = i;
                std::memcpy(pin.Value().WritableBytes().data(), &id, 4);
                return 0;
            }
            return u32(ORBIS_NET_CTL_ERROR_CALLBACK_MAX);
        }
        if (nid == "Rqm2OnZMCz0") {
            const u32 id = a[0];
            if (id >= callbacks.size())
                return u32(ORBIS_NET_CTL_ERROR_INVALID_ID);
            if (!callbacks[id].function)
                return u32(ORBIS_NET_CTL_ERROR_ID_NOT_FOUND);
            callbacks[id] = {};
            return 0;
        }
        return u32(ORBIS_NET_CTL_ERROR_NOT_AVAIL);
    }

private:
    std::mutex mutex;
    bool online_requested{}, initialized{}, ctl_initialized{}, dispatching{}, stopping{};
    std::map<s32, std::vector<u8>> pools;
    struct Resolver {
        s32 pool;
        u32 status;
    };
    std::map<s32, Resolver> resolvers;
    struct OfflineEpoll {
        std::mutex mutex;
        std::condition_variable changed;
        u64 abort_epoch{};
        bool destroyed{}, stopped{};
    };
    std::map<s32, std::shared_ptr<OfflineEpoll>> epolls;
    s32 next_epoll{0x20000};
    s32 next_resolver{0x10000};
    size_t pool_bytes{};
    s32 next_pool{1};
    u64 revision{};
    std::array<Callback, 8> callbacks{};
};
} // namespace Core::HostRuntime
