// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Covers the content-root helpers that let additional content (DLC) live in a
// .zar as well as a plain directory. The interesting property is that both
// shapes must enumerate and read identically, since sceAppContentInitialize and
// sceAppContentAddcontMount walk the same list and have to agree on ordering.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <zarchive/zarchivewriter.h>

#include "core/file_sys/fs.h"
#include "core/file_sys/ifile.h"

namespace fs = std::filesystem;

namespace {

class ContentRootTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() /
               ("shadps4_content_root_" + std::to_string(::testing::UnitTest::GetInstance()
                                                             ->current_test_info()
                                                             ->line()));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static void WriteFile(const fs::path& path, std::string_view contents) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    /// Build a .zar whose root is `src_dir`'s contents, as the emulator expects.
    static bool PackZar(const fs::path& src_dir, const fs::path& out_zar) {
        struct Ctx {
            std::ofstream out;
            bool failed{false};
        } ctx;
        ctx.out.open(out_zar, std::ios::binary | std::ios::trunc);
        if (!ctx.out.is_open()) {
            return false;
        }

        ZArchiveWriter writer{
            // Single-part output: the archive never splits in these tests.
            [](const int32_t, void*) {},
            [](const void* data, size_t length, void* userdata) {
                auto* c = static_cast<Ctx*>(userdata);
                c->out.write(static_cast<const char*>(data), static_cast<std::streamsize>(length));
                if (!c->out) {
                    c->failed = true;
                }
            },
            &ctx};

        std::vector<char> buffer(64 * 1024);
        for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
            const auto rel = fs::relative(entry.path(), src_dir).generic_string();
            if (entry.is_directory()) {
                writer.MakeDir(rel.c_str(), true);
                continue;
            }
            if (!writer.StartNewFile(rel.c_str())) {
                return false;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            while (in) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto got = in.gcount();
                if (got > 0) {
                    writer.AppendData(buffer.data(), static_cast<size_t>(got));
                }
            }
        }
        writer.Finalize();
        ctx.out.close();
        return !ctx.failed && fs::exists(out_zar);
    }

    static std::string AsString(const std::vector<u8>& data) {
        return std::string(reinterpret_cast<const char*>(data.data()), data.size());
    }

    fs::path root;
};

TEST_F(ContentRootTest, ListsDirectoriesAndArchives) {
    fs::create_directories(root / "plain_dlc" / "sce_sys");
    WriteFile(root / "plain_dlc" / "sce_sys" / "param.sfo", "dir-sfo");

    const auto staging = root / "_staging";
    WriteFile(staging / "sce_sys" / "param.sfo", "zar-sfo");
    ASSERT_TRUE(PackZar(staging, root / "packed_dlc.zar"));
    fs::remove_all(staging);

    // A file that is not an archive must be ignored entirely.
    WriteFile(root / "notes.txt", "ignore me");

    const auto roots = Core::FileSys::ListContentRoots(root);
    ASSERT_EQ(roots.size(), 2u);

    std::vector<std::string> names;
    for (const auto& r : roots) {
        names.push_back(r.filename().string());
    }
    EXPECT_TRUE(std::find(names.begin(), names.end(), "plain_dlc") != names.end());
    EXPECT_TRUE(std::find(names.begin(), names.end(), "packed_dlc.zar") != names.end());
}

TEST_F(ContentRootTest, EnumerationOrderIsStable) {
    // Mount point indices are assigned by enumeration order, so it must not
    // depend on the host filesystem's directory ordering.
    for (const auto* name : {"c_dlc", "a_dlc", "b_dlc"}) {
        WriteFile(root / name / "sce_sys" / "param.sfo", name);
    }

    const auto first = Core::FileSys::ListContentRoots(root);
    const auto second = Core::FileSys::ListContentRoots(root);
    ASSERT_EQ(first.size(), 3u);
    EXPECT_EQ(first, second);
    EXPECT_TRUE(std::is_sorted(first.begin(), first.end()));
}

