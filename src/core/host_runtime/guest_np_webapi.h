// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <map>
#include <set>
#include <mutex>
#include "core/host_runtime/guest_http.h"
#include "core/libraries/np/np_web_api/np_web_api.h"
#include "core/libraries/np/np_error.h"
namespace Core::HostRuntime {
// Local control lifecycle. Online user/request/push delivery and unimplemented
// desktop memory-pool statistics remain explicitly unadmitted.
inline constexpr std::string_view NpWebApiControlNids[]{
    "G3AnLNdRBjE", "asz3TtIqGF8", "79M-JqvvGo0", "5Mn7TYwpl30",
    "M2BUB+DNEGE", "pfaJtb7SQ80", "or0e885BlXo", "gVNNyxf-1Sg"};
inline bool IsNpWebApiControlNid(std::string_view nid) {
    return std::ranges::find(NpWebApiControlNids,nid)!=std::end(NpWebApiControlNids);
}
class GuestNpWebApiControl {
    using Param = Libraries::Np::NpWebApi::OrbisNpWebApiExtdPushEventFilterParameter;
    using Key = Libraries::Np::NpWebApi::OrbisNpWebApiExtdPushEventExtdDataKey;
    struct FilterStorage { std::vector<Param> params; std::vector<std::vector<Key>> keys; };
    struct Context { std::set<s32> handles; std::map<s32,FilterStorage> filters; };
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
