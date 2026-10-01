// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <set>
#include <string_view>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/np/np_auth.h"
#include "core/libraries/np/np_error.h"

namespace Core::HostRuntime {
// Only admitted in the signed-out domain. Desktop owns actual request state;
// this bridge owns guest-pointer admission and the session's native handles.
class GuestNpAuthOffline {
public:
    static constexpr std::string_view Nids[]{"6bwFkosYRQg", "N+mr7GjTvr8", "qAUXQ9GdWp8",
                                             "CocbHVIKPE8", "cE7wIsqXdZ8", "SK-S7daqJSE",
                                             "gjSyfzSsDcE", "H8wG9Bk-nPc"};
    static bool IsNid(std::string_view nid) {
        return std::ranges::find(Nids, nid) != std::end(Nids);
    }
    static bool Admits(std::string_view nid, std::string_view suffix, bool offline) {
        return offline && IsNid(nid) && suffix == "#libSceNpAuth#1#libSceNpAuth#Function";
    }
    ~GuestNpAuthOffline() {
        for (s32 id : requests)
            Libraries::Np::NpAuth::sceNpAuthDeleteRequest(id);
    }
    u32 Dispatch(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                 const std::array<u64, 6>& a) {
        using namespace GuestCpu;
        using namespace Libraries::Np;
        using namespace Libraries::Np::NpAuth;
        constexpr u32 bad = ORBIS_NP_AUTH_ERROR_INVALID_ARGUMENT;
        std::lock_guard lock(mutex);
        auto read = [&]<typename T>(u64 at, T& value) {
            return at && bool(space.ReadData({at}, std::as_writable_bytes(std::span{&value, 1})));
        };
        if (nid == "6bwFkosYRQg" || nid == "N+mr7GjTvr8") {
            OrbisNpAuthCreateAsyncRequestParameter param{};
            const bool async = nid == "N+mr7GjTvr8";
            if (async && !read(a[0], param))
                return bad;
            const s32 id = async ? sceNpAuthCreateAsyncRequest(&param) : sceNpAuthCreateRequest();
            if (id > 0)
                requests.insert(id);
            return u32(id);
        }
        if (!IsNid(nid))
            return bad;
        const s32 id = s32(a[0]);
        if (!requests.contains(id))
            return u32(ORBIS_NP_AUTH_ERROR_REQUEST_NOT_FOUND);
        if (nid == "H8wG9Bk-nPc") {
            const auto result = sceNpAuthDeleteRequest(id);
            if (!result)
                requests.erase(id);
            return u32(result);
        }
        if (nid == "cE7wIsqXdZ8")
            return u32(sceNpAuthAbortRequest(id));
        if (nid == "SK-S7daqJSE" || nid == "gjSyfzSsDcE") {
            if (!a[1])
                return bad;
            auto output = space.AcquireDataSpan({{a[1]}, sizeof(s32)}, true);
            if (!output)
                return bad;
            s32 result{};
            const auto status = nid == "SK-S7daqJSE" ? sceNpAuthWaitAsync(id, &result)
                                                     : sceNpAuthPollAsync(id, &result);
            if (!status)
                std::memcpy(output.Value().WritableBytes().data(), &result, sizeof(result));
            return u32(status);
        }
        const bool token = nid == "CocbHVIKPE8";
        // Integer-only wire structs: never dereference a guest address natively.
        struct CodeParam {
            u64 size;
            s32 user;
            u32 padding;
            u64 client, scope;
        } code{};
        struct TokenParam {
            u64 size;
            s32 user;
            u32 padding;
            u64 client, secret, scope;
        } tok{};
        static_assert(sizeof(code) == sizeof(OrbisNpAuthGetAuthorizationCodeParameterA));
        static_assert(sizeof(tok) == sizeof(OrbisNpAuthGetIdTokenParameterA));
        if (token ? !read(a[1], tok) : !read(a[1], code))
            return bad;
        if ((token ? tok.size : code.size) != (token ? sizeof(tok) : sizeof(code)))
            return u32(ORBIS_NP_AUTH_ERROR_INVALID_SIZE);
        const s32 user = token ? tok.user : code.user;
        const u64 client_address = token ? tok.client : code.client;
        const u64 scope_address = token ? tok.scope : code.scope;
        if (user == -1 || !client_address || !scope_address || !a[2] || (token && !tok.secret))
            return bad;
        // Scope is bounded and copied; neither it nor credentials are logged.
        std::array<char, 1025> scope{};
        bool terminated{};
        for (u64 i = 0; i < scope.size(); ++i) {
            if (scope_address > UINT64_MAX - i || !read(scope_address + i, scope[i]))
                return bad;
            if (!scope[i]) {
                terminated = true;
                break;
            }
        }
        if (!terminated)
            return bad;
        std::vector<GuestAddressSpace::DataRequest> ranges{
            {{{client_address}, sizeof(OrbisNpClientId)}, GuestPermission::Read},
            {{{a[2]}, token ? sizeof(OrbisNpIdToken) : sizeof(OrbisNpAuthorizationCode)},
             GuestPermission::Write}};
        if (token)
            ranges.push_back({{{tok.secret}, sizeof(OrbisNpClientSecret)}, GuestPermission::Read});
        else if (a[3])
            ranges.push_back({{{a[3]}, sizeof(s32)}, GuestPermission::Write});
        auto pins = space.AcquireDataBatch(ranges);
        if (!pins)
            return bad;
        OrbisNpClientId client{};
        OrbisNpClientSecret secret{};
        std::memcpy(&client, pins.Value()[0].Bytes().data(), sizeof(client));
        // In the offline native implementation these outputs remain untouched,
        // including async submission success. Never copy temporary zero tokens.
        if (token) {
            std::memcpy(&secret, pins.Value()[2].Bytes().data(), sizeof(secret));
            OrbisNpAuthGetIdTokenParameterA param{sizeof(param), user,    {},
                                                  &client,       &secret, scope.data()};
            OrbisNpIdToken output{};
            return u32(sceNpAuthGetIdTokenA(id, &param, &output));
        }
        OrbisNpAuthGetAuthorizationCodeParameterA param{
            sizeof(param), user, {}, &client, scope.data()};
        OrbisNpAuthorizationCode output{};
        s32 issuer{};
        return u32(sceNpAuthGetAuthorizationCodeA(id, &param, &output, a[3] ? &issuer : nullptr));
    }

private:
    std::mutex mutex;
    std::set<s32> requests;
};
} // namespace Core::HostRuntime
