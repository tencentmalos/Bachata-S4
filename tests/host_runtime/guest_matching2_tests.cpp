// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/host_runtime/guest_matching2.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::Np::NpMatching2;
static unsigned checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); } } while(0)
int main() {
    Common::FS::InitializeAndroidUserPaths(std::filesystem::current_path()/"cache"/("matching-test-"+std::to_string(getpid())));
    EmulatorSettings.SetConnectedToNetwork(false); EmulatorSettings.SetShadNetEnabled(false);
    AddressSpaceConfig config{}; config.reservation_size = 16 << 20;
    auto made=GuestAddressSpace::Create(config); if (!made) return 2;
    auto space=std::move(made).Value(); const u64 base=space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base},0x4000},GuestPermission::Read|GuestPermission::Write));
    auto put=[&]<class T>(u64 p,const T& v) {CHECK(space->WriteData(GuestAddress{p},std::as_bytes(std::span{&v,1})));};
    auto get=[&]<class T>(u64 p) { T v{}; CHECK(space->ReadData(GuestAddress{p},std::as_writable_bytes(std::span{&v,1}))); return v;};
    for (int round=0;round<3;++round) {
        GuestMatching2Offline domain;
        auto call=[&](std::string_view nid,std::array<u64,6> a={}) {CHECK(IsMatching2Nid(nid));return domain.Dispatch(*space,nid,a);};
        CHECK(call("Mqp3lJ+sjy4")==u32(ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED));
        CHECK(call("10t3e5+JPnU",{1})==u32(ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT));
        OrbisNpMatching2InitializeParameter init{};init.size=round?0x30:0x28;
        const auto addr=round?base:base+0x4000-0x28;
        CHECK(space->WriteData(GuestAddress{addr},std::as_bytes(std::span{&init,1}).first(init.size)));
        CHECK(call("10t3e5+JPnU",{addr})==0);
        CHECK(call("10t3e5+JPnU",{addr})==u32(ORBIS_NP_MATCHING2_ERROR_ALREADY_INITIALIZED));
        CHECK(call("fQQfP87I7hs",{1,0})==u32(ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT));
        CHECK(call("fQQfP87I7hs",{0,0x1234})==0);
        Libraries::Np::OrbisNpId np{}; std::memcpy(np.handle.data,"LocalUser",10);put(base+256,np);
        OrbisNpMatching2CreateContextParameter create{};create.size=sizeof(create);create.npId=reinterpret_cast<Libraries::Np::OrbisNpId*>(base+256);put(base+128,create);
        CHECK(call("YfmpW719rMo",{base+128,1})==u32(ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT));
        CHECK(call("YfmpW719rMo",{base+128,base+512})==0);
        const auto id=get.operator()<u16>(base+512); CHECK(id==1);
        CHECK(call("7vjNQ6Z1op0",{id,100})==u32(sceNpMatching2ContextStart(id,100)));
        CHECK(call("-f6M4caNe8k",{id})==u32(ORBIS_NP_MATCHING2_ERROR_CONTEXT_NOT_STARTED));
        CHECK(call("LhCPctIICxQ",{id,base+514})==0 && get.operator()<u16>(base+514)==1);
        for (auto nid:{"4Nj7u5B5yCA","p+2EnxmaAMM","0UMeWRGnZKA","DnPUsBAe8oI","uBESzz4CQws"}) CHECK(call(nid,{id,0,0})==0);
        OrbisNpMatching2RequestOptParam opt{};put(base+128,opt);
        CHECK(call("+8e7wXLmjds",{id,base+128})==0);
        CHECK(call("pFzhpCMlJXQ",{id})==0);
        CHECK(call("Nz-ZE7ur32I",{id})==0);
        CHECK(call("Nz-ZE7ur32I",{id})==u32(ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID));
        if (round!=1) CHECK(call("Mqp3lJ+sjy4")==0);
    }
    std::printf("guest_matching2_tests: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
