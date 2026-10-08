// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <future>
#include <random>
#include <gtest/gtest.h>
#include "core/file_format/pkg_reader.h"
#include "core/file_sys/backends/pkg_fs.h"
#include "core/file_sys/fs.h"
#include "pkg_fixture.h"

namespace {
namespace fs = std::filesystem;
namespace vfs = Core::FileSys;
using Reader = Core::FileFormat::PkgReader;
using Bytes = PkgFixture::Bytes;
using Mode = Common::FS::FileAccessMode;
using Origin = Common::FS::SeekOrigin;

class PkgTest : public ::testing::Test {
protected:
    fs::path root;
    void SetUp() override {
        root = fs::temp_directory_path() /
               ("shadps4-pkg-" +
                std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line()));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    void TearDown() override {
        fs::remove_all(root);
    }
    static Bytes Data(size_t size) {
        std::mt19937 rng(173);
        Bytes data(size);
        for (auto& b : data)
            b = u8(rng());
        return data;
    }
    static std::string Text(std::unique_ptr<vfs::IFile> file) {
        if (!file)
            return "<missing>";
        std::string text(file->Size(), '\0');
        return file->Read(text.data(), text.size()) == s64(text.size()) ? text : "<error>";
    }
};

TEST_F(PkgTest, RandomReadsAndMmapPreserveBytesAndCursor) {
    const auto data = Data(180137);
    PkgFixture::Make(root / "base.pkg",
                     {{"eboot.bin", {'E', 'L', 'F'}}, {"asset", data}, {"empty", {}}});
    vfs::PkgBackend backend(root / "base.pkg");
    ASSERT_TRUE(backend.IsOpen());
    auto file = backend.Open("asset", Mode::Read);
    ASSERT_TRUE(file);
    ASSERT_TRUE(file->Seek(77, Origin::SetOrigin));
    for (const u64 offset : {0ULL, 4090ULL, 65520ULL, 65536ULL, 131000ULL, 180130ULL, 180137ULL}) {
        Bytes got(8201);
        const auto take = std::min<u64>(got.size(), data.size() - offset);
        ASSERT_EQ(file->ReadAt(got.data(), got.size(), offset), s64(take));
        EXPECT_TRUE(std::equal(got.begin(), got.begin() + take, data.begin() + offset));
        EXPECT_EQ(file->Tell(), 77);
    }
    EXPECT_FALSE(file->Seek(INT64_MIN, Origin::CurrentPosition));
    EXPECT_EQ(file->Tell(), 77);
    EXPECT_FALSE(backend.Open("asset", Mode::Write));
    EXPECT_FALSE(backend.Open("../asset", Mode::Read));
    EXPECT_EQ(file->Write(data.data(), data.size()), -1);
    auto empty = backend.Open("empty", Mode::Read);
    ASSERT_TRUE(empty);
    u8 value = 123;
    EXPECT_EQ(empty->Read(&value, 1), 0);
    EXPECT_EQ(value, 123);
    Bytes mapped(196608);
    bool anonymous = false, protected_ = false;
    vfs::FileMapContext ctx;
    ctx.map_anonymous = [&](u8* addr, u64 size) {
        EXPECT_EQ(addr, mapped.data());
        EXPECT_EQ(size, mapped.size());
        anonymous = true;
    };
    ctx.protect = [&](u8*, u64, u32 prot) {
        EXPECT_EQ(prot, 1);
        protected_ = true;
    };
    ASSERT_TRUE(file->Map(mapped.data(), mapped.size(), 0, 1, ctx));
    EXPECT_TRUE(anonymous && protected_);
    EXPECT_TRUE(std::equal(data.begin(), data.end(), mapped.begin()));
    EXPECT_TRUE(
        std::all_of(mapped.begin() + data.size(), mapped.end(), [](u8 b) { return b == 0; }));
}

TEST_F(PkgTest, ParallelPositionedReadsShareOneReader) {
    const auto data = Data(300123);
    PkgFixture::Make(root / "base.pkg", {{"asset", data}});
    auto reader = Reader::Open(root / "base.pkg");
    EXPECT_EQ(reader, Reader::Open(root / "base.pkg"));
    const auto id = reader->Find("asset");
    ASSERT_NE(id, Reader::Missing);
    std::vector<std::future<bool>> tasks;
    for (u64 t = 0; t < 8; ++t)
        tasks.push_back(std::async(std::launch::async, [&, t] {
            for (u64 i = 0; i < 32; ++i) {
                const auto offset = (i * 65317 + t * 997) % (data.size() - 9000);
                Bytes out(9000);
                if (reader->Read(id, offset, out) != out.size() ||
                    !std::equal(out.begin(), out.end(), data.begin() + offset))
                    return false;
            }
            return true;
        }));
    for (auto& task : tasks)
        EXPECT_TRUE(task.get());
    EXPECT_GT(reader->GetStatistics().cache_hits, 0);
}

TEST_F(PkgTest, SparsePackagePastFourGiBUses64BitPhysicalOffsets) {
    const auto data = Data(90000);
    PkgFixture::Make(root / "large.pkg", {{"asset", data}}, "gd", "CUSA99999", "SYNTHETIC0000000",
                     "01.00", true);
    ASSERT_GT(fs::file_size(root / "large.pkg"), u64(UINT32_MAX));
    auto r = Reader::Open(root / "large.pkg");
    Bytes got(data.size());
    ASSERT_EQ(r->Read(r->Find("asset"), 0, got), got.size());
    EXPECT_EQ(got, data);
    EXPECT_LT(r->GetStatistics().source_bytes, 2 * 1024 * 1024);
}

TEST_F(PkgTest, UpdateOverlayFallbackAndModsUseTheSameNamespace) {
    PkgFixture::Make(root / "base.pkg",
                     {{"eboot.bin", {'b'}}, {"asset", {'b'}}, {"base_only", {'b'}}});
    PkgFixture::Make(root / "base-UPD.pkg",
                     {{"eboot.bin", {'u'}}, {"asset", {'u'}}, {"new", {'u'}}}, "gp", "CUSA99999",
                     "SYNTHETIC0000000", "01.02");
    auto metadata = vfs::InspectArchiveInstall(root / "base.pkg");
    PSF sfo;
    ASSERT_TRUE(sfo.Open(metadata.param_sfo));
    EXPECT_EQ(sfo.GetString("APP_VER"), "01.02");
    vfs::MntPoints mounts;
    mounts.Mount(root / "base.pkg", "/app0", true);
    EXPECT_EQ(Text(mounts.Open("/app0/eboot.bin")), "u");
    EXPECT_EQ(Text(mounts.Open("/app0/base_only")), "b");
    EXPECT_EQ(Text(mounts.Open("/app0/new")), "u");
    fs::create_directory(root / "base-mods");
    std::ofstream(root / "base-mods/asset") << "m";
    mounts.UnmountAll();
    mounts.Mount(root / "base.pkg", "/app0", true);
    EXPECT_EQ(Text(mounts.Open("/app0/asset")), "m");
    EXPECT_EQ(vfs::ResolveGameRoot(root / "base"), root / "base.pkg");
    EXPECT_EQ(vfs::BaseGameFromOverlay(root / "base-UPD.pkg"), root / "base");
}

TEST_F(PkgTest, ForeignUpdateAndStandaloneDlcAreRejected) {
    PkgFixture::Make(root / "base.pkg", {{"eboot.bin", {'b'}}});
    PkgFixture::Make(root / "base-UPD.pkg", {{"eboot.bin", {'u'}}}, "gp", "CUSA00001");
    EXPECT_THROW(vfs::InspectArchiveInstall(root / "base.pkg"), std::runtime_error);
    vfs::MntPoints mounts;
    EXPECT_THROW(mounts.Mount(root / "base.pkg", "/app0", true), std::runtime_error);
    PkgFixture::Make(root / "dlc.pkg", {{"eboot.bin", {'d'}}}, "ac");
    EXPECT_THROW(vfs::InspectArchiveInstall(root / "dlc.pkg"), std::runtime_error);
}

TEST_F(PkgTest, DlcIdentitySelectsPayloadOverAnUnlockOnlyPackage) {
    PkgFixture::Make(root / "a-unlock.pkg", {}, "ac");
    PkgFixture::Make(root / "b-data.pkg", {{"asset", {'D', 'L', 'C'}}}, "ac");
    PkgFixture::Make(root / "foreign.pkg", {{"asset", {'x'}}}, "ac", "CUSA00001");
    PkgFixture::Make(root / "other.pkg", {}, "ac", "CUSA99999", "SYNTHETIC0000001");
    const auto selected = vfs::SelectAdditionalContent(vfs::ListContentRoots(root), "CUSA99999");
    ASSERT_EQ(selected.size(), 2);
    EXPECT_EQ(selected[0].root, root / "b-data.pkg");
    EXPECT_TRUE(selected[0].has_data);
    EXPECT_FALSE(selected[1].has_data);
    vfs::MntPoints mounts;
    mounts.Mount(selected[0].root, "/addcont0", true);
    EXPECT_EQ(Text(mounts.Open("/addcont0/asset")), "DLC");
}

TEST_F(PkgTest, TruncationDeltaAndBadCiphertextFailClosed) {
    PkgFixture::Make(root / "truncated.pkg", {{"asset", Data(90000)}});
    fs::resize_file(root / "truncated.pkg", fs::file_size(root / "truncated.pkg") - 1);
    EXPECT_THROW(Reader::Open(root / "truncated.pkg"), std::runtime_error);
    PkgFixture::Make(root / "delta.pkg", {{"asset", {'x'}}});
    {
        std::fstream out(root / "delta.pkg", std::ios::in | std::ios::out | std::ios::binary);
        out.seekp(0x78);
        out.put(1);
    }
    EXPECT_THROW(Reader::Open(root / "delta.pkg"), std::runtime_error);
    PkgFixture::Make(root / "bad.pkg", {{"asset", Data(90000)}});
    {
        std::fstream out(root / "bad.pkg", std::ios::in | std::ios::out | std::ios::binary);
        out.seekp(0x20000);
        out.write("corrupt inode!", 14);
    }
    EXPECT_THROW(Reader::Open(root / "bad.pkg"), std::runtime_error);

    PkgFixture::Make(root / "base.pkg", {{"eboot.bin", {'b'}}});
    PkgFixture::Make(root / "base-UPD.pkg", {{"eboot.bin", {'u'}}}, "gp");
    fs::resize_file(root / "base-UPD.pkg", 4096);
    vfs::MntPoints mounts;
    EXPECT_THROW(mounts.Mount(root / "base.pkg", "/app0", true), std::runtime_error);
}

// Optional local evidence: no commercial data or absolute user paths in fixtures.
TEST_F(PkgTest, RealKingdomHeartsLibraryMountsAndDeduplicatesDlc) {
    const auto* location = std::getenv("SHADPS4_PKG_VALIDATION_DIR");
    if (!location)
        GTEST_SKIP() << "Set SHADPS4_PKG_VALIDATION_DIR to the extracted CUSA15072 folder";
    const fs::path folder(location);
    std::vector<fs::path> bases;
    for (const auto& entry : fs::directory_iterator(folder))
        if (vfs::IsPkgFile(entry.path()))
            bases.push_back(entry.path());
    ASSERT_EQ(bases.size(), 1);
    const auto info = vfs::InspectArchiveInstall(bases.front());
    PSF sfo;
    ASSERT_TRUE(sfo.Open(info.param_sfo));
    EXPECT_EQ(sfo.GetString("TITLE_ID"), "CUSA15072");
    vfs::MntPoints mounts;
    mounts.Mount(bases.front(), "/app0", true);
    auto executable = mounts.Open("/app0/eboot.bin");
    ASSERT_TRUE(executable);
    std::array<u8, 4> magic{};
    ASSERT_EQ(executable->Read(magic.data(), magic.size()), 4);
    EXPECT_EQ(magic, (std::array<u8, 4>{0x4f, 0x15, 0x3d, 0x1d}));
    const auto candidates = vfs::ListGameAdditionalContentRoots(bases.front());
    ASSERT_EQ(candidates.size(), 6);
    const auto selected = vfs::SelectAdditionalContent(candidates, "CUSA15072");
    ASSERT_EQ(selected.size(), 5);
    size_t data_count = 0;
    for (size_t i = 0; i < selected.size(); ++i) {
        const auto& content = selected[i];
        if (!content.has_data)
            continue;
        ++data_count;
        const auto mount = "/addcont" + std::to_string(i);
        mounts.Mount(content.root, mount, true);
        auto backend = vfs::OpenGameBackend(content.root);
        ASSERT_TRUE(backend);
        auto reader = Reader::Open(content.root);
        const auto payload =
            std::find_if(reader->Entries().begin(), reader->Entries().end(), [](const auto& entry) {
                return !entry.directory && entry.size > 0 && !entry.path.starts_with("sce_sys/");
            });
        ASSERT_NE(payload, reader->Entries().end());
        auto file = mounts.Open(mount + "/" + payload->path);
        ASSERT_TRUE(file);
        std::array<u8, 8192> bytes{};
        EXPECT_EQ(file->ReadAt(bytes.data(), bytes.size(), 0),
                  s64(std::min<u64>(bytes.size(), payload->size)));
        if (content.content_id.ends_with("03"))
            EXPECT_EQ(content.root.filename(), "KingdomHearts3ReMind[2468c.com].pkg");
    }
    EXPECT_EQ(data_count, 2);
}

TEST_F(PkgTest, CbcAndXtsDoNotStripPlaintextAsPkcs7Padding) {
    const auto input = Data(4096);
    std::array<u8, 32> ivkey{};
    for (size_t i = 0; i < ivkey.size(); ++i)
        ivkey[i] = i;
    Bytes encrypted(input.size()), plain(input.size());
    auto* ctx = EVP_CIPHER_CTX_new();
    int written = 0;
    ASSERT_EQ(EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, ivkey.data() + 16, ivkey.data()),
              1);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    ASSERT_EQ(EVP_EncryptUpdate(ctx, encrypted.data(), &written, input.data(), input.size()), 1);
    Core::Crypto::AesCbcCfb128Decrypt(ivkey, encrypted, plain);
    EXPECT_EQ(plain, input);
    std::array<u8, 16> data{}, tweak{}, iv{};
    std::copy_n(ivkey.begin(), 16, data.begin());
    std::copy_n(ivkey.begin() + 16, 16, tweak.begin());
    iv[0] = 39;
    ASSERT_EQ(EVP_EncryptInit_ex(ctx, EVP_aes_128_xts(), nullptr, ivkey.data(), iv.data()), 1);
    ASSERT_EQ(EVP_EncryptUpdate(ctx, encrypted.data(), &written, input.data(), input.size()), 1);
    Core::Crypto::DecryptPFS(data, tweak, encrypted, plain, 39);
    EXPECT_EQ(plain, input);
    EVP_CIPHER_CTX_free(ctx);
}
} // namespace
