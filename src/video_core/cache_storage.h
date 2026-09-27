// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <span>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
    Usage, ///< Which pipelines recent sessions used, and when (preload priority).
};

/// Writes a file through a temporary and a rename, so readers never see a partial file.
bool WriteFileAtomic(const std::filesystem::path& path, std::span<const u8> bytes);

/// Every blob is stored inside an envelope (magic, format, length, XXH3 of the payload), so a
/// damaged or truncated file is rejected before it reaches a deserializer. Bumping the format
/// replaces older caches (it is part of the pipeline cache's version check).
constexpr u32 BlobFormatVersion = 1;

enum class LoadResult {
    Missing, ///< No such blob.
    Damaged, ///< Present, but the envelope or checksum does not match.
    Ok,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// Deletes every blob of the opened title, e.g. when they were built for another profile.
    /// Must be called before the first Save.
    void Reset();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    /// `data` is left empty unless the result is Ok.
    LoadResult Load(BlobType type, const std::string& name, std::vector<u8>& data);
    LoadResult Load(BlobType type, const std::string& name, std::vector<u32>& data);

    /// Calls `func` with the payload of every intact blob of `type`; returns how many damaged
    /// blobs were skipped.
    u32 ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);

private:
    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    bool opened{};
};

} // namespace Storage
