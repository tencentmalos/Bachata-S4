// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <barrier>
#include <condition_variable>
#include <set>
#include <zarchive/zarchivewriter.h>
#include "core/file_format/psf.h"
#include "common/path_util.h"
#include "core/file_sys/fs.h"
#include "core/file_sys/directories/normal_directory.h"
#include "core/libraries/kernel/kernel.h"
#include "core/host_runtime/guest_app_content.h"
#include "core/host_runtime/guest_storage_hle.h"
#include "core/libraries/kernel/posix_error.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
static unsigned checks{}, failures{};
static std::mutex io_test_mutex;
static std::condition_variable io_test_changed;
static int delayed_fd{-1};
static bool io_entered{}, io_release{};
static GuestStorage::IoEvent::Kind delayed_kind = GuestStorage::IoEvent::BeforeRead;
static void DelayRead(const GuestStorage::IoEvent& e) {
    if (e.kind != delayed_kind) return;
    std::unique_lock lock(io_test_mutex);
    if ((delayed_fd >= 0 && e.fd != delayed_fd) || io_entered) return;
    io_entered = true;
    io_test_changed.notify_all();
    if (!io_test_changed.wait_for(lock, std::chrono::seconds(8), [] { return io_release; }))
        std::_Exit(3);
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL line %d: %s\n", __LINE__, #x);                                       \
        }                                                                                          \
    } while (0)
static void Pack(const std::filesystem::path& output,
                 const std::map<std::string, std::string>& files) {
    std::ofstream stream(output, std::ios::binary);
    ZArchiveWriter writer([](int32_t, void*) {},
        [](const void* bytes, size_t size, void* ptr) {
            static_cast<std::ofstream*>(ptr)->write(static_cast<const char*>(bytes), size);
        }, &stream);
    for (const auto& [name, data] : files) {
        const auto parent = std::filesystem::path(name).parent_path().generic_string();
        if (!parent.empty()) writer.MakeDir(parent.c_str(), true);
        if (!writer.StartNewFile(name.c_str())) throw std::runtime_error("pack fixture");
        writer.AppendData(data.data(), data.size());
    }
    writer.Finalize();
}
static std::string Sfo(std::string id, std::string version) {
    PSF psf;
    psf.AddString("TITLE_ID", id); psf.AddString("APP_VER", version);
    auto bytes = psf.Encode();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
static void Archives(const std::filesystem::path& root) {
    using namespace Core::FileSys;
    const auto base = root / "CUSA99991.zar";
    const auto update = root / "CUSA99991-UPD.zar";
    const std::string large(140000, 'x'); // three compressed blocks
    Pack(base, {{"sce_sys/param.sfo", Sfo("CUSA99991", "01.00")},
        {"eboot.bin", "base-elf"}, {"base-only", large}, {"shared", "base"},
        {"dir/base", "a"}, {"dir/shared", "old"}});
    Pack(update, {{"sce_sys/param.sfo", Sfo("CUSA99991", "01.08")},
        {"eboot.bin", "update-elf"}, {"shared", "UPDATED"},
        {"dir/update", "b"}, {"dir/shared", "new"}});
    auto metadata = InspectArchiveInstall(base);
    PSF psf;
    CHECK(psf.Open(metadata.param_sfo));
    CHECK(psf.GetString("APP_VER").value_or("") == "01.08");
    MntPoints mounts;
    mounts.Mount(base, "/app0", true);
    mounts.Mount(base, "/hostapp", true);
    GuestStorage storage(mounts, root / "users", "CUSA99991", 1000);
    auto fd = storage.Open("/app0/shared", 0, 0);
    CHECK(!fd.error);
    CHECK(storage.PollReady(fd.value).value == 3);
    CHECK(storage.PollReady(-1).error == EBADF);
    CHECK(storage.PollReady(1023).error == EBADF);
    CHECK(storage.PollReady(0).value == 3);
    CHECK(storage.PollReady(1).value == 3);
    std::array<u8, 8> data{};
    CHECK(storage.Read(fd.value, data).value == 7 && std::memcmp(data.data(), "UPDATED", 7) == 0);
    CHECK(storage.Seek(fd.value, 2, 0).value == 2);
    GuestStorage::Buffer buffer{data.data(), 4};
    CHECK(storage.Positioned(fd.value, std::span{&buffer, 1}, 0, false).value == 4 &&
          std::memcmp(data.data(), "UPDA", 4) == 0);
    CHECK(storage.Seek(fd.value, 0, 1).value == 2);
    CHECK(storage.Seek(fd.value, INT64_MAX, 1).error == EINVAL);
    CHECK(storage.Open("/app0/shared", 0x400, 0).error == EROFS);
    CHECK(storage.Open("/app0/new", 0x200, 0).error == EROFS);
    CHECK(storage.Open("/app0/../shared", 0, 0).error == EACCES);
    CHECK(storage.Open("/app0/shared", 0x20000, 0).error == ENOTDIR);
    Libraries::Kernel::OrbisKernelStat stat{};
    CHECK(storage.Stat("/app0/shared", stat).value == 0 && stat.st_size == 7);
    CHECK(storage.Stat("/app0/base-only", stat).value == 0 && stat.st_size == large.size());
    CHECK(storage.Stat("/app0/missing", stat).error == ENOENT);
    CHECK(storage.Stat("/app0", stat).value == 0 && (stat.st_mode & 0040000));
    CHECK(storage.Sync(fd.value).value == 0);
    auto alias = storage.Open("/hostapp/shared", 0, 0);
    CHECK(!alias.error && storage.Read(alias.value, data).value == 7);
    CHECK(storage.Close(alias.value).value == 0);
    auto directory = storage.Open("/app0/dir", 0x20000, 0);
    CHECK(!directory.error);
    std::array<u8, 512> dirents{};
    auto dents = storage.GetDents(directory.value, dirents, nullptr);
    CHECK(dents.value > 0 && !dents.error);
    std::multiset<std::string> names;
    for (size_t offset = 0; offset < dents.value;) {
        u16 length{}; std::memcpy(&length, dirents.data() + offset + 4, 2);
        if (length < 12 || offset + length > dents.value) { CHECK(false); break; }
        names.emplace(reinterpret_cast<char*>(dirents.data() + offset + 8), dirents[offset + 7]);
        offset += length;
    }
    CHECK(names.count("shared") == 1 && names.count("base") == 1 && names.count("update") == 1);
    CHECK(storage.AcquireMappingFile(directory.value).error == ENODEV);
    CHECK(storage.Close(directory.value).value == 0);
    auto big = storage.Open("/app0/base-only", 0, 0);
    CHECK(!big.error);
    std::vector<u8> across(70000);
    GuestStorage::Buffer block{across.data(), across.size()};
    CHECK(storage.Positioned(big.value, std::span{&block, 1}, 65000, false).value == 70000);
    CHECK(std::all_of(across.begin(), across.end(), [](u8 b) { return b == 'x'; }));
    block.size = 20;
    CHECK(storage.Positioned(big.value, std::span{&block, 1}, large.size()-3, false).value == 3);
    CHECK(storage.Positioned(big.value, std::span{&block, 1}, large.size(), false).value == 0);
    auto mapping = storage.AcquireMappingFile(big.value);
    CHECK(!mapping.error && mapping.host_fd == -1 && mapping.backend && !mapping.writable);
    // A blocked operation owns its backing despite close + mount retirement.
    delayed_kind = GuestStorage::IoEvent::BeforeRead;
    delayed_fd = fd.value; io_entered = io_release = false;
    storage.SetIoObserver(DelayRead);
    auto slow = std::async(std::launch::async, [&] {
        std::array<u8, 7> out{}; GuestStorage::Buffer b{out.data(), out.size()};
        auto result = storage.Positioned(fd.value, std::span{&b, 1}, 0, false);
        return result.value == 7 && std::memcmp(out.data(), "UPDATED", 7) == 0;
    });
    { std::unique_lock lock(io_test_mutex);
      CHECK(io_test_changed.wait_for(lock, std::chrono::seconds(3), [] { return io_entered; })); }
    auto other = std::async(std::launch::async, [&] {
        std::array<u8, 4> out{}; GuestStorage::Buffer b{out.data(), out.size()};
        return storage.Positioned(fd.value, std::span{&b, 1}, 1, false).value == 4;
    });
    CHECK(other.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
    CHECK(storage.Close(fd.value).value == 0);
    CHECK(storage.PollReady(fd.value).error == EBADF);
    mounts.UnmountAll();
    { std::lock_guard lock(io_test_mutex); io_release = true; }
    io_test_changed.notify_all(); CHECK(slow.get()); CHECK(other.get());
    storage.SetIoObserver(nullptr);
    CHECK(storage.Close(big.value).value == 0);
    CHECK(mapping.backend->ReadAt(data.data(), data.size(), 65534) == 8);
    CHECK(std::all_of(data.begin(), data.end(), [](u8 b) { return b == 'x'; }));
    Pack(update, {{"sce_sys/param.sfo", Sfo("CUSA99992", "01.08")}});
    bool rejected{};
    try { (void)InspectArchiveInstall(base); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
    std::ofstream(update, std::ios::binary | std::ios::trunc) << "broken";
    rejected = false;
    try { (void)InspectArchiveInstall(base); } catch (const std::exception&) { rejected = true; }
    CHECK(rejected);
}
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
        for (const auto path : {"/dev/urandom", "/dev/random", "/dev/srandom", "/dev/zero", "/dev/null"}) {
            const auto device = storage.Open(path, 0, 0);
            CHECK(!device.error);
            CHECK(storage.Stat(path, st).value == 0 && (st.st_mode & 0170000) == 0020000);
            CHECK(storage.Fstat(device.value, st).value == 0 && (st.st_mode & 0170000) == 0020000);
            std::array<u8, 256> first{}, second{};
            first.fill(0xa5); second.fill(0xa5);
            const auto a = storage.Read(device.value, first), b = storage.Read(device.value, second);
            const bool null = std::string_view(path) == "/dev/null";
            CHECK(!a.error && !b.error && a.value == (null ? 0 : 256) && b.value == a.value);
            if (std::string_view(path) == "/dev/zero")
                CHECK(std::all_of(first.begin(), first.end(), [](u8 v) { return v == 0; }));
            else if (!null) CHECK(first != second);
            else CHECK(first[0] == 0xa5);
            CHECK(storage.Read(device.value, {}).value == 0);
            CHECK(storage.Write(device.value, first).error == EBADF);
            CHECK(storage.Seek(device.value, 0, 1).value == 0);
            CHECK(storage.Sync(device.value).value == 0);
            CHECK(storage.AcquireMappingFile(device.value).error == ENODEV);
            GuestStorage::Buffer v{first.data(), first.size()};
            CHECK(storage.Positioned(device.value, std::span{&v, 1}, 0, false).error == ESPIPE);
            CHECK(storage.GetDents(device.value, first, nullptr).error == ENOTDIR);
            CHECK(storage.Open(path, 0x20000, 0).error == ENOTDIR);
            CHECK(storage.Close(device.value).value == 0);
            CHECK(storage.Read(device.value, first).error == EBADF);
            const auto writable = storage.Open(path, 1, 0);
            CHECK(!writable.error && storage.Read(writable.value, first).error == EBADF);
            CHECK(storage.Truncate(writable.value, 0).error == EINVAL);
            if (std::string_view(path) == "/dev/zero" || null)
                CHECK(storage.Write(writable.value, first).value == 256);
            else CHECK(storage.Write(writable.value, first).error == ENOTSUP);
            CHECK(storage.Close(writable.value).value == 0);
        }
        CHECK(storage.Open("/dev/urandom/../mem", 0, 0).error != 0);
        CHECK(storage.Open("/dev/urandom-suffix", 0, 0).error != 0);
        CHECK(storage.Open("/dev/mem", 0, 0).error != 0);
        auto fd = storage.Open("/app0/asset", 0, 0);
        CHECK(storage.PollReady(fd.value).value == 3);
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
        {
            delayed_kind = GuestStorage::IoEvent::Opened;
            delayed_fd = -1; io_entered = io_release = false;
            storage.SetIoObserver(DelayRead);
            auto slow = std::async(std::launch::async, [&] { return storage.Open("/app0/asset", 0, 0); });
            {
                std::unique_lock lock(io_test_mutex);
                CHECK(io_test_changed.wait_for(lock, std::chrono::seconds(3), [] { return io_entered; }));
            }
            auto concurrent = std::async(std::launch::async, [&] { return storage.Open("/app0/asset", 0, 0); });
            CHECK(concurrent.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
            { std::lock_guard lock(io_test_mutex); io_release = true; }
            io_test_changed.notify_all();
            const auto a = slow.get(), b = concurrent.get();
            CHECK(!a.error && !b.error && a.value != b.value);
            CHECK(storage.Close(a.value).value == 0 && storage.Close(b.value).value == 0);
            storage.SetIoObserver(nullptr);
            delayed_kind = GuestStorage::IoEvent::BeforeRead;
            io_entered = io_release = false;
        }
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
        // A delayed read retains its backing and native fd, not either global
        // gate. A second pread of this very descriptor can proceed concurrently.
        {
            storage.SetIoObserver(DelayRead);
            delayed_fd = fd.value;
            auto blocked = std::async(std::launch::async, [&] {
                return DispatchStorage(storage, *space, {"concurrent", StorageOp::Pread, false},
                                       {u64(fd.value), base + 64, 3, 0},
                                       [](int) { return UINT64_MAX; });
            });
            {
                std::unique_lock lock(io_test_mutex);
                CHECK(io_test_changed.wait_for(lock, std::chrono::seconds(3), [] { return io_entered; }));
            }
            auto independent = std::async(std::launch::async, [&] {
                std::array<u8, 3> out{};
                const GuestStorage::Buffer b{out.data(), out.size()};
                return storage.Positioned(fd.value, std::span{&b, 1}, 0, false).value == 3;
            });
            CHECK(independent.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
            CHECK(!space->Unmap({GuestAddress{base}, 0x4000})); // admitted output pin remains
            auto closing = std::async(std::launch::async, [&] { return storage.Close(fd.value); });
            const bool close_ready = closing.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
            CHECK(close_ready);
            if (close_ready) CHECK(closing.get().value == 0);
            // An in-flight lease continues to prevent save unmount after Close.
            auto unmounting = std::async(std::launch::async, [&] { return storage.Unmount("/savedata0"); });
            CHECK(unmounting.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
            std::promise<void> draining;
            auto drain = std::async(std::launch::async, [&] {
                auto started = space->BeginDrain();
                draining.set_value();
                if (!started)
                    return false;
                return bool(space->FinishDrain(started.Value(), 3'000'000'000, 0));
            });
            draining.get_future().wait();
            {
                std::lock_guard lock(io_test_mutex); io_release = true;
            }
            io_test_changed.notify_all();
            CHECK(independent.get());
            if (!close_ready) CHECK(closing.get().value == 0);
            CHECK(unmounting.get() == GuestStorage::Error::BUSY);
            CHECK(blocked.get() == 3);
            CHECK(drain.get()); // I/O can retire pins without reacquiring VM gate
            CHECK(storage.Read(fd.value, {}).error == EBADF);
            storage.SetIoObserver(nullptr);
            fd = storage.Open("/savedata0/data", 2, 0600);
            CHECK(!fd.error);
        }
        {
            GuestStorage::MountResult second{};
            CHECK(storage.Mount(1000, "", "parallel", 96, 34, second) == GuestStorage::Error::OK);
            auto a = storage.Open("/savedata0/slow-write", 0x202, 0600);
            auto b = storage.Open("/savedata1/left", 0x202, 0600);
            auto c = storage.Open("/savedata1/right", 0x202, 0600);
            CHECK(!a.error && !b.error && !c.error);
            delayed_kind = GuestStorage::IoEvent::BeforeWrite;
            delayed_fd = a.value; io_entered = io_release = false;
            storage.SetIoObserver(DelayRead);
            auto slow = std::async(std::launch::async, [&] { return storage.Write(a.value, initial); });
            {
                std::unique_lock lock(io_test_mutex);
                CHECK(io_test_changed.wait_for(lock, std::chrono::seconds(3), [] { return io_entered; }));
            }
            auto other_volume = std::async(std::launch::async, [&] { return storage.Write(b.value, initial); });
            CHECK(other_volume.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
            auto other_read = std::async(std::launch::async, [&] {
                std::array<u8, 2> out{};
                GuestStorage::Buffer part{out.data(), out.size()};
                return storage.Positioned(fd.value, std::span{&part, 1}, 0, false);
            });
            CHECK(other_read.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
            { std::lock_guard lock(io_test_mutex); io_release = true; }
            io_test_changed.notify_all();
            CHECK(slow.get().value == 4 && other_volume.get().value == 4 && other_read.get().value == 2);
            storage.SetIoObserver(nullptr);
            std::vector<u8> large(2 * 1024 * 1024, 0x5a);
            CHECK(storage.Seek(b.value, 0, 0).value == 0);
            std::barrier start(3);
            auto left = std::async(std::launch::async, [&] { start.arrive_and_wait(); return storage.Write(b.value, large); });
            auto right = std::async(std::launch::async, [&] { start.arrive_and_wait(); return storage.Write(c.value, large); });
            start.arrive_and_wait();
            const auto l = left.get(), r = right.get();
            CHECK((l.value == large.size() && r.error == ENOSPC) ||
                  (r.value == large.size() && l.error == ENOSPC));
            CHECK(storage.Close(a.value).value == 0 && storage.Close(b.value).value == 0 && storage.Close(c.value).value == 0);
            CHECK(storage.Unlink("/savedata0/slow-write").value == 0);
            CHECK(storage.Unmount("/savedata1") == GuestStorage::Error::OK);
        }
        int posix_error{};
        auto call = [&](StorageOp op, const std::array<u64, 6>& args, bool posix = false) {
            return DispatchStorage(storage, *space, {"test", op, false, posix}, args, [&](int e) {
                posix_error = e;
                return UINT64_MAX;
            });
        };
        CHECK(DispatchStorage(storage, *space, {"errno-retirement", StorageOp::Pread, false, true},
            {999999, base + 128, 4, 0}, [&](int error) {
                CHECK(error == POSIX_EBADF && space->Counts().live_pins == 0);
                return UINT64_MAX;
            }) == UINT64_MAX);
        // Real TMNT opens the save root as a directory before enabling writes.
        CHECK(storage.Open("/savedata0/data", 0x20000, 0).error == ENOTDIR);
        CHECK(storage.Open("/savedata0/", 0x20002, 0).error == EISDIR);
        CHECK(storage.Mkdir("/savedata0/./", 0700).error == EEXIST);
        for (int i = 0; i < 70; ++i)
            std::ofstream(root / "users/1000/savedata/CUSA99991/io" /
                          ("entry-" + std::to_string(i))) << i;
        auto directory = storage.Open("/savedata0/./", 0x20000, 0);
        CHECK(!directory.error);
        CHECK(storage.Unmount("/savedata0") == GuestStorage::Error::BUSY);
        std::array<u8, 512> entries{};
        s64 directory_base{-1};
        CHECK(storage.GetDents(directory.value, std::span{entries}.first(511), nullptr).error == EINVAL);
        CHECK(storage.GetDents(fd.value, entries, nullptr).error == EINVAL);
        CHECK(u32(call(StorageOp::GetDirEntries,
                       {u64(directory.value), base + 2048, 512, 1})) == u32(ORBIS_KERNEL_ERROR_EFAULT));
        CHECK(storage.Seek(directory.value, 0, 1).value == 0); // failed basep did not consume
        CHECK(call(StorageOp::GetDirEntries,
                   {u64(directory.value), base + 2048, 512, base + 1536}) == 512);
        CHECK(space->Read(GuestAddress{base + 1536},
                          std::as_writable_bytes(std::span{&directory_base, 1})) && directory_base == 0);
        CHECK(storage.Seek(directory.value, 0, 0).value == 0);
        std::set<std::string> names;
        s64 total{};
        for (unsigned chunk = 0; chunk < 20; ++chunk) {
            const auto got = storage.GetDents(directory.value, entries, &directory_base);
            CHECK(!got.error && (got.value == 0 || got.value == 512));
            CHECK(directory_base == total);
            if (!got.value) break;
            total += got.value;
            for (size_t at = 0; at < size_t(got.value);) {
                u16 length{};
                std::memcpy(&length, entries.data() + at + 4, 2);
                CHECK(length >= 12 && length % 4 == 0 && length <= got.value - at);
                if (length < 12 || length > got.value - at) break;
                const auto name_length = entries[at + 7];
                CHECK(name_length && size_t(name_length) + 9 <= length &&
                      entries[at + 8 + name_length] == 0);
                names.emplace(reinterpret_cast<char*>(entries.data() + at + 8), name_length);
                at += length;
            }
        }
        CHECK(names.contains(".") && names.contains("..") && names.contains("entry-0") &&
              names.contains("entry-69") && names.contains("data") && total > 512);
        CHECK(storage.GetDents(directory.value, entries, nullptr).value == 0);
        CHECK(storage.Seek(directory.value, INT64_MAX, 1).error == EINVAL);
        CHECK(storage.Seek(directory.value, -1, 0).error == EINVAL);
        CHECK(storage.Seek(directory.value, 0, 0).value == 0);
        CHECK(storage.Read(directory.value, entries).value == 512);
        CHECK(storage.Fstat(directory.value, st).value == 0 && st.st_size == total &&
              st.st_mode == 0040777 && st.st_blksize == 0x8000);
        const std::array<GuestStorage::Buffer, 1> dir_buffers{{{entries.data(), entries.size()}}};
        CHECK(storage.Positioned(directory.value, dir_buffers, 0, false).value == 512);
        CHECK(storage.Seek(directory.value, 0, 1).value == 512);
        CHECK(storage.Positioned(directory.value, dir_buffers, INT64_MAX - 512, false).value == 0);
        CHECK(storage.Seek(directory.value, 0, 1).value == 512);
        CHECK(storage.Positioned(directory.value, dir_buffers, 0, true).error == EBADF);
        CHECK(storage.Close(directory.value).value == 0);
        CHECK(storage.GetDents(directory.value, entries, nullptr).error == EBADF);
        CHECK(Libraries::Kernel::NativeToPosixErrno(EOVERFLOW) == POSIX_EOVERFLOW);
        CHECK(Libraries::Kernel::NativeToPosixErrno(ENAMETOOLONG) == POSIX_ENAMETOOLONG);
        CHECK(Libraries::Kernel::NativeToPosixErrno(ENOTEMPTY) == POSIX_ENOTEMPTY);
        const std::array<u8, 5> log_line{'t', 'e', 's', 't', '\n'};
        CHECK(storage.Write(1, log_line).value == 5 && storage.Write(2, log_line).value == 5);
        CHECK(storage.Sync(1).value == 0 && storage.Close(1).error == EPERM);
        Core::Directories::NormalDirectory empty([](const auto&) {});
        CHECK(empty.getdents(entries.data(), entries.size(), nullptr) == 0);
        bool fail_reader = false;
        Core::Directories::NormalDirectory throwing([&](const auto&) {
            if (fail_reader) throw std::system_error(EIO, std::generic_category());
        });
        fail_reader = true;
        const Libraries::Kernel::OrbisKernelIovec one{entries.data(), entries.size()};
        bool failed{};
        try { (void)throwing.preadv(&one, 1, 1); }
        catch (const std::system_error& e) { failed = e.code().value() == EIO; }
        CHECK(failed && throwing.lseek(0, 1) == 0);
        CHECK(throwing.preadv(&one, 1, -1) == ORBIS_KERNEL_ERROR_EINVAL);
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
            CHECK(storage.Rmdir("/temp0/missing").error == ENOENT);
            CHECK(storage.Rmdir("").error == ENOENT);
            CHECK(storage.Rmdir("/temp0").error == EINVAL);
            CHECK(storage.Rmdir("/temp0/sub/.").error == EINVAL);
            CHECK(storage.Rmdir("/temp0/../escape").error == EACCES);
            CHECK(storage.Rmdir("/app0/dir").error == EROFS);
            CHECK(storage.Rmdir("/savedata0/sce_sys").error == EACCES);
            auto temp_fd = storage.Open("/temp0/sub/data", 0x202, 0600);
            CHECK(!temp_fd.error);
            CHECK(storage.Rmdir("/temp0/sub/data").error == ENOTDIR);
            CHECK(storage.Rmdir("/temp0/sub").error == ENOTEMPTY);
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
            std::filesystem::create_directory_symlink(temporary / "sub", temporary / "dir-link");
            CHECK(storage.Rmdir("/temp0/dir-link").error == ENOTDIR);
            CHECK(std::filesystem::is_directory(temporary / "sub"));
            CHECK(storage.Close(temp_fd.value).value == 0);
            CHECK(storage.Rename("/temp0/sub/data", "/temp0/renamed").value == 0);
            CHECK(storage.Unlink("/temp0/renamed").value == 0);
            const char remove_path[] = "/temp0/sub/";
            CHECK(space->WriteData({base + 2048}, std::as_bytes(std::span{remove_path})));
            const auto kernel_remove = std::find_if(std::begin(StorageEntries), std::end(StorageEntries),
                [](const auto& e) { return e.nid == "naInUjYt3so"; });
            const auto posix_remove = std::find_if(std::begin(StorageEntries), std::end(StorageEntries),
                [](const auto& e) { return e.nid == "c7ZnT7V1B98"; });
            CHECK(kernel_remove != std::end(StorageEntries) && posix_remove != std::end(StorageEntries));
            if (kernel_remove != std::end(StorageEntries) && posix_remove != std::end(StorageEntries)) {
                auto failure = [&](int e) { posix_error = e; return UINT64_MAX; };
                CHECK(DispatchStorage(storage, *space, *kernel_remove, {base + 2048}, failure) == 0);
                CHECK(!std::filesystem::exists(temporary / "sub"));
                CHECK(DispatchStorage(storage, *space, *posix_remove, {base + 2048}, failure) ==
                      UINT64_MAX && posix_error == POSIX_ENOENT);
                CHECK(s32(DispatchStorage(storage, *space, *kernel_remove, {base + 2048}, failure)) ==
                      Libraries::Kernel::ErrnoToSceKernelError(POSIX_ENOENT));
                CHECK(DispatchStorage(storage, *space, *posix_remove, {0}, failure) ==
                      UINT64_MAX && posix_error == POSIX_EFAULT);
            }
            CHECK(storage.Mkdir("/temp0/open-dir", 0700).value == 0);
            auto open_dir = storage.Open("/temp0/open-dir", 0x20000, 0);
            CHECK(!open_dir.error);
            CHECK(storage.Rmdir("/temp0/open-dir/").value == 0);
            CHECK(storage.Close(open_dir.value).value == 0);
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
    Archives(root);
    std::printf("GUEST_FILE_IO checks=%u failures=%u\n", checks, failures);
    // Only the fresh test root above is owned by this executable.
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
