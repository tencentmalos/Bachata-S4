// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

#include <xxhash.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/polyfill_thread.h"
#include "common/profiler.h"
#include "common/thread.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_driver_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_stats.h"

namespace Vulkan {

namespace {

constexpr std::array<char, 8> DriverCacheMagic = {'S', 'P', 'S', '4', 'V', 'K', 'P', 'C'};
constexpr u32 DriverCacheFormat = 1;
// Drivers keep machine code for every pipeline ever built; a larger blob is more likely damage
// than content, and the driver would copy all of it into memory at startup. Saves above the
// limit keep the previous file.
constexpr u64 MaxDriverCacheBytes = 128ULL << 20;

// Save when this many pipelines were created since the last request, at most this often.
constexpr u32 DriverCacheSaveEvery = 16;
constexpr auto DriverCacheSaveInterval = std::chrono::seconds{30};

// Envelope in front of the driver data. Read and written field by field in little-endian order,
// never by casting file bytes to a host struct.
struct DriverCacheEnvelope {
    u32 format{};
    u32 vendor_id{};
    u32 device_id{};
    u32 driver_version{};
    std::array<u8, VK_UUID_SIZE> uuid{};
    u64 data_size{};
    u64 data_hash{};

    static constexpr std::size_t Size = 8 + 4 * 4 + VK_UUID_SIZE + 8 + 8;
};

void PutLe(std::vector<u8>& out, u64 value, u32 bytes) {
    for (u32 i = 0; i < bytes; ++i) {
        out.push_back(u8(value >> (8 * i)));
    }
}

u64 GetLe(std::span<const u8> in, std::size_t offset, u32 bytes) {
    u64 value = 0;
    for (u32 i = 0; i < bytes; ++i) {
        value |= u64(in[offset + i]) << (8 * i);
    }
    return value;
}

DriverCacheEnvelope CurrentIdentity(const Instance& instance) {
    DriverCacheEnvelope envelope{};
    envelope.format = DriverCacheFormat;
    envelope.vendor_id = instance.GetVendorID();
    envelope.device_id = instance.GetDeviceID();
    envelope.driver_version = instance.GetDriverVersion();
    std::ranges::copy(instance.GetPipelineCacheUUID(), envelope.uuid.begin());
    return envelope;
}

std::string DescribeIdentity(const DriverCacheEnvelope& e) {
    std::string uuid;
    for (const u8 byte : e.uuid) {
        uuid += fmt::format("{:02x}", byte);
    }
    return fmt::format("vendor {:#x} device {:#x} driver {:#x} uuid {}", e.vendor_id, e.device_id,
                       e.driver_version, uuid);
}

std::vector<u8> EncodeEnvelope(const DriverCacheEnvelope& e) {
    std::vector<u8> out(DriverCacheMagic.begin(), DriverCacheMagic.end());
    PutLe(out, e.format, 4);
    PutLe(out, e.vendor_id, 4);
    PutLe(out, e.device_id, 4);
    PutLe(out, e.driver_version, 4);
    out.insert(out.end(), e.uuid.begin(), e.uuid.end());
    PutLe(out, e.data_size, 8);
    PutLe(out, e.data_hash, 8);
    ASSERT(out.size() == DriverCacheEnvelope::Size);
    return out;
}

std::optional<DriverCacheEnvelope> DecodeEnvelope(std::span<const u8> in) {
    if (in.size() < DriverCacheEnvelope::Size ||
        !std::equal(DriverCacheMagic.begin(), DriverCacheMagic.end(), in.begin())) {
        return std::nullopt;
    }
    DriverCacheEnvelope e{};
    std::size_t offset = DriverCacheMagic.size();
    const auto u32_at = [&] {
        const auto value = u32(GetLe(in, offset, 4));
        offset += 4;
        return value;
    };
    e.format = u32_at();
    e.vendor_id = u32_at();
    e.device_id = u32_at();
    e.driver_version = u32_at();
    std::copy_n(in.begin() + offset, VK_UUID_SIZE, e.uuid.begin());
    offset += VK_UUID_SIZE;
    e.data_size = GetLe(in, offset, 8);
    e.data_hash = GetLe(in, offset + 8, 8);
    return e;
}

/// Checks VkPipelineCacheHeaderVersionOne at the start of the driver data.
const char* CheckVulkanHeader(std::span<const u8> data, const DriverCacheEnvelope& identity) {
    constexpr std::size_t HeaderSize = 16 + VK_UUID_SIZE;
    if (data.size() < HeaderSize) {
        return "driver data shorter than its header";
    }
    const u64 header_size = GetLe(data, 0, 4);
    if (header_size < HeaderSize || header_size > data.size()) {
        return "driver header size out of range";
    }
    if (GetLe(data, 4, 4) != u32(VK_PIPELINE_CACHE_HEADER_VERSION_ONE)) {
        return "unknown driver header version";
    }
    if (GetLe(data, 8, 4) != identity.vendor_id || GetLe(data, 12, 4) != identity.device_id ||
        !std::equal(identity.uuid.begin(), identity.uuid.end(), data.begin() + 16)) {
        return "driver header names another device";
    }
    return nullptr;
}

u64 ElapsedNs(std::chrono::steady_clock::time_point start) {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count());
}

} // namespace

