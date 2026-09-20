// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <span>
#include "core/libraries/kernel/file_system.h"
#include "core/file_sys/directories/base_directory.h"
#include "core/file_sys/devices/logger.h"
#include "core/libraries/save_data/save_instance.h"
#include "core/libraries/save_data/save_memory.h"
#include "core/libraries/save_data/savedata.h"
#include "core/libraries/save_data/savedata_error.h"

namespace Core::FileSys { class IFile; }

namespace Core::HostRuntime {
// Session-owned save mounts and guest descriptors. All guest pointers are copied
// or pinned by the adapter; this boundary only receives native values/spans.
class GuestStorage {
public:
    using Error = Libraries::SaveData::Error;
    struct MountResult {
        std::array<char, 16> point{};
        u64 required_blocks{};
        u32 unused{}, status{};
        std::array<u8, 32> reserved{};
    };
    struct MountInfo {
        u64 blocks{}, free_blocks{};
        std::array<u8, 32> reserved{};
    };
    struct IoResult {
        s64 value{};
        int error{};
    }; // host errno, converted by guest adapter
    // Opt-in read diagnostics; installed before owners start. BeforeRead is also
    // a deterministic test seam. Production observers must not block/reenter.
    struct IoEvent {
        enum Kind { Opened, BeforeRead, BeforeWrite, ReadDone } kind;
        int fd;
        std::string_view path;
        s64 offset;
        u64 bytes{}, begin_ns{}, syscall_ns{}, end_ns{};
        IoResult result{};
    };
    using IoObserver = void (*)(const IoEvent&);
    void SetIoObserver(IoObserver value) { io_observer = value; }
    bool TraceIo() const { return io_observer != nullptr; }
    GuestStorage(Core::FileSys::MntPoints& mounts, std::filesystem::path home, std::string title,
                 int user);
    ~GuestStorage();
    Error Initialize();
    Error Terminate();
    Error Mount(int user, std::string_view title, std::string_view directory, u64 blocks, u32 mode,
                MountResult& result);
    Error Unmount(std::string_view point);
    Error UnmountBackup(std::string_view point);
    Error CheckBackup(int user, std::string_view title, std::string_view directory,
                      Libraries::SaveData::OrbisSaveDataParam& param, std::vector<u8>& icon);
    Error RestoreBackup(int user, std::string_view title, std::string_view directory);
    Error Delete(int user, std::string_view title, std::string_view directory);
    struct Event {
        u32 type{}, error{};
        s32 user{};
        u32 pad{};
        Libraries::SaveData::OrbisSaveDataTitleId title{};
        Libraries::SaveData::OrbisSaveDataDirName directory{};
        std::array<u8, 40> reserved{};
    };
    Error GetEvent(Event& event);
    void Cancel() {
        cancelled = true;
    }

