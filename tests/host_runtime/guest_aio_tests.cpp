// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <fstream>
#include <future>
#include "common/path_util.h"
#include "core/file_sys/fs.h"
#include "core/host_runtime/guest_aio.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
static unsigned checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
static std::mutex test_mutex;
static std::condition_variable test_cv;
static unsigned entered{};
static bool release{};
static void Delay(const GuestStorage::IoEvent& e) {
    if (e.kind != GuestStorage::IoEvent::BeforeRead) return;
    std::unique_lock lock(test_mutex); ++entered; test_cv.notify_all();
    test_cv.wait_for(lock, std::chrono::seconds(5), []{return release;});
}
int main(int argc, char** argv) {
    if (argc != 2 || std::filesystem::exists(argv[1])) return 2;
    const std::filesystem::path root = argv[1]; Common::FS::InitializeAndroidUserPaths(root);
    std::filesystem::create_directories(root / "content");
    std::ofstream(root / "content/asset") << "ABCDEFGH";
    Core::FileSys::MntPoints mounts; mounts.Mount(root / "content", "/app0");
    GuestStorage storage(mounts, root / "users", "CUSA99991", 1000);
    AddressSpaceConfig config{}; config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config); if (!made) return 2;
    auto space = std::move(made).Value(); const u64 base = space->ReservationBase().value;
    CHECK(space->Map({{base}, 0x10000}, GuestPermission::Read | GuestPermission::Write));
    auto put = [&](u64 at, const auto& v) { CHECK(space->WriteData({at}, std::as_bytes(std::span{&v, 1}))); };
    auto get32 = [&](u64 at) { s32 v{}; CHECK(space->ReadData({at}, std::as_writable_bytes(std::span{&v, 1}))); return v; };
    GuestAio aio(*space, storage);
    CHECK(aio.Dispatch("2pOuoWoCxdk", {1,base+128}) == u32(ORBIS_KERNEL_ERROR_ENXIO));
    CHECK(aio.Dispatch("nu4a0-arQis", {base+0xffff}) == u32(ORBIS_KERNEL_ERROR_EFAULT));
    CHECK(aio.Dispatch("nu4a0-arQis", {base}) == 0);
    std::array<u32,15> param{}; CHECK(space->ReadData({base},std::as_writable_bytes(std::span{param})));
    CHECK(param == GuestAio::DefaultParam);
    CHECK(aio.Dispatch("vYU8P9Td2Zo", {base,59}) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    CHECK(aio.Dispatch("9WK-vhNXimw", {base,129}) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    CHECK(aio.Dispatch("vYU8P9Td2Zo", {base,60}) == 0);
    auto fd=storage.Open("/app0/asset",0,0); CHECK(!fd.error);
    const u64 req=base+256, result=base+512, data=base+4096, idout=base+128, state=base+132;
    GuestAio::Request r{2,4,data,result,s32(fd.value),0}; put(req,r);
    CHECK(aio.Dispatch("HgX7+AORI58",{req,0,2,idout}) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    CHECK(aio.Dispatch("HgX7+AORI58",{req,1,2,result}) == u32(ORBIS_KERNEL_ERROR_EINVAL));
    CHECK(aio.Dispatch("HgX7+AORI58",{req,1,2,idout}) == 0); s32 id=get32(idout);
    CHECK(aio.Dispatch("KOF-oJbQVvc",{u64(id),state,0}) == 0); CHECK(get32(state)==3);
    GuestAio::Result res{}; CHECK(space->ReadData({result},std::as_writable_bytes(std::span{&res,1})));
    CHECK(res.value==4 && res.state==3);
    std::array<char,4> content{}; CHECK(space->ReadData({data},std::as_writable_bytes(std::span{content})));
    CHECK(std::string_view(content.data(),4)=="CDEF");
    CHECK(aio.Dispatch("5TgME6AYty4",{u64(id),state})==0 && get32(state)==0);
    CHECK(aio.Dispatch("2pOuoWoCxdk",{u64(id),state})==u32(ORBIS_KERNEL_ERROR_ESRCH));
    // Two native reads block; a third remains queued. Closing the descriptor
    // after submission must not invalidate any captured open-file description.
    storage.SetIoObserver(Delay);
    std::array<s32,3> ids{};
    for(unsigned i=0;i<3;++i){r.buffer=data+i*64;r.result=result+i*32;put(req,r);CHECK(aio.Dispatch("HgX7+AORI58",{req,1,2,idout})==0);ids[i]=get32(idout);}
    {std::unique_lock lock(test_mutex); CHECK(test_cv.wait_for(lock,std::chrono::seconds(2),[]{return entered==2;}));}
    CHECK(storage.Close(fd.value).value==0);
    CHECK(aio.Dispatch("fR521KIGgb8",{u64(ids[2]),state})==0 && get32(state)==4);
    CHECK(aio.Dispatch("5TgME6AYty4",{u64(ids[0]),state})==0 && get32(state)==ORBIS_KERNEL_ERROR_EBUSY);
    const u32 timeout=1000; put(base+140,timeout);
    CHECK(aio.Dispatch("KOF-oJbQVvc",{u64(ids[0]),state,base+140})==u32(ORBIS_KERNEL_ERROR_ETIMEDOUT));
    CHECK(get32(base+140)==0);
    std::stop_source stop;
    auto waiter=std::async(std::launch::async,[&]{return aio.Dispatch("KOF-oJbQVvc",{u64(ids[0]),base+160,0},stop.get_token());});
    stop.request_stop(); CHECK(waiter.get()==u32(ORBIS_KERNEL_ERROR_EINTR));
    {std::lock_guard lock(test_mutex);release=true;}test_cv.notify_all();
    put(base+192,ids);
    CHECK(aio.Dispatch("lgK+oIWkJyA",{base+192,3,base+208,1,0})==0);
    CHECK(get32(base+208)==3 && get32(base+212)==3 && get32(base+216)==4);
    CHECK(space->ReadData({result+64},std::as_writable_bytes(std::span{&res,1})) && res.value==ORBIS_KERNEL_ERROR_ECANCELED);
    CHECK(aio.Dispatch("Ft3EtsZzAoY",{base+192,3,base+208})==0);
    storage.SetIoObserver(nullptr);
    CHECK(storage.Initialize()==GuestStorage::Error::OK);
    GuestStorage::MountResult mount{}; CHECK(storage.Mount(1000,"","aio",96,34,mount)==GuestStorage::Error::OK);
    fd=storage.Open("/savedata0/aio",0x202,0600);CHECK(!fd.error);
    r={0,4,data,result,s32(fd.value),0};put(req,r);
    CHECK(aio.Dispatch("xT3Cpz0yh6Y",{req,1,2,idout})==0);id=get32(idout);
    CHECK(aio.Dispatch("KOF-oJbQVvc",{u64(id),state})==0 && get32(state)==3);
    CHECK(storage.Seek(fd.value,0,1).value==0);
    std::array<u8,4> written{};CHECK(storage.Read(fd.value,written).value==4 && std::memcmp(written.data(),"CDEF",4)==0);
    CHECK(aio.Dispatch("5TgME6AYty4",{u64(id),state})==0);
    CHECK(storage.Close(fd.value).value==0);
    r.fd=-1;put(req,r);CHECK(aio.Dispatch("lXT0m3P-vs4",{req,1,2,idout})==0);id=get32(idout);
    CHECK(aio.Dispatch("KOF-oJbQVvc",{u64(id),state})==0 && get32(state)==4);
    CHECK(space->ReadData({result},std::as_writable_bytes(std::span{&res,1})) && res.value==ORBIS_KERNEL_ERROR_EBADF);
    aio.RequestStop();CHECK(aio.Dispatch("HgX7+AORI58",{req,1,2,idout})==u32(ORBIS_KERNEL_ERROR_EINTR));
    std::printf("GUEST_AIO checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
