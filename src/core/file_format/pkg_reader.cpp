// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/file_format/pkg_reader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <zlib.h>
#include "core/crypto/crypto.h"
#include "core/file_format/pkg_type.h"

namespace Core::FileFormat {
namespace {
using Bytes = std::vector<u8>;
using ReadFn = std::function<void(u64, std::span<u8>)>;
constexpr u64 SectorSize = 4096;
constexpr u64 MaxEntries = 262144;

void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(std::string("PKG: ") + message);
}
void Range(u64 offset, u64 length, u64 size) {
    Require(offset <= size && length <= size - offset, "read outside container");
}
u64 LE(std::span<const u8> bytes, size_t offset, size_t width) {
    Range(offset, width, bytes.size());
    u64 value = 0;
    for (size_t i = 0; i < width; ++i)
        value |= u64(bytes[offset + i]) << (8 * i);
    return value;
}
u64 BE(std::span<const u8> bytes, size_t offset, size_t width) {
    Range(offset, width, bytes.size());
    u64 value = 0;
    for (size_t i = 0; i < width; ++i)
        value = (value << 8) | bytes[offset + i];
    return value;
}
std::string Name(std::span<const u8> bytes) {
    std::string name(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Require(!name.empty() && name != "." && name != ".." &&
                name.find_first_of("/\\:") == std::string::npos &&
                name.find('\0') == std::string::npos,
            "invalid directory name");
    return name;
}

// PFS inode and directory parsing is shared by the outer signed image and the
// inner filesystem. Consult the inode/block maps, never scan payload for magic.
struct Inode {
    u16 mode{};
    u32 flags{};
    u64 size{}, compressed_size{}, blocks{};
    std::array<u32, 12> direct{};
    std::array<u32, 5> indirect{};
};
class PfsVolume {
public:
    PfsVolume(ReadFn source, u64 size) : read(std::move(source)), length(size) {
        std::array<u8, 0x400> header{};
        Read(0, header);
        Require(LE(header, 0, 8) == 1 && LE(header, 8, 8) == 20130315,
                "invalid PFS header (unsupported layout or decryption key)");
        const auto mode = LE(header, 0x1c, 2);
        Require(!(mode & 2), "64-bit PFS inodes are not supported");
        signed_inodes = mode & 1;
        block_size = LE(header, 0x20, 4);
        Require(block_size >= 4096 && block_size <= 1024 * 1024 &&
                    (block_size & (block_size - 1)) == 0,
                "invalid PFS block size");
        const auto count = LE(header, 0x30, 8);
        const auto table_blocks = LE(header, 0x40, 8);
        const u64 stride = signed_inodes ? 0x2c8 : 0xa8;
        const u64 per_block = block_size / stride;
        Require(count > 0 && count <= MaxEntries && table_blocks > 0 &&
                    table_blocks <= (length / block_size) && count <= table_blocks * per_block,
                "invalid inode table size");
        // The superblock's signed-64 inode describes the inode table. Its
        // indirect signature blocks precede the contiguous table itself.
        u64 table_start = block_size;
        for (u64 i = 0; i < 5; ++i) {
            const auto pointer = LE(header, 0x50 + 0x68 + (12 + i) * 40 + 32, 8);
            if (pointer && pointer != ~u64{0})
                table_start += block_size;
        }
        Bytes block(block_size);
        for (u64 b = 0; inodes.size() < count; ++b) {
            Read(table_start + b * block_size, block);
            for (u64 i = 0; i < per_block && inodes.size() < count; ++i) {
                const auto data = std::span<const u8>(block).subspan(i * stride, stride);
                Inode node;
                node.mode = LE(data, 0, 2);
                node.flags = LE(data, 4, 4);
                node.size = LE(data, 8, 8);
                node.compressed_size = LE(data, 16, 8);
                node.blocks = LE(data, 0x60, 4);
                if (node.size > INT64_MAX || node.blocks > length / block_size)
                    throw std::runtime_error("PKG: invalid inode " + std::to_string(inodes.size()) +
                                             " size=" + std::to_string(node.size) +
                                             " blocks=" + std::to_string(node.blocks) +
                                             " volume=" + std::to_string(length) +
                                             " table=" + std::to_string(table_start));
                const u64 step = signed_inodes ? 36 : 4;
                const u64 skip = signed_inodes ? 32 : 0;
                for (size_t j = 0; j < 12; ++j)
                    node.direct[j] = LE(data, 0x64 + j * step + skip, 4);
                for (size_t j = 0; j < 5; ++j)
                    node.indirect[j] = LE(data, 0x64 + (12 + j) * step + skip, 4);
                inodes.push_back(node);
            }
        }
    }
    const Inode& Node(u32 id) const {
        Require(id < inodes.size(), "inode number out of range");
        return inodes[id];
    }
    void ReadNode(u32 id, u64 pos, std::span<u8> output) const {
        const auto& node = Node(id);
        Range(pos, output.size(), node.size);
        // db[1] == -1 is PFS's contiguous extent representation.
        if (node.blocks <= 1 || node.direct[1] == UINT32_MAX) {
            Range(pos, output.size(), node.blocks * block_size);
            Read(u64(node.direct[0]) * block_size + pos, output);
            return;
        }
        while (!output.empty()) {
            const u64 logical = pos / block_size;
            Require(logical < node.blocks, "file block out of range");
            const auto physical = Block(node, logical);
            const auto take = std::min<u64>(output.size(), block_size - pos % block_size);
            Read(u64(physical) * block_size + pos % block_size, output.first(take));
            output = output.subspan(take);
            pos += take;
        }
    }
    struct Child {
        u32 inode;
        u32 type;
        std::string name;
    };
    std::vector<Child> Children(u32 id) const {
        const auto& node = Node(id);
        Require((node.mode & 0xf000) == 0x4000 && node.size <= 64 * 1024 * 1024,
                "invalid directory inode");
        std::vector<Child> result;
        Bytes block(block_size);
        for (u64 offset = 0; offset < node.size; offset += block_size) {
            const auto take = std::min<u64>(block_size, node.size - offset);
            ReadNode(id, offset, std::span(block).first(take));
            for (u64 p = 0; p + 16 <= take;) {
                const u32 ino = LE(block, p, 4), type = LE(block, p + 4, 4);
                const u32 name_size = LE(block, p + 8, 4), record_size = LE(block, p + 12, 4);
                if (!record_size)
                    break;
                Require(record_size >= 16 && record_size <= take - p &&
                            name_size <= record_size - 16,
                        "invalid directory record");
                if (type == 2 || type == 3) {
                    (void)Node(ino);
                    result.push_back(
                        {ino, type, Name(std::span(block).subspan(p + 16, name_size))});
                    Require(result.size() <= MaxEntries, "directory too large");
                }
                p += record_size;
            }
        }
        return result;
    }
    u32 FindChild(u32 parent, std::string_view name) const {
        for (const auto& child : Children(parent))
            if (child.name == name)
                return child.inode;
        throw std::runtime_error("PKG: missing PFS entry " + std::string(name));
    }

private:
    void Read(u64 offset, std::span<u8> output) const {
        Range(offset, output.size(), length);
        read(offset, output);
    }
    u32 Block(const Inode& node, u64 index) const {
        if (index < 12)
            return node.direct[index];
        index -= 12;
        const u64 step = signed_inodes ? 36 : 4;
        const u64 capacity = block_size / step;
        u64 span = capacity;
        for (size_t level = 0; level < node.indirect.size(); ++level) {
            if (index < span) {
                u32 pointer = node.indirect[level];
                for (size_t depth = level + 1; depth; --depth) {
                    Require(pointer && pointer != UINT32_MAX, "missing indirect block");
                    span /= capacity;
                    std::array<u8, 4> bytes{};
                    Read(u64(pointer) * block_size + (index / span) * step +
                             (signed_inodes ? 32 : 0),
                         bytes);
                    pointer = LE(bytes, 0, 4);
                    index %= span;
                }
                Require(pointer != UINT32_MAX, "missing file block");
                return pointer;
            }
            index -= span;
            if (span > UINT64_MAX / capacity)
                break;
            span *= capacity;
        }
        throw std::runtime_error("PKG: unsupported indirect block depth");
    }
    ReadFn read;
    u64 length{}, block_size{};
    bool signed_inodes{};
    std::vector<Inode> inodes;
};
} // namespace

struct PkgReader::Impl {
    struct File {
        ReadFn read;
    };
    std::ifstream source;
    u64 source_size{};
    mutable std::mutex mutex;
    Statistics stats;
    std::vector<Entry> entries;
    std::vector<File> files;
    std::unordered_map<std::string, size_t> index;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> xts{EVP_CIPHER_CTX_new(),
                                                                        EVP_CIPHER_CTX_free};
    std::array<u8, 32> xts_key{};
    std::shared_ptr<PfsVolume> outer, inner;
    std::vector<u64> offsets;
    u64 pfsc_size{}, block_size{}, logical_size{};
    ReadFn pfsc_read;
    // At most 8 MiB of decompressed data per open package. Directory/index
    // metadata is immutable; this LRU and the seekable source share one lock.
    struct Cached {
        u64 id;
        Bytes data;
    };
    std::list<Cached> cache;
    std::unordered_map<u64, std::list<Cached>::iterator> cached;