DriverPipelineCache::DriverPipelineCache(const Instance& instance_, std::filesystem::path path_)
    : instance{instance_}, path{std::move(path_)} {
    Load();
    if (!path.empty()) {
        last_request = std::chrono::steady_clock::now();
        worker = std::jthread{[this](std::stop_token token) { Worker(token); }};
    }
}

DriverPipelineCache::~DriverPipelineCache() {
    if (worker.joinable()) {
        worker.request_stop();
        worker.join();
        Write();
    }
}

void DriverPipelineCache::Load() {
    Common::Profiler::Scope scope{"Pipeline.CacheLoad"};
    const auto identity = CurrentIdentity(instance);
    PipelineStats::DriverCacheState state{.path = path.string(),
                                          .identity = DescribeIdentity(identity)};
    std::vector<u8> file;
    std::span<const u8> data;
    bool keep_rejected = false;
    const auto reject = [&](std::string reason) {
        state.load_result = std::move(reason);
        data = {};
    };

    if (path.empty()) {
        reject("persistence off");
    } else if (std::error_code error; !std::filesystem::exists(path, error)) {
        reject("no file");
    } else {
        keep_rejected = true;
        std::ifstream in{path, std::ios::binary | std::ios::ate};
        const u64 size = in ? u64(in.tellg()) : 0;
        if (!in || size < DriverCacheEnvelope::Size ||
            size - DriverCacheEnvelope::Size > MaxDriverCacheBytes) {
            reject(fmt::format("unreadable or bad size ({} bytes)", size));
        } else {
            file.resize(size);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(file.data()), std::streamsize(file.size()));
            data = std::span<const u8>{file}.subspan(DriverCacheEnvelope::Size);
            const auto stored = DecodeEnvelope(file);
            if (!in) {
                reject("read failed");
            } else if (!stored || stored->format != DriverCacheFormat) {
                reject("different file format");
            } else if (stored->vendor_id != identity.vendor_id ||
                       stored->device_id != identity.device_id ||
                       stored->driver_version != identity.driver_version ||
                       stored->uuid != identity.uuid) {
                reject(fmt::format("built for {}", DescribeIdentity(*stored)));
            } else if (stored->data_size != data.size() ||
                       stored->data_hash != XXH3_64bits(data.data(), data.size())) {
                reject("length or checksum mismatch");
            } else if (const char* error = CheckVulkanHeader(data, identity)) {
                reject(error);
            } else {
                state.load_result = "loaded";
                state.loaded_bytes = data.size();
                keep_rejected = false;
            }
        }
    }

    const vk::Device device = instance.GetDevice();
    if (!data.empty()) {
        auto [result, loaded] = device.createPipelineCacheUnique({
            .initialDataSize = data.size(),
            .pInitialData = data.data(),
        });
        if (result == vk::Result::eSuccess) {
            cache = std::move(loaded);
            saved_size = data.size();
        } else {
            reject(fmt::format("driver refused it ({})", vk::to_string(result)));
            keep_rejected = true;
        }
    }
    if (!cache) {
        auto [result, empty] = device.createPipelineCacheUnique({});
        ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
                   vk::to_string(result));
        cache = std::move(empty);
    }
    if (keep_rejected) {
        // The next save replaces the file; keep the one that was not used for diagnosis.
        auto rejected = path;
        rejected += ".rejected";
        std::error_code error;
        std::filesystem::rename(path, rejected, error);
    }
    if (!path.empty()) {
        LOG_INFO(Render_Vulkan, "Driver pipeline cache {}: {} ({} bytes; {})", path.string(),
                 state.load_result, state.loaded_bytes, state.identity);
    }
    PipelineStats::SetDriverCacheState(std::move(state));
}

