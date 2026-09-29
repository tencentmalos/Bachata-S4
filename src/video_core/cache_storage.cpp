// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>
#include <xxhash.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

namespace {

// Writes are queued to one IO thread. Close() stops it only after the queue is empty, so every
// blob saved before the session ends reaches the disk.
std::mutex cache_io_mutex{};
std::condition_variable_any cache_io_cv{};
std::deque<std::function<void()>> cache_io_queue{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

} // namespace

namespace Storage {

bool WriteFileAtomic(const std::filesystem::path& path, std::span<const u8> bytes) {
    // A reader never sees a partly written file: write a temporary next to the target and
    // rename it over the target once it is complete.
    auto temp = path;
    temp += ".tmp";
    std::FILE* file = nullptr;
#ifdef _WIN32
    file = _wfopen(temp.c_str(), L"wb");
#else
    file = std::fopen(temp.c_str(), "wb");
#endif
    if (!file) {
        LOG_ERROR(Render, "Cannot create {}: {}", temp.string(), std::strerror(errno));
        return false;
    }
    bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    // The rename must not reach the disk before the data it points to.
    written = written && std::fflush(file) == 0;
#ifdef _WIN32
    written = written && _commit(_fileno(file)) == 0;
#else
    written = written && fsync(fileno(file)) == 0;
#endif
    const bool closed = std::fclose(file) == 0;
    std::error_code error;
    if (!written || !closed) {
        LOG_ERROR(Render, "Cannot write {} ({} bytes)", temp.string(), bytes.size());
        std::filesystem::remove(temp, error);
        return false;
    }
    std::filesystem::rename(temp, path, error);
    if (error) {
        LOG_ERROR(Render, "Cannot replace {}: {}", path.string(), error.message());
        std::filesystem::remove(temp, error);
        return false;
    }
    return true;
}

namespace {

constexpr u32 BlobMagic = 0x42525053; // "SPRB"
constexpr std::size_t BlobHeaderSize = 4 + 4 + 8 + 8;

void PutLe(u8* out, u64 value, u32 bytes) {
    for (u32 i = 0; i < bytes; ++i) {
        out[i] = u8(value >> (8 * i));
    }
}

u64 GetLe(const u8* in, u32 bytes) {
    u64 value = 0;
    for (u32 i = 0; i < bytes; ++i) {
        value |= u64(in[i]) << (8 * i);
    }
    return value;
}

std::vector<u8> WrapBlob(std::span<const u8> payload) {
    std::vector<u8> out(BlobHeaderSize + payload.size());
    PutLe(out.data(), BlobMagic, 4);
    PutLe(out.data() + 4, BlobFormatVersion, 4);
    PutLe(out.data() + 8, payload.size(), 8);
    PutLe(out.data() + 16, XXH3_64bits(payload.data(), payload.size()), 8);
    std::ranges::copy(payload, out.begin() + BlobHeaderSize);
    return out;
}

bool UnwrapBlob(std::vector<u8>& data) {
    if (data.size() < BlobHeaderSize || GetLe(data.data(), 4) != BlobMagic ||
        GetLe(data.data() + 4, 4) != BlobFormatVersion ||
        GetLe(data.data() + 8, 8) != data.size() - BlobHeaderSize ||
        GetLe(data.data() + 16, 8) !=
            XXH3_64bits(data.data() + BlobHeaderSize, data.size() - BlobHeaderSize)) {
        data.clear();
        return false;
    }
    data.erase(data.begin(), data.begin() + BlobHeaderSize);
    return true;
}

} // namespace

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);

