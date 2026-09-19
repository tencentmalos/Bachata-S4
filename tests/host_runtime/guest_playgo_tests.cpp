// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/file_format/playgo_chunk.h"
#include "core/file_sys/fs.h"
#include "core/host_runtime/guest_playgo.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::PlayGo;
static unsigned checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); } } while(0)
int main() {
    const auto root = std::filesystem::current_path()/"cache"/("playgo-test-"+std::to_string(getpid()));
    if (std::filesystem::exists(root)) return 2;
    Common::FS::InitializeAndroidUserPaths(root);
    std::filesystem::create_directories(root/"content/sce_sys");
    Common::Singleton<Core::FileSys::MntPoints>::Instance()->Mount(root/"content", "/app0");
    AddressSpaceConfig config{}; config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config); if (!made) return 2;
    auto space = std::move(made).Value(); const u64 base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 0x204000}, GuestPermission::Read | GuestPermission::Write));
    auto put = [&]<typename T>(u64 p, const T& v) { CHECK(space->WriteData(GuestAddress{p}, std::as_bytes(std::span{&v,1}))); };
    auto read = [&]<typename T>(u64 p) { T v{}; CHECK(space->ReadData(GuestAddress{p}, std::as_writable_bytes(std::span{&v,1}))); return v; };
    OrbisPlayGoInitParams init{reinterpret_cast<void*>(base+0x4000),0x200000,0}; put(base,init);
    auto fixtures = [&](bool with_chunks) {
        if (!with_chunks) return;
        std::vector<u8> bytes(400);
        PlaygoHeader h{}; h.magic=PLAYGO_MAGIC; h.file_size=bytes.size(); h.chunk_count=2; h.mchunk_count=2;
        h.chunk_attrs={256,64}; h.chunk_mchunks={320,4}; h.chunk_labels={324,4}; h.mchunk_attrs={328,32};
        std::memcpy(bytes.data(),&h,sizeof(h));
        for (u16 i=0;i<2;++i) {
            playgo_chunk_attr_entry_t chunk{}; chunk.mchunk_count=1; chunk.mchunks_offset=i*2; chunk.label_offset=i*2;
            std::memcpy(bytes.data()+256+i*32,&chunk,sizeof(chunk));
            std::memcpy(bytes.data()+320+i*2,&i,2);
            bytes[324+i*2]='a'+i;
            playgo_mchunk_attr_entry_t mch{}; mch.size.size=(i+1)*1234;
            std::memcpy(bytes.data()+328+i*16,&mch,sizeof(mch));
        }
        std::ofstream f(root/"content/sce_sys/playgo-chunk.dat",std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    };
    for (int round=0;round<3;++round) {
        fixtures(round != 0);
        GuestPlayGo domain;
        auto call = [&](std::string_view nid,std::array<u64,6> a={}) { CHECK(IsPlayGoNid(nid)); return domain.Dispatch(*space,nid,a); };
        CHECK(call("MPe0EeBGM-E")==u32(ORBIS_PLAYGO_ERROR_NOT_INITIALIZED));
        CHECK(call("ts6GlZOKRrE",{1})==u32(ORBIS_PLAYGO_ERROR_BAD_POINTER));
        CHECK(call("ts6GlZOKRrE",{base})==0);
        CHECK(call("ts6GlZOKRrE",{base})==u32(ORBIS_PLAYGO_ERROR_ALREADY_INITIALIZED));
        CHECK(call("M1Gma1ocrGE",{base+128,1})==u32(ORBIS_PLAYGO_ERROR_INVALID_ARGUMENT));
        CHECK(call("M1Gma1ocrGE",{base+128})==u32(round ? 0 : ORBIS_PLAYGO_ERROR_NOT_SUPPORT_PLAYGO));
        if (round) CHECK(read.operator()<u32>(base+128)==1);
        CHECK(call("73fF1MFU8hA",{1,0,0,base+132})==0);
        CHECK(read.operator()<u32>(base+132)==(round ? 2:0));
        CHECK(call("73fF1MFU8hA",{1,base+160,2,base+132})==0);
        if (round) CHECK(read.operator()<u32>(base+160)==0x10000);
        put(base+160,std::array<u16,2>{0,1});
        CHECK(call("v6EZ-YWRdMs",{1,base+160,2,base+200})==0 && read.operator()<s64>(base+200)==0);
        CHECK(call("-RJWNMK3fC8",{1,base+160,2,base+208})==u32(round?0:ORBIS_PLAYGO_ERROR_BAD_CHUNK_ID));
        if (round) { auto p=read.operator()<OrbisPlayGoProgress>(base+208); CHECK(p.totalSize==3702 && p.progressSize==3702); }
        CHECK(call("uWIYLFkkwqk",{1,base+160,2,base+240})==u32(round?0:ORBIS_PLAYGO_ERROR_NOT_SUPPORT_PLAYGO));
        if (round) CHECK(read.operator()<u16>(base+240)==0x303);
        CHECK(call("-Q1-u1a7p0g",{1,base+160,2,1})==u32(ORBIS_PLAYGO_ERROR_BAD_LOCUS));
        CHECK(call("-Q1-u1a7p0g",{1,base+160,2,3})==0);
        CHECK(call("4AAcTU9R3XM",{1,2})==0);
        CHECK(call("rvBSfTimejE",{1,base+240})==0 && read.operator()<u32>(base+240)==2);
        CHECK(call("LosLlHOpNqQ",{1,0x8000000000000001ULL})==0);
        CHECK(call("3OMbYZBaa50",{1,base+240})==0 && read.operator()<u64>(base+240)==0x8000000000000001ULL);
        CHECK(call("Nn7zKwnA5q0",{1,base+240,2,base+132})==0 && read.operator()<u32>(base+132)==0);
        CHECK(call("gUPGiOQ1tmQ",{1,base+240,2})==0);
        CHECK(call("uWIYLFkkwqk",{1,base+160,UINT64_MAX,base+240})==u32(ORBIS_PLAYGO_ERROR_BAD_SIZE));
        put(base+240,u64{0xfeed});
        CHECK(call("73fF1MFU8hA",{1,base+240,2,1})==u32(ORBIS_PLAYGO_ERROR_BAD_POINTER));
        CHECK(read.operator()<u64>(base+240)==0xfeed); // no first-output mutation on second-output rejection
        if (round) {
            put(base+160,std::array<u16,2>{0,2});
            CHECK(call("uWIYLFkkwqk",{1,base+160,2,base+240})==u32(ORBIS_PLAYGO_ERROR_BAD_CHUNK_ID));
            CHECK(read.operator()<u16>(base+240)==3); // desktop partial output on a bad chunk
        }
        CHECK(call("Uco1I0dlDi8",{2})==u32(ORBIS_PLAYGO_ERROR_BAD_HANDLE));
        CHECK(call("Uco1I0dlDi8",{1})==0);
        if (round!=1) CHECK(call("MPe0EeBGM-E")==0); // middle round tests destructor retirement
    }
    std::printf("guest_playgo_tests: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
