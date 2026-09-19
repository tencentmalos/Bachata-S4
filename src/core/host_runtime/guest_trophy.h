// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_trophy.h"
namespace Core::HostRuntime {
inline constexpr std::string_view TrophyNids[]{"XbkjbobZlCY","q7U6tEAQf7c","E1Wrwd07Lr8","GNcF4oidY0Y","aTnHs7W-9Uk","TJCAxto9SEU","YYP3f2W09og","wTUwGfspKic","qqUVGDgQBm0","LHuSmO3SLd8","28xmRUFao68"};
inline bool IsTrophyNid(std::string_view nid) {return std::ranges::find(TrophyNids,nid)!=std::end(TrophyNids);}
class GuestTrophy {
    using Space=GuestCpu::GuestAddressSpace;
    std::mutex mutex;
    std::set<s32> contexts,handles;
    // Local XML I/O must not retain any guest range pin. Capture complete mapping
    // identities before calling desktop, then atomically admit all writebacks.
    template<class A,class B,class F>
    static u32 Query(Space& space,u64 aa,u64 ba,F&& invoke,bool read_initial=true) {
        using namespace GuestCpu;
        const auto perm=read_initial ? GuestPermission::Read|GuestPermission::Write : GuestPermission::Write;
        std::array<Space::DataRequest,2> req{{{{GuestAddress{aa},sizeof(A)},perm},{{GuestAddress{ba},sizeof(B)},perm}}};
        std::array<std::vector<Space::MappingIdentity>,2> identities;
        A a{};B b{};
        {
            auto pins=space.AcquireDataBatch(req);
            if (!pins) return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;
            if (read_initial) {
                std::memcpy(&a,pins.Value()[0].Bytes().data(),sizeof(a));
                std::memcpy(&b,pins.Value()[1].Bytes().data(),sizeof(b));
            }
            for (unsigned i=0;i<2;++i) {
                for (u64 p=req[i].range.base.value;p<req[i].range.End();) {
                    auto m=space.Query(GuestAddress{p});if (!m) return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;
                    const auto end=std::min(req[i].range.End(),m.Value().range.End());
                    identities[i].push_back({p,end,m.Value().mapping_generation});p=end;
                }
            }
        }
        const auto rc=invoke(&a,&b);
        if (rc) return rc;
        for (unsigned i=0;i<2;++i) { req[i].permission=GuestPermission::Write;req[i].identities=identities[i]; }
        auto pins=space.AcquireDataBatch(req);if (!pins) return ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;
        std::memcpy(pins.Value()[0].WritableBytes().data(),&a,sizeof(a));
        std::memcpy(pins.Value()[1].WritableBytes().data(),&b,sizeof(b));
        return 0;
    }
public:
    ~GuestTrophy() {
        using namespace Libraries::Np::NpTrophy;
        for (auto h:handles) sceNpTrophyDestroyHandle(h);
        for (auto c:contexts) sceNpTrophyDestroyContext(c);
    }
    u32 Dispatch(Space& space,std::string_view nid,const std::array<u64,6>& a) {
        using namespace GuestCpu;
        using namespace Libraries::Np::NpTrophy;
        std::lock_guard lock(mutex);
        constexpr u32 bad=ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT;
        if (nid=="XbkjbobZlCY" || nid=="q7U6tEAQf7c") {
            auto pin=space.AcquireDataSpan({GuestAddress{a[0]},4},true);if (!pin) return bad;
            s32 id{};const auto rc=nid=="XbkjbobZlCY" ? sceNpTrophyCreateContext(&id,a[1],a[2],a[3]):sceNpTrophyCreateHandle(&id);
            if (!rc) { (nid=="XbkjbobZlCY"?contexts:handles).insert(id);std::memcpy(pin.Value().WritableBytes().data(),&id,4); }
            return rc;
        }
        if (nid=="GNcF4oidY0Y" || nid=="aTnHs7W-9Uk") {
            if (!handles.contains(s32(a[0]))) return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
            if (nid=="aTnHs7W-9Uk") return sceNpTrophyAbortHandle(a[0]);
            const auto rc=sceNpTrophyDestroyHandle(a[0]);if (!rc) handles.erase(a[0]);return rc;
        }
        if (!contexts.contains(s32(a[0]))) return ORBIS_NP_TROPHY_ERROR_INVALID_CONTEXT;
        if (nid=="E1Wrwd07Lr8") {
            const auto rc=sceNpTrophyDestroyContext(a[0]);if (!rc) contexts.erase(a[0]);return rc;
        }
        if (!handles.contains(s32(a[1]))) return ORBIS_NP_TROPHY_ERROR_INVALID_HANDLE;
        if (nid=="TJCAxto9SEU") return sceNpTrophyRegisterContext(a[0],a[1],a[2]);
        if (nid=="YYP3f2W09og") return Query<OrbisNpTrophyGameDetails,OrbisNpTrophyGameData>(space,a[2],a[3],[&](auto* d,auto* v){return sceNpTrophyGetGameInfo(a[0],a[1],d,v);});
        if (nid=="wTUwGfspKic") return Query<OrbisNpTrophyGroupDetails,OrbisNpTrophyGroupData>(space,a[3],a[4],[&](auto* d,auto* v){return sceNpTrophyGetGroupInfo(a[0],a[1],a[2],d,v);});
        if (nid=="qqUVGDgQBm0") return Query<OrbisNpTrophyDetails,OrbisNpTrophyData>(space,a[3],a[4],[&](auto* d,auto* v){return sceNpTrophyGetTrophyInfo(a[0],a[1],a[2],d,v);});
        if (nid=="LHuSmO3SLd8") return Query<OrbisNpTrophyFlagArray,u32>(space,a[2],a[3],[&](auto* d,auto* v){return sceNpTrophyGetTrophyUnlockState(a[0],a[1],d,v);},false);
        if (nid=="28xmRUFao68") {
            if (s32(a[2])<0) return ORBIS_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
            // Single output variant, keeping the same mapping identity on return.
            s32 value{};std::vector<Space::MappingIdentity> identities;
            { Space::DataRequest input{{GuestAddress{a[3]},4},GuestPermission::Read|GuestPermission::Write};
              auto pins=space.AcquireDataBatch(std::span{&input,1});if (!pins) return bad;
              std::memcpy(&value,pins.Value()[0].Bytes().data(),4);
              for (u64 p=a[3];p<a[3]+4;) { auto m=space.Query(GuestAddress{p});if (!m) return bad;auto end=std::min(a[3]+4,m.Value().range.End());identities.push_back({p,end,m.Value().mapping_generation});p=end; } }
            const auto rc=UnlockTrophy(a[0],a[1],a[2],&value,false);if (rc) return rc;
            Space::DataRequest req{{GuestAddress{a[3]},4},GuestPermission::Write,identities};
            auto pins=space.AcquireDataBatch(std::span{&req,1});if (!pins) return bad;
            std::memcpy(pins.Value()[0].WritableBytes().data(),&value,4);return 0;
        }
        return bad;
    }
};
}
