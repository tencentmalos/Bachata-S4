// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include "common/path_util.h"
#include "common/elf_info.h"
#include "common/scope_exit.h"
#include "core/file_sys/fs.h"
#include "core/file_sys/directories/normal_directory.h"
#include "guest_storage.h"

namespace Core::HostRuntime {
namespace fs = std::filesystem;
using namespace Libraries::SaveData;
namespace {
bool Component(std::string_view value) {
    return !value.empty() && value != "." && value != ".." &&
           value.find_first_of("/\\\0", 0, 3) == std::string_view::npos;
}
// The app owns the root. Reject symlinks at every level before backend metadata
// operations; guest file operations use descriptor-relative O_NOFOLLOW below.
void SafeDirectory(const fs::path& root, const fs::path& relative) {
    fs::path current = root;
    fs::create_directories(root);
    for (const auto& part : relative) {
        current /= part;
        auto status = fs::symlink_status(current);
        if (fs::is_symlink(status))
            throw fs::filesystem_error(
                "Save path is a symlink", current,
                std::make_error_code(std::errc::too_many_symbolic_link_levels));
        if (!fs::exists(status))
            fs::create_directory(current);
        else if (!fs::is_directory(status))
            throw fs::filesystem_error("Save path is not a directory", current,
                                       std::make_error_code(std::errc::not_a_directory));
    }
}
GuestStorage::Error Failure(const fs::filesystem_error& e) {
    LOG_ERROR(Lib_SaveData, "Session savedata operation failed: {}", e.what());
    if (e.code() == std::errc::no_space_on_device)
        return GuestStorage::Error::NO_SPACE_FS;
    if (e.code() == std::errc::illegal_byte_sequence)
        return GuestStorage::Error::BROKEN;
    return GuestStorage::Error::INTERNAL;
}
void SyncPath(const fs::path& path, bool directory = false) {
    int fd =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : 0));
    if (fd < 0)
        throw fs::filesystem_error("Save open for sync", path,
                                   std::error_code(errno, std::generic_category()));
    int result = ::fsync(fd);
    int error = errno;
    ::close(fd);
    if (result)
        throw fs::filesystem_error("Save sync", path,
                                   std::error_code(error, std::generic_category()));
}
} // namespace
bool GuestStorage::ValidTitle(std::string_view id) {
    return id.size() == 9 &&
           std::all_of(id.begin(), id.begin() + 4, [](char c) { return c >= 'A' && c <= 'Z'; }) &&
           std::all_of(id.begin() + 4, id.end(), [](char c) { return c >= '0' && c <= '9'; });
}
GuestStorage::GuestStorage(FileSys::MntPoints& m, fs::path h, std::string t, int u)
    : mounts(m), home(std::move(h)), title(std::move(t)), user(u) {
    if (!home.is_absolute() || !ValidTitle(title) || user < 0)
        throw std::invalid_argument("invalid persistent savedata identity/root");
    // Context.filesDir may use Android's legitimate /data/user/0 symlink.
    // Resolve the host-provided root once; reject symlinks only below that root.
    home = fs::weakly_canonical(home);
    stdio[0] = std::make_unique<Core::Devices::Logger>("stdin", false);
    stdio[1] = std::make_unique<Core::Devices::Logger>("stdout", false);
    stdio[2] = std::make_unique<Core::Devices::Logger>("stderr", true);
}
GuestStorage::~GuestStorage() {
    for (auto& device : stdio) device->fsync();
    files.clear();
    file_leases.clear();
    if (const int error = UnmountTemporaryLocked())
        LOG_ERROR(Lib_AppContent, "Session temporary retirement failed: {}", error);
    for (auto& slot : slots)
        if (slot) {
            auto result = UnmountLocked(slot->GetMountPoint());
            if (result != Error::OK) {
                LOG_ERROR(Lib_SaveData, "Session save retirement failed: {:#x}",
                          static_cast<u32>(result));
                if (slot)
                    slot->Abandon();
            }
        }
}
int GuestStorage::MountTemporary(u32 option, std::array<char, 16>& point) {
    std::lock_guard lock(namespace_mutex);
    if (option > 1)
        return EINVAL;
    if (!temporary_root.empty() || mounts.GetMountSnapshot("/temp0"))
        return EBUSY;
    try {
        const auto base = Common::FS::GetUserPath(Common::FS::PathType::TempDataDir);
        if (!fs::is_directory(fs::symlink_status(base)))
            return ENOENT;
        // Both NONE and FORMAT start with empty session-owned storage. Never
        // format a live mount, another session's directory or persistent saves.
        std::string pattern = (base / (title + "-XXXXXX")).string();
        if (!::mkdtemp(pattern.data()))
            return errno;
        bool published = false;
        SCOPE_EXIT {
            if (!published) {
                std::error_code ignored;
                fs::remove_all(pattern, ignored);
                temporary_root.clear();
            }
        };
        temporary_root = pattern;
        mounts.Mount(temporary_root, "/temp0");
        published = true;
        point = {};
        std::memcpy(point.data(), "/temp0", 6);
        return 0;
    } catch (const fs::filesystem_error& e) {
        return e.code().value();
    }
}
int GuestStorage::TemporarySpace(std::string_view point, u64& available_kib) {
    std::lock_guard lock(namespace_mutex);
    if (point != "/temp0")
        return EINVAL;
    const auto mount = mounts.GetMountSnapshot("/temp0");
    if (temporary_root.empty() || !mount || mount->host_path != temporary_root)
        return ENOENT;
    std::error_code error;
    const auto info = fs::space(temporary_root, error);
    if (error)
        return error.value();
    available_kib = info.available / 1024;
    return 0;
}
int GuestStorage::UnmountTemporaryLocked() {
    if (temporary_root.empty())
        return 0;
    if (HasFileLease(-2)) return EBUSY;
    mounts.UnmountOwned(temporary_root, "/temp0");
    std::error_code error;
    fs::remove_all(temporary_root, error);
    if (error)
        return error.value(); // retain ownership so cleanup can be retried
    temporary_root.clear();
    return 0;
}
int GuestStorage::UnmountTemporary() {
    std::lock_guard lock(namespace_mutex);
    return UnmountTemporaryLocked();
}
GuestStorage::Error GuestStorage::Initialize() {
    std::lock_guard lock(namespace_mutex);
    try {
        SafeDirectory(home, fs::path(std::to_string(user)) / "savedata" / title);
    } catch (const fs::filesystem_error& e) {
        return Failure(e);
    }
    initialized = true;
    return Error::OK;
}
GuestStorage::Error GuestStorage::Terminate() {
    std::lock_guard lock(namespace_mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    if (std::any_of(slots.begin(), slots.end(), [](const auto& s) { return bool(s); }))
        return Error::BUSY;
    initialized = false;
    save_memory.reset();
    return Error::OK;
}
u64 GuestStorage::Used(const fs::path& root) {
    u64 used{};
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator();
         ++it) {
        const auto& e = *it;
        if (it.depth() == 0 && e.path().filename().string().starts_with("sce_backup")) {
            it.disable_recursion_pending();
            continue;
        }
        if (e.is_symlink())
            throw fs::filesystem_error(
                "Symlink in save", e.path(),
                std::make_error_code(std::errc::too_many_symbolic_link_levels));
        if (e.is_regular_file())
            used += e.file_size();
    }
    return used;
}
GuestStorage::Error GuestStorage::Mount(int uid, std::string_view tid, std::string_view dir,
                                        u64 blocks, u32 mode, MountResult& result) {
    std::lock_guard lock(namespace_mutex);
    result = {};
    if (!initialized)
        return Error::NOT_INITIALIZED;
    if (uid != user)
        return Error::INVALID_LOGIN_USER;
    if ((!tid.empty() && tid != title) || !Component(dir) || dir.starts_with(".") ||
        dir.size() >= 32 || (mode & ~0x3fu) || ((mode & 3) != 1 && (mode & 3) != 2) ||
        ((mode & 4) && (mode & 32)) || ((mode & 1) && (mode & (4 | 32))))
        return Error::PARAMETER;
    int slot = -1;
    for (int i = 0; i < 16; ++i) {
        if (slots[i] && slots[i]->GetDirName() == dir)
            return Error::BUSY;
        if (!slots[i] && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return Error::MOUNT_FULL;
    const auto path = home / std::to_string(user) / "savedata" / title / dir;
    bool created_directory = false;
    try {
        // An existing partial/corrupt save must never be recreated over in place.
        bool existed = fs::exists(path);
        if (fs::is_symlink(fs::symlink_status(path)))
            return Error::BROKEN;
        if (!existed && !(mode & (4 | 32)))
            return Error::NOT_FOUND;
        if (existed && (mode & 4))
            return Error::EXISTS;
        if (!existed && (blocks < 96 || blocks > 32768))
            return Error::PARAMETER;
        if (existed && !fs::exists(Save::GetParamSFOPath(path)))
            return Error::BROKEN;
        SafeDirectory(home, path.lexically_relative(home));
        created_directory = !existed;
        // Also reject symlink metadata and files before opening the save backend.
        (void)Used(path);
        if (!existed) {
            const u64 available = fs::space(path).available;
            if (available < blocks * 32768) {
                result.required_blocks = (blocks * 32768 - available + 32767) / 32768;
                fs::remove(path);
                return Error::NO_SPACE_FS;
            }
        }
        auto instance = std::make_unique<Save>(
            slot, uid, title, dir, static_cast<int>(std::min<u64>(blocks, 32768)), &mounts, path);
        instance->SetupAndMount(mode & 1, mode & 16, mode & 8, true);
        std::copy(instance->GetMountPoint().begin(), instance->GetMountPoint().end(),
                  result.point.begin());
        result.status = !existed && (mode & 32) ? 1 : 0;
        auto quota = std::make_shared<Quota>();
        quota->root = instance->GetSavePath();
        quota->capacity = u64(instance->GetMaxBlocks()) * 32768;
        quotas[slot] = std::move(quota);
        slots[slot] = std::move(instance);
        return Error::OK;
    } catch (const fs::filesystem_error& e) {
        if (created_directory) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
        return Failure(e);
    }
}
GuestStorage::Save* GuestStorage::Find(std::string_view point) {
    for (auto& slot : slots)
        if (slot && slot->GetMountPoint() == point)
            return slot.get();
    return nullptr;
}
GuestStorage::Error GuestStorage::UnmountLocked(std::string_view point) {
    for (int i = 0; i < 16; ++i)
        if (slots[i] && slots[i]->GetMountPoint() == point) {
            if (HasFileLease(i)) return Error::BUSY;
            try {
                if (!slots[i]->IsReadOnly()) {
                    for (const auto& e :
                         fs::recursive_directory_iterator(slots[i]->GetSavePath())) {
                        if (e.is_regular_file())
                            SyncPath(e.path());
                    }
                }
                slots[i]->Umount();
                slots[i].reset();
                quotas[i].reset();
                return Error::OK;
            } catch (const fs::filesystem_error& e) {
                return Failure(e);
            }
        }
    return Error::NOT_MOUNTED;
}
GuestStorage::Error GuestStorage::Unmount(std::string_view point) {
    std::lock_guard lock(namespace_mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    return UnmountLocked(point);
}
GuestStorage::Error GuestStorage::Info(std::string_view point, MountInfo& result) {
    std::lock_guard lock(namespace_mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    auto* s = Find(point);
    if (!s)
        return Error::NOT_MOUNTED;
    std::unique_lock<std::mutex> quota;
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].get() == s) { quota = std::unique_lock(quotas[i]->mutex); break; }
    try {
        result = {};
        result.blocks = s->GetMaxBlocks();
        auto used = (Used(s->GetSavePath()) + 32767) / 32768;
        result.free_blocks = result.blocks > used ? result.blocks - used : 0;
    } catch (const fs::filesystem_error& e) {
        return Failure(e);
    }
    return Error::OK;
}
GuestStorage::Error GuestStorage::GetParam(std::string_view point, u32 type, std::span<u8> out,
                                           u64& size) {
    std::lock_guard lock(namespace_mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    auto* s = Find(point);
    if (!s)
        return Error::NOT_MOUNTED;
    OrbisSaveDataParam param{};
    param.FromSFO(s->GetParamSFO());
    const void* data{};
    switch (type) {
    case 0:
        data = &param;
        size = sizeof(param);
        break;
    case 1:
        data = &param.title;
        size = sizeof(param.title);
        break;
    case 2:
        data = &param.subTitle;
        size = sizeof(param.subTitle);
        break;
    case 3:
        data = &param.detail;
        size = sizeof(param.detail);
        break;
    case 4:
        data = &param.userParam;
        size = sizeof(param.userParam);
        break;
    case 5:
        data = &param.mtime;
        size = sizeof(param.mtime);
        break;
    default:
        return Error::PARAMETER;
    }
    if (out.size() < size)
        return Error::PARAMETER;
    std::memcpy(out.data(), data, size);
    return Error::OK;
}
GuestStorage::Error GuestStorage::SetParam(std::string_view point, u32 type,
                                           std::span<const u8> in) {
    std::lock_guard lock(namespace_mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    auto* s = Find(point);
    if (!s)
        return Error::NOT_MOUNTED;
    std::unique_lock<std::mutex> quota;
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].get() == s) { quota = std::unique_lock(quotas[i]->mutex); break; }
    if (s->IsReadOnly())
        return Error::BAD_MOUNTED;
    OrbisSaveDataParam param{};
    param.FromSFO(s->GetParamSFO());
    void* data{};
    size_t size{};
    switch (type) {
    case 0:
        data = &param;
        size = sizeof(param);
        break;
    case 1:
        data = &param.title;
        size = sizeof(param.title);
        break;
    case 2:
        data = &param.subTitle;
        size = sizeof(param.subTitle);
        break;
    case 3:
        data = &param.detail;
        size = sizeof(param.detail);
        break;
    case 4:
        data = &param.userParam;
        size = sizeof(param.userParam);
        break;
    default:
        return Error::PARAMETER;
    }
    if (in.size() != size)
        return Error::PARAMETER;
    std::memcpy(data, in.data(), size);
    if (!std::memchr(&param.title, 0, sizeof(param.title)) ||
        !std::memchr(&param.subTitle, 0, sizeof(param.subTitle)) ||
        !std::memchr(&param.detail, 0, sizeof(param.detail)))
        return Error::PARAMETER;
    param.ToSFO(s->GetParamSFO());
    return Error::OK;
}
// Walk from the selected mount root with O_NOFOLLOW. No host path, symlink,
// '..', or stale /savedataN handle can escape into the host application's files.
GuestStorage::Parent GuestStorage::Resolve(std::string_view path, bool write, bool allow_root) {
    Parent p;
    if (path.empty() || path.front() != '/' || path.find('\0') != path.npos) {
        p.error = EINVAL;
        return p;
    }
    // POSIX separators and '.' do not change mount ownership. Keep '..' rejected
    // instead of normalizing it through a mount or metadata boundary.
    std::string normalized;
    for (size_t start = 1; start <= path.size();) {
        auto end = path.find('/', start);
        if (end == path.npos) end = path.size();
        const auto part = path.substr(start, end - start);
        if (!part.empty() && part != ".") {
            if (!Component(part)) { p.error = EACCES; return p; }
            normalized += '/';
            normalized += part;
        }
        start = end + 1;
    }
    path = normalized;
    const auto slash = path.find('/', 1);
    if (!allow_root && (slash == path.npos || slash + 1 == path.size())) {
        p.error = EINVAL;
        return p;
    }
    auto mount = path.substr(0, slash);
    fs::path root;
    for (int i = 0; i < 16; ++i)
        if (slots[i] && slots[i]->GetMountPoint() == mount) {
            p.slot = i;
            root = slots[i]->GetSavePath();
            if (write && slots[i]->IsReadOnly()) {
                p.error = EROFS;
                return p;
            }
        }
    if (root.empty() && mount == "/temp0" && !temporary_root.empty()) {
        const auto m = mounts.GetMountSnapshot("/temp0");
        if (!m || m->host_path != temporary_root) {
            p.error = ENOENT;
            return p;
        }
        p.slot = -2;
        root = temporary_root;
    }
    if (root.empty()) {
        const bool addcont = mount.starts_with("/addcont") && mount.size() > 8 &&
                             std::all_of(mount.begin() + 8, mount.end(),
                                         [](char c) { return c >= '0' && c <= '9'; });
        if (mount != "/app0" && !addcont) {
            p.error = ENOENT;
            return p;
        }
        if (write) {
            p.error = EROFS;
            return p;
        }
        const auto m = mounts.GetMountSnapshot(std::string(mount));
        if (!m) {
            p.error = ENOENT;
            return p;
        }
        root = m->host_path;
    }
    int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        p.error = errno;
        return p;
    }
    if (slash == path.npos || slash + 1 == path.size()) {
        p.fd = fd;
        p.leaf = "."; // Only Stat may address the selected mount itself.
        return p;
    }
    auto tail = path.substr(slash + 1);
    while (true) {
        auto next = tail.find('/');
        auto part = tail.substr(0, next);
        if (!Component(part) ||
            (p.slot >= 0 && (part == "sce_sys" || part.starts_with("sce_backup")))) {
            ::close(fd);
            p.error = EACCES;
            return p;
        }
        if (next == tail.npos) {
            p.fd = fd;
            p.leaf = part;
            return p;
        }
        int child = ::openat(fd, std::string(part).c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int error = errno;
        ::close(fd);
        if (child < 0) {
            p.error = error;
            return p;
        }
        fd = child;
        tail.remove_prefix(next + 1);
    }
}
GuestStorage::Error GuestStorage::SetupMemory(int uid, u64 size,
    const Libraries::SaveData::OrbisSaveDataParam* param) {
    std::lock_guard lock(namespace_mutex);
    const auto status = CheckIdentity(uid, title, "sce_sdmemory");
    if (status != Error::OK) return status;
    if (!size || size > 64 * 1024 * 1024) return Error::PARAMETER;
    try {
        SafeDirectory(home, fs::path(std::to_string(user)) / "savedata" / title / "sce_sdmemory/sce_sys");
        if (!save_memory) save_memory = std::make_unique<Libraries::SaveData::SaveMemory::Store>(false, home);
        const auto existed = save_memory->SetupSaveMemory(user, 0, title, size);
        if (!existed) { save_memory->SaveSFO(0); save_memory->SetIcon(0); }
        if (param) { param->ToSFO(save_memory->GetParamSFO(0)); save_memory->SaveSFO(0); }
        return Error::OK;
    } catch (const fs::filesystem_error& e) { return Failure(e); }
}
GuestStorage::Error GuestStorage::Memory(int uid, std::span<u8> bytes, s64 offset, bool write) {
    std::shared_lock lock(namespace_mutex);
    if (!initialized) return Error::NOT_INITIALIZED;
    if (uid != user) return Error::INVALID_LOGIN_USER;
    if (!save_memory || !save_memory->IsSaveMemoryInitialized(0)) return Error::MEMORY_NOT_READY;
    const auto size = save_memory->MemorySize(0);
    if (offset < 0 || u64(offset) > size || bytes.size() > size - u64(offset)) return Error::PARAMETER;
    try {
        if (write) save_memory->WriteMemory(0, bytes.data(), bytes.size(), offset);
        else save_memory->ReadMemory(0, bytes.data(), bytes.size(), offset);
        return Error::OK;
    } catch (const fs::filesystem_error& e) { return Failure(e); }
}
GuestStorage::Error GuestStorage::SaveIcon(std::string_view point, std::span<const u8> bytes) {
    std::lock_guard lock(namespace_mutex);
    if (!initialized) return Error::NOT_INITIALIZED;
    auto* save = Find(point);
    if (!save) return Error::NOT_MOUNTED;
    if (save->IsReadOnly()) return Error::BAD_MOUNTED;
    try {
        Common::FS::IOFile file(save->GetIconPath(), Common::FS::FileAccessMode::Create);
        if (!file.IsOpen() || file.WriteRaw<u8>(bytes.data(), bytes.size()) != bytes.size()) return Error::INTERNAL;
        return Error::OK;
    } catch (const fs::filesystem_error& e) { return Failure(e); }
}
GuestStorage::Error GuestStorage::Search(const Libraries::SaveData::OrbisSaveDataDirNameSearchCond& c,
    Libraries::SaveData::OrbisSaveDataDirNameSearchResult& r) {
    std::shared_lock lock(namespace_mutex);
    if (!initialized) return Error::NOT_INITIALIZED;
    if (c.userId != user) return Error::INVALID_LOGIN_USER;
    if (c.titleId && std::string_view(c.titleId->data) != title) return Error::PARAMETER;
    try { return Libraries::SaveData::SearchSaveDirectories(&c, &r, title, Common::ElfInfo::Instance().FirmwareVer(), home); }
    catch (const fs::filesystem_error& e) { return Failure(e); }
}
GuestStorage::IoResult GuestStorage::Open(std::string_view path, u32 flags, u32 mode) {
    std::unique_lock mutation(namespace_mutex, std::defer_lock);
    std::shared_lock read_only(namespace_mutex, std::defer_lock);
    // Zero flags and O_DIRECTORY are the regular read-only FIOS paths. Other
    // modes may create/truncate and retain exclusive namespace admission.
    if (flags & ~u32{0x20000}) mutation.lock();
    else read_only.lock();
    constexpr u32 allowed = 3 | 4 | 8 | 0x80 | 0x200 | 0x400 | 0x800 | 0x1000 |
                            0x10000 | 0x20000;
    if ((flags & ~allowed) || (flags & 3) == 3)
        return {-1, EINVAL};
    bool write = (flags & 3) != 0;
    auto p = Resolve(path, write, true);
    if (p.error)
        return {-1, p.error};
    int native = (flags & 3) == 2 ? O_RDWR : (write ? O_WRONLY : O_RDONLY);
    if (flags & 8)
        native |= O_APPEND;
    if (flags & 0x80)
        native |= O_SYNC;
    if (flags & 0x200)
        native |= O_CREAT;
    if (flags & 0x400)
        native |= O_TRUNC;
    if (flags & 0x800)
        native |= O_EXCL;
    if (flags & 0x1000)
        native |= O_DSYNC;
    if (flags & 0x20000)
        native |= O_DIRECTORY;
    // Desktop also admits directory opens without O_DIRECTORY. Detect type
    // before open so a writable-directory request reports the guest EISDIR.
    struct stat before{};
    if (::fstatat(p.fd, p.leaf.c_str(), &before, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(before.st_mode) && (write || (flags & 0x400))) {
        ::close(p.fd);
        return {-1, EISDIR};
    }
    int fd =
        ::openat(p.fd, p.leaf.c_str(), native | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, mode & 0777);
    int error = errno;
    ::close(p.fd);
    if (fd < 0)
        return {-1, error};
    bool owned = false;
    SCOPE_EXIT { if (!owned) ::close(fd); };
    struct stat st {};
    if (::fstat(fd, &st))
        return {-1, errno};
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        return {-1, EACCES};
    }
    if (next_fd == std::numeric_limits<int>::max()) {
        return {-1, EMFILE};
    }
    std::shared_ptr<Core::Directories::BaseDirectory> directory;
    if (S_ISDIR(st.st_mode)) {
        try {
            directory = std::make_shared<Core::Directories::NormalDirectory>(
                [fd](const Core::Directories::NormalDirectory::Visitor& visitor) {
                    // New open description for each scan; dup would share the
                    // previous readdir cursor. The owned fd pins the directory.
                    int scan = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                    if (scan < 0) throw std::system_error(errno, std::generic_category());
                    DIR* dir = ::fdopendir(scan);
                    if (!dir) {
                        const int error = errno;
                        ::close(scan);
                        throw std::system_error(error, std::generic_category());
                    }
                    SCOPE_EXIT { ::closedir(dir); };
                    for (;;) {
                        errno = 0;
                        const auto* entry = ::readdir(dir);
                        if (!entry) {
                            if (errno) throw std::system_error(errno, std::generic_category());
                            break;
                        }
                        struct stat item{};
                        if (::fstatat(scan, entry->d_name, &item, AT_SYMLINK_NOFOLLOW))
                            continue; // An entry removed during enumeration.
                        if (S_ISREG(item.st_mode) || S_ISDIR(item.st_mode))
                            visitor(entry->d_name, S_ISREG(item.st_mode));
                    }
                });
        } catch (const std::system_error& e) { return {-1, e.code().value()}; }
    }
    auto file = std::make_shared<File>(fd, p.slot, write, bool(flags & 8),
                                       std::move(directory), std::string(path));
    owned = true;
    if (p.slot >= 0) file->quota = quotas[p.slot];
    int id;
    {
        std::lock_guard registry(files_mutex);
        id = next_fd++;
        files.emplace(id, file);
        std::erase_if(file_leases, [](const auto& f) { return f.second.expired(); });
        file_leases.emplace_back(p.slot, file);
    }
    if (io_observer) io_observer({IoEvent::Opened, id, file->path, 0});
    return {id, 0};
}
GuestStorage::File::~File() { if (host >= 0) ::close(host); }
std::shared_ptr<GuestStorage::File> GuestStorage::AcquireFile(int fd) {
    std::lock_guard lock(files_mutex);
    const auto it = files.find(fd);
    return it == files.end() ? nullptr : it->second;
}
bool GuestStorage::HasFileLease(int slot) {
    std::lock_guard registry(files_mutex);
    return std::any_of(file_leases.begin(), file_leases.end(), [slot](const auto& lease) {
        return lease.first == slot && !lease.second.expired();
    });
}
GuestStorage::MappingFile GuestStorage::AcquireMappingFile(int fd) {
    auto file = AcquireFile(fd);
    if (!file) return {{}, -1, false, EBADF};
    if (file->directory) return {{}, -1, false, ENODEV};
    const int flags = ::fcntl(file->host, F_GETFL);
    if (flags < 0) return {{}, -1, false, errno};
    if ((flags & O_ACCMODE) == O_WRONLY) return {{}, -1, false, EACCES};
    return {file, file->host, file->writable, 0};
}
namespace {
u64 IoNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
GuestStorage::IoResult GuestStorage::Close(int fd) {
    // Removing the guest handle prevents new operations. An already admitted
    // operation retains the native fd until its last lease is released, so fd
    // reuse cannot redirect an in-flight read to a different file.
    std::shared_ptr<File> retired;
    {
        std::lock_guard lock(files_mutex);
        if (fd >= 0 && fd < 3) return {-1, EPERM};
        auto it = files.find(fd);
        if (it == files.end()) return {-1, EBADF};
        retired = std::move(it->second);
        files.erase(it);
    }
    return {0, 0};
}
GuestStorage::IoResult GuestStorage::Read(int fd, std::span<u8> data) {
    const auto begin = io_observer ? IoNow() : 0;
    auto file = AcquireFile(fd);
    if (!file) return {-1, EBADF};
    std::lock_guard cursor(file->cursor);
    if (cancelled) return {-1, EINTR};
    if (file->directory) {
        try { return {file->directory->read(data.data(), data.size()), 0}; }
        catch (const std::system_error& e) { return {-1, e.code().value()}; }
    }
    const s64 offset = io_observer ? ::lseek(file->host, 0, SEEK_CUR) : 0;
    if (io_observer) io_observer({IoEvent::BeforeRead, fd, file->path, offset, data.size()});
    const auto syscall = io_observer ? IoNow() : 0;
    auto r = ::read(file->host, data.data(), data.size());
    const int error = r < 0 ? errno : 0;
    if (io_observer) io_observer({IoEvent::ReadDone, fd, file->path, offset, data.size(),
                                  begin, syscall, IoNow(), {r, error}});
    return {r, error};
}
GuestStorage::IoResult GuestStorage::Write(int fd, std::span<const u8> data) {
    if (fd >= 0 && fd < 3) {
        std::lock_guard output(stdio_mutex[fd]);
        return {data.empty() ? 0 : stdio[fd]->write(data.data(), data.size()), 0};
    }
    auto file = AcquireFile(fd);
    if (!file || !file->writable) return {-1, EBADF};
    std::lock_guard cursor(file->cursor);
    std::unique_lock<std::mutex> quota;
    if (file->quota) quota = std::unique_lock(file->quota->mutex);
    auto& f = *file;
    struct stat st {};
    if (::fstat(f.host, &st))
        return {-1, errno};
    auto offset = f.append ? st.st_size : ::lseek(f.host, 0, SEEK_CUR);
    if (offset < 0)
        return {-1, errno};
    const u64 end = u64(offset) + data.size();
    if (end < u64(offset))
        return {-1, EFBIG};
    if (!data.empty()) {
        if (int error = CheckGrowth(f, st.st_size, end))
            return {-1, error};
    }
    if (io_observer) io_observer({IoEvent::BeforeWrite, fd, f.path, offset, data.size()});
    auto r = ::write(f.host, data.data(), data.size());
    return {r, r < 0 ? errno : 0};
}
// The same quota admission applies to ordinary writes, positioned writes and
// truncation. Admission and mutation share only this save volume's quota lock.
int GuestStorage::CheckGrowth(const File& file, u64 old_size, u64 end) {
    if (!file.quota || end <= old_size)
        return 0;
    try {
        const auto used = Used(file.quota->root);
        const u64 cap = file.quota->capacity;
        if (end > cap || end - old_size > cap - std::min(cap, used))
            return ENOSPC;
    } catch (const fs::filesystem_error&) {
        return EIO;
    }
    return 0;
}
namespace {
void GuestStat(const struct stat& native, Libraries::Kernel::OrbisKernelStat& out) {
    // PS4/FreeBSD ABI, never copy an Android/Darwin struct stat into guest memory.
    // Match desktop's logical file sizes and block geometry for extracted content.
    out = {};
    const bool directory = S_ISDIR(native.st_mode);
    out.st_mode = (directory ? 0040000 : 0100000) | 0777;
    out.st_size = directory ? 65536 : native.st_size;
    out.st_blksize = directory ? 65536 : 512;
    out.st_blocks = (out.st_size + 511) / 512;
#if defined(__APPLE__)
    const auto atime = native.st_atimespec, mtime = native.st_mtimespec,
               ctime = native.st_ctimespec;
#else
    const auto atime = native.st_atim, mtime = native.st_mtim, ctime = native.st_ctim;
#endif
    out.st_atim = {atime.tv_sec, atime.tv_nsec};
    out.st_mtim = {mtime.tv_sec, mtime.tv_nsec};
    out.st_ctim = {ctime.tv_sec, ctime.tv_nsec};
}
static_assert(sizeof(Libraries::Kernel::OrbisKernelStat) == 120);
static_assert(offsetof(Libraries::Kernel::OrbisKernelStat, st_size) == 72);
} // namespace
GuestStorage::IoResult GuestStorage::Stat(std::string_view path,
                                          Libraries::Kernel::OrbisKernelStat& out) {
    std::shared_lock lock(namespace_mutex);
    auto p = Resolve(path, false, true);
    if (p.error)
        return {-1, p.error};
    struct stat native {};
    const int result = ::fstatat(p.fd, p.leaf.c_str(), &native, AT_SYMLINK_NOFOLLOW);
    const int error = errno;
    ::close(p.fd);
    if (result < 0)
        return {-1, error};
    if (!S_ISREG(native.st_mode) && !S_ISDIR(native.st_mode))
        return {-1, EACCES};
    GuestStat(native, out);
    return {0, 0};
}
GuestStorage::IoResult GuestStorage::Fstat(int fd, Libraries::Kernel::OrbisKernelStat& out) {
    auto file = AcquireFile(fd);
    if (!file) return {-1, EBADF};
    std::lock_guard cursor(file->cursor);
    if (file->directory) {
        out = {};
        return {file->directory->fstat(&out), 0};
    }
    struct stat native {};
    if (::fstat(file->host, &native))
        return {-1, errno};
    GuestStat(native, out);
    return {0, 0};
}
GuestStorage::IoResult GuestStorage::Positioned(int fd, std::span<const Buffer> buffers, s64 offset,
                                                bool write) {
    const auto begin = io_observer ? IoNow() : 0;
    if (offset < 0 || buffers.size() > 1024)
        return {-1, EINVAL};
    auto lease = AcquireFile(fd);
    if (!lease || (write && !lease->writable)) return {-1, EBADF};
    auto& file = *lease;
    // Regular positioned reads do not use/update the shared cursor or flags.
    // Directories emulate positioned reads by seek/read/restore and need it.
    std::unique_lock cursor(file.cursor, std::defer_lock);
    if (write || file.directory) cursor.lock();
    std::unique_lock<std::mutex> quota;
    if (write && file.quota) quota = std::unique_lock(file.quota->mutex);
    std::vector<iovec> vectors;
    u64 total{};
    for (const auto& buffer : buffers) {
        if (buffer.size > u64(INT64_MAX) - total)
            return {-1, EINVAL};
        total += buffer.size;
        vectors.push_back({buffer.data, buffer.size});
    }
    if (total > u64(INT64_MAX) - u64(offset))
        return {-1, EOVERFLOW};
    if (cancelled)
        return {-1, EINTR};
    if (file.directory) {
        std::vector<Libraries::Kernel::OrbisKernelIovec> directory_vectors;
        for (const auto& buffer : buffers)
            directory_vectors.push_back({buffer.data, buffer.size});
        try {
            return {file.directory->preadv(directory_vectors.data(), directory_vectors.size(), offset),
                    0};
        } catch (const std::system_error& e) { return {-1, e.code().value()}; }
    }
    if (write) {
        struct stat native {};
        if (::fstat(file.host, &native))
            return {-1, errno};
        if (total) {
            if (int error = CheckGrowth(file, native.st_size, u64(offset) + total))
                return {-1, error};
        }
    }
    // Linux pwritev otherwise honors O_APPEND, unlike the positioned API. The
    // descriptor is private; writes/seek share its cursor guard. Read leases
    // retain its lifetime; preadv ignores both the cursor and O_APPEND.
    int flags = 0;
    if (write && file.append) {
        flags = ::fcntl(file.host, F_GETFL);
        if (flags < 0 || ::fcntl(file.host, F_SETFL, flags & ~O_APPEND))
            return {-1, errno};
    }
    if (!write && io_observer) io_observer({IoEvent::BeforeRead, fd, file.path, offset, total});
    const auto syscall = !write && io_observer ? IoNow() : 0;
    const auto result = write ? ::pwritev(file.host, vectors.data(), vectors.size(), offset)
                              : ::preadv(file.host, vectors.data(), vectors.size(), offset);
    const int error = result < 0 ? errno : 0;
    if (!write && io_observer) io_observer({IoEvent::ReadDone, fd, file.path, offset, total,
                                           begin, syscall, IoNow(), {result, error}});
    if (write && file.append && ::fcntl(file.host, F_SETFL, flags))
        return {-1, errno};
    return {result, error};
}
GuestStorage::IoResult GuestStorage::Truncate(int fd, s64 length) {
    if (length < 0)
        return {-1, EINVAL};
    auto file = AcquireFile(fd);
    if (!file || !file->writable) return {-1, EBADF};
    std::lock_guard cursor(file->cursor);
    std::unique_lock<std::mutex> quota;
    if (file->quota) quota = std::unique_lock(file->quota->mutex);
    struct stat native {};
    if (::fstat(file->host, &native))
        return {-1, errno};
    if (int error = CheckGrowth(*file, native.st_size, length))
        return {-1, error};
    const int result = ::ftruncate(file->host, length);
    return {result, result < 0 ? errno : 0};
}
GuestStorage::IoResult GuestStorage::Seek(int fd, s64 offset, int whence) {
    auto file = AcquireFile(fd);
    if (!file) return {-1, EBADF};
    std::lock_guard cursor(file->cursor);
    if (whence < 0 || whence > 2)
        return {-1, EINVAL};
    if (file->directory) {
        const auto result = file->directory->lseek(offset, whence);
        return {result, result < 0 ? EINVAL : 0};
    }
    auto r = ::lseek(file->host, offset, whence);
    return {r, r < 0 ? errno : 0};
}
GuestStorage::IoResult GuestStorage::Sync(int fd) {
    if (fd >= 0 && fd < 3) {
        std::lock_guard output(stdio_mutex[fd]); return {stdio[fd]->fsync(), 0};
    }
    auto file = AcquireFile(fd);
    if (!file) return {-1, EBADF};
    std::lock_guard cursor(file->cursor);
    auto r = ::fsync(file->host);
    return {r, r < 0 ? errno : 0};
}
GuestStorage::IoResult GuestStorage::GetDents(int fd, std::span<u8> bytes, s64* base) {
    auto file = AcquireFile(fd);
    if (!file) return {-1, EBADF};
    std::lock_guard cursor(file->cursor);
    if (!file->directory || bytes.size() < 512) return {-1, EINVAL};
    if (cancelled) return {-1, EINTR};
    try { return {file->directory->getdents(bytes.data(), bytes.size(), base), 0}; }
    catch (const std::system_error& e) { return {-1, e.code().value()}; }
}
GuestStorage::IoResult GuestStorage::Mkdir(std::string_view path, u32 mode) {
    std::lock_guard lock(namespace_mutex);
    auto p = Resolve(path, true, true);
    if (p.error)
        return {-1, p.error};
    std::unique_lock<std::mutex> quota;
    if (p.slot >= 0) quota = std::unique_lock(quotas[p.slot]->mutex);
    auto r = ::mkdirat(p.fd, p.leaf.c_str(), mode & 0777);
    int e = errno;
    ::close(p.fd);
    return {r, r < 0 ? e : 0};
}
GuestStorage::IoResult GuestStorage::Unlink(std::string_view path) {
    std::lock_guard lock(namespace_mutex);
    auto p = Resolve(path, true);
    if (p.error)
        return {-1, p.error};
    std::unique_lock<std::mutex> quota;
    if (p.slot >= 0) quota = std::unique_lock(quotas[p.slot]->mutex);
    auto r = ::unlinkat(p.fd, p.leaf.c_str(), 0);
    int e = errno;
    ::close(p.fd);
    return {r, r < 0 ? e : 0};
}
GuestStorage::IoResult GuestStorage::Rename(std::string_view from, std::string_view to) {
    std::lock_guard lock(namespace_mutex);
    auto a = Resolve(from, true);
    if (a.error)
        return {-1, a.error};
    auto b = Resolve(to, true);
    if (b.error) {
        ::close(a.fd);
        return {-1, b.error};
    }
    int r = -1, e = EXDEV;
    std::unique_lock<std::mutex> quota;
    if (a.slot >= 0) quota = std::unique_lock(quotas[a.slot]->mutex);
    if (a.slot == b.slot) {
        r = ::renameat(a.fd, a.leaf.c_str(), b.fd, b.leaf.c_str());
        e = errno;
    }
    ::close(a.fd);
    ::close(b.fd);
    return {r, r < 0 ? e : 0};
}

namespace {
void PublishDirectory(const fs::path& pending, const fs::path& destination) {
    if (!fs::exists(destination)) {
        fs::rename(pending, destination);
        return;
    }
#if defined(__linux__)
    // Same-filesystem atomic exchange leaves the previous version recoverable
    // at pending until publication and parent-directory fsync have succeeded.
    if (::syscall(SYS_renameat2, AT_FDCWD, pending.c_str(), AT_FDCWD, destination.c_str(), 2) != 0)
        throw fs::filesystem_error("atomic savedata exchange", pending, destination,
                                   std::error_code(errno, std::generic_category()));
#else
    throw fs::filesystem_error("atomic savedata exchange unavailable", pending,
                               std::make_error_code(std::errc::operation_not_supported));
#endif
}
} // namespace
GuestStorage::Error GuestStorage::CheckIdentity(int uid, std::string_view tid,
                                                std::string_view directory) {
    if (!initialized)
        return Error::NOT_INITIALIZED;
    if (uid != user)
        return Error::INVALID_LOGIN_USER;
    if ((!tid.empty() && tid != title) || !Component(directory) || directory.starts_with(".") ||
        directory.size() >= 32)
        return Error::PARAMETER;
    for (const auto& s : slots)
        if (s && s->GetDirName() == directory)
            return Error::BUSY;
    return Error::OK;
}
void GuestStorage::CopyTree(const fs::path& from, const fs::path& to) {
    if (cancelled)
        throw fs::filesystem_error("Save operation cancelled", from,
                                   std::make_error_code(std::errc::operation_canceled));
    const auto state = fs::symlink_status(from);
    if (fs::is_symlink(state))
        throw fs::filesystem_error("Save symlink", from,
                                   std::make_error_code(std::errc::too_many_symbolic_link_levels));
    if (fs::is_directory(state)) {
        fs::create_directory(to);
        for (const auto& e : fs::directory_iterator(from)) {
            auto name = e.path().filename().string();
            if (name.starts_with("sce_backup") || name == "corrupted" ||
                name == "param.sfo.pending")
                continue;
            CopyTree(e.path(), to / e.path().filename());
        }
        SyncPath(to, true);
        return;
    }
    if (!fs::is_regular_file(state))
        throw fs::filesystem_error("Save special file", from,
                                   std::make_error_code(std::errc::invalid_argument));
    Common::FS::IOFile input(from, Common::FS::FileAccessMode::Read);
    Common::FS::IOFile output(to, Common::FS::FileAccessMode::Create);
    std::array<u8, 65536> buffer{};
    u64 remaining = input.GetSize();
    if (!input.IsOpen() || !output.IsOpen())
        throw fs::filesystem_error("Save copy open", from,
                                   std::make_error_code(std::errc::io_error));
    while (remaining) {
        if (cancelled)
            throw fs::filesystem_error("Save operation cancelled", from,
                                       std::make_error_code(std::errc::operation_canceled));
        size_t count = std::min<u64>(remaining, buffer.size());
        if (input.ReadRaw<u8>(buffer.data(), count) != count ||
            output.WriteRaw<u8>(buffer.data(), count) != count)
            throw fs::filesystem_error("Save short copy", from, to,
                                       std::make_error_code(std::errc::io_error));
        remaining -= count;
    }
    output.Close();
    if (fs::file_size(to) != input.GetSize())
        throw fs::filesystem_error("Save incomplete flush", to,
                                   std::make_error_code(std::errc::io_error));
    SyncPath(to);
}
GuestStorage::Error GuestStorage::UnmountBackup(std::string_view point) {
    std::lock_guard lock(namespace_mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    auto* save = Find(point);
    if (!save)
        return Error::NOT_MOUNTED;
    const auto path = save->GetSavePath();
    const auto directory = save->GetDirName();
    if (events.size() >= 64)
        return Error::BUSY;
    auto status = UnmountLocked(point);
    if (status != Error::OK)
        return status;
    try {
        const auto pending = path / "sce_backup_tmp";
        // Only reserved scratch, never the last committed backup.
        fs::remove_all(pending);
        CopyTree(path, pending);
        PublishDirectory(pending, path / "sce_backup");
        SyncPath(path, true);
        fs::remove_all(pending);
    } catch (const fs::filesystem_error& e) {
        status = Failure(e);
    }
    Event event{};
    event.type = 1;
    event.error = u32(status);
    event.user = user;
    event.title.data.FromString(title);
    event.directory.data.FromString(directory);
    events.push_back(event);
    return status;
}
GuestStorage::Error GuestStorage::CheckBackup(int uid, std::string_view tid,
                                              std::string_view directory, OrbisSaveDataParam& param,
                                              std::vector<u8>& icon) {
    std::lock_guard lock(namespace_mutex);
    auto status = CheckIdentity(uid, tid, directory);
    if (status != Error::OK)
        return status;
    try {
        const auto path =
            home / std::to_string(user) / "savedata" / title / directory / "sce_backup";
        if (!fs::exists(path))
            return Error::NOT_FOUND;
        (void)Used(path);
        PSF psf;
        if (!psf.Open(Save::GetParamSFOPath(path)))
            return Error::BROKEN;
        param.FromSFO(psf);
        const auto icon_path = path / "sce_sys/icon0.png";
        if (fs::exists(icon_path)) {
            if (fs::file_size(icon_path) > 4 * 1024 * 1024)
                return Error::BROKEN;
            Common::FS::IOFile input(icon_path, Common::FS::FileAccessMode::Read);
            icon.resize(input.GetSize());
            if (input.ReadRaw<u8>(icon.data(), icon.size()) != icon.size())
                return Error::INTERNAL;
        }
        return Error::OK;
    } catch (const fs::filesystem_error& e) {
        return Failure(e);
    }
}
GuestStorage::Error GuestStorage::RestoreBackup(int uid, std::string_view tid,
                                                std::string_view directory) {
    std::lock_guard lock(namespace_mutex);
    auto status = CheckIdentity(uid, tid, directory);
    if (status != Error::OK)
        return status;
    try {
        const auto path = home / std::to_string(user) / "savedata" / title / directory;
        const auto backup = path / "sce_backup";
        if (!fs::exists(backup))
            return Error::NOT_FOUND;
        (void)Used(backup);
        PSF psf;
        if (!psf.Open(Save::GetParamSFOPath(backup)))
            return Error::BROKEN;
        const auto pending = path.parent_path() / (".restore-" + std::string(directory));
        fs::remove_all(pending);
        CopyTree(backup, pending);
        // Keep a known-good backup in the restored version as well.
        CopyTree(backup, pending / "sce_backup");
        if (cancelled)
            return Error::INTERNAL;
        PublishDirectory(pending, path);
        SyncPath(path.parent_path(), true);
        fs::remove_all(pending);
        return Error::OK;
    } catch (const fs::filesystem_error& e) {
        return Failure(e);
    }
}
GuestStorage::Error GuestStorage::Delete(int uid, std::string_view tid,
                                         std::string_view directory) {
    std::lock_guard lock(namespace_mutex);
    auto status = CheckIdentity(uid, tid, directory);
    if (status != Error::OK)
        return status;
    try {
        const auto path = home / std::to_string(user) / "savedata" / title / directory;
        if (!fs::exists(path))
            return Error::NOT_FOUND;
        (void)Used(path);
        const auto retired = path.parent_path() / (".delete-" + std::string(directory));
        fs::remove_all(retired);
        fs::rename(path, retired);
        SyncPath(path.parent_path(), true);
        fs::remove_all(retired);
        return Error::OK;
    } catch (const fs::filesystem_error& e) {
        return Failure(e);
    }
}
GuestStorage::Error GuestStorage::GetEvent(Event& event) {
    std::lock_guard lock(namespace_mutex);
    if (!initialized)
        return Error::NOT_INITIALIZED;
    if (events.empty())
        return Error::NOT_FOUND;
    event = events.front();
    events.pop_front();
    return Error::OK;
}

} // namespace Core::HostRuntime
