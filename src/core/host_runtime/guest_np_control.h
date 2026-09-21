// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/np/np_manager.h"

namespace Core::HostRuntime {
inline constexpr std::string_view NpControlNids[]{
    "0c7HbXRKUt4", // sceNpRegisterStateCallbackForToolkit
    "YIvqqvJyjEc", // sceNpUnregisterStateCallbackForToolkit
    "GpLQDNKICac", // sceNpCreateRequest
    "eiqMCt9UshI", // sceNpCreateAsyncRequest
    "2rsFmlGWleQ", // sceNpCheckNpAvailability
    "8Z2Jc5GvGDI", // sceNpCheckNpAvailabilityA
    "KfGZg2y73oM", // sceNpCheckNpReachability
    "r6MyYJkryz8", // sceNpCheckPlus
    "ilwLM4zOmu4", // sceNpGetParentalControlInfo
    "m9L3O6yst-U", // sceNpGetParentalControlInfoA
    "KZ1Mj9yEGYc", // sceNpGetAccountLanguage
    "TPMbgIxvog0", // sceNpGetAccountLanguageA
    "OzKvTvg3ZYU", // sceNpAbortRequest
    "-QglDeRr8D8", // sceNpSetTimeout
    "jyi5p9XWUSs", // sceNpWaitAsync
    "uqcPJLWL08M", // sceNpPollAsync
    "S7QTn72PrDw", // sceNpDeleteRequest
    "VfRSmPmj8Q8", // sceNpRegisterStateCallback
    "mjjTXh+NHWY", // sceNpUnregisterStateCallback
    "GImICnh+boA", // sceNpRegisterPlusEventCallback
    "xViqJdDgKl0", // sceNpUnregisterPlusEventCallback
    "uFJpaKNBAj4", // sceNpRegisterGamePresenceCallback
};
inline bool IsNpControlNid(std::string_view nid) {
    return std::ranges::find(NpControlNids, nid) != std::end(NpControlNids);
}
// Desktop owns request completion and offline policy. This layer owns guest
// admission, per-session resources, and callback translation through InvokeGuest.
class GuestNpControl {
    using Space = GuestCpu::GuestAddressSpace;
    std::mutex mutex, event_mutex, native_calls;
    std::set<s32> requests;
    struct Slot { u64 function{}, argument{}, revision{}; } state, toolkit, plus, presence;
    u64 next_revision{1};
    bool checking{};
public:
    struct Callback { u64 function, argument, revision; s32 user; u32 state; bool unsupported_identity; bool toolkit{}; };
private:
    std::vector<Callback> events;
    static void PS4_SYSV_ABI State(s32 user, Libraries::Np::NpManager::OrbisNpState value,
                                  Libraries::Np::OrbisNpId* identity, void* opaque) {
        auto& self = *static_cast<GuestNpControl*>(opaque);
        std::scoped_lock lock(self.mutex, self.event_mutex);
        if (self.state.function)
            self.events.push_back({self.state.function,self.state.argument,self.state.revision,
                                   user,u32(value),identity != nullptr});
    }
    static void PS4_SYSV_ABI Toolkit(s32 user, Libraries::Np::NpManager::OrbisNpState value,
                                     void* opaque) {
        auto& self = *static_cast<GuestNpControl*>(opaque);
        std::scoped_lock lock(self.mutex, self.event_mutex);
        if (self.toolkit.function)
            self.events.push_back({self.toolkit.function, self.toolkit.argument,
                                   self.toolkit.revision, user, u32(value), false, true});
    }
    static void PS4_SYSV_ABI Plus(s32, s32, void*) {} // desktop has no offline event producer
    static void PS4_SYSV_ABI Presence(const Libraries::Np::OrbisNpOnlineId*, void*) {}
public:
    ~GuestNpControl() {
        using namespace Libraries::Np::NpManager;
        // Runtime joins all guest/callback owners before destroying the domain.
        if (state.function) sceNpUnregisterStateCallback();
        if (toolkit.function) sceNpUnregisterStateCallbackForToolkit();
        if (plus.function) sceNpUnregisterPlusEventCallback();
        if (presence.function) sceNpRegisterGamePresenceCallback(nullptr,nullptr);
        for (const auto id : requests) sceNpDeleteRequest(id);
    }
    std::optional<std::vector<Callback>> BeginCallbacks() {
        std::lock_guard native_lock(native_calls);
        { std::lock_guard lock(mutex); if (checking) return {}; checking=true; }
        Libraries::Np::NpManager::sceNpCheckCallback(); // queues translated events only
        std::lock_guard lock(event_mutex);
        std::vector<Callback> out; out.swap(events); return out;
    }
    bool IsCurrent(const Callback& cb) { std::lock_guard lock(mutex); const auto& slot = cb.toolkit ? toolkit : state; return slot.function==cb.function && slot.revision==cb.revision; }
    void EndCallbacks() { std::lock_guard lock(mutex); checking=false; }
    u32 Dispatch(Space& space, std::string_view nid, const std::array<u64,6>& a) {
        using namespace GuestCpu;
        using namespace Libraries::Np;
        using namespace Libraries::Np::NpManager;
        constexpr u32 bad=ORBIS_NP_ERROR_INVALID_ARGUMENT;
        auto read = [&]<typename T>(u64 address, T& value) {
            return address && bool(space.ReadData(GuestAddress{address},std::as_writable_bytes(std::span{&value,1})));
        };
        std::lock_guard native_lock(native_calls);
        std::lock_guard lock(mutex);
        if (nid=="GpLQDNKICac" || nid=="eiqMCt9UshI") {
            s32 result;
            if (nid=="GpLQDNKICac") result=sceNpCreateRequest();
            else {
                OrbisNpCreateAsyncRequestParameter value{};
                if (!read(a[0],value)) return bad;
                result=sceNpCreateAsyncRequest(&value);
            }
            if (result>0) requests.insert(result);
            return u32(result);
        }
        if (nid=="VfRSmPmj8Q8" || nid=="GImICnh+boA" || nid=="uFJpaKNBAj4" || nid=="0c7HbXRKUt4") {
            if (!a[0] || !space.ValidateRange({GuestAddress{a[0]},1},GuestPermission::Execute)) return bad;
            Slot* slot = nid=="VfRSmPmj8Q8" ? &state : nid=="0c7HbXRKUt4" ? &toolkit : nid=="GImICnh+boA" ? &plus : &presence;
            s32 result=0;
            if (slot==&state) result=sceNpRegisterStateCallback(State,this);
            else if (slot==&toolkit) result=sceNpRegisterStateCallbackForToolkit(Toolkit,this);
            else if (slot==&plus) result=sceNpRegisterPlusEventCallback(Plus,this);
            else sceNpRegisterGamePresenceCallback(Presence,this);
            if (!result) *slot={a[0],a[1],next_revision++};
            return u32(result);
        }
        if (nid=="mjjTXh+NHWY" || nid=="xViqJdDgKl0" || nid=="YIvqqvJyjEc") {
            Slot* slot=nid=="mjjTXh+NHWY" ? &state : nid=="YIvqqvJyjEc" ? &toolkit : &plus;
            if (!slot->function) return u32(ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED);
            s32 result=slot==&state ? sceNpUnregisterStateCallback() : slot==&toolkit ? sceNpUnregisterStateCallbackForToolkit() : sceNpUnregisterPlusEventCallback();
            if (!result) *slot={};
            return u32(result);
        }
        const s32 id=s32(a[0]);
        if (!requests.contains(id)) return u32(ORBIS_NP_ERROR_REQUEST_NOT_FOUND);
        if (nid=="OzKvTvg3ZYU") return u32(sceNpAbortRequest(id));
        if (nid=="S7QTn72PrDw") { const s32 result=sceNpDeleteRequest(id); if (!result) requests.erase(id); return u32(result); }
        if (nid=="-QglDeRr8D8") return u32(sceNpSetTimeout(id,s32(a[1]),u32(a[2]),u32(a[3]),u32(a[4]),u32(a[5])));
        if (nid=="uqcPJLWL08M" || nid=="jyi5p9XWUSs") {
            auto pin=space.AcquireDataSpan({GuestAddress{a[1]},4},true);
            if (!a[1] || !pin) return bad;
            s32 value{}; const s32 result=nid=="uqcPJLWL08M" ? sceNpPollAsync(id,&value) : sceNpWaitAsync(id,&value);
            if (!result) std::memcpy(pin.Value().WritableBytes().data(),&value,4);
            return u32(result); // desktop offline Wait is immediate, never a host wait
        }
        if (nid=="8Z2Jc5GvGDI") return u32(sceNpCheckNpAvailabilityA(id,s32(a[1])));
        if (nid=="KfGZg2y73oM") return u32(sceNpCheckNpReachability(id,s32(a[1])));
        if (nid=="2rsFmlGWleQ") { OrbisNpOnlineId value{}; return read(a[1],value) ? u32(sceNpCheckNpAvailability(id,&value)) : bad; }
        if (nid=="r6MyYJkryz8") {
            OrbisNpCheckPlusParameter param{};
            if (!read(a[1],param)) return bad;
            auto pin=space.AcquireDataSpan({GuestAddress{a[2]},sizeof(OrbisNpCheckPlusResult)},true);
            if (!a[2] || !pin) return bad;
            OrbisNpCheckPlusResult value{}; std::memcpy(&value,pin.Value().Bytes().data(),sizeof(value));
            const s32 result=sceNpCheckPlus(id,&param,&value);
            std::memcpy(pin.Value().WritableBytes().data(),&value,sizeof(value));
            return u32(result);
        }
        if (nid=="ilwLM4zOmu4" || nid=="m9L3O6yst-U") {
            OrbisNpOnlineId online{};
            if (nid=="ilwLM4zOmu4" && !read(a[1],online)) return bad;
            const std::array<Space::DataRequest,2> outputs{{
                {{GuestAddress{a[2]},1},GuestPermission::Write},
                {{GuestAddress{a[3]},sizeof(OrbisNpParentalControlInfo)},GuestPermission::Write}}};
            if (!a[2] || !a[3]) return bad;
            auto pins=space.AcquireDataBatch(outputs); if (!pins) return bad;
            s8 age{}; OrbisNpParentalControlInfo info{};
            std::memcpy(&age,pins.Value()[0].Bytes().data(),1); std::memcpy(&info,pins.Value()[1].Bytes().data(),sizeof(info));
            const s32 result=nid=="ilwLM4zOmu4" ? sceNpGetParentalControlInfo(id,&online,&age,&info) : sceNpGetParentalControlInfoA(id,s32(a[1]),&age,&info);
            std::memcpy(pins.Value()[0].WritableBytes().data(),&age,1); std::memcpy(pins.Value()[1].WritableBytes().data(),&info,sizeof(info));
            return u32(result);
        }
        if (nid=="KZ1Mj9yEGYc" || nid=="TPMbgIxvog0") {
            OrbisNpOnlineId online{};
            if (nid=="KZ1Mj9yEGYc" && !read(a[1],online)) return bad;
            auto pin=space.AcquireDataSpan({GuestAddress{a[2]},sizeof(OrbisNpLanguageCode)},true);
            if (!a[2] || !pin) return bad;
            OrbisNpLanguageCode value{}; std::memcpy(&value,pin.Value().Bytes().data(),sizeof(value));
            const s32 result=nid=="KZ1Mj9yEGYc" ? sceNpGetAccountLanguage(id,&online,&value) : sceNpGetAccountLanguageA(id,s32(a[1]),&value);
            std::memcpy(pin.Value().WritableBytes().data(),&value,sizeof(value)); return u32(result);
        }
        return bad;
    }
};
} // namespace Core::HostRuntime
