// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include <zstd.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/replay/gpu_replay_file.h"

namespace VideoCore::Replay {

namespace {
constexpr size_t CompressThreshold = 4_KB;
// zstd level 1: a snapshot is several GiB and is written while the game waits.
constexpr int CompressionLevel = 1;
} // namespace

TraceWriter::TraceWriter(size_t max_pending_bytes_, u32 workers)
    : max_pending_bytes{max_pending_bytes_},
      worker_count{workers ? workers
                           : std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u)} {}

TraceWriter::~TraceWriter() {
    if (open) {
        Close();
    }
}

bool TraceWriter::Open(const std::filesystem::path& path, const FileHeader& header) {
    if (open) {
        return false;
    }
    file.Open(path, Common::FS::FileAccessMode::Create);
    if (!file.IsOpen()) {
        return false;
    }
    if (file.WriteRaw<u8>(&header, sizeof(header)) != sizeof(header)) {
        file.Close();
        return false;
    }
    {
        std::scoped_lock lock{mutex};
        failed = false;
        closing = false;
        next_sequence = 0;
        pending_bytes = 0;
        raw_bytes = stored_bytes = records = 0;
    }
    open = true;
    for (u32 i = 0; i < worker_count; ++i) {
        compressors.emplace_back([this](std::stop_token token) { CompressLoop(token); });
    }
    writer = std::jthread([this](std::stop_token token) { WriteLoop(token); });
    return true;
}

void TraceWriter::Write(RecordType type, std::vector<u8> payload) {
    auto job = std::make_shared<Job>();
    job->type = type;
    job->payload = std::move(payload);
    const size_t bytes = job->payload.size();

    std::unique_lock lock{mutex};
    // A single record larger than the limit is still admitted once the queue drains.
    space_cv.wait(lock, [&] { return failed || pending_bytes == 0 || pending_bytes + bytes <= max_pending_bytes; });
    job->sequence = next_sequence++;
    pending_bytes += bytes;
    in_order.push_back(job);
    if (job->payload.size() >= CompressThreshold) {
        to_compress.push_back(job);
        work_cv.notify_one();
    } else {
        job->done = true;
        done_cv.notify_all();
    }
}

void TraceWriter::CompressLoop(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:GpuReplayZstd");
    ZSTD_CCtx* context = ZSTD_createCCtx();
    while (true) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock{mutex};
            work_cv.wait(lock, token, [&] { return !to_compress.empty() || closing; });
            if (to_compress.empty()) {
                break;
            }
            job = std::move(to_compress.front());
            to_compress.pop_front();
        }
        std::vector<u8> stored(ZSTD_compressBound(job->payload.size()));
        const size_t size = ZSTD_compressCCtx(context, stored.data(), stored.size(),
                                              job->payload.data(), job->payload.size(),
                                              CompressionLevel);
        const bool use = !ZSTD_isError(size) && size < job->payload.size();
        if (use) {
            stored.resize(size);
        }
        std::scoped_lock lock{mutex};
        if (use) {
            job->stored = std::move(stored);
            job->compressed = true;
        }
        job->done = true;
        done_cv.notify_all();
    }
    ZSTD_freeCCtx(context);
}

void TraceWriter::WriteLoop(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:GpuReplayWrite");
    while (true) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock{mutex};
            done_cv.wait(lock, token, [&] {
                return (!in_order.empty() && in_order.front()->done) ||
                       (closing && in_order.empty());
            });
            if (in_order.empty() || !in_order.front()->done) {
                break;
            }
            job = in_order.front();
        }
        const std::vector<u8>& data = job->compressed ? job->stored : job->payload;
        RecordHeader header{};
        header.type = static_cast<u32>(job->type);
        header.flags = job->compressed ? RecordFlagZstd : RecordFlagNone;
        header.stored_bytes = data.size();
        header.raw_bytes = job->payload.size();
        const bool ok = file.WriteRaw<u8>(&header, sizeof(header)) == sizeof(header) &&
                        (data.empty() || file.WriteRaw<u8>(data.data(), data.size()) == data.size());
        std::scoped_lock lock{mutex};
        in_order.pop_front();
        pending_bytes -= job->payload.size();
        raw_bytes += job->payload.size() + sizeof(header);
        stored_bytes += data.size() + sizeof(header);
        ++records;
        if (!ok && !failed) {
            failed = true;
            LOG_ERROR(Render, "GPU replay: trace write failed");
        }
        space_cv.notify_all();
        done_cv.notify_all();
    }
}

bool TraceWriter::Close() {
    if (!open) {
        return false;
    }
    {
        std::scoped_lock lock{mutex};
        closing = true;
        work_cv.notify_all();
        done_cv.notify_all();
    }
    compressors.clear();
    {
        std::scoped_lock lock{mutex};
        done_cv.notify_all();
    }
    writer = {};
    const bool flushed = file.Flush();
    file.Close();
    open = false;
    std::scoped_lock lock{mutex};
    return flushed && !failed && in_order.empty();
}

u64 TraceWriter::RawBytes() const {
    std::scoped_lock lock{mutex};
    return raw_bytes;
}

u64 TraceWriter::StoredBytes() const {
    std::scoped_lock lock{mutex};
    return stored_bytes;
}

u64 TraceWriter::Records() const {
    std::scoped_lock lock{mutex};
    return records;
}

bool TraceReader::Open(const std::filesystem::path& path) {
    file.Open(path, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        error = "cannot open trace";
        return false;
    }
    if (file.ReadRaw<u8>(&header, sizeof(header)) != sizeof(header) || header.magic != FileMagic) {
        error = "not a shadPS4 GPU replay trace";
        return false;
    }
    if (header.version != FormatVersion || header.page_bits != PageBits) {
        error = "unsupported trace version";
        return false;
    }
    if (header.header_bytes > sizeof(header)) {
        file.Seek(header.header_bytes);
    }
    return true;
}

bool TraceReader::Next(RecordHeader& record, std::vector<u8>& payload) {
    if (file.ReadRaw<u8>(&record, sizeof(record)) != sizeof(record)) {
        return false;
    }
    // A stored size beyond the remaining file is damage, not a request to allocate it.
    const u64 remaining = file.GetSize() - static_cast<u64>(file.Tell());
    if (record.stored_bytes > remaining || record.raw_bytes > 64_GB) {
        error = "damaged record header";
        return false;
    }
    if ((record.flags & RecordFlagZstd) == 0) {
        payload.resize(record.stored_bytes);
        if (!payload.empty() && file.ReadRaw<u8>(payload.data(), payload.size()) != payload.size()) {
            error = "truncated record";
            return false;
        }
        return true;
    }
    stored.resize(record.stored_bytes);
    if (file.ReadRaw<u8>(stored.data(), stored.size()) != stored.size()) {
        error = "truncated record";
        return false;
    }
    payload.resize(record.raw_bytes);
    const size_t size =
        ZSTD_decompress(payload.data(), payload.size(), stored.data(), stored.size());
    if (ZSTD_isError(size) || size != payload.size()) {
        error = "corrupt compressed record";
        return false;
    }
    return true;
}

} // namespace VideoCore::Replay