TEST_F(ContentRootTest, ReadsParamSfoFromDirectory) {
    WriteFile(root / "dlc" / "sce_sys" / "param.sfo", "hello-from-dir");

    const auto data = Core::FileSys::ReadGameFile(root / "dlc", "sce_sys/param.sfo");
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(AsString(*data), "hello-from-dir");
}

TEST_F(ContentRootTest, ReadsParamSfoFromArchive) {
    const auto staging = root / "_staging";
    WriteFile(staging / "sce_sys" / "param.sfo", "hello-from-zar");
    const auto zar = root / "dlc.zar";
    ASSERT_TRUE(PackZar(staging, zar));

    const auto data = Core::FileSys::ReadGameFile(zar, "sce_sys/param.sfo");
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(AsString(*data), "hello-from-zar");
}

TEST_F(ContentRootTest, MissingFileReportsNullopt) {
    WriteFile(root / "dlc" / "sce_sys" / "other.dat", "x");
    EXPECT_FALSE(
        Core::FileSys::ReadGameFile(root / "dlc", "sce_sys/param.sfo").has_value());

    const auto staging = root / "_staging";
    WriteFile(staging / "sce_sys" / "other.dat", "x");
    const auto zar = root / "dlc.zar";
    ASSERT_TRUE(PackZar(staging, zar));
    EXPECT_FALSE(Core::FileSys::ReadGameFile(zar, "sce_sys/param.sfo").has_value());
}

TEST_F(ContentRootTest, NonArchiveFileIsNotAContentRoot) {
    WriteFile(root / "bogus.zar", "this is not a real archive");
    EXPECT_FALSE(
        Core::FileSys::ReadGameFile(root / "bogus.zar", "sce_sys/param.sfo").has_value());
}

TEST_F(ContentRootTest, MissingParentYieldsEmptyList) {
    EXPECT_TRUE(Core::FileSys::ListContentRoots(root / "does_not_exist").empty());
}

// ── bundle archives ──────────────────────────────────────────────────
// One .zar holding many pieces of content, each in its own top-level directory.
// This is how a title's whole DLC set ships as a single file rather than
// hundreds of small archives.

TEST_F(ContentRootTest, BundleArchiveExpandsToOneRootPerEntry) {
    const auto staging = root / "_staging";
    WriteFile(staging / "P1S1" / "sce_sys" / "param.sfo", "first");
    WriteFile(staging / "P1S2" / "sce_sys" / "param.sfo", "second");
    WriteFile(staging / "P1S3" / "sce_sys" / "param.sfo", "third");
    const auto bundle = root / "addcont.zar";
    ASSERT_TRUE(PackZar(staging, bundle));
    fs::remove_all(staging);

    const auto roots = Core::FileSys::ListContentRoots(root);
    ASSERT_EQ(roots.size(), 3u);
    for (const auto& r : roots) {
        // Each root points inside the one archive.
        EXPECT_EQ(r.parent_path(), bundle);
    }
    EXPECT_TRUE(std::is_sorted(roots.begin(), roots.end()));
}

TEST_F(ContentRootTest, ReadsThroughBundleSubPath) {
    const auto staging = root / "_staging";
    WriteFile(staging / "P1S1" / "sce_sys" / "param.sfo", "first-sfo");
    WriteFile(staging / "P1S2" / "sce_sys" / "param.sfo", "second-sfo");
    const auto bundle = root / "addcont.zar";
    ASSERT_TRUE(PackZar(staging, bundle));

    const auto first = Core::FileSys::ReadGameFile(bundle / "P1S1", "sce_sys/param.sfo");
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(AsString(*first), "first-sfo");

    const auto second = Core::FileSys::ReadGameFile(bundle / "P1S2", "sce_sys/param.sfo");
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(AsString(*second), "second-sfo");

    // A sibling's file must not be reachable from the wrong sub-path.
    EXPECT_FALSE(
        Core::FileSys::ReadGameFile(bundle / "P1S1", "P1S2/sce_sys/param.sfo").has_value());
    EXPECT_FALSE(Core::FileSys::ReadGameFile(bundle / "nope", "sce_sys/param.sfo").has_value());
}

