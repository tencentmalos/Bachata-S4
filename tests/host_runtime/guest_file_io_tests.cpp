// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <fstream>
#include "common/path_util.h"
#include "core/file_sys/fs.h"
#include "core/host_runtime/guest_app_content.h"
#include "core/host_runtime/guest_storage_hle.h"
#include "core/libraries/kernel/posix_error.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
static unsigned checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL line %d: %s\n", __LINE__, #x);                                       \
        }                                                                                          \
    } while (0)
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    const std::filesystem::path root = argv[1];
    if (std::filesystem::exists(root))
        return 2; // never reuse a real save directory
    Common::FS::InitializeAndroidUserPaths(root);
    {
        std::filesystem::create_directory(root / "content");
        std::ofstream(root / "content/asset") << "ABCDEFGH";
        Core::FileSys::MntPoints mounts;
        mounts.Mount(root / "content", "/app0");
        GuestStorage storage(mounts, root / "users", "CUSA99991", 1000);
        using Stat = Libraries::Kernel::OrbisKernelStat;
        Stat st{};
        CHECK(storage.Stat("/app0", st).value == 0 && st.st_mode == 0040777 && st.st_size == 65536);
        CHECK(storage.Stat("/app0/asset", st).value == 0 && st.st_size == 8 && st.st_blocks == 1);
        CHECK(storage.Stat("/app0/missing", st).error == ENOENT && st.st_size == 8);
        CHECK(storage.Stat("/app0/../asset", st).error == EACCES);
        std::filesystem::create_symlink(root / "content/asset", root / "content/link");
        CHECK(storage.Stat("/app0/link", st).error == EACCES);
        auto fd = storage.Open("/app0/asset", 0, 0);
        CHECK(!fd.error);
        CHECK(storage.Seek(fd.value, 3, 0).value == 3);
        std::array<char, 8> data{};
        std::array<GuestStorage::Buffer, 2> buffers{{{data.data(), 2}, {data.data() + 2, 3}}};
        CHECK(storage.Positioned(fd.value, buffers, 1, false).value == 5 &&
              std::string(data.data(), 5) == "BCDEF");
        CHECK(storage.Seek(fd.value, 0, 1).value == 3);
        CHECK(storage.Positioned(fd.value, buffers, -1, false).error == EINVAL);
        CHECK(storage.Positioned(fd.value, buffers, INT64_MAX, false).error == EOVERFLOW);
        CHECK(storage.Positioned(fd.value, buffers, 0, true).error == EBADF);
        CHECK(storage.Truncate(fd.value, 1).error == EBADF);
        CHECK(storage.Fstat(fd.value, st).value == 0 && st.st_size == 8);
        CHECK(storage.Close(fd.value).value == 0);
        CHECK(storage.Fstat(fd.value, st).error == EBADF);
        CHECK(storage.Initialize() == GuestStorage::Error::OK);
        GuestStorage::MountResult mounted{};
        CHECK(storage.Mount(1000, "", "io", 96, 34, mounted) == GuestStorage::Error::OK);
        fd = storage.Open("/savedata0/data", 0x20a, 0600); // RDWR|CREAT|APPEND
        CHECK(!fd.error);
        const std::array<u8, 4> initial{'0', '1', '2', '3'};
        CHECK(storage.Write(fd.value, initial).value == 4);
        CHECK(storage.Positioned(fd.value, buffers, 1, true).value == 5);
        CHECK(storage.Seek(fd.value, 0, 1).value == 4); // pwritev did not advance the cursor
        CHECK(storage.Fstat(fd.value, st).value == 0 && st.st_size == 6); // not appended
        CHECK(storage.Truncate(fd.value, 3).value == 0);
        CHECK(storage.Fstat(fd.value, st).value == 0 && st.st_size == 3);
        CHECK(storage.Truncate(fd.value, -1).error == EINVAL);
        CHECK(storage.Truncate(fd.value, 96 * 32768 + 1).error == ENOSPC);
        CHECK(storage.Positioned(fd.value, buffers, 96 * 32768, true).error == ENOSPC);
        CHECK(storage.Fstat(fd.value, st).value == 0 && st.st_size == 3);
        CHECK(storage.Write(fd.value, initial).value == 4); // append flag restored
        CHECK(storage.Fstat(fd.value, st).value == 0 && st.st_size == 7);
        AddressSpaceConfig cfg{};
        cfg.reservation_size = 16 << 20;
        auto made = GuestAddressSpace::Create(cfg);
        if (!made)
            return 2;
        auto space = std::move(made).Value();
        const u64 base = space->ReservationBase().value;
        CHECK(space->Map({GuestAddress{base}, 0x4000},
                         GuestPermission::Read | GuestPermission::Write));
        int posix_error{};
        auto call = [&](StorageOp op, const std::array<u64, 6>& args, bool posix = false) {
            return DispatchStorage(storage, *space, {"test", op, false, posix}, args, [&](int e) {
                posix_error = e;
                return UINT64_MAX;
            });
        };
        auto write = [&](u64 address, const auto& value) {
            CHECK(space->Write(GuestAddress{address}, std::as_bytes(std::span{&value, 1})));
        };
        std::array<u64, 4> vectors{base + 128, 2, 1, 3};
        write(base, vectors);
        const std::array<char, 8> sentinel{'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
        write(base + 128, sentinel);
        CHECK(u32(call(StorageOp::Preadv, {u64(fd.value), base, 2, 0, 0, 0})) ==
              u32(ORBIS_KERNEL_ERROR_EFAULT));
        std::array<char, 8> observed{};
        CHECK(space->Read(GuestAddress{base + 128}, std::as_writable_bytes(std::span{observed})) &&
              observed == sentinel);
        CHECK(call(StorageOp::Pwritev, {u64(fd.value), base, 2, 0, 0, 0}, true) == UINT64_MAX &&
              posix_error == POSIX_EFAULT);
        CHECK(storage.Fstat(fd.value, st).value == 0 && st.st_size == 7);
        CHECK(u32(call(StorageOp::Fstat, {u64(fd.value), 1, 0, 0, 0, 0})) ==
              u32(ORBIS_KERNEL_ERROR_EFAULT));
        CHECK(u32(call(StorageOp::Preadv, {u64(fd.value), base, UINT32_MAX, 0, 0, 0})) ==
              u32(ORBIS_KERNEL_ERROR_EINVAL));
        vectors = {base + 128, UINT64_MAX, base + 132, 1};
        write(base, vectors);
        CHECK(u32(call(StorageOp::Preadv, {u64(fd.value), base, 2, 0, 0, 0})) ==
              u32(ORBIS_KERNEL_ERROR_EINVAL));
        vectors = {base + 128, 2, base + 132, 2};
        write(base, vectors);
        CHECK(call(StorageOp::Preadv, {u64(fd.value), base, 2, 0, 0, 0}) == 4);
        CHECK(call(StorageOp::Pread, {u64(fd.value), 1, 0, 0, 0, 0}) == 0);
        CHECK(call(StorageOp::Pwrite, {u64(fd.value), 1, 0, 0, 0, 0}) == 0);
        CHECK(call(StorageOp::Fstat, {u64(fd.value), base + 256, 0, 0, 0, 0}) == 0);
        CHECK(space->Read(GuestAddress{base + 256}, std::as_writable_bytes(std::span{&st, 1})) &&
              st.st_size == 7);
        // A private metadata snapshot, real DLC directory, and separate session.
        std::filesystem::create_directories(root / "dlc/sce_sys");
        PSF dlc;
        dlc.AddString("CATEGORY", "ac");
        dlc.AddString("CONTENT_ID", "UP0000-CUSA99991_00-EXTRACONTENT0001");
        const auto encoded = dlc.Encode();
        {
            std::ofstream out(root / "dlc/sce_sys/param.sfo", std::ios::binary);
            out.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        }
        std::ofstream(root / "dlc/asset") << "DLC";
        {
            unsigned notifications{};
            bool admit_notification = false;
            GuestAppContent content(mounts, 0x9000000, "CUSA99991", {3, 11, 22, 33, 44},
                                    {root / "dlc"}, [&] {
                                        if (!admit_notification)
                                            return false;
                                        ++notifications;
                                        return true;
                                    });
            auto dispatch = [&](std::string_view nid, std::array<u64, 6> a) {
                CHECK(IsAppContentNid(nid));
                return content.Dispatch(*space, nid, a, &storage);
            };
            CHECK(dispatch("R9lA82OraNs", {1, 0}) == u32(ORBIS_APP_CONTENT_ERROR_PARAMETER));
            CHECK(dispatch("R9lA82OraNs", {0, 1}) == u32(ORBIS_APP_CONTENT_ERROR_PARAMETER));
            CHECK(dispatch("R9lA82OraNs", {}) == u32(ORBIS_APP_CONTENT_ERROR_BUSY));
            admit_notification = true;
            CHECK(dispatch("R9lA82OraNs", {}) == 0 && notifications == 1);
            CHECK(dispatch("R9lA82OraNs", {}) == u32(ORBIS_APP_CONTENT_ERROR_BUSY) &&
                  notifications == 1);
            CHECK(dispatch("99b82IKXpH4", {2, base + 256}) == 0);
            u32 value{};
            CHECK(space->Read(GuestAddress{base + 256},
                              std::as_writable_bytes(std::span{&value, 1})) &&
                  value == 22);
            CHECK(dispatch("xnd8BJzAxmk", {0, 0, 0, base + 256}) == 0);
            CHECK(space->Read(GuestAddress{base + 256},
                              std::as_writable_bytes(std::span{&value, 1})) &&
                  value == 1);
            CHECK(dispatch("xnd8BJzAxmk", {0, base + 512, 1, base + 256}) == 0);
            Libraries::AppContent::OrbisAppContentAddcontInfo info{};
            CHECK(space->Read(GuestAddress{base + 512},
                              std::as_writable_bytes(std::span{&info, 1})) &&
                  info.status ==
                      Libraries::AppContent::OrbisAppContentAddcontDownloadStatus::Installed);
            CHECK(std::string(info.entitlement_label.data) == "EXTRACONTENT0001");
            CHECK(dispatch("m47juOmH0VE", {0, base + 512, base + 768}) == 0);
            CHECK(dispatch("XTWR0UXvcgs", {0, base + 512, base + 768}) ==
                  u32(ORBIS_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT));
            CHECK(dispatch("VANhIWcqYak", {0, base + 512, 1}) ==
                      u32(ORBIS_APP_CONTENT_ERROR_PARAMETER) &&
                  !mounts.GetMount("/addcont0"));
            CHECK(dispatch("VANhIWcqYak", {0, base + 512, base + 768}) == 0 &&
                  mounts.GetMount("/addcont0"));
            CHECK(storage.Stat("/addcont0/asset", st).value == 0 && st.st_size == 3);
            auto extra = storage.Open("/addcont0/asset", 0, 0);
            CHECK(!extra.error);
            std::array<u8, 3> extra_bytes{};
            CHECK(storage.Read(extra.value, extra_bytes).value == 3 && extra_bytes[0] == 'D');
            CHECK(storage.Close(extra.value).value == 0);
            CHECK(storage.Open("/addcont0/asset", 2, 0600).error == EROFS);
            CHECK(storage.Open("/addcont0/../asset", 0, 0).error == EACCES);
            GuestAppContent empty(mounts, 0x9000000, "CUSA99991", {3, 0, 0, 0, 0}, {});
            CHECK(empty.Dispatch(*space, "R9lA82OraNs", {}) == 0);
            CHECK(empty.Dispatch(*space, "xnd8BJzAxmk", {0, 0, 0, base + 256}) == 0);
            CHECK(space->Read(GuestAddress{base + 256},
                              std::as_writable_bytes(std::span{&value, 1})) &&
                  value == 0);
            CHECK(empty.Dispatch(*space, "m47juOmH0VE", {0, base + 512, base + 768}) ==
                  u32(ORBIS_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT));
            CHECK(dispatch("buYbeLOGWmA", {0, 1}) == u32(ORBIS_APP_CONTENT_ERROR_PARAMETER));
            CHECK(!mounts.GetMountSnapshot("/temp0"));
            CHECK(dispatch("buYbeLOGWmA", {2, base + 1024}) ==
                  u32(ORBIS_APP_CONTENT_ERROR_PARAMETER));
            CHECK(!mounts.GetMountSnapshot("/temp0"));
            CHECK(dispatch("buYbeLOGWmA", {1, base + 1024}) == 0);
            Libraries::AppContent::OrbisAppContentMountPoint point{};
            CHECK(space->Read(GuestAddress{base + 1024},
                              std::as_writable_bytes(std::span{&point, 1})) &&
                  std::string(point.data) == "/temp0");
            const auto temporary = mounts.GetMountSnapshot("/temp0")->host_path;
            CHECK(std::filesystem::is_empty(temporary));
            CHECK(dispatch("SaKib2Ug0yI", {base + 1024, 1}) ==
                  u32(ORBIS_APP_CONTENT_ERROR_PARAMETER));
            CHECK(dispatch("SaKib2Ug0yI", {base + 1024, base + 1280}) == 0);
            u64 available{};
            CHECK(space->Read(GuestAddress{base + 1280},
                              std::as_writable_bytes(std::span{&available, 1})) &&
                  available > 0);
            const auto actual = std::filesystem::space(temporary).available / 1024;
            CHECK(available > actual / 2 && available < actual * 2);
            CHECK(storage.Mkdir("/temp0/sub", 0700).value == 0);
            auto temp_fd = storage.Open("/temp0/sub/data", 0x202, 0600);
            CHECK(!temp_fd.error);
            CHECK(storage.Write(temp_fd.value, initial).value == 4);
            CHECK(storage.UnmountTemporary() == EBUSY);
            CHECK(dispatch("buYbeLOGWmA", {1, base + 1536}) == u32(ORBIS_APP_CONTENT_ERROR_BUSY));
            CHECK(storage.Truncate(temp_fd.value, 4 << 20).value == 0); // no savedata quota
            CHECK(storage.Truncate(temp_fd.value, 4).value == 0);
            CHECK(storage.Seek(temp_fd.value, 0, 0).value == 0);
            std::array<u8, 4> reread{};
            CHECK(storage.Read(temp_fd.value, reread).value == 4 && reread == initial);
            CHECK(storage.Rename("/temp0/sub/data", "/savedata0/temp").error == EXDEV);
            CHECK(storage.Open("/temp0/../escape", 0x202, 0600).error == EACCES);
            std::filesystem::create_symlink(root / "content/asset", temporary / "link");
            CHECK(storage.Open("/temp0/link", 0x202, 0600).error == ELOOP);
            CHECK(storage.Close(temp_fd.value).value == 0);
            CHECK(storage.Rename("/temp0/sub/data", "/temp0/renamed").value == 0);
            CHECK(storage.Unlink("/temp0/renamed").value == 0);
            CHECK(storage.UnmountTemporary() == 0);
            CHECK(!mounts.GetMountSnapshot("/temp0") && !std::filesystem::exists(temporary));
            CHECK(std::filesystem::file_size(root / "content/asset") == 8);
            CHECK(dispatch("SaKib2Ug0yI", {base + 1024, base + 1280}) ==
                  u32(ORBIS_APP_CONTENT_ERROR_NOT_FOUND));
            CHECK(dispatch("buYbeLOGWmA", {0, base + 1024}) == 0);
            CHECK(std::filesystem::is_empty(mounts.GetMountSnapshot("/temp0")->host_path));
            CHECK(storage.UnmountTemporary() == 0);
        }
        CHECK(!mounts.GetMount("/addcont0"));
        CHECK(storage.Close(fd.value).value == 0);
        CHECK(storage.Unmount("/savedata0") == GuestStorage::Error::OK);
    }
    // Stop closes remaining temporary descriptors, removes only its owned
    // directory, and the next Session mounts a fresh empty root.
    Core::FileSys::MntPoints temporary_mounts;
    temporary_mounts.Mount(root / "content", "/owned-test");
    temporary_mounts.UnmountOwned(root / "different-owner", "/owned-test");
    CHECK(temporary_mounts.GetMountSnapshot("/owned-test"));
    temporary_mounts.UnmountOwned(root / "content", "/owned-test");
    CHECK(!temporary_mounts.GetMountSnapshot("/owned-test"));
    std::filesystem::path previous;
    for (unsigned generation = 0; generation < 3; ++generation) {
        {
            GuestStorage storage(temporary_mounts, root / "users", "CUSA99991", 1000);
            std::array<char, 16> point{};
            CHECK(storage.MountTemporary(0, point) == 0);
            const auto current = temporary_mounts.GetMountSnapshot("/temp0")->host_path;
            CHECK(current != previous && std::filesystem::is_empty(current));
            previous = current;
            auto fd = storage.Open("/temp0/left-open", 0x202, 0600);
            CHECK(!fd.error);
        }
        CHECK(!temporary_mounts.GetMountSnapshot("/temp0") && !std::filesystem::exists(previous));
    }
    std::printf("GUEST_FILE_IO checks=%u failures=%u\n", checks, failures);
    // Only the fresh test root above is owned by this executable.
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
