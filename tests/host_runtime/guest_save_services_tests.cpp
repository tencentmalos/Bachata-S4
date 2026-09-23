// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unistd.h>
#include "common/elf_info.h"
#include "common/path_util.h"
#include "core/file_sys/fs.h"
#include "core/host_runtime/guest_storage_hle.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::SaveData;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                       \
        }                                                                                          \
    } while (0)
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root =
        std::filesystem::current_path() / "cache" / ("save-services-" + std::to_string(getpid()));
    if (std::filesystem::exists(root))
        return 2;
    Common::FS::InitializeAndroidUserPaths(root);
    Common::ElfInfo::Instance().InitializeGuestMetadata(root, 0x3500000, "CUSA99991", {}, {}, 0,
                                                        0x3500000);
    Core::FileSys::MntPoints mounts;
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 65536}, GuestPermission::Read | GuestPermission::Write));
    CHECK(space->Map({GuestAddress{base + 65536}, 65536}, GuestPermission::Read | GuestPermission::Write));
    auto put = [&]<class T>(u64 at, const T& v) {
        CHECK(space->WriteData(GuestAddress{at}, std::as_bytes(std::span{&v, 1})));
    };
    auto get = [&]<class T>(u64 at) {
        T v{};
        CHECK(space->ReadData(GuestAddress{at}, std::as_writable_bytes(std::span{&v, 1})));
        return v;
    };
    for (unsigned round = 0; round < 3; ++round) {
        GuestStorage storage(mounts, root / "users", "CUSA99991", 1000);
        auto call = [&](StorageOp op, std::array<u64, 6> a) {
            const auto* e = std::find_if(std::begin(StorageEntries), std::end(StorageEntries),
                                         [&](auto& v) { return v.op == op; });
            CHECK(e != std::end(StorageEntries));
            return DispatchStorage(storage, *space, *e, a, [](int e) { return u64(e); });
        };
        CHECK(call(StorageOp::SetupMemory, {1000, 128}) ==
              u32(GuestStorage::Error::NOT_INITIALIZED));
        CHECK(storage.Initialize() == GuestStorage::Error::OK);
        CHECK(call(StorageOp::GetMemory, {1000, base, 8, 0}) ==
              u32(GuestStorage::Error::MEMORY_NOT_READY));
        CHECK(call(StorageOp::SetupMemory, {1000, 128}) == 0);
        if (!round) {
            put(base, u64(0x1122334455667788));
            CHECK(call(StorageOp::SetMemory, {1000, base, 8, 0}) == 0);
            put(base, u64(0x9988776655443322));
            CHECK(call(StorageOp::SetMemory, {1000, base, 8, 64}) == 0);
        } else {
            // First operation on a reloaded store is a partial write: retain byte 0.
            put(base, u64(0x9988776655443322));
            CHECK(call(StorageOp::SetMemory, {1000, base, 8, 64}) == 0);
        }
        put(base, u64(0));
        CHECK(call(StorageOp::GetMemory, {1000, base, 8, 0}) == 0);
        CHECK(get.operator()<u64>(base) == 0x1122334455667788);
        CHECK(call(StorageOp::GetMemory, {1000, base, 8, 64}) == 0);
        CHECK(get.operator()<u64>(base) == 0x9988776655443322);
        put(base, u64(0xdeadbeef));
        CHECK(call(StorageOp::GetMemory, {1000, base, 8, 127}) ==
              u32(GuestStorage::Error::PARAMETER));
        CHECK(get.operator()<u64>(base) == 0xdeadbeef);
        CHECK(call(StorageOp::SetMemory, {1000, base, 8, UINT64_MAX}) ==
              u32(GuestStorage::Error::PARAMETER));
        CHECK(call(StorageOp::GetMemory, {1001, base, 8, 0}) ==
              u32(GuestStorage::Error::INVALID_LOGIN_USER));
        CHECK(call(StorageOp::GetMemory, {1000, 1, 8, 0}) == u32(GuestStorage::Error::PARAMETER));
        GuestStorage::MountResult mounted{};
        CHECK(storage.Mount(1000, "", "alpha", 96, 2 | 32, mounted) == GuestStorage::Error::OK);
        std::string point(mounted.point.data());
        put(base + 128, mounted.point);
        struct Icon {
            u64 buffer, size, data_size;
            std::array<u8, 32> reserved;
        };
        put(base + 256, Icon{base, 8, 4, {}});
        CHECK(call(StorageOp::SaveIcon, {base + 128, base + 256}) == 0);
        CHECK(std::filesystem::file_size(
                  root / "users/1000/savedata/CUSA99991/alpha/sce_sys/icon0.png") == 4);
        CHECK(storage.Unmount(point) == GuestStorage::Error::OK);
        CHECK(storage.Mount(1000, "", "beta", 96, 2 | 32, mounted) == GuestStorage::Error::OK);
        CHECK(storage.Unmount(mounted.point.data()) == GuestStorage::Error::OK);
        OrbisSaveDataDirNameSearchCond cond{};
        cond.userId = 1000;
        OrbisSaveDataDirNameSearchResult result{};
        result.dirNamesNum = 1;
        result.dirNames = reinterpret_cast<OrbisSaveDataDirName*>(base + 1024);
        result.params = reinterpret_cast<OrbisSaveDataParam*>(base + 2048);
        result.infos = reinterpret_cast<OrbisSaveDataSearchInfo*>(base + 4096);
        put(base + 512, cond);
        put(base + 640, result);
        CHECK(call(StorageOp::Search, {base + 512, base + 640}) == 0);
        auto found = get.operator()<OrbisSaveDataDirNameSearchResult>(base + 640);
        CHECK(found.hitNum == 2 && found.setNum == 1 && found.dirNames == result.dirNames);
        CHECK(std::string_view(get.operator()<OrbisSaveDataDirName>(base + 1024).data) == "alpha");
        // Atomic output rejection, no hit count or first name modified.
        result.hitNum = 0xfeed;
        result.infos = reinterpret_cast<OrbisSaveDataSearchInfo*>(1);
        put(base + 640, result);
        CHECK(call(StorageOp::Search, {base + 512, base + 640}) ==
              u32(GuestStorage::Error::PARAMETER));
        CHECK(get.operator()<OrbisSaveDataDirNameSearchResult>(base + 640).hitNum == 0xfeed);
        result.dirNamesNum = 2;
        result.dirNames = reinterpret_cast<OrbisSaveDataDirName*>(base + 65536 - 32);
        result.params = nullptr; result.infos = nullptr;
        put(base + 640,result);
        CHECK(call(StorageOp::Search,{base + 512,base + 640}) == 0);
        CHECK(get.operator()<OrbisSaveDataDirNameSearchResult>(base + 640).setNum == 2);
        CHECK(std::string_view(get.operator()<OrbisSaveDataDirName>(base+65536-32).data) == "alpha");
        CHECK(std::string_view(get.operator()<OrbisSaveDataDirName>(base+65536).data) == "beta");
        CHECK(storage.Terminate() == GuestStorage::Error::OK);
        CHECK(storage.Initialize() == GuestStorage::Error::OK);
        CHECK(call(StorageOp::GetMemory, {1000, base, 8, 0}) ==
              u32(GuestStorage::Error::MEMORY_NOT_READY));
    }
    Common::ElfInfo::Instance().InitializeGuestMetadata(root, 0x5500000, "CUSA99991", {}, {}, 0,
                                                        0x5500000);
    for (unsigned round = 0; round < 2; ++round) {
        GuestStorage storage(mounts, root / "users", "CUSA99991", 1000);
        CHECK(storage.Initialize() == GuestStorage::Error::OK);
        auto call = [&](StorageOp op, std::array<u64, 6> args) {
            const auto* entry = std::find_if(std::begin(StorageEntries), std::end(StorageEntries),
                                             [&](auto& x) { return x.op == op; });
            CHECK(entry != std::end(StorageEntries));
            return DispatchStorage(storage, *space, *entry, args, [](int e) { return u64(e); });
        };
        struct Setup {
            u32 option;
            s32 user;
            u64 size, icon_size, param, icon;
            u32 slot;
            u8 reserved[20];
        };
        struct Set {
            s32 user;
            u32 pad;
            u64 data, param, icon;
            u32 count, slot;
            u8 reserved[32];
        };
        struct Get {
            s32 user;
            u32 pad;
            u64 data, param, icon;
            u32 slot;
            u8 reserved[28];
        };
        struct Data {
            u64 buffer, size;
            s64 offset;
            u8 reserved[40];
        };
        struct Icon {
            u64 buffer, capacity, size;
            u8 reserved[32];
        };
        struct Sync {
            s32 user;
            u32 slot, option;
            u8 reserved[28];
        };
        auto path = root / "users/1000/savedata/CUSA99991/sce_sdmemory1";
        Setup setup{1, 1000, round ? 64u : 128u, 8, 0, 0, 1, {}};
        put(base + 0x1000, setup);
        CHECK(call(StorageOp::SetupMemory2, {base + 0x1000, base + 0x1100}) == 0);
        CHECK(get.operator()<u64>(base + 0x1100) == (round ? 128u : 0u));
        // Setup -> Sync, without a read or write, must retain existing data.
        put(base + 0x1200, Sync{1000, 1, 1, {}});
        CHECK(call(StorageOp::SyncMemory, {base + 0x1200}) == 0);
        CHECK(std::filesystem::file_size(path / "memory.dat") == 128);
        GuestStorage::Event event{};
        CHECK(storage.GetEvent(event) == GuestStorage::Error::OK);
        CHECK(event.type == 3 && event.error == 0 && event.user == 1000);
        CHECK(std::filesystem::file_size(path / "sce_backup/memory.dat") == 128);
        Data descriptor{base + 0x2000, 8, 64, {}};
        put(base + 0x1300, descriptor);
        Get request{1000, 0, base + 0x1300, 0, 0, 1, {}};
        put(base + 0x1400, request);
        CHECK(call(StorageOp::GetMemory2, {base + 0x1400}) == 0);
        CHECK(get.operator()<u64>(base + 0x2000) == (round ? 0xfedcba9876543210ull : 0ull));
        OrbisSaveDataParam param{};
        param.title.FromString("Memory test");
        param.userParam = 37;
        put(base + 0x3000, param);
        put(base + 0x2000, u64(0xfedcba9876543210ull));
        put(base + 0x1500, Icon{base + 0x2000, 8, 8, {}});
        Set set{1000, 0, base + 0x1300, base + 0x3000, base + 0x1500, 1, 1, {}};
        put(base + 0x1600, set);
        CHECK(call(StorageOp::SetMemory2, {base + 0x1600}) == 0);
        request.param = base + 0x3800;
        request.icon = base + 0x1500;
        put(base + 0x1400, request);
        CHECK(call(StorageOp::GetMemory2, {base + 0x1400}) == 0);
        CHECK(get.operator()<OrbisSaveDataParam>(base + 0x3800).userParam == 37);
        CHECK(get.operator()<Icon>(base + 0x1500).size == 8);
        // Later invalid descriptor rejects the complete batch before modifying byte64.
        std::array<Data, 2> batch{descriptor, Data{1, 8, 0, {}}};
        put(base + 0x1700, batch);
        set.data = base + 0x1700;
        set.count = 2;
        set.param = 0;
        set.icon = 0;
        put(base + 0x1600, set);
        put(base + 0x2000, u64(0x1111));
        CHECK(call(StorageOp::SetMemory2, {base + 0x1600}) == u32(GuestStorage::Error::PARAMETER));
        CHECK(call(StorageOp::GetMemory2, {base + 0x1400}) == 0);
        CHECK(get.operator()<u64>(base + 0x2000) == 0xfedcba9876543210ull);
        batch[1] = {base + 0x2000, 8, 127, {}};
        put(base + 0x1700, batch);
        CHECK(call(StorageOp::SetMemory2, {base + 0x1600}) == u32(GuestStorage::Error::PARAMETER));
        // Invalid setup result must not create another save directory.
        setup.slot = 2;
        put(base + 0x1000, setup);
        CHECK(call(StorageOp::SetupMemory2, {base + 0x1000, 1}) == u32(GuestStorage::Error::PARAMETER));
        CHECK(!std::filesystem::exists(root / "users/1000/savedata/CUSA99991/sce_sdmemory2"));
        // Disk publication failure keeps old cache and old committed bytes.
        std::filesystem::rename(path / "memory.dat", path / "memory.keep");
        std::filesystem::create_directory(path / "memory.dat");
        set.data = base + 0x1300;
        set.count = 1;
        put(base + 0x1600, set);
        put(base + 0x2000, u64(0x2222));
        CHECK(call(StorageOp::SetMemory2, {base + 0x1600}) != 0);
        std::filesystem::remove(path / "memory.dat");
        std::filesystem::rename(path / "memory.keep", path / "memory.dat");
        CHECK(call(StorageOp::GetMemory2, {base + 0x1400}) == 0);
        CHECK(get.operator()<u64>(base + 0x2000) == 0xfedcba9876543210ull);
        for (const auto& entry : std::filesystem::directory_iterator(path))
            CHECK(!entry.path().filename().string().starts_with("memory.dat.pending-"));
        CHECK(storage.Terminate() == GuestStorage::Error::OK);
    }
    // Firmware option accounting, no disk mutations for rejected reservations.
    {
        GuestStorage storage(mounts, root / "options", "CUSA99991", 1000);
        CHECK(storage.Initialize() == GuestStorage::Error::OK);
        u64 existed{};
        CHECK(storage.SetupMemory2(1000,0,8u<<20,3,nullptr,nullptr,0,existed) == GuestStorage::Error::PARAMETER);
        CHECK(storage.SetupMemory2(1000,0,8u<<20,7,nullptr,nullptr,0,existed) == GuestStorage::Error::OK);
        CHECK(storage.SetupMemory2(1000,1,8u<<20,7,nullptr,nullptr,0,existed) == GuestStorage::Error::OK);
        CHECK(u32(storage.SetupMemory2(1000,2,1,4,nullptr,nullptr,0,existed)) == 0x809f0017);
        CHECK(!std::filesystem::exists(root / "options/1000/savedata/CUSA99991/sce_sdmemory2"));
        CHECK(storage.Terminate() == GuestStorage::Error::OK);
    }
    printf("SAVE_SERVICES %u checks / %u failures\n", checks, failures);
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
