// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/network/http.h"
#include "core/libraries/network/http_error.h"

namespace Core::HostRuntime {
inline constexpr std::string_view HttpNids[]{
    "A9cVMUtEp4Y", // sceHttpInit
    "Ik-KpLTlf7Q", // sceHttpTerm
    "0gYjPTR-6cY", // sceHttpCreateTemplate
    "qgxDBjorUxs", // sceHttpCreateConnectionWithURL
    "Aeu5wVKkF9w", // sceHttpCreateRequestWithURL
    "tsGVru3hCe8", // sceHttpCreateRequest
    "rGNm+FjIXKk", // sceHttpCreateRequest2
    "Cnp77podkCU", // sceHttpCreateRequestWithURL2
    "4I8vEpuEhZ8", // sceHttpDeleteTemplate
    "P6A3ytpsiYc", // sceHttpDeleteConnection
    "qe7oZ+v4PWA", // sceHttpDeleteRequest
    "6381dWF+xsQ", // sceHttpCreateEpoll
    "wYhXVfS2Et4", // sceHttpDestroyEpoll
    "-xm7kZQNpHI", // sceHttpSetEpoll
    "59tL1AQBb8U", // sceHttpUnsetEpoll
    "sWQiqKvYTVA", // sceHttpAbortWaitRequest
    "qISjDHrxONc", // sceHttpWaitRequest
    "EY28T2bkN7k", // sceHttpAddRequestHeader
    "zNGh-zoQTD0", // sceHttpRemoveRequestHeader
    "1e2BNwI-XzE", // sceHttpSendRequest
    "P5pdoykPYTk", // sceHttpReadData
    "0a2TBNfE3BU", // sceHttpGetStatusCode
    "yuO2H2Uvnos", // sceHttpGetResponseContentLength
    "hvG6GfBMXg8", // sceHttpAbortRequest
    "JKl06ZIAl6A", // sceHttpAbortRequestForce
    "s2-NPIvz+iA", // sceHttpSetNonblock
    "fmOs6MzCRqk", // sceHttpTrySetNonblock
    "f42K37mm5RM", // sceHttpsEnableOption
    "mSQCxzWTwVI", // sceHttpsDisableOption
    "I4+4hKttt1w", // sceHttpsEnableOptionPrivate
    "zJYi5br6ZiQ", // sceHttpsDisableOptionPrivate
    "PTiFIUxCpJc", // sceHttpSetRequestContentLength
    "0S9tTH0uqTU", // sceHttpSetConnectTimeOut
    "Tc-hAYDKtQc", // sceHttpSetResolveTimeOut
    "xegFfZKBVlw", // sceHttpSetSendTimeOut
    "yigr4V0-HTM", // sceHttpSetRecvTimeOut
    "K1d1LqZRQHQ", // sceHttpSetResolveRetry
    "a4VsZ4oqn68", // sceHttpSetResponseHeaderMaxSize
    "T-mGo9f3Pu4", // sceHttpSetAutoRedirect
    "qFg2SuyTJJY", // sceHttpSetAuthEnabled
    "XNUoD2B9a6A", // sceHttpSetCookieEnabled
    "PDxS48xGQLs", // sceHttpSetChunkedTransferEnabled
    "HRX1iyDoKR8", // sceHttpSetAcceptEncodingGZIPEnabled
    "i9mhafzkEi8", // sceHttpSetInflateGZIPEnabled
    "mMcB2XIDoV4", // sceHttpSetRecvBlockSize
    "22buO-UufJY", // sceHttpSetDelayBuildRequestEnabled
    "pk0AuomQM1o", // sceHttpSetHttp09Enabled
    "Wq4RNB3snSQ", // sceHttpGetNonblock
    "V-noPEjSB8c", // sceHttpTryGetNonblock
    "9m8EcOGzcIQ", // sceHttpGetAuthEnabled
    "iSZjWw1TGiA", // sceHttpGetCookieEnabled
    "mmLexUbtnfY", // sceHttpGetAutoRedirect
    "1rpZqxdMRwQ", // sceHttpGetAcceptEncodingGZIPEnabled
    "0onIrKx9NIE", // sceHttpGetLastErrno
};
inline bool IsHttpNid(std::string_view nid) {
    return std::ranges::find(HttpNids, nid) != std::end(HttpNids);
}
inline bool AdmitsHttp(std::string_view nid, std::string_view suffix, bool offline) {
    return offline && IsHttpNid(nid) && suffix == "#libSceHttp#1#libSceHttp#Function";
}

// Thin guest ABI boundary over the desktop HTTP state machine, including its
// asynchronous offline failure/epoll notifications. No native pointer is exposed.
// The table lock protects ownership only; it is released before response/epoll
// waits. Desktop functions copy request input before returning.
class GuestHttp {
    using Space = GuestCpu::GuestAddressSpace;
    using Identity = Space::MappingIdentity;
    enum class Kind { Context, Template, Connection, Request };
    struct Object { Kind kind; int context; int native; };
    struct Epoll { int context; Libraries::Http::OrbisHttpEpollHandle native; };
    std::mutex mutex;
    std::map<int, Object> objects;
    std::map<u64, Epoll> epolls;
    u64 next_epoll{1};
    static constexpr u64 MaxTransfer = 16ULL << 20;
    static constexpr size_t MaxObjects = 1024;
    struct Output {
        u64 address{}, size{};
        std::vector<Identity> identities;
    };
    static std::optional<std::string> Text(Space& space, u64 address, size_t limit) {
        if (!address || address > UINT64_MAX - limit) return {};
        std::string text;
        for (size_t i = 0; i < limit; ++i) {
            char c{};
            if (!space.ReadData(GuestCpu::GuestAddress{address+i}, std::as_writable_bytes(std::span{&c, 1}))) return {};
            if (!c) return text;
            text += c;
        }
        return {};
    }
    static std::optional<Output> Snapshot(Space& space, u64 address, u64 size) {
        using namespace GuestCpu;
        if (!address || !size || size > MaxTransfer || address > UINT64_MAX - size) return {};
        auto pin = space.AcquireDataSpan({GuestAddress{address}, size}, true);
        if (!pin) return {};
        Output out{address, size, {}};
        for (u64 cursor = address; cursor < address + size;) {
            auto m = space.Query(GuestAddress{cursor});
            if (!m) return {};
            const auto end = std::min(address + size, m.Value().range.End());
            out.identities.push_back({cursor, end, m.Value().mapping_generation});
            cursor = end;
        }
        return out; // pin released before a native wait
    }
    static bool Publish(Space& space, const Output& out, const void* value, size_t size) {
        if (size > out.size) return false;
        const Space::DataRequest request{{GuestCpu::GuestAddress{out.address}, out.size},
                                        GuestCpu::GuestPermission::Write, out.identities};
        auto pins = space.AcquireDataBatch(std::span{&request, 1});
        if (!pins) return false;
        std::memcpy(pins.Value()[0].WritableBytes().data(), value, size);
        return true;
    }
    int TermLocked(int context) {
        using namespace Libraries::Http;
        // Desktop Term only clears all children on the last context. Retire our
        // context's children explicitly, also when another session domain exists.
        const int native_context = objects.at(context).native;
        for (const auto kind : {Kind::Request, Kind::Connection, Kind::Template}) {
            for (auto it = objects.begin(); it != objects.end();) {
                if (it->second.context != context || it->second.kind != kind) { ++it; continue; }
                if (kind == Kind::Request) sceHttpDeleteRequest(it->second.native);
                else if (kind == Kind::Connection) sceHttpDeleteConnection(it->second.native);
                else sceHttpDeleteTemplate(it->second.native);
                it = objects.erase(it);
            }
        }
        for (auto it = epolls.begin(); it != epolls.end();) {
            if (it->second.context != context) { ++it; continue; }
            sceHttpDestroyEpoll(native_context, it->second.native);
            it = epolls.erase(it);
        }
        objects.erase(context);
        return sceHttpTerm(native_context);
    }
public:
    ~GuestHttp() {
        std::lock_guard lock(mutex);
        while (!objects.empty()) TermLocked(objects.begin()->second.context);
    }
    u64 Dispatch(Space& space, std::string_view nid, const std::array<u64, 6>& a,
                 std::stop_token stop = {}) {
        using namespace GuestCpu;
        using namespace Libraries::Http;
        constexpr u32 bad = ORBIS_HTTP_ERROR_INVALID_VALUE;
        constexpr u32 invalid = ORBIS_HTTP_ERROR_INVALID_ID;
        if (stop.stop_requested()) return u32(ORBIS_HTTP_ERROR_ABORTED);
        const int guest_id = s32(a[0]);
        std::unique_lock lock(mutex);
        auto own = [&](int value, Kind kind) {
            const auto it = objects.find(value);
            return it != objects.end() && it->second.kind == kind;
        };
        auto record = [&](int value, Kind kind, int context) -> u64 {
            if (value <= 0) return u32(value);
            // Desktop contexts and objects have separate integer namespaces.
            // Tag guest handles so a template never aliases a context with the same native id.
            const int handle = (int(kind) + 1) * 0x10000000 | value;
            objects.emplace(handle, Object{kind, context ? context : handle, value});
            return u32(handle);
        };
        if (nid == "A9cVMUtEp4Y") {
            if (objects.size() >= MaxObjects) return u32(ORBIS_HTTP_ERROR_OUT_OF_MEMORY);
            return record(sceHttpInit(guest_id, s32(a[1]), a[2]), Kind::Context, 0);
        }
        if (nid == "qISjDHrxONc" || nid == "sWQiqKvYTVA") {
            const auto ep = epolls.find(a[0]);
            if (ep == epolls.end()) return invalid;
            const auto native = ep->second.native;
            lock.unlock();
            if (nid == "sWQiqKvYTVA") return u32(sceHttpAbortWaitRequest(native));
            if (s32(a[2]) <= 0 || a[2] > 1024) return bad;
            auto out = Snapshot(space, a[1], a[2] * 24);
            if (!out) return bad;
            std::vector<OrbisHttpNBEvent> events(a[2]);
            std::stop_callback cancel(stop, [native] { sceHttpAbortWaitRequest(native); });
            const int count = sceHttpWaitRequest(native, events.data(), s32(a[2]), s32(a[3]));
            if (count <= 0) return u32(count);
            // Serialize the guest ABI explicitly (including zeroed padding).
            struct GuestEvent { u32 events, detail; s32 id; u32 padding; u64 argument; };
            static_assert(sizeof(GuestEvent) == 24);
            std::vector<GuestEvent> guest(count);
            for (int i=0; i<count; ++i)
                guest[i] = {events[i].events, events[i].eventDetail, events[i].id | 0x40000000, 0,
                            reinterpret_cast<u64>(events[i].userArg)};
            return Publish(space, *out, guest.data(), guest.size()*24) ? u32(count) : bad;
        }
        const auto object = objects.find(guest_id);
        if (object == objects.end()) return invalid;
        const int id = object->second.native;
        const auto context = object->second.context;
        if (nid == "Ik-KpLTlf7Q") return own(guest_id, Kind::Context) ? u32(TermLocked(guest_id)) : invalid;
        if (nid == "0gYjPTR-6cY" || nid == "qgxDBjorUxs" || nid == "Aeu5wVKkF9w" ||
            nid == "tsGVru3hCe8" || nid == "rGNm+FjIXKk" || nid == "Cnp77podkCU") {
            if (objects.size() >= MaxObjects) return u32(ORBIS_HTTP_ERROR_OUT_OF_MEMORY);
            if (nid == "0gYjPTR-6cY") {
                if (!own(guest_id, Kind::Context)) return invalid;
                auto agent = Text(space, a[1], 1024);
                return agent ? record(sceHttpCreateTemplate(id, agent->c_str(), s32(a[2]), s32(a[3])), Kind::Template, context) : bad;
            }
            if (nid == "qgxDBjorUxs") {
                if (!own(guest_id, Kind::Template)) return invalid;
                auto url = Text(space, a[1], 8192);
                return url ? record(sceHttpCreateConnectionWithURL(id, url->c_str(), a[2] != 0), Kind::Connection, context) : bad;
            }
            if (!own(guest_id, Kind::Connection)) return invalid;
            auto url = Text(space, a[2], 8192);
            if (!url) return bad;
            int result;
            if (nid == "rGNm+FjIXKk" || nid == "Cnp77podkCU") {
                auto method = Text(space, a[1], 32);
                if (!method) return bad;
                result = nid == "rGNm+FjIXKk" ? sceHttpCreateRequest2(id, method->c_str(), url->c_str(), a[3]) : sceHttpCreateRequestWithURL2(id, method->c_str(), url->c_str(), a[3]);
            } else result = nid == "tsGVru3hCe8" ? sceHttpCreateRequest(id, s32(a[1]), url->c_str(), a[3]) : sceHttpCreateRequestWithURL(id, s32(a[1]), url->c_str(), a[3]);
            return record(result, Kind::Request, context);
        }
        if (nid == "4I8vEpuEhZ8" || nid == "P6A3ytpsiYc" || nid == "qe7oZ+v4PWA") {
            const auto kind = nid == "4I8vEpuEhZ8" ? Kind::Template : nid == "P6A3ytpsiYc" ? Kind::Connection : Kind::Request;
            if (!own(guest_id, kind)) return invalid;
            const int result = kind == Kind::Template ? sceHttpDeleteTemplate(id) : kind == Kind::Connection ? sceHttpDeleteConnection(id) : sceHttpDeleteRequest(id);
            if (!result) objects.erase(guest_id);
            return u32(result);
        }
        if (nid == "6381dWF+xsQ") {
            if (!own(guest_id, Kind::Context)) return invalid;
            if (epolls.size() >= MaxObjects || next_epoll == UINT64_MAX) return u32(ORBIS_HTTP_ERROR_OUT_OF_MEMORY);
            auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, 8}, true);
            if (!a[1] || !pin) return bad;
            OrbisHttpEpollHandle native{};
            const int result = sceHttpCreateEpoll(id, &native);
            if (result) return u32(result);
            const u64 token = next_epoll++;
            epolls.emplace(token, Epoll{guest_id, native});
            std::memcpy(pin.Value().WritableBytes().data(), &token, 8);
            return 0;
        }
        if (nid == "wYhXVfS2Et4" || nid == "-xm7kZQNpHI") {
            auto ep = epolls.find(a[1]);
            if (ep == epolls.end() || ep->second.context != context) return invalid;
            if (nid == "-xm7kZQNpHI")
                return u32(sceHttpSetEpoll(id, ep->second.native, reinterpret_cast<void*>(a[2])));
            if (!own(guest_id, Kind::Context)) return invalid;
            const int result = sceHttpDestroyEpoll(id, ep->second.native);
            if (!result) epolls.erase(ep);
            return u32(result);
        }
        if (nid == "59tL1AQBb8U") return u32(sceHttpUnsetEpoll(id));
        if (nid == "EY28T2bkN7k" || nid == "zNGh-zoQTD0") {
            auto name = Text(space, a[1], 256);
            if (!name) return bad;
            if (nid == "zNGh-zoQTD0") return u32(sceHttpRemoveRequestHeader(id, name->c_str()));
            auto value = Text(space, a[2], 8192);
            return value ? u32(sceHttpAddRequestHeader(id, name->c_str(), value->c_str(), s32(a[3]))) : bad;
        }
        if (nid == "s2-NPIvz+iA") return u32(sceHttpSetNonblock(id, a[1]));
        if (nid == "fmOs6MzCRqk") return u32(sceHttpTrySetNonblock(id, a[1]));
        if (nid == "f42K37mm5RM") return u32(sceHttpsEnableOption(id, a[1]));
        if (nid == "mSQCxzWTwVI") return u32(sceHttpsDisableOption(id, a[1]));
        if (nid == "I4+4hKttt1w") return u32(sceHttpsEnableOptionPrivate(id, a[1]));
        if (nid == "zJYi5br6ZiQ") return u32(sceHttpsDisableOptionPrivate(id, a[1]));
        if (nid == "PTiFIUxCpJc") return u32(sceHttpSetRequestContentLength(id, a[1]));
        if (nid == "0S9tTH0uqTU") return u32(sceHttpSetConnectTimeOut(id, a[1]));
        if (nid == "Tc-hAYDKtQc") return u32(sceHttpSetResolveTimeOut(id, a[1]));
        if (nid == "xegFfZKBVlw") return u32(sceHttpSetSendTimeOut(id, a[1]));
        if (nid == "yigr4V0-HTM") return u32(sceHttpSetRecvTimeOut(id, a[1]));
        if (nid == "K1d1LqZRQHQ") return u32(sceHttpSetResolveRetry(id, a[1]));
        if (nid == "a4VsZ4oqn68") return u32(sceHttpSetResponseHeaderMaxSize(id, a[1]));
        if (nid == "T-mGo9f3Pu4") return u32(sceHttpSetAutoRedirect(id, a[1]));
        if (nid == "qFg2SuyTJJY") return u32(sceHttpSetAuthEnabled(id, a[1]));
        if (nid == "XNUoD2B9a6A") return u32(sceHttpSetCookieEnabled(id, a[1]));
        if (nid == "PDxS48xGQLs") return u32(sceHttpSetChunkedTransferEnabled(id, a[1]));
        if (nid == "HRX1iyDoKR8") return u32(sceHttpSetAcceptEncodingGZIPEnabled(id, a[1]));
        if (nid == "i9mhafzkEi8") return u32(sceHttpSetInflateGZIPEnabled(id, a[1]));
        if (nid == "mMcB2XIDoV4") return u32(sceHttpSetRecvBlockSize(id, a[1]));
        if (nid == "22buO-UufJY") return u32(sceHttpSetDelayBuildRequestEnabled(id, a[1]));
        if (nid == "pk0AuomQM1o") return u32(sceHttpSetHttp09Enabled(id, a[1]));
        if (nid == "Wq4RNB3snSQ") {
            auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, 4}, true);
            if (!a[1] || !pin) return bad;
            int value{}; const int result = sceHttpGetNonblock(id, &value);
            if (!result) std::memcpy(pin.Value().WritableBytes().data(), &value, 4);
            return u32(result);
        }
        if (nid == "V-noPEjSB8c") {
            auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, 4}, true);
            if (!a[1] || !pin) return bad;
            int value{}; const int result = sceHttpTryGetNonblock(id, &value);
            if (!result) std::memcpy(pin.Value().WritableBytes().data(), &value, 4);
            return u32(result);
        }
        if (nid == "9m8EcOGzcIQ") {
            auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, 4}, true);
            if (!a[1] || !pin) return bad;
            int value{}; const int result = sceHttpGetAuthEnabled(id, &value);
            if (!result) std::memcpy(pin.Value().WritableBytes().data(), &value, 4);
            return u32(result);
        }
        if (nid == "iSZjWw1TGiA") {
            auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, 4}, true);
            if (!a[1] || !pin) return bad;
            int value{}; const int result = sceHttpGetCookieEnabled(id, &value);
            if (!result) std::memcpy(pin.Value().WritableBytes().data(), &value, 4);
            return u32(result);
        }
        if (nid == "mmLexUbtnfY") {
            auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, 4}, true);
            if (!a[1] || !pin) return bad;
            int value{}; const int result = sceHttpGetAutoRedirect(id, &value);
            if (!result) std::memcpy(pin.Value().WritableBytes().data(), &value, 4);
            return u32(result);
        }
        if (nid == "1rpZqxdMRwQ") {
            auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, 4}, true);
            if (!a[1] || !pin) return bad;
            int value{}; const int result = sceHttpGetAcceptEncodingGZIPEnabled(id, &value);
            if (!result) std::memcpy(pin.Value().WritableBytes().data(), &value, 4);
            return u32(result);
        }
        if (nid == "0onIrKx9NIE") {
            auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, 4}, true);
            if (!a[1] || !pin) return bad;
            int value{}; const int result = sceHttpGetLastErrno(id, &value);
            if (!result) std::memcpy(pin.Value().WritableBytes().data(), &value, 4);
            return u32(result);
        }
        if (!own(guest_id, Kind::Request)) return invalid;
        if (nid == "hvG6GfBMXg8" || nid == "JKl06ZIAl6A") return u32(sceHttpAbortRequest(id));
        if (nid == "1e2BNwI-XzE") {
            if (a[2] > MaxTransfer) return u32(ORBIS_HTTP_ERROR_OUT_OF_SIZE);
            if (!a[2]) return u32(sceHttpSendRequest(id, nullptr, 0));
            auto pin = space.AcquireDataSpan({GuestAddress{a[1]}, a[2]}, false);
            if (!a[1] || !pin) return bad;
            return u32(sceHttpSendRequest(id, pin.Value().Bytes().data(), a[2]));
        }
        lock.unlock();
        // Request deletion/Term may race the native wait. Desktop retains the
        // shared request until each waiter returns; its worker owns all input.
        std::stop_callback cancel(stop, [id] { sceHttpAbortRequest(id); });
        if (nid == "P5pdoykPYTk") {
            if (a[2] > MaxTransfer) return u32(ORBIS_HTTP_ERROR_OUT_OF_SIZE);
            auto out = Snapshot(space, a[1], std::max<u64>(1, a[2]));
            if (!out) return bad;
            std::vector<u8> data(std::max<u64>(1, a[2]));
            const int count = sceHttpReadData(id, data.data(), a[2]);
            if (count <= 0) return u32(count);
            return Publish(space, *out, data.data(), count) ? u32(count) : bad;
        }
        if (nid == "0a2TBNfE3BU") {
            auto out = Snapshot(space, a[1], 4);
            if (!out) return bad;
            int value{}; const int result = sceHttpGetStatusCode(id, &value);
            return result ? u32(result) : Publish(space, *out, &value, 4) ? 0 : bad;
        }
        if (nid == "yuO2H2Uvnos") {
            auto first = Snapshot(space, a[1], 4), second = Snapshot(space, a[2], 8);
            if (!first || !second) return bad;
            int value{}; u64 length{};
            const int result = sceHttpGetResponseContentLength(id, &value, &length);
            if (result) return u32(result);
            const std::array<Space::DataRequest, 2> outputs{{
                {{GuestAddress{a[1]}, 4}, GuestPermission::Write, first->identities},
                {{GuestAddress{a[2]}, 8}, GuestPermission::Write, second->identities}}};
            auto pins = space.AcquireDataBatch(outputs);
            if (!pins) return bad;
            std::memcpy(pins.Value()[0].WritableBytes().data(), &value, 4);
            std::memcpy(pins.Value()[1].WritableBytes().data(), &length, 8);
            return 0;
        }
        return u32(ORBIS_HTTP_ERROR_UNKNOWN);
    }
};
} // namespace Core::HostRuntime
