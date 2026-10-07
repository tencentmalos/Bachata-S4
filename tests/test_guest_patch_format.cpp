// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sdk_version 3 of the guest function package format: builds named by located signatures and
// hooks on an import's PLT entry. The two ASTRO BOT packages (guest/games/CUSA12392) are built by
// tools/guest-functions/build.py; this test writes small packages of its own.

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "core/host_runtime/guest_patch_format.h"
#include "core/loader/plt_import.h"

using namespace Core::GuestPatch;
using Json = nlohmann::json;

namespace {

std::string Hex(std::span<const std::byte> bytes) {
    std::string out;
    for (const auto b : bytes) {
        out += "0123456789abcdef"[unsigned(b) >> 4];
        out += "0123456789abcdef"[unsigned(b) & 15];
    }
    return out;
}

// A minimal valid package: one RX page with an exported replacement and an import slot.
Json BasePackage() {
    std::array<std::byte, 64> code{};
    code.fill(std::byte{0xcc});
    const auto hex = Hex(code);
    return Json{
        {"schema", "shadps4.guest-functions.v1"},
        {"abi", "x86_64-sysv"},
        {"sdk_version", 3},
        {"id", "test_package"},
        {"title", "CUSA00000"},
        {"module", "eboot.bin"},
        {"segments",
         Json::array({{{"offset", 0},
                       {"size", 4096},
                       {"executable", true},
                       {"hex", hex},
                       {"sha256", Sha256(code)}}})},
        {"exports", {{"replacement", 0}}},
        {"rebase64", Json::array()},
        {"imports", Json::array({{{"name", "original"}, {"slot", 8}}})},
        {"hooks", Json::array({{{"name", "submit_done"},
                                {"import", "yvZ73uQUqrk"},
                                {"replacement", "replacement"},
                                {"original", "original"},
                                {"prototype", "int original(void)"},
                                {"evidence", "test"}}})},
        {"counters", Json::array()},
        {"module_signatures",
         Json::array({{{"offset", 0x100}, {"hex", std::string(64, 'a')}}})},
    };
}

Package LoadJson(const Json& j) {
    const auto path = std::filesystem::temp_directory_path() / "shadps4_guest_patch_format.json";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << j.dump();
    }
    return Package::Load(path);
}

} // namespace

TEST(GuestPatchFormat, SignaturesReplaceTheFileHash) {
    const auto p = LoadJson(BasePackage());
    EXPECT_TRUE(p.module_sha256.empty());
    ASSERT_EQ(p.signatures.size(), 1u);
    EXPECT_EQ(p.signatures[0].offset, 0x100u);
    EXPECT_EQ(p.signatures[0].bytes.size(), 32u);

    std::vector<std::byte> image(0x200, std::byte{0});
    std::fill(image.begin() + 0x100, image.begin() + 0x120, std::byte{0xaa});
    const auto read = [&](uint64_t offset, std::span<std::byte> out) {
        if (offset > image.size() || out.size() > image.size() - offset)
            return false;
        std::memcpy(out.data(), image.data() + offset, out.size());
        return true;
    };
    EXPECT_TRUE(SignaturesMatch(p, read));
    image[0x11f] = std::byte{0xab}; // another build
    EXPECT_FALSE(SignaturesMatch(p, read));
    image.resize(0x110); // signature outside the image
    EXPECT_FALSE(SignaturesMatch(p, read));
}

TEST(GuestPatchFormat, SignaturesMustBeLongEnoughAndNeedVersion3) {
    auto j = BasePackage();
    j["module_signatures"] = Json::array({{{"offset", 0}, {"hex", std::string(62, 'a')}}});
    EXPECT_THROW(LoadJson(j), std::exception); // 31 bytes
    j = BasePackage();
    j["sdk_version"] = 2;
    EXPECT_THROW(LoadJson(j), std::exception);
    j = BasePackage();
    j.erase("module_signatures");
    EXPECT_THROW(LoadJson(j), std::exception); // neither SHA nor signatures
}

TEST(GuestPatchFormat, ImportHooksResolveToThePltEntry) {
    auto p = LoadJson(BasePackage());
    ASSERT_EQ(p.hooks.size(), 1u);
    EXPECT_EQ(p.hooks[0].import_nid, "yvZ73uQUqrk");
    EXPECT_TRUE(p.hooks[0].expected.empty());
    const Bytes entry{std::byte{0xff}, std::byte{0x25}, std::byte{1}, std::byte{2},
                      std::byte{3},    std::byte{4}};
    ResolveImportHooks(p, [&](std::string_view nid) -> std::optional<std::pair<uint64_t, Bytes>> {
        EXPECT_EQ(nid, "yvZ73uQUqrk");
        return std::pair{uint64_t{0x1234}, entry};
    });
    EXPECT_EQ(p.hooks[0].offset, 0x1234u);
    EXPECT_EQ(p.hooks[0].expected, entry);

    auto missing = LoadJson(BasePackage());
    EXPECT_THROW(ResolveImportHooks(missing, [](std::string_view) { return std::nullopt; }),
                 std::exception);
}

TEST(GuestPatchFormat, ImportHooksRejectOffsetsAndSites) {
    auto j = BasePackage();
    j["hooks"][0]["offset"] = 16;
    EXPECT_THROW(LoadJson(j), std::exception);
    j = BasePackage();
    j["hooks"][0]["import"] = "short";
    EXPECT_THROW(LoadJson(j), std::exception);
    j = BasePackage();
    j["sdk_version"] = 2;
    j["module_sha256"] = std::string(64, '0');
    j.erase("module_signatures");
    EXPECT_THROW(LoadJson(j), std::exception); // import hook in version 2
}

TEST(PltImport, FindsTheEntryThroughAGotSlot) {
    // Three PLT rows at 0x1000: jmp [rip+disp] through 0x5000, 0x5008, 0x5010.
    std::array<std::uint8_t, 48> code{};
    code.fill(0xcc);
    for (int i = 0; i < 3; ++i) {
        const std::uint64_t pc = 0x1000 + 16 * i;
        const auto disp = static_cast<std::int32_t>(0x5000 + 8 * i - (pc + 6));
        code[16 * i] = 0xff;
        code[16 * i + 1] = 0x25;
        std::memcpy(&code[16 * i + 2], &disp, 4);
    }
    auto found = Core::Loader::FindPltEntries(code, 0x1000, 0x5008);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0], 16u);
    EXPECT_TRUE(Core::Loader::FindPltEntries(code, 0x1000, 0x5018).empty());
    code[16 + 9] = 0x90; // not an intact row
    EXPECT_TRUE(Core::Loader::FindPltEntries(code, 0x1000, 0x5008).empty());
}

// The packages for both known builds of ASTRO BOT, as built from the recipes.
TEST(GuestPatchFormat, AstroBotPackagesLoad) {
    for (const char* build : {"01.00", "01.04"}) {
        const auto path = std::filesystem::path(SHADPS4_SOURCE_DIR) / "build/guest-patches" /
                          (std::string("CUSA12392-") + build) / "patch.json";
        if (!std::filesystem::exists(path))
            GTEST_SKIP() << "build the package first: " << path.string();
        const auto p = Package::Load(path);
        EXPECT_EQ(p.title, "CUSA12392");
        EXPECT_TRUE(p.module_sha256.empty());
        EXPECT_EQ(p.signatures.size(), 6u);
        ASSERT_EQ(p.hooks.size(), 1u);
        EXPECT_EQ(p.hooks[0].import_nid, "yvZ73uQUqrk");
        EXPECT_EQ(p.bindings.size(), 3u);
    }
}
