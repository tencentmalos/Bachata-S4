// SPDX-License-Identifier: GPL-2.0-or-later
// PKG reader on a real package (Android device test): AES-XTS known answers and
// hardware-vs-LibreSSL comparisons, then random reads through PkgReader::Open (bulk
// positional reads, hardware AES, concurrent readers) compared byte for byte with
// PkgReader::OpenReference (the original serial per-sector LibreSSL path), and the
// throughput of both. Usage: pkg_reader_tests [package.pkg] [reads] [threads]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/crypto/aes_xts.h"
#include "core/file_format/pkg_reader.h"

namespace {
using Reader = Core::FileFormat::PkgReader;
unsigned checks{}, failures{};
void Check(const std::string& name, bool ok) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name.c_str());
    }
}
std::vector<u8> Hex(const char* text) {
    std::vector<u8> out;
    for (size_t i = 0; text[i] && text[i + 1]; i += 2)
        out.push_back(u8(std::stoi(std::string(text + i, 2), nullptr, 16)));
    return out;
}
double Seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

void CipherTests() {
    // IEEE 1619-2007 XTS-AES-128 vectors 1 and 2 (32-byte data units).
    struct Vector {
        const char* key1;
        const char* key2;
        u64 unit;
        const char* plain;
        const char* cipher;
    };
    const Vector vectors[] = {
        {"00000000000000000000000000000000", "00000000000000000000000000000000", 0,
         "0000000000000000000000000000000000000000000000000000000000000000",
         "917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e"},
        {"11111111111111111111111111111111", "22222222222222222222222222222222", 0x3333333333,
         "4444444444444444444444444444444444444444444444444444444444444444",
         "c454185e6a16936e39334038acef838bfb186fff7480adc4289382ecd6d394f0"},
    };
    for (const auto& v : vectors) {
        std::array<u8, 32> key{};
        const auto k1 = Hex(v.key1), k2 = Hex(v.key2);
        std::copy(k1.begin(), k1.end(), key.begin());
        std::copy(k2.begin(), k2.end(), key.begin() + 16);
        for (bool hardware : {true, false}) {
            Core::Crypto::Aes128Xts xts(key, hardware);
            auto data = Hex(v.cipher);
            xts.Decrypt(v.unit, data.size(), data);
            Check(std::string("IEEE 1619 vector ") + Core::Crypto::Aes128Xts::Name(xts.GetBackend()),
                  data == Hex(v.plain));
        }
    }
    std::mt19937_64 rng(1619);
    const char* backend = "";
    for (int round = 0; round < 64; ++round) {
        std::array<u8, 32> key{};
        for (auto& b : key)
            b = u8(rng());
        Core::Crypto::Aes128Xts hardware(key, true), software(key, false);
        backend = Core::Crypto::Aes128Xts::Name(hardware.GetBackend());
        const size_t unit = round % 2 ? 4096 : 512;
        std::vector<u8> a(unit * (1 + rng() % 8));
        for (auto& b : a)
            b = u8(rng());
        auto b = a;
        const u64 first = rng();
        hardware.Decrypt(first, unit, a);
        software.Decrypt(first, unit, b);
        Check("hardware == LibreSSL", a == b);
    }
    std::printf("cipher backend: %s\n", backend);
    // Throughput of both paths on 64 MiB of 4 KiB sectors.
    std::array<u8, 32> key{};
    for (auto& b : key)
        b = u8(rng());
    std::vector<u8> data(64u << 20);
    for (bool hw : {true, false}) {
        Core::Crypto::Aes128Xts xts(key, hw);
        const auto start = std::chrono::steady_clock::now();
        xts.Decrypt(0, 4096, data);
        std::printf("decrypt %-10s %7.0f MB/s\n", Core::Crypto::Aes128Xts::Name(xts.GetBackend()),
                    data.size() / 1e6 / Seconds(start));
    }
}