TEST_F(ContentRootTest, SingleContentArchiveIsNotTreatedAsBundle) {
    // sce_sys at the archive root means the archive *is* the content, so it must
    // be listed once rather than expanded into its subdirectories.
    const auto staging = root / "_staging";
    WriteFile(staging / "sce_sys" / "param.sfo", "single");
    WriteFile(staging / "data" / "payload.bin", "x");
    const auto single = root / "one_dlc.zar";
    ASSERT_TRUE(PackZar(staging, single));
    fs::remove_all(staging);

    const auto roots = Core::FileSys::ListContentRoots(root);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots.front(), single);

    const auto data = Core::FileSys::ReadGameFile(single, "sce_sys/param.sfo");
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(AsString(*data), "single");
}

TEST_F(ContentRootTest, SplitArchivePathSeparatesArchiveFromInnerPath) {
    const auto staging = root / "_staging";
    WriteFile(staging / "P1S1" / "sce_sys" / "param.sfo", "x");
    const auto bundle = root / "addcont.zar";
    ASSERT_TRUE(PackZar(staging, bundle));

    const auto split = Core::FileSys::SplitArchivePath(bundle / "P1S1" / "sce_sys");
    ASSERT_TRUE(split.has_value());
    EXPECT_EQ(split->archive, bundle);
    EXPECT_EQ(split->inner, "P1S1/sce_sys");

    // The archive itself splits with an empty inner path.
    const auto whole = Core::FileSys::SplitArchivePath(bundle);
    ASSERT_TRUE(whole.has_value());
    EXPECT_TRUE(whole->inner.empty());

    // A plain directory is not an archive path at all.
    EXPECT_FALSE(Core::FileSys::SplitArchivePath(root / "plain" / "sce_sys").has_value());
}

// ── sibling suffixes ─────────────────────────────────────────────────
// A title can ship as CUSA12878.zar + CUSA12878-UPD.zar + CUSA12878-DLC.zar
// in one directory, so the suffixes have to round-trip.

TEST_F(ContentRootTest, ShortUpdateSuffixResolvesToBaseGame) {
    for (const auto* name : {"CUSA12878-UPD", "CUSA12878-UPDATE", "CUSA12878-patch"}) {
        const auto base = Core::FileSys::BaseGameFromOverlay(root / name);
        ASSERT_TRUE(base.has_value()) << name;
        EXPECT_EQ(base->filename(), "CUSA12878") << name;
    }
    // Also with the archive extension attached.
    const auto from_zar = Core::FileSys::BaseGameFromOverlay(root / "CUSA12878-UPD.zar");
    ASSERT_TRUE(from_zar.has_value());
    EXPECT_EQ(from_zar->filename(), "CUSA12878");
}

TEST_F(ContentRootTest, OverlayPathAppendsAfterStrippingExtension) {
    const auto game = root / "CUSA12878.zar";
    EXPECT_EQ(Core::FileSys::OverlayPath(game, "-UPD").filename(), "CUSA12878-UPD");
    EXPECT_EQ(Core::FileSys::OverlayPath(game, Core::FileSys::DlcSuffix).filename(),
              "CUSA12878-DLC");
    // A directory-backed game has no extension to strip.
    EXPECT_EQ(Core::FileSys::OverlayPath(root / "CUSA12878", "-UPD").filename(), "CUSA12878-UPD");
}

