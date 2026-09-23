// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <map>
#include <set>
#include <mutex>
#include "core/host_runtime/guest_http.h"
#include "core/libraries/np/np_web_api/np_web_api.h"
#include "core/libraries/np/np_error.h"
#include "core/emulator_settings.h"
namespace Core::HostRuntime {
// Session-owned local controls and offline request lifecycle. No online delivery.
// Push delivery and desktop memory-pool statistics remain unadmitted.
inline constexpr std::string_view NpWebApiControlNids[]{
    "G3AnLNdRBjE", "asz3TtIqGF8", "79M-JqvvGo0", "5Mn7TYwpl30",
    "M2BUB+DNEGE", "pfaJtb7SQ80", "or0e885BlXo", "gVNNyxf-1Sg",
    "zk6c65xoyO0", "XUjdsSTTZ3U", "rdgs5Z1MyFw", "noQgleu+KLE",
    "qWcbJkBj1Lg", "KjNeZ-29ysQ", "CQtPRSF6Ds8", "VwJ5L0Higg0", "743ZzEBzlV8"};
inline bool IsNpWebApiControlNid(std::string_view nid) {
    return std::ranges::find(NpWebApiControlNids,nid)!=std::end(NpWebApiControlNids);
}
class GuestNpWebApiControl {
    using Param = Libraries::Np::NpWebApi::OrbisNpWebApiExtdPushEventFilterParameter;
    using Key = Libraries::Np::NpWebApi::OrbisNpWebApiExtdPushEventExtdDataKey;
    struct FilterStorage { std::vector<Param> params; std::vector<std::vector<Key>> keys; };
    struct Context {
        std::set<s32> handles, users;
        std::map<s32,FilterStorage> filters;
        std::map<s64,u32> requests; // native request -> last send error (never fake EOF)
    };
    GuestHttp& http;
    std::mutex mutex;
    std::map<s32,Context> contexts;
public:
    explicit GuestNpWebApiControl(GuestHttp& http) : http(http) {}
    // This is a void periodic control pump, not an online request or a fake
    // successful completion. Reuse desktop's actual timeout processing; the
    // runtime bridge preserves RAX and passes no guest pointers to it.
    void CheckTimeout() {
        std::lock_guard lock(mutex);
        Libraries::Np::NpWebApi::sceNpWebApiCheckTimeout();
    }
    ~GuestNpWebApiControl() {
        for (auto& [id, context] : contexts)
            Libraries::Np::NpWebApi::RetireOfflineControlContext(id);
    }
    u32 Dispatch(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                 const std::array<u64,6>& a) {
        using namespace Libraries::Np::NpWebApi;
        using namespace GuestCpu;
        constexpr u32 bad = ORBIS_NP_WEBAPI_ERROR_INVALID_ARGUMENT;
        std::lock_guard lock(mutex);
        if (nid=="or0e885BlXo") {
            auto text=HttpGuestText(space,a[0],1024); if (!text) return u32(ORBIS_NP_ERROR_INVALID_ARGUMENT);
            Libraries::Np::OrbisNpId value{};
            const auto result=sceNpWebApiUtilityParseNpId(text->c_str(),a[1]?&value:nullptr);
            if (!result && a[1] && !space.WriteData({a[1]},std::as_bytes(std::span{&value,1}))) return bad;
            return u32(result);
        }
        if (nid=="G3AnLNdRBjE") {
            const auto native=http.NativeContext(s32(a[0]));
            if (!native || !a[1]) return bad;
            if (contexts.size()>=16) return u32(ORBIS_NP_WEBAPI_ERROR_LIB_CONTEXT_MAX);
            const auto id=sceNpWebApiInitialize(*native,a[1]);
            if (id>0) {
                try { contexts.try_emplace(id); }
                catch (...) { RetireOfflineControlContext(id); return u32(ORBIS_NP_WEBAPI_ERROR_OUT_OF_MEMORY); }
            }
            return u32(id);
        }
        if (nid=="zk6c65xoyO0") {
            if (s32(a[0]) >= 0x8000) return u32(ORBIS_NP_WEBAPI_ERROR_INVALID_LIB_CONTEXT_ID);
            if (s32(a[1]) == -1) return bad;
            auto it=contexts.find(s32(a[0]));
            if (it==contexts.end()) return u32(ORBIS_NP_WEBAPI_ERROR_LIB_CONTEXT_NOT_FOUND);
            if (it->second.users.size()>=16) return u32(ORBIS_NP_WEBAPI_ERROR_USER_CONTEXT_MAX);
            const s32 user=sceNpWebApiCreateContextA(s32(a[0]),s32(a[1]));
            if (user>0) {
                try { it->second.users.insert(user); }
                catch (...) { sceNpWebApiDeleteContext(user); return u32(ORBIS_NP_WEBAPI_ERROR_OUT_OF_MEMORY); }
            }
            return u32(user);
        }
        if (nid=="rdgs5Z1MyFw" || nid=="XUjdsSTTZ3U") {
            std::optional<std::string> group,path,content_type;
            OrbisNpWebApiContentParameter content{};
            std::optional<PinnedSpan> output;
            if (nid=="rdgs5Z1MyFw") {
                group=HttpGuestText(space,a[1],256); path=HttpGuestText(space,a[2],8192);
                if (!group || !path || u32(a[3])>3 || !a[5]) return bad;
                if (a[4]) {
                    if (!space.ReadData({a[4]},std::as_writable_bytes(std::span{&content,1}))) return bad;
                    if (content.contentLength && !content.pContentType)
                        return u32(ORBIS_NP_WEBAPI_ERROR_INVALID_CONTENT_PARAMETER);
                    if (content.pContentType) {
                        content_type=HttpGuestText(space,reinterpret_cast<u64>(content.pContentType),256);
                        if (!content_type) return bad;
                        content.pContentType=content_type->c_str();
                    }
                }
                auto pin=space.AcquireDataSpan({{a[5]},sizeof(s64)},true);
                if (!pin) return bad;
                output.emplace(std::move(pin).Value());
            }
            const s32 user=s32(a[0]);
            auto it=contexts.find(user>>16);
            if (it==contexts.end()) return u32(ORBIS_NP_WEBAPI_ERROR_LIB_CONTEXT_NOT_FOUND);
            auto& context=it->second;
            if (!context.users.contains(user)) return u32(ORBIS_NP_WEBAPI_ERROR_USER_CONTEXT_NOT_FOUND);
            if (nid=="XUjdsSTTZ3U") {
                const auto result=sceNpWebApiDeleteContext(user);
                if (!result) {
                    context.users.erase(user);
                    std::erase_if(context.requests,[&](const auto& pair) { return s32(u64(pair.first)>>32)==user; });
                }
                return u32(result);
            }
            if (context.requests.size()>=128) return u32(ORBIS_NP_WEBAPI_ERROR_OUT_OF_MEMORY);
            s64 request{};
            const auto result=sceNpWebApiCreateRequest(user,group->c_str(),path->c_str(),
                static_cast<OrbisNpWebApiHttpMethod>(a[3]),a[4]?&content:nullptr,&request);
            if (!result) {
                try { context.requests.emplace(request,0); }
                catch (...) { sceNpWebApiDeleteRequest(request); return u32(ORBIS_NP_WEBAPI_ERROR_OUT_OF_MEMORY); }
                std::memcpy(output->WritableBytes().data(),&request,sizeof(request));
            }
            return u32(result);
        }
        if (nid=="noQgleu+KLE" || nid=="qWcbJkBj1Lg" || nid=="KjNeZ-29ysQ" ||
            nid=="CQtPRSF6Ds8" || nid=="VwJ5L0Higg0" || nid=="743ZzEBzlV8") {
            const s64 request=s64(a[0]);
            auto it=contexts.find(s32(a[0]>>48));
            if (it==contexts.end()) return u32(ORBIS_NP_WEBAPI_ERROR_LIB_CONTEXT_NOT_FOUND);
            auto& context=it->second;
            if (!context.users.contains(s32(a[0]>>32))) return u32(ORBIS_NP_WEBAPI_ERROR_USER_CONTEXT_NOT_FOUND);
            auto found=context.requests.find(request);
            if (found==context.requests.end()) return u32(ORBIS_NP_WEBAPI_ERROR_REQUEST_NOT_FOUND);
            if (nid=="noQgleu+KLE") {
                const auto result=sceNpWebApiDeleteRequest(request);
                if (!result) context.requests.erase(found);
                return u32(result);
            }
            if (nid=="qWcbJkBj1Lg") return u32(sceNpWebApiSetRequestTimeout(request,u32(a[1])));
            if (nid=="KjNeZ-29ysQ") {
                // No guest pointer is passed to the desktop API. Offline send
                // exits before using a body or writing response information.
                std::optional<PinnedSpan> data;
                if (a[2]) {
                    if (!a[1] || a[2]>(16u<<20)) return bad;
                    auto pin=space.AcquireDataSpan({{a[1]},a[2]},false);
                    if (!pin) return bad;
                    data.emplace(std::move(pin).Value());
                }
                if (a[3]) {
                    OrbisNpWebApiResponseInformationOption option{};
                    if (!space.ReadData({a[3]},std::as_writable_bytes(std::span{&option,1}))) return bad;
                    auto pin=space.AcquireDataSpan({{a[3]},sizeof(option)},true);
                    if (!pin) return bad;
                    if (option.errorObjectSize) {
                        auto error=space.AcquireDataSpan({{reinterpret_cast<u64>(option.pErrorObject)},option.errorObjectSize},true);
                        if (!error) return bad;
                    }
                }
                // This provider is only admitted by the runtime's offline gate.
                // Also fail closed if the global setting changes underneath it.
                if (EmulatorSettings.IsShadNetEnabled()) return u32(ORBIS_NP_WEBAPI_ERROR_PROHIBITED_FUNCTION_CALL);
                const auto result=sceNpWebApiSendRequest2(request,nullptr,0,nullptr);
                found->second=u32(result);
                return u32(result);
            }
            if (nid=="CQtPRSF6Ds8") {
                if (!a[1] || !a[2]) return bad;
                auto pin=space.AcquireDataSpan({{a[1]},a[2]},true);
                if (!pin) return bad;
            } else {
                auto name=HttpGuestText(space,a[1],256); if (!name || !a[2]) return bad;
                const u64 size=nid=="743ZzEBzlV8"?sizeof(u64):a[3];
                if (!size) return bad;
                auto pin=space.AcquireDataSpan({{a[2]},size},true);
                if (!pin) return bad;
            }
            // A rejected send has no response: preserve its real error and all
            // output bytes. The desktop read helper otherwise turns HTTP errors
            // into zero-byte success, which would falsely complete this request.
            return found->second ? found->second : u32(ORBIS_HTTP_ERROR_INVALID_ID);
        }
        auto it=contexts.find(s32(a[0]));
        if (it==contexts.end()) return u32(ORBIS_NP_WEBAPI_ERROR_LIB_CONTEXT_NOT_FOUND);
        auto& context=it->second;
        const auto id=it->first;
        if (nid=="asz3TtIqGF8") {
            const auto result=RetireOfflineControlContext(id);
            if (!result) contexts.erase(it);
            return u32(result);
        }
        if (nid=="79M-JqvvGo0") {
            if (context.handles.size()>=128) return u32(ORBIS_NP_WEBAPI_ERROR_OUT_OF_MEMORY);
            const auto handle=sceNpWebApiCreateHandle(id);
            if (handle>0) {
                try { context.handles.insert(handle); }
                catch (...) { sceNpWebApiDeleteHandle(id,handle); return u32(ORBIS_NP_WEBAPI_ERROR_OUT_OF_MEMORY); }
            }
            return u32(handle);
        }
        if (nid=="5Mn7TYwpl30") {
            if (!context.handles.contains(s32(a[1]))) return u32(ORBIS_NP_WEBAPI_ERROR_HANDLE_NOT_FOUND);
            const auto result=sceNpWebApiDeleteHandle(id,s32(a[1]));
            if (!result) context.handles.erase(s32(a[1]));
            return u32(result);
        }
        if (nid=="pfaJtb7SQ80") {
            if (!context.filters.contains(s32(a[1]))) return u32(ORBIS_NP_WEBAPI_ERROR_EXTD_PUSH_EVENT_FILTER_NOT_FOUND);
            const auto result=sceNpWebApiDeleteExtdPushEventFilter(id,s32(a[1]));
            if (!result) context.filters.erase(s32(a[1]));
            return u32(result);
        }
        if (!context.handles.contains(s32(a[1]))) return u32(ORBIS_NP_WEBAPI_ERROR_HANDLE_NOT_FOUND);
        if (!a[4] || !a[5] || a[5]>64 || (a[2] && u32(a[3])==UINT32_MAX)) return bad;
        if (context.filters.size()>=128) return u32(ORBIS_NP_WEBAPI_ERROR_OUT_OF_MEMORY);
        std::optional<std::string> service;
        if (a[2]) { service=HttpGuestText(space,a[2],256); if (!service) return bad; }
        struct GuestParam { char type[65]; u8 pad[7]; u64 keys,count; };
        static_assert(sizeof(GuestParam)==88);
        FilterStorage storage;
        storage.params.resize(a[5]); storage.keys.resize(a[5]);
        for (size_t i=0;i<a[5];++i) {
            GuestParam p{};
            if (a[4]>UINT64_MAX-(i+1)*sizeof(p) ||
                !space.ReadData({a[4]+i*sizeof(p)},std::as_writable_bytes(std::span{&p,1})) ||
                !std::memchr(p.type,0,sizeof(p.type)) || p.count>64 || (p.count && !p.keys)) return bad;
            auto& keys=storage.keys[i]; keys.resize(p.count);
            if (p.count && !space.ReadData({p.keys},std::as_writable_bytes(std::span{keys}))) return bad;
            for (auto& key:keys) if (!std::memchr(key.val,0,sizeof(key.val))) return bad;
            auto& param=storage.params[i];
            std::memcpy(param.dataType.val,p.type,65);
            param.pExtdDataKey=keys.empty()?nullptr:keys.data(); param.extdDataKeyNum=keys.size();
        }
        const auto filter=sceNpWebApiCreateExtdPushEventFilter(id,s32(a[1]),service?service->c_str():nullptr,
            u32(a[3]),storage.params.data(),storage.params.size());
        if (filter>0) {
            try { context.filters.emplace(filter,std::move(storage)); }
            catch (...) { sceNpWebApiDeleteExtdPushEventFilter(id,filter); return u32(ORBIS_NP_WEBAPI_ERROR_OUT_OF_MEMORY); }
        }
        return u32(filter);
    }
};
}
