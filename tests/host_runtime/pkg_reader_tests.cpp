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
#include <fcntl.h>
#include <unistd.h>
#include "../pkg_fixture.h"
#include "core/file_sys/fs.h"
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

void DescriptorTests(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    namespace vfs = Core::FileSys;
    const auto dir=root/"descriptor-fixture";
    fs::create_directories(dir/"lease");
    PkgFixture::Make(dir/"base.pkg",{{"eboot.bin",{'E','L','F'}},{"asset",{'b','a','s','e'}}});
    PkgFixture::Make(dir/"update.pkg",{{"asset",{'p','a','t','c','h'}}},"gp","CUSA99999","SYNTHETIC0000000","01.06");
    const int base=::open((dir/"base.pkg").c_str(),O_RDONLY|O_CLOEXEC);
    const int update=::open((dir/"update.pkg").c_str(),O_RDONLY|O_CLOEXEC);
    const auto alias=dir/"lease/CUSA99999.pkg";
    fs::create_symlink("/proc/self/fd/"+std::to_string(base),alias);
    fs::create_symlink("/proc/self/fd/"+std::to_string(update),dir/"lease/CUSA99999-UPD.pkg");
    Check("granted path retains overlay siblings",Reader::ResolvePath(alias)==fs::absolute(alias));
    const auto metadata=vfs::InspectArchiveInstall(alias);
    PSF psf;Check("base/update descriptor inspection",psf.Open(metadata.param_sfo)&&psf.GetString("APP_VER").value_or("")=="01.06");
    vfs::MntPoints mount;mount.Mount(alias,"/app0",true);
    // The reader owns dup'd regular FDs. Removing source names (or the provider
    // exiting) cannot revoke the already-open generation's inode capability.
    ::close(base);::close(update);fs::remove(dir/"base.pkg");fs::remove(dir/"update.pkg");
    auto file=mount.Open("/app0/asset");std::string got(5,'\0');
    Check("read after original descriptor/name release",file&&file->Read(got.data(),got.size())==5&&got=="patch");
    Check("managed PKG cannot be written",!mount.Open("/app0/asset",true));
    mount.UnmountAll();file.reset();
    PkgFixture::Make(dir/"writable.pkg",{{"eboot.bin",{'E'}}});
    const int writable=::open((dir/"writable.pkg").c_str(),O_RDWR|O_CLOEXEC);
    fs::create_symlink("/proc/self/fd/"+std::to_string(writable),dir/"lease/writable.pkg");
    bool rejected=false;try{(void)Reader::ResolvePath(dir/"lease/writable.pkg");}catch(...){rejected=true;}
    Check("writable descriptor alias rejected",rejected);::close(writable);fs::remove_all(dir);
}

void CaseLookupTests(const std::filesystem::path& root) {
    const auto base = root/"case.pkg";
    const auto update = root/"case-UPD.pkg";
    PkgFixture::Make(base, {{"r_system.wad", {'b','a','s','e'}},
        {"EXACT", {'A'}}, {"exact", {'B'}}, {"unique", {'U'}}});
    PkgFixture::Make(update, {{"R_SYSTEM.WAD", {'p','a','t','c','h'}}},
        "gp", "CUSA99999", "SYNTHETIC0000000", "01.06");
    auto reader = Reader::Open(base);
    Check("case fallback finds real payload", reader->Find("R_System.wad") == reader->Find("r_system.wad"));
    Check("exact case remains authoritative", reader->Find("EXACT") != reader->Find("exact"));
    Check("ambiguous fallback refused", reader->Find("Exact") == Reader::Missing);
    Check("missing name stays missing", reader->Find("absent") == Reader::Missing);
    Check("directory traversal not normalized into file", reader->Find("../unique") == Reader::Missing);
    Core::FileSys::MntPoints mount;
    mount.Mount(base, "/app0", true);
    auto file = mount.Open("/app0/R_System.wad");
    std::string got(5, '\0');
    Check("case fallback respects update overlay", file && file->Read(got.data(), got.size()) == 5 && got == "patch");
    Check("case lookup stays read-only", !mount.Open("/app0/R_System.wad", true));
    mount.UnmountAll(); file.reset(); reader.reset();
    std::filesystem::remove(base); std::filesystem::remove(update);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root = std::filesystem::current_path() / "pkg-reader-root";
    std::filesystem::create_directories(root);
    Common::FS::InitializeAndroidUserPaths(root);
    Common::Log::Setup("pkg-reader-tests");
    if (argc == 4 && std::string_view(argv[1]) == "--export-fixture") {
        const std::filesystem::path dest{argv[2]};
        std::ifstream input(argv[3],std::ios::binary);
        PkgFixture::Bytes eboot(std::istreambuf_iterator<char>{input},{});
        if (eboot.empty()) return 2;
        PkgFixture::Make(dest/"base.pkg",{{"eboot.bin",eboot},{"asset",{'b','a','s','e'}}});
        PkgFixture::Make(dest/"update.pkg",{{"asset",{'p','a','t','c','h'}}},"gp","CUSA99999","SYNTHETIC0000000","01.06");
        PkgFixture::Make(dest/"dlc.pkg",{{"addon",{'D','L','C'}}},"ac","CUSA99999","EXTRAS0000000000","01.00");
        Common::Log::Shutdown();return 0;
    }
    CaseLookupTests(root);
    CipherTests();
    if (argc > 1 && std::string_view(argv[1])=="--descriptor-fixture")
        DescriptorTests(root);
    else if (argc > 1)
        PackageTests(argv[1], argc > 2 ? unsigned(std::stoul(argv[2])) : 2000,
                     argc > 3 ? unsigned(std::stoul(argv[3])) : 4);
    std::printf("pkg reader: %u checks, %u failures\n", checks, failures);
    Common::Log::Shutdown();
    return failures ? 1 : 0;
}
