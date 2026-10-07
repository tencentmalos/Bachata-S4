// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "common/io_file.h"
#include "common/types.h"
#include "video_core/replay/gpu_replay_format.h"

namespace VideoCore::Replay {

/// Little-endian byte builder for one record payload.
class PayloadBuilder {
public:
    template <TriviallySerializable T>
    void Put(const T& value) {
        PutBytes(&value, sizeof(T));
    }

    template <TriviallySerializable T>
    void PutSpan(std::span<const T> values) {
        PutBytes(values.data(), values.size_bytes());
    }

    void PutBytes(const void* data, size_t size) {
        if (size == 0) {
            return;
        }
        std::memcpy(Extend(size), data, size);
    }

    /// Reserve size bytes and return a pointer to them, for callers that fill memory in place.
    u8* Extend(size_t size) {
        const size_t offset = bytes.size();
        bytes.resize(offset + size);
        return bytes.data() + offset;
    }

    void Pad(size_t alignment) {
        const size_t padded = (bytes.size() + alignment - 1) / alignment * alignment;
        bytes.resize(padded);
    }

    /// Drops the bytes past size (size must not exceed Size()).
    void Shrink(size_t size) {
        bytes.resize(size);
    }

    u8* Data() {
        return bytes.data();
    }

    size_t Size() const {
        return bytes.size();
    }

    std::vector<u8> Take() {
        return std::move(bytes);
    }

private:
    std::vector<u8> bytes;
};

/// Reads a payload written by PayloadBuilder; every read is bounds checked.
class PayloadReader {
public:
    explicit PayloadReader(std::span<const u8> bytes_) : bytes{bytes_} {}

    template <TriviallySerializable T>
    bool Get(T& value) {
        return GetBytes(&value, sizeof(T));
    }

    bool GetBytes(void* out, size_t size) {
        if (size > Remaining()) {
            return false;
        }
        std::memcpy(out, bytes.data() + offset, size);
        offset += size;
        return true;
    }

    /// The next size bytes without copying, or an empty span when fewer remain.
    std::span<const u8> View(size_t size) {
        if (size > Remaining()) {
            return {};
        }
        const auto view = bytes.subspan(offset, size);
        offset += size;
        return view;
    }

    bool Skip(size_t size) {
        return !View(size).empty() || size == 0;
    }

    void Align(size_t alignment) {
        offset = std::min(bytes.size(), (offset + alignment - 1) / alignment * alignment);
    }

    size_t Remaining() const {
        return bytes.size() - offset;
    }

private:
    std::span<const u8> bytes;
    size_t offset{};
};

/// Writes records in order. Payloads of 4 KiB or more are compressed on worker threads; a record
/// keeps the compressed form only when it is smaller. When more than max_pending_bytes of
/// payload wait for compression or disk, Write blocks, which bounds memory while a snapshot of
/// several GiB is written.
class TraceWriter {
public:
    explicit TraceWriter(size_t max_pending_bytes = 512_MB, u32 workers = 0);
    ~TraceWriter();

    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;

    bool Open(const std::filesystem::path& path, const FileHeader& header);
    void Write(RecordType type, std::vector<u8> payload);
    /// Waits for every queued record and closes the file; false when any write failed.
    bool Close();

    bool IsOpen() const {
        return open;
    }
    u64 RawBytes() const;
    u64 StoredBytes() const;
    u64 Records() const;

private:
    struct Job {
        u64 sequence{};
        RecordType type{};
        std::vector<u8> payload;
        std::vector<u8> stored;
        bool compressed{};
        bool done{};
    };

    void CompressLoop(std::stop_token token);
    void WriteLoop(std::stop_token token);

    Common::FS::IOFile file;
    bool open{};
    size_t max_pending_bytes;
    u32 worker_count;

    mutable std::mutex mutex;
    std::condition_variable_any work_cv;
    std::condition_variable_any done_cv;
    std::condition_variable_any space_cv;
    std::deque<std::shared_ptr<Job>> to_compress;
    std::deque<std::shared_ptr<Job>> in_order;
    size_t pending_bytes{};
    u64 next_sequence{};
    u64 raw_bytes{};
    u64 stored_bytes{};
    u64 records{};
    bool failed{};
    bool closing{};
    // Linux/Android: the written trace is pushed to storage and dropped from the page cache
    // every SyncChunk bytes. A snapshot of several GiB otherwise fills memory with dirty pages
    // faster than they are written back (Bloodborne on the AYN Thor: killed by lmkd).
    int sync_fd{-1};
    u64 synced_until{};
    u64 unsynced{};

    std::vector<std::jthread> compressors;
    std::jthread writer;
};

/// Sequential record reader for a trace file.
class TraceReader {
public:
    TraceReader() = default;
    ~TraceReader();
    TraceReader(const TraceReader&) = delete;
    TraceReader& operator=(const TraceReader&) = delete;

    bool Open(const std::filesystem::path& path);

    const FileHeader& Header() const {
        return header;
    }

    /// Reads the next record; false at the end of the file or on a damaged record (see Error()).
    bool Next(RecordHeader& record, std::vector<u8>& payload);
    /// Reads the next record without decompressing it (see Decompress).
    bool NextStored(RecordHeader& record, std::vector<u8>& data);
    /// Turns a stored record into its payload; consumes data. Thread safe.
    static bool Decompress(const RecordHeader& record, std::vector<u8>& data,
                           std::vector<u8>& payload);

    const std::string& Error() const {
        return error;
    }

private:
    /// Linux/Android: drops the page cache of what was read (every SyncChunk bytes). A replay
    /// reads several GiB once; reclaiming that cache under the replay's own allocations put the
    /// AYN Thor under critical memory pressure (lmkd).
    void DropRead();

    Common::FS::IOFile file;
    FileHeader header{};
    std::vector<u8> stored;
    std::string error;
    int drop_fd{-1};
    u64 dropped_until{};
};

} // namespace VideoCore::Replay