    for (;;) {
        std::function<void()> request;
        {
            std::unique_lock lock{cache_io_mutex};
            Common::CondvarWait(cache_io_cv, lock, stoken, [] { return !cache_io_queue.empty(); });
            if (cache_io_queue.empty()) {
                return; // Stop requested and nothing left to write.
            }
            request = std::move(cache_io_queue.front());
            cache_io_queue.pop_front();
        }
        request();
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    case BlobType::Usage: {
        return "use";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();
    if (game_info.GameSerial().empty()) {
        // Without a title there is no directory of our own to keep (and reset) blobs in.
        LOG_WARNING(Render, "No game serial, the pipeline cache stays off");
        return;
    }

    using namespace Common::FS;
    ar_is_read_only = true;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        cache_path = GetUserPath(PathType::CacheDir) / game_info.GameSerial();
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    // The worker drains the queue before it returns.
    io_worker.request_stop();
    io_worker.join();

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    auto request = [type, path = std::move(path_), v = std::move(v)]() mutable {
        path.replace_extension(GetBlobFileExtension(type));
        const auto blob =
            WrapBlob({reinterpret_cast<const u8*>(v.data()), v.size() * sizeof(T)});
        if (EmulatorSettings.IsPipelineCacheArchived()) {
            ASSERT_MSG(!ar_is_read_only,
                       "The archive is read-only. Did you forget to call `FinishPreload`?");
            if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), blob.data(), blob.size(),
                                       MZ_BEST_COMPRESSION)) {
                LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
            }
        } else {
            WriteFileAtomic(path, blob);
        }
    };
    {
        std::scoped_lock lock{cache_io_mutex};
        cache_io_queue.emplace_back(std::move(request));
    }
    cache_io_cv.notify_one();
    return true;
}

/// Reads a blob file as stored (still wrapped). Nullopt when it does not exist.
std::optional<std::vector<u8>> LoadRaw(BlobType type, std::filesystem::path& path) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const int index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            return std::nullopt;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        std::vector<u8> data(stat.m_uncomp_size);
        if (!mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0)) {
            data.clear(); // Present but unreadable: reported as damaged.
        }
        return data;
    }
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return std::nullopt;
    }
    const auto file = IOFile{path, FileAccessMode::Read};
    std::vector<u8> data(file.IsOpen() ? file.GetSize() : 0);
    if (!data.empty() && file.Read(data) != data.size()) {
        data.clear();
    }
    return data;
}

LoadResult LoadPayload(BlobType type, std::filesystem::path& path, std::vector<u8>& data) {
    auto raw = LoadRaw(type, path);
    if (!raw) {
        data.clear();
        return LoadResult::Missing;
    }
    data = std::move(*raw);
    if (!UnwrapBlob(data)) {
        LOG_WARNING(Render, "Pipeline cache blob {} is damaged, ignoring it", path.string());
        return LoadResult::Damaged;
    }
    return LoadResult::Ok;
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

LoadResult DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    data.clear();
    if (!opened) {
        return LoadResult::Missing;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadPayload(type, path, data);
}

LoadResult DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    data.clear();
    std::vector<u8> bytes;
    const auto result = Load(type, name, bytes);
    if (result != LoadResult::Ok) {
        return result;
    }
    if (bytes.size() % sizeof(u32) != 0) {
        return LoadResult::Damaged;
    }
    data.resize(bytes.size() / sizeof(u32));
    std::memcpy(data.data(), bytes.data(), bytes.size());
    return LoadResult::Ok;
}

u32 DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    u32 damaged{};
    const auto deliver = [&](std::vector<u8>&& data) {
        if (UnwrapBlob(data)) {
            func(std::move(data));
        } else {
            ++damaged;
        }
    };
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                if (!mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0)) {
                    data.clear();
                }
                deliver(std::move(data));
            }
        }
    } else {
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
            if (file_name.path().extension().string().ends_with(ext)) {
                using namespace Common::FS;
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    if (file.Read(data) != data.size()) {
                        data.clear();
                    }
                    deliver(std::move(data));
                }
            }
        }
    }
    if (damaged) {
        LOG_WARNING(Render, "Skipped {} damaged pipeline cache blobs", damaged);
    }
    return damaged;
}

void DataBase::Reset() {
    if (!opened) {
        return;
    }
    std::error_code error;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        if (zip_ar.m_zip_mode == MZ_ZIP_MODE_READING) {
            mz_zip_reader_end(&zip_ar);
            std::filesystem::remove(cache_path, error);
            mz_zip_zero_struct(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
        return;
    }
    u32 removed{};
    for (const auto& entry : std::filesystem::directory_iterator{cache_path, error}) {
        const auto ext = entry.path().extension().string();
        if (entry.is_regular_file(error) &&
            (ext == ".meta" || ext == ".spv" || ext == ".key" || ext == ".bin" || ext == ".use" ||
             ext == ".tmp")) {
            removed += std::filesystem::remove(entry.path(), error) ? 1 : 0;
        }
    }
    LOG_INFO(Render, "Removed {} stale pipeline cache files from {}", removed, cache_path.string());
}

void DataBase::FinishPreload() {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
