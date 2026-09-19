// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>
#include <core/libraries/system/userservice.h>
#include "core/libraries/save_data/save_backup.h"

class PSF;

namespace Libraries::SaveData::SaveMemory {

// One store per runtime; desktop keeps a default instance through the legacy API.
// Session stores fail promptly on persistence errors and own all cached slot data.
class Store {
public:
    explicit Store(bool desktop_backups = true, std::filesystem::path home = {});
    ~Store();
    size_t SetupSaveMemory(s32 user, u32 slot, std::string_view title, size_t size);
    bool IsSaveMemoryInitialized(u32 slot);
    size_t MemorySize(u32 slot);
    void PersistMemory(u32 slot, bool lock = true);
    void ReadMemory(u32 slot, void* data, size_t size, int64_t offset);
    void WriteMemory(u32 slot, void* data, size_t size, int64_t offset);
    PSF& GetParamSFO(u32 slot);
    std::vector<u8> GetIcon(u32 slot);
    void SetIcon(u32 slot, void* buf = nullptr, size_t size = 0);
    void SaveSFO(u32 slot);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

void PersistMemory(u32 slot_id, bool lock = true);

[[nodiscard]] std::string GetSaveDir(u32 slot_id);

[[nodiscard]] std::filesystem::path GetSavePath(
    Libraries::UserService::OrbisUserServiceUserId user_id, u32 slot_id,
    std::string_view game_serial);

// returns the size of the save memory if exists
size_t SetupSaveMemory(Libraries::UserService::OrbisUserServiceUserId user_id, u32 slot_id,
                       std::string_view game_serial, size_t memory_size);

// Write the icon. Set buf to null to read the standard icon.
void SetIcon(u32 slot_id, void* buf = nullptr, size_t buf_size = 0);

[[nodiscard]] bool IsSaveMemoryInitialized(u32 slot_id);

[[nodiscard]] PSF& GetParamSFO(u32 slot_id);

[[nodiscard]] std::vector<u8> GetIcon(u32 slot_id);

// Save now or wait for the background thread to save
void SaveSFO(u32 slot_id);

void ReadMemory(u32 slot_id, void* buf, size_t buf_size, int64_t offset);

void WriteMemory(u32 slot_id, void* buf, size_t buf_size, int64_t offset);

} // namespace Libraries::SaveData::SaveMemory