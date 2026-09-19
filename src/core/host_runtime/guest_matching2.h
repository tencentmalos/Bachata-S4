// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <map>
#include <cstring>
#include "core/libraries/np/np_error.h"
#include <mutex>
#include <set>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/np/np_matching2/np_matching2.h"
namespace Core::HostRuntime {
inline constexpr std::string_view Matching2Nids[]{
    "10t3e5+JPnU", "Mqp3lJ+sjy4", "YfmpW719rMo", "ajvzc8e2upo",
    "fQQfP87I7hs", "4Nj7u5B5yCA", "p+2EnxmaAMM", "0UMeWRGnZKA",
    "DnPUsBAe8oI", "uBESzz4CQws", "+8e7wXLmjds", "7vjNQ6Z1op0",
    "-f6M4caNe8k", "LhCPctIICxQ", "pFzhpCMlJXQ", "Nz-ZE7ur32I"};
inline bool IsMatching2Nid(std::string_view nid) {
    return std::ranges::find(Matching2Nids,nid)!=std::end(Matching2Nids);
}
// Admitted only under immutable Session offline policy. Desktop ContextStart
// refuses offline operation synchronously, so no callback event can be produced.
// Guest callback addresses are retained as guest values, never native callables.
class GuestMatching2Offline {
    std::mutex mutex;
    bool initialized{};
    std::set<u16> contexts;
    std::map<std::pair<u16,std::string_view>,std::array<u64,2>> callbacks;
public:
    ~GuestMatching2Offline() { if (initialized) Libraries::Np::NpMatching2::sceNpMatching2Terminate(); }
    u32 Dispatch(GuestCpu::GuestAddressSpace& space,std::string_view nid,const std::array<u64,6>& a) {
        using namespace GuestCpu;
        using namespace Libraries::Np::NpMatching2;
        constexpr u32 bad = ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
        std::lock_guard lock(mutex);
        auto read = [&]<class T>(u64 p,T& v) { return p && bool(space.ReadData(GuestAddress{p},std::as_writable_bytes(std::span{&v,1}))); };
        auto callback_valid = [&](u64 p) { return !p || bool(space.ValidateRange({GuestAddress{p},1},GuestPermission::Execute)); };
        if (nid=="10t3e5+JPnU") {
            if (initialized) return ORBIS_NP_MATCHING2_ERROR_ALREADY_INITIALIZED;
            OrbisNpMatching2InitializeParameter p{};
            // Old SDKs only supply the 0x28-byte prefix.
            if (!a[0] || !space.ReadData(GuestAddress{a[0]},std::as_writable_bytes(std::span{&p,1}).first(0x28))) return bad;
            if (p.size==0x30 && !read(a[0],p)) return bad;
            const auto rc=Initialize(&p,false);
            if (!rc) initialized=true;
            return rc;
        }
        if (!initialized) return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
        if (nid=="Mqp3lJ+sjy4") {
            const auto rc=sceNpMatching2Terminate();
            if (!rc) { initialized=false; contexts.clear(); callbacks.clear(); }
            return rc;
        }
        if (nid=="YfmpW719rMo" || nid=="ajvzc8e2upo") {
            OrbisNpMatching2CreateContextParameter p{};
            OrbisNpMatching2CreateContextParameterA pa{};
            Libraries::Np::OrbisNpId np{};
            if (nid=="YfmpW719rMo") {
                if (!read(a[0],p) || p.size!=sizeof(p) || !read(reinterpret_cast<u64>(p.npId),np)) return bad;
                p.npId=&np; p.npCommunicationId=nullptr; p.npPassphrase=nullptr;
            } else if (!read(a[0],pa)) return bad;
            auto output=space.AcquireDataSpan({GuestAddress{a[1]},2},true);
            if (!output) return bad;
            u16 id{};
            const s32 rc = nid=="YfmpW719rMo" ? sceNpMatching2CreateContext(&p,&id) : sceNpMatching2CreateContextA(&pa,&id);
            if (!rc) { contexts.insert(id); std::memcpy(output.Value().WritableBytes().data(),&id,2); }
            return rc;
        }
        if (nid=="fQQfP87I7hs") {
            if (!callback_valid(a[0])) return bad;
            callbacks[{0,nid}]={a[0],a[1]}; return 0;
        }
        // The shared desktop start path tests offline status before context ID.
        if (nid=="7vjNQ6Z1op0") return OfflineContextStartResult;
        if (a[0]>UINT16_MAX || !contexts.contains(u16(a[0]))) return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
        const u16 id=a[0];
        if (nid=="-f6M4caNe8k") return sceNpMatching2ContextStop(id);
        if (nid=="pFzhpCMlJXQ") return sceNpMatching2AbortContextStart(id);
        if (nid=="Nz-ZE7ur32I") {
            const auto rc=sceNpMatching2DestroyContext(id);
            if (!rc) { contexts.erase(id); std::erase_if(callbacks,[&](const auto& entry){return entry.first.first==id;}); }
            return rc;
        }
        if (nid=="LhCPctIICxQ") {
            auto output=space.AcquireDataSpan({GuestAddress{a[1]},2},true);
            if (!output) return bad;
            u16 result{}; const auto rc=sceNpMatching2GetServerId(id,&result);
            if (!rc) std::memcpy(output.Value().WritableBytes().data(),&result,2);
            return rc;
        }
        if (nid=="+8e7wXLmjds") {
            OrbisNpMatching2RequestOptParam p{};
            if (!read(a[1],p) || !callback_valid(reinterpret_cast<u64>(p.callback))) return bad;
            callbacks[{id,nid}]={reinterpret_cast<u64>(p.callback),reinterpret_cast<u64>(p.arg)};
            return 0;
        }
        if (!callback_valid(a[1])) return bad;
        callbacks[{id,nid}]={a[1],a[2]};
        return 0;
    }
};
}
