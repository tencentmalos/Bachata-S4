// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include "common/elf_info.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/host_runtime/guest_trophy.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::Np::NpTrophy;
static unsigned checks{},failures{};
#define CHECK(...) do {++checks;if (!(__VA_ARGS__)) {++failures;std::printf("FAIL line %d: %s\n",__LINE__,#__VA_ARGS__);}} while(0)
int main() {
    const auto root=std::filesystem::current_path()/"cache"/("trophy-test-"+std::to_string(getpid()));
    if (std::filesystem::exists(root)) return 2;
    Common::FS::InitializeAndroidUserPaths(root);
    EmulatorSettings.SetHomeDir(root/"home");EmulatorSettings.SetShadNetEnabled(false);
    auto& elf=Common::ElfInfo::Instance();elf.InitializeGuestMetadata(root,0x3500000,"CUSA99991");
    elf.SetTrophyIndexMap({{0,"NPWRTEST00001"}});
    const auto xml=Common::FS::GetUserPath(Common::FS::PathType::TrophyDir)/"NPWRTEST00001/Xml";
    std::filesystem::create_directories(xml);
    const auto user=root/"home/1000/trophy";std::filesystem::create_directories(user);
    const char* text=R"(<trophyconf><title-name>Offline Test</title-name><title-detail>Local trophies</title-detail><trophy id="0" ttype="P" gid="-1" hidden="no" unlockstate="false"><name>All</name><detail>All</detail></trophy><trophy id="1" ttype="B" gid="-1" hidden="no" unlockstate="false"><name>First</name><detail>First</detail></trophy><trophy id="2" ttype="B" gid="-1" hidden="no" unlockstate="false"><name>Second</name><detail>Second</detail></trophy></trophyconf>)";
    std::ofstream(xml/"TROP.XML")<<text;
    std::ofstream(user/"NPWRTEST00001.xml")<<text;
    AddressSpaceConfig config{};config.reservation_size=16<<20;
    auto made=GuestAddressSpace::Create(config);if (!made) return 2;auto space=std::move(made).Value();auto base=space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base},0x8000},GuestPermission::Read|GuestPermission::Write));
    auto put=[&]<class T>(u64 p,const T& v){CHECK(space->WriteData(GuestAddress{p},std::as_bytes(std::span{&v,1})));};
    auto get=[&]<class T>(u64 p){T v{};CHECK(space->ReadData(GuestAddress{p},std::as_writable_bytes(std::span{&v,1})));return v;};
    for(int round=0;round<3;++round) {
        GuestTrophy domain;
        auto call=[&](std::string_view nid,std::array<u64,6> a={}){CHECK(IsTrophyNid(nid));return domain.Dispatch(*space,nid,a);};
        CHECK(call("XbkjbobZlCY",{1,1000,0,0})==u32(ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT));
        CHECK(call("XbkjbobZlCY",{base,1000,0,0})==0);auto ctx=get.operator()<s32>(base);
        CHECK(call("q7U6tEAQf7c",{base+4})==0);auto h=get.operator()<s32>(base+4);
        CHECK(call("TJCAxto9SEU",{u64(ctx),u64(h),0})==0);
        CHECK(call("TJCAxto9SEU",{u64(ctx),u64(h),0})==u32(ORBIS_NP_TROPHY_ERROR_ALREADY_REGISTERED));
        OrbisNpTrophyGameDetails detail{};detail.size=sizeof(detail);put(base+256,detail);
        OrbisNpTrophyGameData data{};data.size=sizeof(data);put(base+2048,data);
        CHECK(call("YYP3f2W09og",{u64(ctx),u64(h),base+256,1})==u32(ORBIS_NP_TROPHY_ERROR_INVALID_ARGUMENT));
        CHECK(get.operator()<OrbisNpTrophyGameDetails>(base+256).num_trophies==0);
        CHECK(call("YYP3f2W09og",{u64(ctx),u64(h),base+256,base+2048})==0);
        CHECK(get.operator()<OrbisNpTrophyGameDetails>(base+256).num_trophies==3);
        CHECK(call("LHuSmO3SLd8",{u64(ctx),u64(h),base+4096,base+4200})==0);
        CHECK(get.operator()<u32>(base+4200)==3);
        OrbisNpTrophyDetails td{};td.size=sizeof(td);put(base+256,td);
        OrbisNpTrophyData tv{};tv.size=sizeof(tv);put(base+2048,tv);
        CHECK(call("qqUVGDgQBm0",{u64(ctx),u64(h),1,base+256,base+2048})==0);
        CHECK(get.operator()<OrbisNpTrophyDetails>(base+256).trophy_id==1);
        if(!round) { put(base+4300,s32{-1});CHECK(call("28xmRUFao68",{u64(ctx),u64(h),1,base+4300})==0); }
        CHECK(call("qqUVGDgQBm0",{u64(ctx),u64(h),1,base+256,base+2048})==0);
        CHECK(get.operator()<OrbisNpTrophyData>(base+2048).unlocked);
        CHECK(call("aTnHs7W-9Uk",{u64(h)})==0);
        if(round!=1) {CHECK(call("GNcF4oidY0Y",{u64(h)})==0);CHECK(call("E1Wrwd07Lr8",{u64(ctx)})==0);}
    }
    std::printf("guest_trophy_tests: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
