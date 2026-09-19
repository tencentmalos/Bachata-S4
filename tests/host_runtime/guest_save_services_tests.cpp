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
        CHECK(storage.Terminate() == GuestStorage::Error::OK);
        CHECK(storage.Initialize() == GuestStorage::Error::OK);
        CHECK(call(StorageOp::GetMemory, {1000, base, 8, 0}) ==
              u32(GuestStorage::Error::MEMORY_NOT_READY));
    }
    printf("SAVE_SERVICES %u checks / %u failures\n", checks, failures);
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
