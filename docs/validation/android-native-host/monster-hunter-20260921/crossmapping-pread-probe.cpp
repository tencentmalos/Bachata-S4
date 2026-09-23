#include <cstdio>
#include <fstream>
#include "common/path_util.h"
#include "core/file_sys/fs.h"
#include "core/host_runtime/guest_storage_hle.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
int main(int argc,char**argv) {
 if(argc!=2 || std::filesystem::exists(argv[1])) return 2;
 std::filesystem::path root=argv[1]; std::filesystem::create_directory(root);
 Common::FS::InitializeAndroidUserPaths(root);
 std::filesystem::create_directory(root/"content"); std::ofstream(root/"content/data")<<"ABCDEFGH";
 Core::FileSys::MntPoints mounts; mounts.Mount(root/"content","/app0");
 GuestStorage storage(mounts,root/"users","CUSA99991",1000);
 AddressSpaceConfig cfg{};cfg.reservation_size=16<<20; auto made=GuestAddressSpace::Create(cfg);
 if(!made)return 3;auto space=std::move(made).Value();u64 b=space->ReservationBase().value;
 if(!space->Map({{b},0x4000},GuestPermission::Read|GuestPermission::Write) || !space->Map({{b+0x4000},0x4000},GuestPermission::Read|GuestPermission::Write))return 4;
 auto fd=storage.Open("/app0/data",0,0);if(fd.error)return 5;
 // Do not pass DataRequest across DSOs: this probes the unchanged file ABI.
 StorageEntry entry{"+r3rMFwItV4",StorageOp::Pread,false,false};
 auto result=s64(DispatchStorage(storage,*space,entry,{u64(fd.value),b+0x3ffb,8,0},[](int){return UINT64_MAX;}));
 std::array<char,8> out{};
 if(!space->ReadData({b+0x3ffb},std::as_writable_bytes(std::span{out}).first(5)) || !space->ReadData({b+0x4000},std::as_writable_bytes(std::span{out}).subspan(5)))return 6;
 bool passed=result==8 && std::string_view(out.data(),8)=="ABCDEFGH" && space->Counts().live_pins==0;
 printf("pread result=%lld bytes=%02x %02x %02x %02x %02x %02x %02x %02x passed=%d\n",(long long)result,out[0],out[1],out[2],out[3],out[4],out[5],out[6],out[7],passed);
 return passed?0:1;
}
