// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "save_memory.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <fmt/format.h>

#include "boost/icl/concept/interval.hpp"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/file_sys/fs.h"
#include "core/libraries/system/msgdialog_ui.h"
#include "save_instance.h"

using Common::FS::IOFile;
namespace fs = std::filesystem;

constexpr std::string_view sce_sys = "sce_sys"; // system folder inside save
constexpr std::string_view StandardDirnameSaveDataMemory = "sce_sdmemory";
constexpr std::string_view FilenameSaveDataMemory = "memory.dat";
constexpr std::string_view IconName = "icon0.png";
constexpr std::string_view CorruptFileName = "corrupted";

namespace Libraries::SaveData::SaveMemory {

static Core::FileSys::MntPoints* GetMounts() {
    return Common::Singleton<Core::FileSys::MntPoints>::Instance();
}

struct SlotData {
    OrbisUserServiceUserId user_id{};
    std::string game_serial;
    std::filesystem::path folder_path;
    PSF sfo;
    std::vector<u8> memory_cache;
    size_t memory_cache_size{};
};

struct Store::Impl {
    std::mutex mutex;
    std::unordered_map<u32, SlotData> slots;
    bool desktop_backups;
    fs::path home;
};
Store::Store(bool desktop_backups, fs::path home) : impl(std::make_unique<Impl>()) {
    impl->desktop_backups = desktop_backups;
    impl->home = std::move(home);
}
Store::~Store() = default;
size_t Store::MemorySize(u32 slot) {
    std::lock_guard lock(impl->mutex);
    const auto it = impl->slots.find(slot);
    return it == impl->slots.end() ? 0 : it->second.memory_cache_size;
}
static Store default_store;

void Store::PersistMemory(u32 slot_id, bool lock) {
    std::unique_lock lck{impl->mutex, std::defer_lock};
    if (lock) {
        lck.lock();
    }
    auto& data = impl->slots[slot_id];
    auto memoryPath = data.folder_path / FilenameSaveDataMemory;
    fs::create_directories(memoryPath.parent_path());

    int n = 0;
    std::string errMsg;
    while (n++ < 10) {
        try {
            IOFile f;
            int r = f.Open(memoryPath, Common::FS::FileAccessMode::Create);
            if (f.IsOpen()) {
                if (f.WriteRaw<u8>(data.memory_cache.data(), data.memory_cache.size()) !=
                    data.memory_cache.size())
                    throw fs::filesystem_error("short save memory write", memoryPath,
                                               std::make_error_code(std::errc::io_error));
                f.Close();
                return;
            }
            const auto err = std::error_code{r, std::iostream_category()};
            throw std::filesystem::filesystem_error{err.message(), err};
        } catch (const std::filesystem::filesystem_error& e) {
            if (!impl->desktop_backups)
                throw;
            errMsg = std::string{e.what()};
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    const MsgDialog::MsgDialogState dialog{MsgDialog::MsgDialogState::UserState{
        .type = MsgDialog::ButtonType::OK,
        .msg = "Failed to persist save memory:\n" + errMsg + "\nat " +
               Common::FS::PathToUTF8String(memoryPath),
    }};
    MsgDialog::ShowMsgDialog(dialog);
}

std::string GetSaveDir(u32 slot_id) {
    std::string dir(StandardDirnameSaveDataMemory);
    if (slot_id > 0) {
        dir += std::to_string(slot_id);
    }
    return dir;
}

std::filesystem::path GetSavePath(Libraries::UserService::OrbisUserServiceUserId user_id,
                                  u32 slot_id, std::string_view game_serial) {
    std::string dir(StandardDirnameSaveDataMemory);
    if (slot_id > 0) {
        dir += std::to_string(slot_id);
    }
    return SaveInstance::MakeDirSavePath(user_id, game_serial, dir);
}

size_t Store::SetupSaveMemory(Libraries::UserService::OrbisUserServiceUserId user_id, u32 slot_id,
                              std::string_view game_serial, size_t memory_size) {
    std::lock_guard lck{impl->mutex};

    const auto save_dir = impl->home.empty() ? GetSavePath(user_id, slot_id, game_serial)
                                             : impl->home / std::to_string(user_id) / "savedata" /
                                                   game_serial / GetSaveDir(slot_id);

    auto& data = impl->slots[slot_id];
    data = SlotData{
        .user_id = user_id,
        .game_serial = std::string{game_serial},
        .folder_path = save_dir,
        .sfo = {},
        .memory_cache = {},
        .memory_cache_size = memory_size,
    };

    SaveInstance::SetupDefaultParamSFO(data.sfo, GetSaveDir(slot_id), std::string{game_serial});

    auto param_sfo_path = SaveInstance::GetParamSFOPath(save_dir);
    if (!fs::exists(param_sfo_path)) {
        return 0;
    }

    if (!data.sfo.Open(param_sfo_path) || fs::exists(save_dir / CorruptFileName)) {
        if (!Backup::Restore(save_dir)) { // Could not restore the backup
            return 0;
        }
    }

    const auto memory = save_dir / FilenameSaveDataMemory;
    if (fs::exists(memory)) {
        return fs::file_size(memory);
    }

    return 0;
}

void Store::SetIcon(u32 slot_id, void* buf, size_t buf_size) {
    std::lock_guard lck{impl->mutex};
    const auto& data = impl->slots[slot_id];
    const auto icon_path = data.folder_path / sce_sys / "icon0.png";
    if (buf == nullptr) {
        if (fs::exists(icon_path)) {
            fs::remove(icon_path);
        }
        // Read the source through the mount stack so archive-backed base
        // games can seed save-slot icons too.
        if (auto bytes = GetMounts()->ReadFile("/app0/sce_sys/save_data.png")) {
            fs::create_directories(icon_path.parent_path());
            IOFile dst(icon_path, Common::FS::FileAccessMode::Create);
            dst.WriteRaw<u8>(bytes->data(), bytes->size());
        }
    } else {
        IOFile file(icon_path, Common::FS::FileAccessMode::Create);
        file.WriteRaw<u8>(buf, buf_size);
        file.Close();
    }
}

bool Store::IsSaveMemoryInitialized(u32 slot_id) {
    std::lock_guard lck{impl->mutex};
    return impl->slots.contains(slot_id);
}

PSF& Store::GetParamSFO(u32 slot_id) {
    std::lock_guard lck{impl->mutex};
    auto& data = impl->slots[slot_id];
    return data.sfo;
}

std::vector<u8> Store::GetIcon(u32 slot_id) {
    std::lock_guard lck{impl->mutex};
    auto& data = impl->slots[slot_id];
    const auto icon_path = data.folder_path / sce_sys / "icon0.png";
    IOFile f{icon_path, Common::FS::FileAccessMode::Read};
    if (!f.IsOpen()) {
        return {};
    }
    const u64 size = f.GetSize();
    std::vector<u8> ret;
    ret.resize(size);
    f.ReadSpan(std::span{ret});
    return ret;
}

void Store::SaveSFO(u32 slot_id) {
    std::lock_guard lck{impl->mutex};
    const auto& data = impl->slots[slot_id];
    const auto sfo_path = SaveInstance::GetParamSFOPath(data.folder_path);
    fs::create_directories(sfo_path.parent_path());
    const bool ok = data.sfo.Encode(sfo_path);
    if (!ok) {
        LOG_ERROR(Lib_SaveData, "Failed to encode param.sfo");
        throw std::filesystem::filesystem_error("Failed to write param.sfo", sfo_path,
                                                std::make_error_code(std::errc::permission_denied));
    }
}

void Store::ReadMemory(u32 slot_id, void* buf, size_t buf_size, int64_t offset) {
    std::lock_guard lk{impl->mutex};
    auto& data = impl->slots[slot_id];
    auto& memory = data.memory_cache;
    if (memory.empty()) { // Load file
        memory.resize(data.memory_cache_size);
        IOFile f{data.folder_path / FilenameSaveDataMemory, Common::FS::FileAccessMode::Read};
        if (f.IsOpen()) {
            f.Seek(0);
            f.ReadSpan(std::span{memory});
        }
    }
    if (offset < 0 || u64(offset) > memory.size())
        throw std::out_of_range("save memory offset");
    const auto read_size = std::min(buf_size, memory.size() - size_t(offset));
    if (read_size)
        std::memcpy(buf, memory.data() + offset, read_size);
}

void Store::WriteMemory(u32 slot_id, void* buf, size_t buf_size, int64_t offset) {
    std::lock_guard lk{impl->mutex};
    auto& data = impl->slots[slot_id];
    auto& memory = data.memory_cache;
    if (offset < 0 || buf_size > SIZE_MAX - u64(offset))
        throw std::out_of_range("save memory range");
    // A first write must retain previously persisted bytes outside its range.
    if (memory.empty()) {
        memory.resize(data.memory_cache_size);
        IOFile file{data.folder_path / FilenameSaveDataMemory, Common::FS::FileAccessMode::Read};
        if (file.IsOpen())
            file.ReadSpan(std::span{memory});
    }
    if (offset + buf_size > memory.size()) {
        memory.resize(offset + buf_size);
    }
    std::memcpy(memory.data() + offset, buf, buf_size);
    PersistMemory(slot_id, false);
    if (impl->desktop_backups)
        Backup::NewRequest(data.user_id, data.game_serial, GetSaveDir(slot_id),
                           Backup::OrbisSaveDataEventType::__DO_NOT_SAVE);
}
void PersistMemory(u32 slot, bool lock) {
    default_store.PersistMemory(slot, lock);
}
size_t SetupSaveMemory(s32 user, u32 slot, std::string_view title, size_t size) {
    return default_store.SetupSaveMemory(user, slot, title, size);
}
void SetIcon(u32 slot, void* buf, size_t size) {
    default_store.SetIcon(slot, buf, size);
}
bool IsSaveMemoryInitialized(u32 slot) {
    return default_store.IsSaveMemoryInitialized(slot);
}
PSF& GetParamSFO(u32 slot) {
    return default_store.GetParamSFO(slot);
}
std::vector<u8> GetIcon(u32 slot) {
    return default_store.GetIcon(slot);
}
void SaveSFO(u32 slot) {
    default_store.SaveSFO(slot);
}
void ReadMemory(u32 slot, void* buf, size_t size, int64_t offset) {
    default_store.ReadMemory(slot, buf, size, offset);
}
void WriteMemory(u32 slot, void* buf, size_t size, int64_t offset) {
    default_store.WriteMemory(slot, buf, size, offset);
}
} // namespace Libraries::SaveData::SaveMemory