void PackageTests(const std::filesystem::path& path, unsigned reads, unsigned threads) {
    auto fast = Reader::Open(path);
    auto reference = Reader::OpenReference(path);
    const auto& entries = fast->Entries();
    Check("same index", entries.size() == reference->Entries().size());
    std::vector<size_t> files;
    for (size_t i = 0; i < entries.size(); ++i)
        if (!entries[i].directory && entries[i].size)
            files.push_back(i);
    std::sort(files.begin(), files.end(),
              [&](size_t a, size_t b) { return entries[a].size > entries[b].size; });
    std::printf("%zu files, largest %s (%llu bytes)\n", files.size(), entries[files[0]].path.c_str(),
                (unsigned long long)entries[files[0]].size);
    struct Request {
        size_t file;
        u64 offset;
        size_t length;
    };
    std::mt19937_64 rng(4096);
    std::vector<Request> requests;
    for (unsigned i = 0; i < reads; ++i) {
        // Mostly the large pak files, some small files; any alignment, up to 1 MiB.
        const size_t file = rng() % 4 ? files[rng() % std::min<size_t>(files.size(), 32)]
                                      : files[rng() % files.size()];
        const u64 size = entries[file].size;
        const size_t length = size_t(1 + rng() % std::min<u64>(size, rng() % 2 ? 4096 : 1 << 20));
        const u64 offset = rng() % (size - length + 1);
        requests.push_back({file, offset, length});
    }
    // Reference results, serially.
    std::vector<std::vector<u8>> want(requests.size());
    auto start = std::chrono::steady_clock::now();
    u64 bytes = 0;
    for (size_t i = 0; i < requests.size(); ++i) {
        want[i].resize(requests[i].length);
        reference->Read(requests[i].file, requests[i].offset, want[i]);
        bytes += requests[i].length;
    }
    std::printf("reference random reads: %.2f s, %.0f MB/s\n", Seconds(start),
                bytes / 1e6 / Seconds(start));
    // Same requests, fast reader, one thread then `threads` threads.
    for (unsigned count : {1u, threads}) {
        std::atomic<size_t> next{0}, mismatches{0};
        start = std::chrono::steady_clock::now();
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < count; ++t)
            pool.emplace_back([&] {
                std::vector<u8> got;
                for (size_t i; (i = next.fetch_add(1)) < requests.size();) {
                    got.assign(requests[i].length, 0xcd);
                    fast->Read(requests[i].file, requests[i].offset, got);
                    if (got != want[i])
                        mismatches.fetch_add(1);
                }
            });
        for (auto& t : pool)
            t.join();
        const double elapsed = Seconds(start);
        std::printf("fast random reads x%u: %.2f s, %.0f MB/s\n", count, elapsed, bytes / 1e6 / elapsed);
        Check("fast reader == reference (" + std::to_string(count) + " threads)", mismatches == 0);
    }
    // Sequential throughput over the largest file, 256 KiB reads.
    const size_t big = files[0];
    const u64 span = std::min<u64>(entries[big].size, 256ull << 20);
    std::vector<u8> chunk(256 << 10), other(256 << 10);
    for (int pass = 0; pass < 2; ++pass) {
        for (auto* reader : {reference.get(), fast.get()}) {
            start = std::chrono::steady_clock::now();
            for (u64 offset = 0; offset < span; offset += chunk.size())
                reader->Read(big, offset, std::span(chunk).first(std::min<u64>(chunk.size(), span - offset)));
            if (pass)
                std::printf("sequential %-9s %.0f MB/s\n", reader == fast.get() ? "fast" : "reference",
                            span / 1e6 / Seconds(start));
        }
    }
    bool same = true;
    for (u64 offset = 0; offset < span && same; offset += 7 * chunk.size()) {
        const auto n = std::min<u64>(chunk.size(), span - offset);
        fast->Read(big, offset, std::span(chunk).first(n));
        reference->Read(big, offset, std::span(other).first(n));
        same = std::equal(chunk.begin(), chunk.begin() + n, other.begin());
    }
    Check("sequential samples equal", same);
    const auto s = fast->GetStatistics();
    std::printf("fast statistics: source=%llu MB inflated=%llu hits=%llu\n",
                (unsigned long long)(s.source_bytes >> 20), (unsigned long long)s.inflated_blocks,
                (unsigned long long)s.cache_hits);
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root = std::filesystem::current_path() / "pkg-reader-root";
    std::filesystem::create_directories(root);
    Common::FS::InitializeAndroidUserPaths(root);
    Common::Log::Setup("pkg-reader-tests");
    CipherTests();
    if (argc > 1)
        PackageTests(argv[1], argc > 2 ? unsigned(std::stoul(argv[2])) : 2000,
                     argc > 3 ? unsigned(std::stoul(argv[3])) : 4);
    std::printf("pkg reader: %u checks, %u failures\n", checks, failures);
    Common::Log::Shutdown();
    return failures ? 1 : 0;
}