    Error Info(std::string_view point, MountInfo& result);
    Error GetParam(std::string_view point, u32 type, std::span<u8> out, u64& size);
    Error SetParam(std::string_view point, u32 type, std::span<const u8> in);
    Error SetupMemory(int uid, u64 size, const Libraries::SaveData::OrbisSaveDataParam* param);
    Error Memory(int uid, std::span<u8> bytes, s64 offset, bool write);
    Error SaveIcon(std::string_view point, std::span<const u8> bytes);
    Error Search(const Libraries::SaveData::OrbisSaveDataDirNameSearchCond& condition,
                 Libraries::SaveData::OrbisSaveDataDirNameSearchResult& result);
    IoResult Open(std::string_view path, u32 flags, u32 mode);
    IoResult Close(int fd);
    IoResult Read(int fd, std::span<u8> data);
    IoResult Write(int fd, std::span<const u8> data);
    IoResult Seek(int fd, s64 offset, int whence);
    IoResult Sync(int fd);
    IoResult GetDents(int fd, std::span<u8> bytes, s64* base);
    IoResult Stat(std::string_view path, Libraries::Kernel::OrbisKernelStat& out);
    IoResult Fstat(int fd, Libraries::Kernel::OrbisKernelStat& out);
    // Pins the open description across mmap/close races. The guest descriptor
    // is never a host fd or a desktop HandleTable index.
    struct MappingFile {
        std::shared_ptr<const void> lifetime;
        int host_fd{-1};
        bool writable{};
        int error{};
        Core::FileSys::IFile* backend{};
    };
    MappingFile AcquireMappingFile(int fd);
    // Buffers here are already checked and pinned by the guest ABI adapter.
    struct Buffer {
        void* data;
        size_t size;
    };
    IoResult Positioned(int fd, std::span<const Buffer> buffers, s64 offset, bool write);
    IoResult Truncate(int fd, s64 length);
    IoResult Mkdir(std::string_view path, u32 mode);
    IoResult Unlink(std::string_view path);
    IoResult Rename(std::string_view from, std::string_view to);
    // AppContent temporary data is separate from persistent save mounts. These
    // return host errno; the AppContent adapter translates to its own errors.
    int MountTemporary(u32 option, std::array<char, 16>& point);
    int TemporarySpace(std::string_view point, u64& available_kib);
    int UnmountTemporary();
    static bool ValidTitle(std::string_view title);

private:
    using Save = Libraries::SaveData::SaveInstance;
    struct Quota {
        std::mutex mutex;
        std::filesystem::path root;
        u64 capacity{};
    };
    struct File {
        int host{-1};
        int slot{-1};
        bool writable{};
        bool append{};
        std::shared_ptr<Core::Directories::BaseDirectory> directory;
        std::string path;
        std::shared_ptr<Quota> quota;
        std::mutex cursor;
        std::shared_ptr<Core::FileSys::IFile> backend;
        File(int host, int slot, bool writable, bool append,
             std::shared_ptr<Core::Directories::BaseDirectory> directory, std::string path)
            : host(host), slot(slot), writable(writable), append(append),
              directory(std::move(directory)), path(std::move(path)) {}
        ~File();
    };
    struct Parent {
        int fd{-1};
        int slot{-1};
        std::string leaf;
        int error{};
        std::string virtual_path;
    };
    // Read-only opens/stat share namespace stability; mutations/mount teardown
    // exclude them. Established descriptor I/O never acquires this gate.
    std::shared_mutex namespace_mutex;
    // Descriptor registry only: never held across a syscall or file guard.
    std::mutex files_mutex;
    Core::FileSys::MntPoints& mounts;
    std::filesystem::path home;
    std::string title;
    int user;
    bool initialized{};
    std::atomic<bool> cancelled{};
    std::deque<Event> events;
    std::unique_ptr<Libraries::SaveData::SaveMemory::Store> save_memory;
    void CopyTree(const std::filesystem::path& from, const std::filesystem::path& to);
    Error CheckIdentity(int uid, std::string_view tid, std::string_view directory);
    std::array<std::unique_ptr<Save>, 16> slots;
    std::array<std::shared_ptr<Quota>, 16> quotas;
    std::map<int, std::shared_ptr<File>> files;
    // Closed handles can still have admitted I/O. Mount retirement must include
    // those leases, not just the visible descriptor table.
    std::vector<std::pair<int, std::weak_ptr<File>>> file_leases;
    std::shared_ptr<File> AcquireFile(int fd);
    bool HasFileLease(int slot);
    IoObserver io_observer{};
    std::array<std::mutex, 3> stdio_mutex;
    std::array<std::unique_ptr<Core::Devices::Logger>, 3> stdio;
    std::filesystem::path temporary_root;
    int next_fd{3};
    Save* Find(std::string_view point);
    Parent Resolve(std::string_view path, bool write, bool allow_root = false);
    int CheckGrowth(const File& file, u64 old_size, u64 end);
    Error UnmountLocked(std::string_view point);
    int UnmountTemporaryLocked();
    static u64 Used(const std::filesystem::path& root);
};
static_assert(sizeof(GuestStorage::MountResult) == 64);
static_assert(sizeof(GuestStorage::MountInfo) == 48);
} // namespace Core::HostRuntime
