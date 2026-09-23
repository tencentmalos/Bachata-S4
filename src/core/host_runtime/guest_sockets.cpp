// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common/logging/log.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/network/net.h"
#include "guest_sockets.h"
namespace Core::HostRuntime {
namespace {
using namespace GuestCpu;
using namespace std::chrono;
using Space = GuestAddressSpace;
using Args = std::array<u64, 6>;
constexpr size_t MaxTransfer = 64 * 1024 * 1024;
int Error() {
    return Libraries::Kernel::NativeToPosixErrno(errno);
}
enum class Op {
    Unknown,
    Socket,
    Bind,
    Listen,
    Connect,
    Accept,
    Close,
    Shutdown,
    Name,
    Peer,
    Set,
    Get,
    Send,
    Receive,
    SendTo,
    ReceiveFrom,
    SendMsg,
    ReceiveMsg,
    Fcntl,
    Select,
    EpollCreate,
    EpollControl,
    EpollWait,
    EpollDestroy,
    EpollAbort,
    Abort,
    Info
};
struct Entry {
    std::string_view posix, net;
    Op op;
};
constexpr Entry Entries[]{{"TU-d9PfIHPM", "Q4qBuN-c0ZM", Op::Socket},
                          {"KuOmgKoqCdY", "bErx49PgxyY", Op::Bind},
                          {"pxnCmagrtao", "kOj1HiAGE54", Op::Listen},
                          {"XVL8So3QJUk", "OXXX4mUk3uk", Op::Connect},
                          {"3e+4Iv7IJ8U", "PIWqhn9oSxc", Op::Accept},
                          {"socket.close", "45ggEzakPJQ", Op::Close},
                          {"TUuiYS2kE8s", "TSM6whtekok", Op::Shutdown},
                          {"RenI1lL1WFk", "hoOAofhhRvE", Op::Name},
                          {"TXFFFiNldU8", "TCkRD0DWNLg", Op::Peer},
                          {"fFxGkxF2bVo", "2mKX2Spso7I", Op::Set},
                          {"6O8EwYOgH9Y", "xphrZusl78E", Op::Get},
                          {"fZOeZIOEmLw", "beRjXBn-z+o", Op::Send},
                          {"Ez8xjo9UF4E", "9wO9XrMsNhc", Op::Receive},
                          {"oBr313PppNE", "gvD1greCu0A", Op::SendTo},
                          {"lUk6wrGXyMw", "304ooNZxWDY", Op::ReceiveFrom},
                          {"aNeavPDNKzA", "2eKbgcboJso", Op::SendMsg},
                          {"hI7oVeOluPM", "wvuUDv0jrMI", Op::ReceiveMsg},
                          {"8nY19bKoiZk", "", Op::Fcntl},
                          {"T8fER+tIGgk", "", Op::Select},
                          {"", "SF47kB2MNTo", Op::EpollCreate},
                          {"", "ZVw46bsasAk", Op::EpollControl},
                          {"", "drjIbDbA7UQ", Op::EpollWait},
                          {"", "Inp1lfL+Jdw", Op::EpollDestroy},
                          {"", "w21YgGGNtBk", Op::EpollAbort},
                          {"", "zJGf8xjFnQE", Op::Abort},
                          {"", "hLuXdjHnhiI", Op::Info}};
Op Find(std::string_view nid) {
    for (const auto& e : Entries)
        if ((!e.posix.empty() && e.posix == nid) || (!e.net.empty() && e.net == nid))
            return e.op;
    return Op::Unknown;
}
template <class T>
bool Read(Space& space, u64 at, T& out) {
    return bool(space.ReadData(GuestAddress{at}, std::as_writable_bytes(std::span{&out, 1})));
}
// Capture identities before the operation, release pins before native I/O, then
// re-admit the entire output batch before writing anything (including length fields).
struct Outputs {
    Space& space;
    std::vector<Space::DataRequest> requests;
    std::vector<std::vector<Space::MappingIdentity>> identities;
    std::vector<std::vector<u8>> bytes;
    bool Add(u64 at, u64 size) {
        Space::DataRequest request{{GuestAddress{at}, size}, GuestPermission::Write};
        std::vector<Space::MappingIdentity> identity;
        if (size) {
            auto pin = space.AcquireDataBatch(std::span{&request, 1});
            if (!pin)
                return false;
            for (u64 cursor = at; cursor < at + size;) {
                auto map = space.Query(GuestAddress{cursor});
                if (!map)
                    return false;
                const auto end = std::min(at + size, map.Value().range.End());
                identity.push_back({cursor, end, map.Value().mapping_generation});
                cursor = end;
            }
        }
        identities.push_back(std::move(identity));
        request.identities = identities.back();
        requests.push_back(std::move(request));
        bytes.emplace_back(size);
        return true;
    }
    bool Publish() {
        std::vector<Space::DataRequest> active;
        for (const auto& r : requests)
            if (r.range.size)
                active.push_back(r);
        auto pins = space.AcquireDataBatch(active);
        if (!pins)
            return false;
        size_t j{};
        for (size_t i = 0; i < requests.size(); ++i)
            if (requests[i].range.size)
                std::memcpy(pins.Value()[j++].WritableBytes().data(), bytes[i].data(),
                            requests[i].range.size);
        return true;
    }
    void Trim(size_t i, size_t n) {
        auto& request = requests[i];
        request.range.size = n;
        auto& expected = identities[i];
        const auto end = request.range.End();
        std::erase_if(expected, [&](const auto& id) { return id.begin >= end; });
        for (auto& id : expected)
            id.end = std::min(id.end, end);
        request.identities = expected;
    }
    template <class T>
    void Put(size_t i, T value) {
        std::memcpy(bytes[i].data(), &value, sizeof(value));
    }
};
struct Address {
    sockaddr_storage native{};
    socklen_t size{};
    bool Loopback() const {
        if (native.ss_family == AF_INET)
            return (ntohl(reinterpret_cast<const sockaddr_in*>(&native)->sin_addr.s_addr) >> 24) ==
                   127;
        if (native.ss_family == AF_INET6) {
            const auto& ip = reinterpret_cast<const sockaddr_in6*>(&native)->sin6_addr;
            if (IN6_IS_ADDR_LOOPBACK(&ip))
                return true;
            return IN6_IS_ADDR_V4MAPPED(&ip) && ip.s6_addr[12] == 127;
        }
        return false;
    }
    bool Any() const {
        if (native.ss_family == AF_INET)
            return !reinterpret_cast<const sockaddr_in*>(&native)->sin_addr.s_addr;
        return native.ss_family == AF_INET6 &&
               IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<const sockaddr_in6*>(&native)->sin6_addr);
    }
    int Load(Space& space, u64 at, u32 length) {
        std::array<u8, 28> raw{};
        if (length < 2)
            return POSIX_EINVAL;
        if (!space.ReadData(GuestAddress{at}, std::as_writable_bytes(std::span{raw}).first(2)))
            return POSIX_EFAULT;
        size = raw[1] == 2 ? 16 : raw[1] == 28 ? 28 : 0;
        if (!size)
            return POSIX_EAFNOSUPPORT;
        if (length < size)
            return POSIX_EINVAL;
        if (!space.ReadData(GuestAddress{at}, std::as_writable_bytes(std::span{raw}).first(size)))
            return POSIX_EFAULT;
        if (raw[1] == 2) {
            auto& ip = reinterpret_cast<sockaddr_in&>(native);
            ip.sin_family = AF_INET;
            std::memcpy(&ip.sin_port, raw.data() + 2, 2);
            std::memcpy(&ip.sin_addr, raw.data() + 4, 4);
            size = sizeof(ip);
        } else {
            auto& ip = reinterpret_cast<sockaddr_in6&>(native);
            ip.sin6_family = AF_INET6;
            std::memcpy(&ip.sin6_port, raw.data() + 2, 2);
            std::memcpy(&ip.sin6_flowinfo, raw.data() + 4, 4);
            std::memcpy(&ip.sin6_addr, raw.data() + 8, 16);
            std::memcpy(&ip.sin6_scope_id, raw.data() + 24, 4);
            size = sizeof(ip);
        }
        return 0;
    }
    std::vector<u8> Guest() const {
        std::vector<u8> out(native.ss_family == AF_INET6 ? 28 : 16);
        out[0] = u8(out.size());
        out[1] = native.ss_family == AF_INET6 ? 28 : 2;
        if (native.ss_family == AF_INET6) {
            const auto& ip = reinterpret_cast<const sockaddr_in6&>(native);
            std::memcpy(out.data() + 2, &ip.sin6_port, 2);
            std::memcpy(out.data() + 4, &ip.sin6_flowinfo, 4);
            std::memcpy(out.data() + 8, &ip.sin6_addr, 16);
            std::memcpy(out.data() + 24, &ip.sin6_scope_id, 4);
        } else {
            const auto& ip = reinterpret_cast<const sockaddr_in&>(native);
            std::memcpy(out.data() + 2, &ip.sin_port, 2);
            std::memcpy(out.data() + 4, &ip.sin_addr, 4);
        }
        return out;
    }
};
struct Wake {
    int fd[2]{-1, -1};
    Wake() {
        if (::pipe(fd))
            return;
        for (int f : fd) {
            ::fcntl(f, F_SETFL, O_NONBLOCK);
            ::fcntl(f, F_SETFD, FD_CLOEXEC);
        }
    }
    ~Wake() {
        for (int f : fd)
            if (f >= 0)
                ::close(f);
    }
    void Signal() {
        char byte{};
        if (fd[1] >= 0)
            (void)::write(fd[1], &byte, 1);
    }
};
struct Timeval {
    s64 seconds{}, micros{};
};
int Duration(Timeval t, std::optional<nanoseconds>& duration) {
    if (t.seconds < 0 || t.micros < 0 || t.micros >= 1000000 ||
        t.seconds > (INT64_MAX - t.micros * 1000) / 1000000000)
        return POSIX_EINVAL;
    duration = nanoseconds(t.seconds * 1000000000 + t.micros * 1000);
    return 0;
}
} // namespace
struct GuestSockets::Impl {
    struct Socket {
        int fd{-1}, type{}, family{};
        std::string name;
        std::mutex mutex;
        bool nonblock{}, cloexec{};
        std::atomic<bool> closed{};
        std::stop_source closing, abort;
        std::optional<nanoseconds> receive_timeout, send_timeout, accept_timeout, connect_timeout;
        ~Socket() {
            if (fd >= 0)
                ::close(fd);
        }
    };
    using Lease = std::shared_ptr<Socket>;
    struct Epoll {
        std::mutex mutex;
        struct Watch {
            Lease socket;
            u32 events{};
            u64 data{};
        };
        std::map<int, Watch> watches;
        std::stop_source changed, abort, destroy;
    };
    std::shared_ptr<GuestDescriptorIds> ids;
    bool online{};
    std::stop_source stopping;
    mutable std::mutex mutex;
    std::map<int, Lease> sockets;
    std::map<int, std::shared_ptr<Epoll>> epolls;
    int next_epoll{0x20000};
    std::atomic<size_t> waiters{};
    std::atomic<u32> diagnostics{};
    void Trace(std::string_view nid, int descriptor, s64 result) {
        if (diagnostics.fetch_add(1) < 64)
            LOG_INFO(Lib_Net, "Session socket nid={} guest_fd={} result={}", nid, descriptor,
                     result);
    }
    Lease Get(int id) {
        std::lock_guard lock(mutex);
        auto it = sockets.find(id);
        return it == sockets.end() ? nullptr : it->second;
    }
    ~Impl() {
        for (const auto& [id, socket] : sockets)
            ids->Release(id);
    }
    int Add(Lease socket) {
        std::lock_guard lock(mutex);
        if (stopping.stop_requested())
            return -POSIX_EINTR;
        if (sockets.size() >= 1024)
            return -POSIX_EMFILE;
        const int id = ids->Allocate();
        if (id < 0)
            return -POSIX_EMFILE;
        try {
            sockets.emplace(id, std::move(socket));
        } catch (...) {
            ids->Release(id);
            throw;
        }
        return id;
    }
    int Create(int family, int type, int protocol, std::string name = {}) {
        if (family != 2 && family != 28)
            return -POSIX_EAFNOSUPPORT;
        if (type != 1 && type != 2)
            return -POSIX_EPROTOTYPE;
        if (protocol && protocol != (type == 1 ? 6 : 17))
            return -POSIX_EPROTONOSUPPORT;
        auto s = std::make_shared<Socket>();
        s->type = type;
        s->family = family;
        s->name = std::move(name);
        s->fd = ::socket(family == 2 ? AF_INET : AF_INET6, type == 1 ? SOCK_STREAM : SOCK_DGRAM,
                         protocol);
        if (s->fd < 0)
            return -Error();
        if (::fcntl(s->fd, F_SETFL, O_NONBLOCK) < 0 || ::fcntl(s->fd, F_SETFD, FD_CLOEXEC) < 0)
            return -Error();
#ifdef SO_NOSIGPIPE
        int on = 1;
        if (::setsockopt(s->fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)))
            return -Error();
#endif
        return Add(std::move(s));
    }
    int Close(int id) {
        Lease socket;
        {
            std::lock_guard lock(mutex);
            auto it = sockets.find(id);
            if (it == sockets.end())
                return POSIX_EBADF;
            socket = std::move(it->second);
            sockets.erase(it);
            ids->Release(id);
            socket->closed = true;
            // Registrations are not active I/O leases. Remove them now so an
            // idle epoll cannot keep a logically closed connection alive.
            for (auto& [epid, epoll] : epolls) {
                std::lock_guard watch_lock(epoll->mutex);
                auto watch = epoll->watches.find(id);
                if (watch != epoll->watches.end() && watch->second.socket == socket)
                    epoll->watches.erase(watch);
            }
        }
        socket->closing.request_stop();
        // The last admitted lease closes native fd. A blocked caller wakes and
        // cannot issue a syscall against a reused native descriptor.
        return 0;
    }
    // Returns readiness count, 0 on deadline, or negative Orbis errno. Each wait
    // owns a wake pipe; cancellation broadcasts to every waiter, without polling.
    int Poll(std::vector<pollfd>& fds, std::optional<nanoseconds> duration, std::stop_token owner,
             std::vector<std::stop_token> tokens = {}) {
        Wake wake;
        if (wake.fd[0] < 0)
            return -POSIX_EMFILE;
        auto signal = [&] { wake.Signal(); };
        std::stop_callback session(stopping.get_token(), signal), caller(owner, signal);
        std::vector<std::unique_ptr<std::stop_callback<std::function<void()>>>> callbacks;
        for (auto token : tokens)
            callbacks.push_back(
                std::make_unique<std::stop_callback<std::function<void()>>>(token, signal));
        auto cancelled = [&] {
            return stopping.stop_requested() || owner.stop_requested() ||
                   std::ranges::any_of(tokens, [](auto t) { return t.stop_requested(); });
        };
        const auto begin = steady_clock::now();
        const size_t n = fds.size();
        fds.push_back({wake.fd[0], POLLIN, 0});
        struct Count {
            std::atomic<size_t>& value;
            Count(std::atomic<size_t>& v) : value(v) {
                ++value;
            }
            ~Count() {
                --value;
            }
        } count(waiters);
        int result;
        for (;;) {
            if (cancelled()) {
                result = -POSIX_EINTR;
                break;
            }
            int ms = -1;
            if (duration) {
                const auto left =
                    std::max(nanoseconds::zero(),
                             *duration - duration_cast<nanoseconds>(steady_clock::now() - begin));
                ms = int(
                    std::min<s64>(INT_MAX, duration_cast<milliseconds>(left).count() +
                                               (left % milliseconds(1) != nanoseconds::zero())));
            }
            result = ::poll(fds.data(), fds.size(), ms);
            if (cancelled()) {
                result = -POSIX_EINTR;
                break;
            }
            if (result < 0) {
                if (errno == EINTR)
                    continue;
                result = -Error();
                break;
            }
            if (result || !duration || steady_clock::now() - begin >= *duration)
                break;
        }
        fds.resize(n);
        return result;
    }
    int Wait(const Lease& s, short events, std::optional<nanoseconds> duration,
             std::stop_token owner, std::stop_token abort) {
        std::vector<pollfd> fds{{s->fd, events, 0}};
        int rc = Poll(fds, duration, owner, {s->closing.get_token(), abort});
        if (s->closed)
            return POSIX_EBADF;
        if (abort.stop_requested())
            return POSIX_ECANCELED;
        return rc < 0 ? -rc : rc == 0 ? POSIX_EAGAIN : 0;
    }
    std::stop_token AbortToken(const Lease& s) {
        std::lock_guard lock(s->mutex);
        return s->abort.get_token();
    }
    int Fcntl(const Lease& s, int cmd, u64 value) {
        std::lock_guard lock(s->mutex);
        if (s->closed)
            return -POSIX_EBADF;
        switch (cmd) {
        case 1:
            return s->cloexec ? 1 : 0;
        case 2:
            if (value & ~u64{1})
                return -POSIX_EINVAL;
            s->cloexec = value & 1;
            return 0;
        case 3:
            return 2 | (s->nonblock ? 4 : 0);
        case 4:
            if (value & ~u64{7})
                return -POSIX_EOPNOTSUPP;
            s->nonblock = value & 4;
            return 0;
        default:
            return -POSIX_EOPNOTSUPP;
        }
    }
    int AddressCall(const Lease& s, Op op, Address& address, int value, std::stop_token owner) {
        const auto begin = steady_clock::now();
        std::optional<nanoseconds> timeout;
        bool nonblock;
        std::stop_token abort;
        {
            std::lock_guard lock(s->mutex);
            if (s->closed)
                return -POSIX_EBADF;
            nonblock = s->nonblock;
            timeout = s->connect_timeout;
            abort = s->abort.get_token();
            int rc;
            if (op == Op::Listen)
                rc = ::listen(s->fd, value);
            else if (op == Op::Shutdown) {
                if (value < 0 || value > 2)
                    return -POSIX_EINVAL;
                rc = ::shutdown(s->fd, value);
            } else if (op == Op::Name || op == Op::Peer) {
                address.size = sizeof(address.native);
                rc = op == Op::Name
                         ? ::getsockname(s->fd, reinterpret_cast<sockaddr*>(&address.native),
                                         &address.size)
                         : ::getpeername(s->fd, reinterpret_cast<sockaddr*>(&address.native),
                                         &address.size);
            } else {
                if (!online && !(address.Loopback() || (op == Op::Bind && address.Any())))
                    return -POSIX_ENETDOWN;
                rc = op == Op::Bind
                         ? ::bind(s->fd, reinterpret_cast<sockaddr*>(&address.native), address.size)
                         : ::connect(s->fd, reinterpret_cast<sockaddr*>(&address.native),
                                     address.size);
            }
            if (rc == 0)
                return 0;
            const int error = Error();
            if (op != Op::Connect || error != POSIX_EINPROGRESS || nonblock)
                return -error;
        }
        if (int error = Wait(s, POLLOUT, timeout, owner, abort))
            return -(error == POSIX_EAGAIN ? POSIX_ETIMEDOUT : error);
        std::lock_guard lock(s->mutex);
        if (s->closed)
            return -POSIX_EBADF;
        int error{};
        socklen_t size = sizeof(error);
        if (::getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &size))
            return -Error();
        return error ? -Libraries::Kernel::NativeToPosixErrno(error) : 0;
    }
    int Accept(const Lease& s, Address& address, std::stop_token owner) {
        auto abort = AbortToken(s);
        const auto begin = steady_clock::now();
        for (;;) {
            bool nonblock;
            std::optional<nanoseconds> timeout;
            {
                std::lock_guard lock(s->mutex);
                if (s->closed)
                    return -POSIX_EBADF;
                if (owner.stop_requested() || stopping.stop_requested())
                    return -POSIX_EINTR;
                nonblock = s->nonblock;
                timeout = s->accept_timeout;
                address.size = sizeof(address.native);
                auto accepted = std::make_shared<Socket>();
                accepted->fd =
                    ::accept(s->fd, reinterpret_cast<sockaddr*>(&address.native), &address.size);
                if (accepted->fd >= 0) {
                    if (!online && !address.Loopback())
                        return -POSIX_ENETDOWN;
                    accepted->type = s->type;
                    accepted->family = s->family;
                    accepted->nonblock = nonblock;
                    accepted->receive_timeout = s->receive_timeout;
                    accepted->send_timeout = s->send_timeout;
                    if (::fcntl(accepted->fd, F_SETFL, O_NONBLOCK) < 0 ||
                        ::fcntl(accepted->fd, F_SETFD, FD_CLOEXEC) < 0)
                        return -Error();
#ifdef SO_NOSIGPIPE
                    int on = 1;
                    if (::setsockopt(accepted->fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)))
                        return -Error();
#endif
                    return Add(std::move(accepted));
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    return -Error();
                if (nonblock)
                    return -POSIX_EAGAIN;
            }
            if (timeout)
                timeout =
                    std::max(nanoseconds::zero(),
                             *timeout - duration_cast<nanoseconds>(steady_clock::now() - begin));
            if (int error = Wait(s, POLLIN, timeout, owner, abort))
                return -error;
        }
    }
    s64 Transfer(const Lease& s, bool send, std::span<u8> data, int flags, Address* address,
                 int& message_flags, std::stop_token owner) {
        // BSD and Linux MSG_* numbers differ. Ancillary/crypto extensions are
        // rejected explicitly, never silently interpreted as native flags.
        if (flags & ~(1 | 2 | 4 | 0x40 | 0x80 | 0x20000))
            return -POSIX_EOPNOTSUPP;
        if ((flags & (2 | 0x40)) == (2 | 0x40))
            return -POSIX_EOPNOTSUPP;
        int native_flags = (flags & 1 ? MSG_OOB : 0) | (flags & 2 ? MSG_PEEK : 0) |
                           (flags & 4 ? MSG_DONTROUTE : 0);
#ifdef MSG_NOSIGNAL
        if (send)
            native_flags |= MSG_NOSIGNAL;
#endif
        if (send && address && !online && !address->Loopback())
            return -POSIX_ENETDOWN;
        const bool waitall = !send && s->type == 1 && (flags & 0x40) && !(flags & 2);
        const auto begin = steady_clock::now();
        auto abort = AbortToken(s);
        size_t done{};
        for (;;) {
            bool nonblock;
            std::optional<nanoseconds> timeout;
            ssize_t rc;
            {
                std::lock_guard lock(s->mutex);
                if (s->closed)
                    return done ? s64(done) : -POSIX_EBADF;
                if (owner.stop_requested() || stopping.stop_requested())
                    return done ? s64(done) : -POSIX_EINTR;
                if (abort.stop_requested())
                    return done ? s64(done) : -POSIX_ECANCELED;
                nonblock = s->nonblock || (flags & 0x80);
                timeout = send ? s->send_timeout : s->receive_timeout;
                iovec vec{data.empty() ? nullptr : data.data() + done, data.size() - done};
                msghdr msg{};
                msg.msg_iov = &vec;
                msg.msg_iovlen = 1;
                Address peer;
                // Receive the source even when the guest discards it, so the
                // offline policy also applies to unconnected datagram input.
                Address* addr = address ? address : &peer;
                if (!send || address) {
                    msg.msg_name = &addr->native;
                    msg.msg_namelen = send ? addr->size : sizeof(addr->native);
                }
                rc = send ? ::sendmsg(s->fd, &msg, native_flags)
                          : ::recvmsg(s->fd, &msg, native_flags);
                if (rc >= 0) {
                    if (!send) {
                        addr->size = msg.msg_namelen;
                        if (s->type == 2 && !online && !addr->Loopback())
                            return done ? s64(done) : -POSIX_ENETDOWN;
                        message_flags = (msg.msg_flags & MSG_TRUNC ? 0x10 : 0) |
                                        (msg.msg_flags & MSG_CTRUNC ? 0x20 : 0);
                    }
                    done += rc;
                    if (!waitall || nonblock || !rc || done == data.size())
                        return done;
                } else {
                    const int error = Error();
                    if (error != POSIX_EAGAIN)
                        return done ? s64(done) : -error;
                    if (nonblock)
                        return done ? s64(done) : -error;
                }
            }
            if (timeout)
                timeout =
                    std::max(nanoseconds::zero(),
                             *timeout - duration_cast<nanoseconds>(steady_clock::now() - begin));
            if (int error =
                    Wait(s, send ? POLLOUT : (flags & 1 ? POLLPRI : POLLIN), timeout, owner, abort))
                return done ? s64(done) : -error;
        }
    }
    int Option(const Lease& s, bool set, int level, int option, std::vector<u8>& value) {
        std::lock_guard lock(s->mutex);
        if (s->closed)
            return POSIX_EBADF;
        if (level == 0xffff &&
            (option == 0x1200 || option == 0x1105 || option == 0x1106 || option == 0x1005 ||
             option == 0x1006 || option == 0x1108 || option == 0x1109)) {
            const bool tv = option == 0x1005 || option == 0x1006;
            const size_t size = tv ? sizeof(Timeval) : sizeof(s32);
            if (set && value.size() < size)
                return POSIX_EINVAL;
            if (option == 0x1200) {
                s32 v = s->nonblock;
                if (set) {
                    std::memcpy(&v, value.data(), 4);
                    s->nonblock = v != 0;
                } else {
                    value.resize(4);
                    std::memcpy(value.data(), &v, 4);
                }
                return 0;
            }
            auto& duration = option == 0x1108                         ? s->accept_timeout
                             : option == 0x1109                       ? s->connect_timeout
                             : (option == 0x1105 || option == 0x1005) ? s->send_timeout
                                                                      : s->receive_timeout;
            if (set) {
                std::optional<nanoseconds> d;
                if (tv) {
                    Timeval t;
                    std::memcpy(&t, value.data(), sizeof(t));
                    if (int e = Duration(t, d))
                        return e;
                } else {
                    s32 us;
                    std::memcpy(&us, value.data(), 4);
                    if (us < 0)
                        return POSIX_EINVAL;
                    d = microseconds(us);
                }
                duration = *d == nanoseconds::zero() ? std::nullopt : d;
            } else {
                const s64 us = duration ? duration_cast<microseconds>(*duration).count() : 0;
                value.resize(size);
                if (tv) {
                    Timeval t{us / 1000000, us % 1000000};
                    std::memcpy(value.data(), &t, size);
                } else {
                    if (us > INT_MAX)
                        return POSIX_EOVERFLOW;
                    const s32 v = us;
                    std::memcpy(value.data(), &v, 4);
                }
            }
            return 0;
        }
        int native_level{}, native_option{};
        size_t size = 4;
        if (level == 0xffff) {
            native_level = SOL_SOCKET;
            switch (option) {
            case 2:
                native_option = SO_ACCEPTCONN;
                break;
            case 4:
                native_option = SO_REUSEADDR;
                break;
            case 8:
                native_option = SO_KEEPALIVE;
                break;
            case 0x10:
                native_option = SO_DONTROUTE;
                break;
            case 0x20:
                native_option = SO_BROADCAST;
                break;
            case 0x80:
                native_option = SO_LINGER;
                size = 8;
                break;
            case 0x100:
                native_option = SO_OOBINLINE;
                break;
            case 0x200:
                native_option = SO_REUSEPORT;
                break;
            case 0x1001:
                native_option = SO_SNDBUF;
                break;
            case 0x1002:
                native_option = SO_RCVBUF;
                break;
            case 0x1004:
                if (set) {
                    s32 low{};
                    if (value.size() < 4)
                        return POSIX_EINVAL;
                    std::memcpy(&low, value.data(), 4);
                    if (low != 1)
                        return POSIX_EOPNOTSUPP;
                }
                native_option = SO_RCVLOWAT;
                break;
            case 0x1007:
                native_option = SO_ERROR;
                break;
            case 0x1008:
                native_option = SO_TYPE;
                break;
            default:
                return POSIX_ENOPROTOOPT;
            }
            if (set && (option == 2 || option == 0x1007 || option == 0x1008))
                return POSIX_ENOPROTOOPT;
        } else if (level == 6) {
            native_level = IPPROTO_TCP;
            if (option == 1)
                native_option = TCP_NODELAY;
            else if (option == 2)
                native_option = TCP_MAXSEG;
            else
                return POSIX_ENOPROTOOPT;
        } else if (level == 0) {
            native_level = IPPROTO_IP;
            if (option == 3)
                native_option = IP_TOS;
            else if (option == 4)
                native_option = IP_TTL;
            else
                return POSIX_ENOPROTOOPT;
        } else if (level == 41 && option == 27) {
            native_level = IPPROTO_IPV6;
            native_option = IPV6_V6ONLY;
        } else
            return POSIX_ENOPROTOOPT;
        if (set) {
            if (value.size() < size)
                return POSIX_EINVAL;
            // Positive linger makes close block even with O_NONBLOCK on Linux.
            // Refuse it until the guest close/drain lifecycle is implemented.
            if (option == 0x80 && level == 0xffff) {
                s32 on{}, seconds{};
                std::memcpy(&on, value.data(), 4);
                std::memcpy(&seconds, value.data() + 4, 4);
                if (seconds < 0)
                    return POSIX_EINVAL;
                if (on && seconds)
                    return POSIX_EOPNOTSUPP;
            }
            return ::setsockopt(s->fd, native_level, native_option, value.data(), size) ? Error()
                                                                                        : 0;
        }
        value.resize(size);
        socklen_t length = size;
        if (::getsockopt(s->fd, native_level, native_option, value.data(), &length))
            return Error();
        value.resize(length);
        if (level == 0xffff && option == 0x1007) {
            s32 error;
            std::memcpy(&error, value.data(), 4);
            error = Libraries::Kernel::NativeToPosixErrno(error);
            std::memcpy(value.data(), &error, 4);
        }
        return 0;
    }
    u64 Select(Space& space, const Args& a, const Failure& fail, const FileReady& files,
               std::stop_token stop) {
        const s32 count = s32(a[0]);
        if (count < 0 || count > 1024)
            return fail(POSIX_EINVAL);
        std::optional<nanoseconds> duration;
        if (a[4]) {
            Timeval t;
            if (!Read(space, a[4], t))
                return fail(POSIX_EFAULT);
            if (int e = Duration(t, duration))
                return fail(e);
        }
        const size_t bytes = ((count + 63) / 64) * 8;
        Outputs output{space};
        std::array<std::array<u64, 16>, 3> input{}, result{};
        for (int i = 0; i < 3; ++i) {
            if (!output.Add(a[i + 1], a[i + 1] ? bytes : 0))
                return fail(POSIX_EFAULT);
            if (a[i + 1] && bytes &&
                !space.ReadData(GuestAddress{a[i + 1]},
                                std::as_writable_bytes(std::span{input[i]}).first(bytes)))
                return fail(POSIX_EFAULT);
        }
        struct Item {
            int fd, ready;
            Lease socket;
        };
        std::vector<Item> items;
        std::vector<pollfd> polls;
        std::vector<std::stop_token> tokens;
        bool immediate{};
        for (int fd = 0; fd < count; ++fd) {
            const u64 bit = u64{1} << (fd % 64);
            short events{};
            if (input[0][fd / 64] & bit)
                events |= POLLIN;
            if (input[1][fd / 64] & bit)
                events |= POLLOUT;
            if (input[2][fd / 64] & bit)
                events |= POLLPRI;
            if (!events)
                continue;
            auto s = Get(fd);
            if (s) {
                items.push_back({fd, 0, s});
                polls.push_back({s->fd, events, 0});
                tokens.push_back(s->closing.get_token());
            } else {
                const int ready = files ? files(fd) : -POSIX_EBADF;
                if (ready < 0)
                    return fail(-ready);
                items.push_back({fd, ready, {}});
                polls.push_back({-1, events, 0});
                immediate |= ((ready & 1) && (events & POLLIN)) ||
                             ((ready & 2) && (events & POLLOUT)) ||
                             ((ready & 4) && (events & POLLPRI));
            }
        }
        int rc = Poll(polls, immediate ? std::optional<nanoseconds>{nanoseconds::zero()} : duration,
                      stop, tokens);
        for (const auto& item : items)
            if (item.socket && item.socket->closed)
                return fail(POSIX_EBADF);
        if (rc < 0)
            return fail(-rc);
        int ready_count{};
        for (size_t n = 0; n < items.size(); ++n) {
            int mask = items[n].ready;
            const auto flags = polls[n].revents;
            if (flags & POLLNVAL)
                return fail(POSIX_EBADF);
            if (flags & (POLLIN | POLLHUP | POLLERR))
                mask |= 1;
            if (flags & (POLLOUT | POLLHUP | POLLERR))
                mask |= 2;
            if (flags & POLLPRI)
                mask |= 4;
            int fd = items[n].fd;
            const u64 bit = u64{1} << (fd % 64);
            for (int i = 0; i < 3; ++i)
                if ((mask & (1 << i)) && (input[i][fd / 64] & bit)) {
                    result[i][fd / 64] |= bit;
                    ++ready_count;
                }
        }
        for (int i = 0; i < 3; ++i)
            if (a[i + 1] && bytes)
                std::memcpy(output.bytes[i].data(), result[i].data(), bytes);
        return output.Publish() ? u64(ready_count) : fail(POSIX_EFAULT);
    }
    u64 EpollCall(Space& space, Op op, const Args& a, const Failure& fail, std::stop_token stop) {
        using Event = Libraries::Net::OrbisNetEpollEvent;
        static_assert(sizeof(Event) == 24);
        if (op == Op::EpollCreate) {
            if (a[1])
                return fail(POSIX_EINVAL);
            if (a[0]) {
                bool end{};
                for (u64 i = 0; i < 64; ++i) {
                    char c;
                    if (!Read(space, a[0] + i, c))
                        return fail(POSIX_EFAULT);
                    if (!c) {
                        end = true;
                        break;
                    }
                }
                if (!end)
                    return fail(POSIX_ENAMETOOLONG);
            }
            std::lock_guard lock(mutex);
            if (epolls.size() >= 64 || next_epoll == INT_MAX)
                return fail(POSIX_EMFILE);
            const int id = next_epoll++;
            epolls.emplace(id, std::make_shared<Epoll>());
            return id;
        }
        std::shared_ptr<Epoll> epoll;
        {
            std::lock_guard lock(mutex);
            auto it = epolls.find(s32(a[0]));
            if (it == epolls.end())
                return fail(POSIX_EBADF);
            epoll = it->second;
            if (op == Op::EpollDestroy)
                epolls.erase(it);
        }
        if (op == Op::EpollDestroy) {
            epoll->destroy.request_stop();
            return 0;
        }
        if (op == Op::EpollAbort) {
            std::stop_source old;
            {
                std::lock_guard lock(epoll->mutex);
                old = epoll->abort;
                epoll->abort = std::stop_source{};
            }
            old.request_stop();
            return 0;
        }
        if (op == Op::EpollControl) {
            const int command = s32(a[1]), fd = s32(a[2]);
            Event event{};
            if (command < 1 || command > 3)
                return fail(POSIX_EINVAL);
            if (command != 3 && !Read(space, a[3], event))
                return fail(POSIX_EFAULT);
            if (event.events & ~u32{0x1 | 0x2 | 0x8 | 0x10})
                return fail(POSIX_EOPNOTSUPP);
            auto socket = Get(fd);
            if (!socket)
                return fail(POSIX_EBADF);
            std::stop_source old;
            {
                std::lock_guard lock(epoll->mutex);
                if (epoll->destroy.stop_requested() || socket->closed)
                    return fail(POSIX_EBADF);
                auto it = epoll->watches.find(fd);
                if (command == 1 && it != epoll->watches.end())
                    return fail(POSIX_EEXIST);
                if (command != 1 && it == epoll->watches.end())
                    return fail(POSIX_ENOENT);
                if (command == 3)
                    epoll->watches.erase(it);
                else
                    epoll->watches[fd] = {socket, event.events, event.data.data_u64};
                old = epoll->changed;
                epoll->changed = std::stop_source{};
            }
            old.request_stop();
            return 0;
        }
        if (s32(a[2]) <= 0 || a[2] > 65536)
            return fail(POSIX_EINVAL);
        Outputs output{space};
        if (!output.Add(a[1], a[2] * sizeof(Event)))
            return fail(POSIX_EFAULT);
        const auto begin = steady_clock::now();
        std::optional<nanoseconds> duration;
        if (s32(a[3]) >= 0)
            duration = microseconds(s32(a[3]));
        std::stop_token abort;
        {
            std::lock_guard lock(epoll->mutex);
            abort = epoll->abort.get_token();
        }
        for (;;) {
            std::vector<std::pair<int, Epoll::Watch>> watches;
            std::vector<pollfd> polls;
            std::vector<std::stop_token> tokens{epoll->destroy.get_token(), abort};
            std::stop_token changed;
            {
                std::lock_guard lock(epoll->mutex);
                changed = epoll->changed.get_token();
                tokens.push_back(changed);
                std::erase_if(epoll->watches,
                              [](const auto& p) { return p.second.socket->closed.load(); });
                for (const auto& w : epoll->watches) {
                    watches.push_back(w);
                    tokens.push_back(w.second.socket->closing.get_token());
                    polls.push_back({w.second.socket->fd,
                                     short((w.second.events & 1 ? POLLIN : 0) |
                                           (w.second.events & 2 ? POLLOUT : 0)),
                                     0});
                }
            }
            auto left = duration;
            if (left)
                left = std::max(nanoseconds::zero(),
                                *left - duration_cast<nanoseconds>(steady_clock::now() - begin));
            const int rc = Poll(polls, left, stop, tokens);
            if (epoll->destroy.stop_requested())
                return fail(POSIX_EBADF);
            if (abort.stop_requested())
                return fail(POSIX_ECANCELED);
            if (stop.stop_requested() || stopping.stop_requested())
                return fail(POSIX_EINTR);
            if (changed.stop_requested() || std::ranges::any_of(watches, [](const auto& w) {
                    return w.second.socket->closed.load();
                }))
                continue;
            if (rc < 0)
                return fail(-rc);
            std::vector<Event> events;
            for (size_t i = 0; i < polls.size() && events.size() < a[2]; ++i) {
                const auto f = polls[i].revents;
                u32 bits = (f & POLLIN ? 1 : 0) | (f & POLLOUT ? 2 : 0) | (f & POLLERR ? 8 : 0) |
                           (f & POLLHUP ? 16 : 0);
                if (bits)
                    events.push_back(
                        {bits, 0, u64(watches[i].first), {.data_u64 = watches[i].second.data}});
            }
            const size_t bytes = events.size() * sizeof(Event);
            output.Trim(0, bytes);
            if (bytes)
                std::memcpy(output.bytes[0].data(), events.data(), bytes);
            return output.Publish() ? events.size() : fail(POSIX_EFAULT);
        }
    }
};
GuestSockets::GuestSockets(std::shared_ptr<GuestDescriptorIds> ids, bool online)
    : impl(std::make_unique<Impl>()) {
    impl->ids = std::move(ids);
    impl->online = online;
}
GuestSockets::~GuestSockets() {
    RequestStop();
}
void GuestSockets::RequestStop() {
    impl->stopping.request_stop();
}
size_t GuestSockets::WaiterCount() const {
    return impl->waiters.load();
}
bool GuestSockets::Contains(s32 fd) const {
    return bool(impl->Get(fd));
}
bool GuestSockets::Empty() const {
    std::lock_guard lock(impl->mutex);
    return impl->sockets.empty() && impl->epolls.empty();
}
bool GuestSockets::IsNetNid(std::string_view nid) {
    for (const auto& e : Entries)
        if (!e.net.empty() && e.net == nid)
            return true;
    return false;
}
u64 GuestSockets::Dispatch(Space& space, std::string_view nid, const Args& original,
                           const Failure& fail, const FileReady& files, std::stop_token stop) {
    try {
        Op op = Find(nid);
        Args a = original;
        if (impl->stopping.stop_requested() || stop.stop_requested())
            return fail(POSIX_EINTR);
        if (op == Op::Select)
            return impl->Select(space, a, fail, files, stop);
        if (op >= Op::EpollCreate && op <= Op::EpollAbort)
            return impl->EpollCall(space, op, a, fail, stop);
        if (op == Op::Info) {
            using namespace Libraries::Net;
            // Firmware rejects these bits before entering its syscall. Other
            // special/global enumeration modes have no session-local provider.
            if (u32(a[3]) & 0x31000) return fail(POSIX_EINVAL);
            if (u32(a[3])) return fail(POSIX_EOPNOTSUPP);
            if (s32(a[0]) >= 0 && !a[1]) return fail(POSIX_EINVAL);
            if (a[1] && s32(a[2]) <= 0) return fail(POSIX_EINVAL);
            std::vector<std::pair<int, Impl::Lease>> snapshot;
            int lookup_error{};
            {
                std::lock_guard lock(impl->mutex);
                if (s32(a[0]) >= 0) {
                    auto it = impl->sockets.find(s32(a[0]));
                    if (it == impl->sockets.end()) lookup_error = POSIX_EBADF;
                    else if (it->second->family != 2) lookup_error = POSIX_EAFNOSUPPORT;
                    else snapshot.push_back(*it);
                } else {
                    for (const auto& item : impl->sockets)
                        if (item.second->family == 2) snapshot.push_back(item);
                }
            }
            if (lookup_error) return fail(lookup_error);
            if (!a[1]) return snapshot.size();
            snapshot.resize(std::min(snapshot.size(), size_t(s32(a[2]))));
            if (snapshot.empty()) return 0;
            std::vector<OrbisNetSockInfo> infos(snapshot.size());
            // Use an output identity snapshot while host queries run; no pin
            // or descriptor-map lock spans a host socket operation.
            Outputs output{space};
            if (!output.Add(a[1], infos.size() * sizeof(OrbisNetSockInfo))) return fail(POSIX_EFAULT);
            for (size_t i = 0; i < snapshot.size(); ++i) {
                const auto& [id, s] = snapshot[i];
                std::lock_guard lock(s->mutex);
                FillNativeSockInfo(infos[i], id, s->fd, s->type, s->name, s->nonblock);
            }
            if (impl->stopping.stop_requested() || stop.stop_requested()) return fail(POSIX_EINTR);
            std::memcpy(output.bytes[0].data(), infos.data(), output.bytes[0].size());
            return output.Publish() ? infos.size() : fail(POSIX_EFAULT);
        }
        if (op == Op::Socket) {
            std::string name;
            if (nid == "Q4qBuN-c0ZM") {
                if (a[0]) {
                    bool end{};
                    for (u64 i = 0; i < 33; ++i) {
                        char c;
                        if (!Read(space, a[0] + i, c))
                            return fail(POSIX_EFAULT);
                        if (!c) {
                            end = true;
                            break;
                        }
                        name += c;
                    }
                    if (!end)
                        return fail(POSIX_ENAMETOOLONG);
                }
                a = {a[1], a[2], a[3]};
            }
            int rc = impl->Create(s32(a[0]), s32(a[1]), s32(a[2]), std::move(name));
            impl->Trace(nid, -1, rc);
            return rc < 0 ? fail(-rc) : u64(rc);
        }
        auto socket = impl->Get(s32(a[0]));
        if (!socket) {
            int ready = files ? files(s32(a[0])) : -POSIX_EBADF;
            return fail(ready < 0 ? -ready : op == Op::Fcntl ? POSIX_ENOSYS : POSIX_ENOTSOCK);
        }
        auto result = [&](s64 rc) -> u64 { return rc < 0 ? fail(int(-rc)) : u64(rc); };
        if (op == Op::Close)
            return result(-impl->Close(s32(a[0])));
        if (op == Op::Abort) {
            // Flags select send/receive on retail; only whole-socket abort (0)
            // is admitted until directional cancellation is modelled.
            if (a[1])
                return fail(POSIX_EOPNOTSUPP);
            std::stop_source old;
            {
                std::lock_guard lock(socket->mutex);
                old = socket->abort;
                socket->abort = std::stop_source{};
            }
            old.request_stop();
            return 0;
        }
        if (op == Op::Fcntl)
            return result(impl->Fcntl(socket, s32(a[1]), a[2]));
        Address address;
        if (op == Op::Bind || op == Op::Connect) {
            if (int e = address.Load(space, a[1], u32(a[2])))
                return fail(e);
            const auto rc = impl->AddressCall(socket, op, address, 0, stop);
            impl->Trace(nid, s32(a[0]), rc);
            return result(rc);
        }
        if (op == Op::Listen || op == Op::Shutdown)
            return result(impl->AddressCall(socket, op, address, s32(a[1]), stop));
        if (op == Op::Name || op == Op::Peer || op == Op::Accept) {
            Outputs output{space};
            u32 capacity{};
            if (op != Op::Accept || a[1]) {
                if (!Read(space, a[2], capacity) ||
                    !output.Add(a[1], std::min<u32>(capacity, 28)) || !output.Add(a[2], 4))
                    return fail(POSIX_EFAULT);
            }
            const int rc = op == Op::Accept ? impl->Accept(socket, address, stop)
                                            : impl->AddressCall(socket, op, address, 0, stop);
            if (op == Op::Accept)
                impl->Trace(nid, s32(a[0]), rc);
            if (rc < 0)
                return fail(-rc);
            struct AcceptedGuard {
                Impl* domain;
                int descriptor;
                ~AcceptedGuard() {
                    if (descriptor >= 0)
                        domain->Close(descriptor);
                }
            } accepted{impl.get(), op == Op::Accept ? rc : -1};
            if (!output.bytes.empty()) {
                auto data = address.Guest();
                const auto n = std::min<size_t>(capacity, data.size());
                output.Trim(0, n);
                std::copy_n(data.begin(), n, output.bytes[0].begin());
                output.Put(1, u32(data.size()));
            }
            if (!output.Publish())
                return fail(POSIX_EFAULT);
            accepted.descriptor = -1;
            return rc;
        }
        if (op == Op::Set || op == Op::Get) {
            u32 length = u32(a[4]);
            Outputs output{space};
            if (op == Op::Get &&
                (!Read(space, a[4], length) || !output.Add(a[3], std::min<u32>(length, 16)) ||
                 !output.Add(a[4], 4)))
                return fail(POSIX_EFAULT);
            if (op == Op::Set && length > 256)
                return fail(POSIX_EINVAL);
            std::vector<u8> bytes(op == Op::Set ? length : 0);
            if (op == Op::Set && length &&
                !space.ReadData(GuestAddress{a[3]}, std::as_writable_bytes(std::span{bytes})))
                return fail(POSIX_EFAULT);
            if (int e = impl->Option(socket, op == Op::Set, s32(a[1]), s32(a[2]), bytes))
                return fail(e);
            if (op == Op::Set)
                return 0;
            const auto n = std::min<size_t>(length, bytes.size());
            output.Trim(0, n);
            std::copy_n(bytes.begin(), n, output.bytes[0].begin());
            output.Put(1, u32(n));
            return output.Publish() ? 0 : fail(POSIX_EFAULT);
        }
        const bool send = op == Op::Send || op == Op::SendTo || op == Op::SendMsg;
        const bool receive = op == Op::Receive || op == Op::ReceiveFrom || op == Op::ReceiveMsg;
        if (!send && !receive)
            return fail(POSIX_EOPNOTSUPP);
        struct Message {
            u64 name;
            u32 name_size, pad;
            u64 iov;
            s32 count;
            u32 pad2;
            u64 control;
            u32 control_size;
            s32 flags;
        } message{};
        struct Iov {
            u64 address, size;
        };
        static_assert(sizeof(Message) == 48 && sizeof(Iov) == 16);
        const bool vector = op == Op::SendMsg || op == Op::ReceiveMsg;
        std::vector<Iov> buffers;
        u64 name{}, name_length_address{};
        u32 name_capacity{};
        int flags = s32(a[3]);
        Outputs output{space};
        size_t message_index = SIZE_MAX, name_index = SIZE_MAX, length_index = SIZE_MAX;
        if (vector) {
            if (!Read(space, a[1], message))
                return fail(POSIX_EFAULT);
            if (message.count < 0 || message.count > 1024)
                return fail(POSIX_EMSGSIZE);
            // No ancillary control messages are silently dropped or passed to
            // Android. In particular guest SCM_RIGHTS must never expose host fds.
            if (message.control_size)
                return fail(POSIX_EOPNOTSUPP);
            buffers.resize(message.count);
            if (!buffers.empty() && !space.ReadData(GuestAddress{message.iov},
                                                    std::as_writable_bytes(std::span{buffers})))
                return fail(POSIX_EFAULT);
            flags = s32(a[2]);
            name = message.name;
            name_capacity = message.name_size;
        } else {
            buffers.push_back({a[1], a[2]});
            if (op == Op::SendTo) {
                name = a[4];
                name_capacity = u32(a[5]);
            }
            if (op == Op::ReceiveFrom) {
                name = a[4];
                name_length_address = a[5];
                if (name && !Read(space, name_length_address, name_capacity))
                    return fail(POSIX_EFAULT);
            }
        }
        size_t total{};
        for (const auto& b : buffers) {
            if (b.size > MaxTransfer - total)
                return fail(POSIX_EMSGSIZE);
            total += b.size;
            if (receive && !output.Add(b.address, b.size))
                return fail(POSIX_EFAULT);
        }
        std::vector<u8> data(total);
        size_t offset{};
        if (send)
            for (const auto& b : buffers) {
                if (b.size && !space.ReadData(
                                  GuestAddress{b.address},
                                  std::as_writable_bytes(std::span{data}).subspan(offset, b.size)))
                    return fail(POSIX_EFAULT);
                offset += b.size;
            }
        if (send && name) {
            if (int e = address.Load(space, name, name_capacity))
                return fail(e);
        }
        if (receive && name) {
            name_index = output.bytes.size();
            if (!output.Add(name, std::min<u32>(name_capacity, 28)))
                return fail(POSIX_EFAULT);
            if (!vector) {
                length_index = output.bytes.size();
                if (!output.Add(name_length_address, 4))
                    return fail(POSIX_EFAULT);
            }
        }
        if (receive && vector) {
            message_index = output.bytes.size();
            if (!output.Add(a[1], sizeof(Message)))
                return fail(POSIX_EFAULT);
        }
        int message_flags{};
        const s64 rc = impl->Transfer(socket, send, data, flags, name ? &address : nullptr,
                                      message_flags, stop);
        if (rc < 0)
            return fail(int(-rc));
        if (send)
            return rc;
        size_t remaining = rc;
        offset = 0;
        for (size_t i = 0; i < buffers.size(); ++i) {
            const auto n = std::min<size_t>(remaining, buffers[i].size);
            output.Trim(i, n);
            if (n)
                std::memcpy(output.bytes[i].data(), data.data() + offset, n);
            offset += n;
            remaining -= n;
        }
        if (name) {
            auto bytes = address.Guest();
            const auto n = std::min<size_t>(name_capacity, bytes.size());
            output.Trim(name_index, n);
            std::copy_n(bytes.begin(), n, output.bytes[name_index].begin());
            if (vector)
                message.name_size = bytes.size();
            else
                output.Put(length_index, u32(bytes.size()));
        }
        if (vector) {
            message.flags = message_flags;
            message.control_size = 0;
            output.Put(message_index, message);
        }
        return output.Publish() ? u64(rc) : fail(POSIX_EFAULT);
    } catch (const std::bad_alloc&) {
        return fail(POSIX_ENOMEM);
    }
}
} // namespace Core::HostRuntime
