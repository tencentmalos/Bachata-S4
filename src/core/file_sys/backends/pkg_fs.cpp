// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <mutex>
#include "common/logging/log.h"
#include "core/file_sys/backends/pkg_fs.h"

namespace Core::FileSys {
namespace {
using Reader = FileFormat::PkgReader;

class PkgFile final : public IFile {
public:
    PkgFile(std::shared_ptr<Reader> reader, size_t node, std::filesystem::path path)
        : reader(std::move(reader)), node(node), path(std::move(path)) {}
    s64 Read(void* dst, u64 size) override {
        std::scoped_lock lock(mutex);
        const auto got = ReadAt(dst, size, position);
        if (got > 0)
            position += got;
        return got;
    }
    s64 ReadAt(void* dst, u64 size, u64 offset) override {
        if (size > INT64_MAX)
            return -1;
        try {
            return reader->Read(node, offset, {static_cast<u8*>(dst), size});
        } catch (const std::exception& e) {
            LOG_ERROR(Kernel_Fs, "PKG read {}: {}", path.string(), e.what());
            return -1;
        }
    }
    s64 Write(const void*, u64) override {
        return -1;
    }
    bool Seek(s64 offset, Common::FS::SeekOrigin origin) override {
        std::scoped_lock lock(mutex);
        u64 base{};
        switch (origin) {
        case Common::FS::SeekOrigin::SetOrigin:
            break;
        case Common::FS::SeekOrigin::CurrentPosition:
            base = position;
            break;
        case Common::FS::SeekOrigin::End:
            base = Size();
            break;
        default:
            return false;
        }
        if (offset < 0) {
            const u64 distance = u64(-(offset + 1)) + 1;
            if (distance > base)
                return false;
            position = base - distance;
        } else {
            if (u64(offset) > u64(INT64_MAX) - base)
                return false;
            position = base + offset;
        }
        return true;
    }
    u64 Tell() const override {
        std::scoped_lock lock(mutex);
        return position;
    }
    u64 Size() const override {
        return reader->Entries()[node].size;
    }
    bool Flush() override {
        return true;
    }
    bool IsOpen() const override {
        return true;
    }
    bool IsReadOnly() const override {
        return true;
    }
    std::optional<std::filesystem::path> GetMetadataHostPath() const override {
        return path;
    }
    MmapPolicy GetMmapPolicy() const override {
        return MmapPolicy::Copy;
    }
    bool Map(u8* addr, u64 size, u64 offset, u32 protection, const FileMapContext& ctx) override {
        // Like ZAR, populate anonymous memory at mmap time. This is bounded by
        // the mapping requested by the guest, not a full-file extraction.
        if (offset > Size())
            return false;
        ctx.map_anonymous(addr, size);
        const u64 available = std::min(size, Size() - offset);
        for (u64 done = 0; done < available;) {
            const auto take = std::min<u64>(available - done, 1024 * 1024);
            if (ReadAt(addr + done, take, offset + done) != s64(take))
                return false;
            done += take;
        }
        std::memset(addr + available, 0, size - available);
        ctx.protect(addr, size, protection);
        return true;
    }

private:
    std::shared_ptr<Reader> reader;
    size_t node;
    std::filesystem::path path;
    mutable std::mutex mutex;
    u64 position{};
};

class PkgDirectory final : public IDirectory {
public:
    explicit PkgDirectory(std::vector<DirEntry> entries) : entries(std::move(entries)) {}
    bool Next(DirEntry& out) override {
        if (position == entries.size())
            return false;
        out = entries[position++];
        return true;
    }
    void Rewind() override {
        position = 0;
    }

private:
    std::vector<DirEntry> entries;
    size_t position{};
};

std::optional<std::string> Normalize(std::string_view path) {
    while (path.starts_with('/'))
        path.remove_prefix(1);
    const auto normal = std::filesystem::path(path).lexically_normal();
    for (const auto& component : normal)
        if (component == "..")
            return std::nullopt;
    auto text = normal.generic_string();
    while (text.ends_with('/'))
        text.pop_back();
    return text == "." ? "" : text;
}
} // namespace

PkgBackend::PkgBackend(std::filesystem::path path) : path(std::move(path)) {
    try {
        reader = Reader::Open(this->path);
    } catch (const std::exception& e) {
        LOG_ERROR(Kernel_Fs, "Cannot mount PKG {}: {}", this->path.string(), e.what());
    }
}
size_t PkgBackend::Find(std::string_view path) const {
    const auto normal = Normalize(path);
    return reader && normal ? reader->Find(*normal) : Reader::Missing;
}
bool PkgBackend::Exists(std::string_view path) {
    return Find(path) != Reader::Missing;
}
bool PkgBackend::IsDirectory(std::string_view path) {
    const auto id = Find(path);
    return id != Reader::Missing && reader->Entries()[id].directory;
}
std::unique_ptr<IFile> PkgBackend::Open(std::string_view name, Common::FS::FileAccessMode mode) {
    if (mode != Common::FS::FileAccessMode::Read)
        return nullptr;
    const auto id = Find(name);
    if (id == Reader::Missing || reader->Entries()[id].directory)
        return nullptr;
    return std::make_unique<PkgFile>(reader, id, path);
}
std::unique_ptr<IDirectory> PkgBackend::OpenDir(std::string_view name) {
    const auto id = Find(name);
    if (id == Reader::Missing || !reader->Entries()[id].directory)
        return nullptr;
    auto prefix = reader->Entries()[id].path;
    if (!prefix.empty())
        prefix += '/';
    std::vector<DirEntry> entries;
    for (const auto& entry : reader->Entries()) {
        if (!entry.path.starts_with(prefix))
            continue;
        const auto suffix = std::string_view(entry.path).substr(prefix.size());
        if (suffix.empty() || suffix.find('/') != std::string_view::npos)
            continue;
        entries.push_back({std::string(suffix), entry.directory, entry.size});
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return std::make_unique<PkgDirectory>(std::move(entries));
}
std::optional<std::vector<u8>> PkgBackend::ReadFile(std::string_view name) const {
    const auto id = Find(name);
    if (id == Reader::Missing || reader->Entries()[id].directory)
        return std::nullopt;
    try {
        // This convenience method is for metadata/assets. Large game data must
        // go through IFile's positioned reads, not allocate an entire package file.
        const auto size = reader->Entries()[id].size;
        if (size > 256 * 1024 * 1024)
            return std::nullopt;
        std::vector<u8> data(size);
        if (reader->Read(id, 0, data) != size)
            return std::nullopt;
        return data;
    } catch (const std::exception& e) {
        LOG_ERROR(Kernel_Fs, "PKG read {}: {}", path.string(), e.what());
        return std::nullopt;
    }
}
} // namespace Core::FileSys