void DriverPipelineCache::NotePipelineCreated() {
    if (path.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock{mutex};
        if (++created_since_request < DriverCacheSaveEvery ||
            now - last_request < DriverCacheSaveInterval) {
            return;
        }
        created_since_request = 0;
        last_request = now;
        save_requested = true;
    }
    cv.notify_one();
}

void DriverPipelineCache::RequestSave() {
    if (path.empty()) {
        return;
    }
    {
        std::scoped_lock lock{mutex};
        created_since_request = 0;
        last_request = std::chrono::steady_clock::now();
        save_requested = true;
    }
    cv.notify_one();
}

void DriverPipelineCache::Worker(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheSave");
    for (;;) {
        {
            std::unique_lock lock{mutex};
            Common::CondvarWait(cv, lock, token, [this] { return save_requested; });
            if (token.stop_requested()) {
                return; // The destructor writes the final state.
            }
            save_requested = false;
        }
        Write();
    }
}

void DriverPipelineCache::Write() {
    // Pipeline caches are internally synchronized (no EXTERNALLY_SYNCHRONIZED flag), so reading
    // the data while other threads create pipelines with the same cache is allowed.
    std::scoped_lock lock{write_mutex};
    Common::Profiler::Scope scope{"Pipeline.CacheSave"};
    const auto start = std::chrono::steady_clock::now();
    const vk::Device device = instance.GetDevice();

    // The size can grow between the two calls; VK_INCOMPLETE means the buffer was too small and
    // holds a partial copy, which must never be written. Retry a few times, then skip this save.
    std::vector<u8> data;
    vk::Result result = vk::Result::eIncomplete;
    for (u32 attempt = 0; attempt < 4 && result == vk::Result::eIncomplete; ++attempt) {
        std::size_t size = 0;
        result = device.getPipelineCacheData(*cache, &size, nullptr);
        if (result != vk::Result::eSuccess) {
            break;
        }
        if (size > MaxDriverCacheBytes) {
            LOG_WARNING(Render_Vulkan, "Driver pipeline cache grew to {} bytes, keeping the "
                                       "previous file",
                        size);
            return;
        }
        data.resize(size);
        result = device.getPipelineCacheData(*cache, &size, data.data());
        data.resize(size);
    }
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Cannot read the driver pipeline cache: {}",
                    vk::to_string(result));
        return;
    }
    // Driver caches only grow, so an unchanged size means nothing new to save.
    if (data.empty() || data.size() == saved_size) {
        return;
    }
    auto envelope = CurrentIdentity(instance);
    envelope.data_size = data.size();
    envelope.data_hash = XXH3_64bits(data.data(), data.size());
    auto file = EncodeEnvelope(envelope);
    file.insert(file.end(), data.begin(), data.end());
    if (Storage::WriteFileAtomic(path, file)) {
        saved_size = data.size();
        PipelineStats::RecordDriverCacheSave(data.size(), ElapsedNs(start));
        LOG_INFO(Render_Vulkan, "Saved driver pipeline cache ({} bytes)", data.size());
    }
}

} // namespace Vulkan