TEST_F(ContentRootTest, DlcSiblingArchiveExpandsLikeAnAddcontBundle) {
    // CUSA12878-DLC.zar next to the game, holding one directory per package.
    const auto staging = root / "_staging";
    WriteFile(staging / "P1S1" / "sce_sys" / "param.sfo", "first");
    WriteFile(staging / "P1S2" / "sce_sys" / "param.sfo", "second");
    const auto sibling = root / "CUSA12878-DLC.zar";
    ASSERT_TRUE(PackZar(staging, sibling));
    fs::remove_all(staging);

    // Resolve the way app_content does: game path -> -DLC sibling -> roots.
    const auto resolved =
        Core::FileSys::ResolveGameRoot(Core::FileSys::OverlayPath(root / "CUSA12878.zar", "-DLC"));
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(*resolved, sibling);

    const auto roots = Core::FileSys::ExpandBundleRoots(*resolved);
    ASSERT_EQ(roots.size(), 2u);
    const auto data = Core::FileSys::ReadGameFile(roots.front(), "sce_sys/param.sfo");
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(AsString(*data), "first");
}

// ── all-in-one archives ──────────────────────────────────────────────
// One .zar holding a whole title: app/ + update/ + dlc/. Recognised by the
// absence of sce_sys at the root, so it never collides with a plain game
// archive.

TEST_F(ContentRootTest, DetectsAllInOneArchiveByAppDirectory) {
    const auto staging = root / "_staging";
    WriteFile(staging / "app" / "eboot.bin", "elf");
    WriteFile(staging / "app" / "sce_sys" / "param.sfo", "base");
    WriteFile(staging / "update" / "sce_sys" / "param.sfo", "upd");
    WriteFile(staging / "dlc" / "P1S1" / "sce_sys" / "param.sfo", "dlc1");
    const auto title = root / "CUSA12878.zar";
    ASSERT_TRUE(PackZar(staging, title));
    fs::remove_all(staging);

    EXPECT_TRUE(Core::FileSys::IsAllInOneArchive(title));

    // The game, its update and its DLC all read through the one archive.
    const auto base = Core::FileSys::ReadGameFile(title / "app", "sce_sys/param.sfo");
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(AsString(*base), "base");

    const auto upd = Core::FileSys::ReadGameFile(title / "update", "sce_sys/param.sfo");
    ASSERT_TRUE(upd.has_value());
    EXPECT_EQ(AsString(*upd), "upd");

    const auto dlc_roots = Core::FileSys::ListContentRoots(title / "dlc");
    ASSERT_EQ(dlc_roots.size(), 1u);
    const auto dlc = Core::FileSys::ReadGameFile(dlc_roots.front(), "sce_sys/param.sfo");
    ASSERT_TRUE(dlc.has_value());
    EXPECT_EQ(AsString(*dlc), "dlc1");
}

TEST_F(ContentRootTest, PlainGameArchiveIsNotAllInOne) {
    const auto staging = root / "_staging";
    WriteFile(staging / "eboot.bin", "elf");
    WriteFile(staging / "sce_sys" / "param.sfo", "base");
    const auto title = root / "CUSA12878.zar";
    ASSERT_TRUE(PackZar(staging, title));

    EXPECT_FALSE(Core::FileSys::IsAllInOneArchive(title));
}

TEST_F(ContentRootTest, DlcBundleIsNotMistakenForAllInOne) {
    // A bundle has no sce_sys at the root either, but also no "app".
    const auto staging = root / "_staging";
    WriteFile(staging / "P1S1" / "sce_sys" / "param.sfo", "x");
    const auto bundle = root / "CUSA12878-DLC.zar";
    ASSERT_TRUE(PackZar(staging, bundle));

    EXPECT_FALSE(Core::FileSys::IsAllInOneArchive(bundle));
}

TEST_F(ContentRootTest, SubPathMountRejectsMissingDirectory) {
    // A sub-path that does not exist must fail to open rather than silently
    // producing a backend where every lookup misses.
    const auto staging = root / "_staging";
    WriteFile(staging / "app" / "sce_sys" / "param.sfo", "base");
    const auto title = root / "CUSA12878.zar";
    ASSERT_TRUE(PackZar(staging, title));

    EXPECT_FALSE(Core::FileSys::ReadGameFile(title / "update", "sce_sys/param.sfo").has_value());
    EXPECT_TRUE(Core::FileSys::ListContentRoots(title / "dlc").empty());
}

} // namespace