    void Raw(u64 offset, std::span<u8> output) {
        Range(offset, output.size(), source_size);
        Require(offset <= INT64_MAX && output.size() <= INT64_MAX, "offset too large");
        source.clear();
        source.seekg(static_cast<std::streamoff>(offset));
        source.read(reinterpret_cast<char*>(output.data()),
                    static_cast<std::streamsize>(output.size()));
        Require(source && source.gcount() == static_cast<std::streamsize>(output.size()),
                "truncated package read");
        stats.source_bytes += output.size();
    }
    void Add(std::string path, u64 size, bool directory, ReadFn read = {}, bool replace = false) {
        Require(path.size() < 4096 && entries.size() < MaxEntries, "filesystem index too large");
        const auto found = index.find(path);
        if (found != index.end()) {
            Require(entries[found->second].directory == directory, "file/directory collision");
            if (replace) {
                entries[found->second].size = size;
                files[found->second].read = std::move(read);
            } else
                Require(directory, "duplicate file path");
            return;
        }
        index.emplace(path, entries.size());
        entries.push_back({std::move(path), size, directory});
        files.push_back({std::move(read)});
    }
    void Walk(u32 id, const std::string& prefix, std::set<u32>& ancestors, unsigned depth = 0) {
        Require(depth < 128 && ancestors.insert(id).second, "cyclic or excessively deep directory");
        for (const auto& child : inner->Children(id)) {
            const auto path = prefix.empty() ? child.name : prefix + '/' + child.name;
            const auto& node = inner->Node(child.inode);
            const bool directory = child.type == 3;
            Require((node.mode & 0xf000) == (directory ? 0x4000 : 0x8000), "inode type mismatch");
            Require(directory || !(node.flags & 1), "nested compressed files are not supported");
            Add(path, node.size, directory, [this, ino = child.inode](u64 off, std::span<u8> out) {
                inner->ReadNode(ino, off, out);
            });
            if (directory)
                Walk(child.inode, path, ancestors, depth + 1);
        }
        ancestors.erase(id);
    }
    void InflateRead(u64 pos, std::span<u8> output) {
        Range(pos, output.size(), logical_size);
        while (!output.empty()) {
            const u64 block = pos / block_size;
            auto found = cached.find(block);
            if (found == cached.end()) {
                const auto packed_size = offsets[block + 1] - offsets[block];
                Require(packed_size > 0 && packed_size <= block_size,
                        "file references a PFSC padding block");
                Bytes packed(packed_size), data(block_size);
                pfsc_read(offsets[block], packed);
                if (packed_size == block_size)
                    data = std::move(packed);
                else {
                    z_stream z{};
                    Require(inflateInit(&z) == Z_OK, "cannot initialize inflater");
                    z.next_in = packed.data();
                    z.avail_in = packed.size();
                    z.next_out = data.data();
                    z.avail_out = data.size();
                    const auto status = inflate(&z, Z_FINISH);
                    const auto produced = z.total_out;
                    inflateEnd(&z);
                    // Some PFSC writers omit the trailer; a full output block
                    // is required even then. Never turn an I/O failure into zeros.
                    Require((status == Z_STREAM_END || status == Z_BUF_ERROR) &&
                                produced == block_size,
                            "invalid compressed block");
                }
                ++stats.inflated_blocks;
                if (cache.size() >= (8 * 1024 * 1024 / block_size)) {
                    cached.erase(cache.back().id);
                    cache.pop_back();
                }
                cache.push_front({block, std::move(data)});
                found = cached.emplace(block, cache.begin()).first;
            } else {
                ++stats.cache_hits;
                cache.splice(cache.begin(), cache, found->second);
            }
            const auto take = std::min<u64>(output.size(), block_size - pos % block_size);
            std::memcpy(output.data(), found->second->data.data() + pos % block_size, take);
            output = output.subspan(take);
            pos += take;
        }
    }
    explicit Impl(const std::filesystem::path& path) : source(path, std::ios::binary) {
        Require(source.is_open() && xts, "cannot open package");
        source_size = std::filesystem::file_size(path);
        std::array<u8, 0x1000> header{};
        Raw(0, header);
        Require(BE(header, 0, 4) == 0x7f434e54, "invalid package magic");
        Require(BE(header, 0x430, 8) == source_size, "package size mismatch or split package");
        // Delta containers need reconstruction before being used as file overlays.
        Require((BE(header, 0x78, 4) & 0x01000000) == 0, "delta updates are not supported");
        const u64 count = BE(header, 0x10, 4), table_offset = BE(header, 0x18, 4);
        Require(count > 0 && count <= 65536, "invalid package entry count");
        Bytes table(count * 32);
        Raw(table_offset, table);
        std::array<u8, 32> dk3{}, ekpfs{};
        bool has_dk3 = false, has_image_key = false;
        const auto entry_bytes = [&](size_t i) {
            return std::span<const u8>(table).subspan(i * 32, 32);
        };
        for (size_t i = 0; i < count; ++i) {
            const auto entry = entry_bytes(i);
            Range(BE(entry, 16, 4), BE(entry, 20, 4), source_size);
            if (BE(entry, 0, 4) == 0x10) {
                Require(BE(entry, 20, 4) >= 0x20 + 7 * 0x20 + 4 * 0x100, "short entry keys");
                std::array<u8, 256> key{};
                Raw(BE(entry, 16, 4) + 0x20 + 7 * 0x20 + 3 * 0x100, key);
                Crypto::RSA2048Decrypt(dk3, key, true);
                has_dk3 = std::any_of(dk3.begin(), dk3.end(), [](u8 b) { return b != 0; });
            }
        }
        Add("", 0, true);
        Add("sce_sys", 0, true);
        for (size_t i = 0; i < count; ++i) {
            const auto entry = entry_bytes(i);
            const u32 id = BE(entry, 0, 4);
            const u64 offset = BE(entry, 16, 4), size = BE(entry, 20, 4);
            std::array<u8, 64> key_input{};
            std::copy(entry.begin(), entry.end(), key_input.begin());
            std::copy(dk3.begin(), dk3.end(), key_input.begin() + 32);
            std::array<u8, 32> ivkey{};
            if (id == 0x20 || (id >= 0x400 && id <= 0x403)) {
                Require(has_dk3, "cannot unwrap package entry key");
                Crypto::IvKeyHash256(key_input, ivkey);
            }
            if (id == 0x20) {
                Require(size == 256, "invalid image key size");
                std::array<u8, 256> encrypted{}, decrypted{};
                Raw(offset, encrypted);
                Crypto::AesCbcCfb128Decrypt(ivkey, encrypted, decrypted);
                Crypto::RSA2048Decrypt(ekpfs, decrypted, false);
                has_image_key =
                    std::any_of(ekpfs.begin(), ekpfs.end(), [](u8 b) { return b != 0; });
            }
            auto name = std::string(GetEntryNameByType(id));
            if (name.empty())
                name = std::to_string(id);
            if (id >= 0x400 && id <= 0x403) {
                Require(size <= 16 * 1024 * 1024, "oversized encrypted metadata");
                Bytes encrypted((size + 15) & ~u64{15});
                auto plain = std::make_shared<Bytes>(encrypted.size());
                Raw(offset, std::span(encrypted).first(size));
                Crypto::AesCbcCfb128Decrypt(ivkey, encrypted, *plain);
                plain->resize(size);
                Add("sce_sys/" + name, size, false, [plain](u64 off, std::span<u8> out) {
                    Range(off, out.size(), plain->size());
                    std::memcpy(out.data(), plain->data() + off, out.size());
                });
            } else {
                Add("sce_sys/" + name, size, false,
                    [this, offset, size](u64 off, std::span<u8> out) {
                        Range(off, out.size(), size);
                        Raw(offset + off, out);
                    });
            }
        }
        const u64 pfs_offset = BE(header, 0x410, 8), pfs_size = BE(header, 0x418, 8);
        if (!pfs_size)
            return; // License-only content is represented by sce_sys metadata.
        Require(has_image_key, "cannot decrypt image key (only supported FPKG keys are available)");
        Range(pfs_offset, pfs_size, source_size);
        std::array<u8, 0x400> pfs_header{};
        Raw(pfs_offset, pfs_header);
        const u64 crypt_start = LE(pfs_header, 0x20, 4);
        Require(crypt_start >= SectorSize && crypt_start <= 1024 * 1024 &&
                    crypt_start % SectorSize == 0,
                "invalid encryption boundary");
        std::array<u8, 16> seed{}, data{}, tweak{};
        std::copy_n(pfs_header.begin() + 0x370, 16, seed.begin());
        const auto original_ekpfs = ekpfs;
        const bool new_crypt = BE(header, 0x408, 8) & 0x2000000000000000ULL;
        const auto derive = [&](bool use_new_crypt) {
            ekpfs = original_ekpfs;
            if (use_new_crypt) {
                std::array<u8, 32> derived{};
                unsigned int length = 0;
                Require(HMAC(EVP_sha256(), ekpfs.data(), ekpfs.size(), seed.data(), seed.size(),
                             derived.data(), &length) &&
                            length == derived.size(),
                        "key derivation failed");
                ekpfs = derived;
            }
            Crypto::PfsGenCryptoKey(ekpfs, seed, data, tweak);
            std::copy(data.begin(), data.end(), xts_key.begin());
            std::copy(tweak.begin(), tweak.end(), xts_key.begin() + 16);
        };
        derive(new_crypt);
        const bool encrypted_pfs = LE(pfs_header, 0x1c, 2) & 4;
        ReadFn decrypt = [this, pfs_offset, pfs_size, crypt_start,
                          encrypted_pfs](u64 pos, std::span<u8> out) {
            Range(pos, out.size(), pfs_size);
            if (!encrypted_pfs) {
                Raw(pfs_offset + pos, out);
                return;
            }
            std::array<u8, SectorSize> encrypted{}, plain{};
            while (!out.empty()) {
                const u64 aligned = pos & ~(SectorSize - 1);
                Range(aligned, SectorSize, pfs_size);
                Raw(pfs_offset + aligned, encrypted);
                if (aligned < crypt_start)
                    plain = encrypted;
                else {
                    std::array<u8, 16> iv{};
                    for (size_t j = 0; j < 8; ++j)
                        iv[j] = (aligned / SectorSize) >> (j * 8);
                    int written = 0;
                    Require(EVP_DecryptInit_ex(xts.get(), EVP_aes_128_xts(), nullptr,
                                               xts_key.data(), iv.data()) == 1 &&
                                EVP_DecryptUpdate(xts.get(), plain.data(), &written,
                                                  encrypted.data(), encrypted.size()) == 1 &&
                                written == SectorSize,
                            "XTS decryption failed");
                }
                const auto take = std::min<u64>(out.size(), SectorSize - pos % SectorSize);
                std::memcpy(out.data(), plain.data() + pos % SectorSize, take);
                out = out.subspan(take);
                pos += take;
            }
        };
        try {
            outer = std::make_shared<PfsVolume>(decrypt, pfs_size);
        } catch (const std::runtime_error&) {
            // Repacked FPKGs can retain the retail "new crypt" flag while
            // using the older derivation. Accept a fallback only if the PFS
            // structure validates, as in the Android importer.
            derive(!new_crypt);
            outer = std::make_shared<PfsVolume>(decrypt, pfs_size);
        }
        const auto root = outer->FindChild(0, "uroot");
        const auto image = outer->FindChild(root, "pfs_image.dat");
        pfsc_size = outer->Node(image).size;
        pfsc_read = [this, image](u64 pos, std::span<u8> out) { outer->ReadNode(image, pos, out); };
        std::array<u8, 0x30> pfsc_header{};
        pfsc_read(0, pfsc_header);
        ReadFn inner_read = pfsc_read;
        logical_size = pfsc_size;
        if (LE(pfsc_header, 0, 4) == 0x43534650) {
            block_size = LE(pfsc_header, 12, 4);
            Require(block_size >= 4096 && block_size <= 1024 * 1024 &&
                        (block_size & (block_size - 1)) == 0 &&
                        LE(pfsc_header, 16, 8) == block_size,
                    "invalid PFSC block size");
            logical_size = LE(pfsc_header, 40, 8);
            const auto blocks = logical_size / block_size;
            Require(logical_size && logical_size % block_size == 0 && blocks <= 8 * 1024 * 1024,
                    "invalid PFSC logical size");
            const auto map_offset = LE(pfsc_header, 24, 8);
            Range(map_offset, (blocks + 1) * 8, pfsc_size);
            Bytes map((blocks + 1) * 8);
            pfsc_read(map_offset, map);
            for (u64 i = 0; i <= blocks; ++i) {
                const auto offset = LE(map, i * 8, 8);
                Require(offset <= pfsc_size && offset >= map_offset + map.size(),
                        "PFSC offset out of range");
                // Alignment padding can occupy a logical block that no file
                // references (e.g. the 64 MiB boundary in large FPKGs). Validate
                // ordering here; only data reads require a decodable block span.
                if (i)
                    Require(offset >= offsets.back(), "non-monotonic PFSC block map");
                offsets.push_back(offset);
            }
            inner_read = [this](u64 pos, std::span<u8> out) { InflateRead(pos, out); };
        }
        inner = std::make_shared<PfsVolume>(inner_read, logical_size);
        std::set<u32> ancestors;
        Walk(inner->FindChild(0, "uroot"), "", ancestors);
    }
};

PkgReader::PkgReader(const std::filesystem::path& path) : impl(std::make_unique<Impl>(path)) {}
PkgReader::~PkgReader() = default;
std::shared_ptr<PkgReader> PkgReader::Open(const std::filesystem::path& path) {
    // Mounts, the loader and metadata inspection share the same reader. Weak
    // ownership releases file handles and caches when the last user closes it.
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::weak_ptr<PkgReader>> readers;
    std::scoped_lock lock(mutex);
    const auto canonical = std::filesystem::canonical(path);
    if (auto found = readers.find(canonical); found != readers.end())
        if (auto reader = found->second.lock())
            return reader;
    auto reader = std::shared_ptr<PkgReader>(new PkgReader(canonical));
    std::erase_if(readers, [](const auto& item) { return item.second.expired(); });
    readers[canonical] = reader;
    return reader;
}
const std::vector<PkgReader::Entry>& PkgReader::Entries() const {
    return impl->entries;
}
size_t PkgReader::Find(std::string_view path) const {
    auto found = impl->index.find(std::string(path));
    return found == impl->index.end() ? Missing : found->second;
}
size_t PkgReader::Read(size_t entry, u64 offset, std::span<u8> output) {
    Require(entry < impl->entries.size() && !impl->entries[entry].directory, "invalid file handle");
    const auto size = impl->entries[entry].size;
    if (output.empty() || offset >= size)
        return 0;
    const auto take = std::min<u64>(output.size(), size - offset);
    std::scoped_lock lock(impl->mutex);
    impl->files[entry].read(offset, output.first(take));
    return take;
}
PkgReader::Statistics PkgReader::GetStatistics() const {
    std::scoped_lock lock(impl->mutex);
    return impl->stats;
}
} // namespace Core::FileFormat
