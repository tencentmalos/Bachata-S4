// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Host CLI to extract a PS4 .pkg into a flat game directory (eboot.bin +
// sce_sys/param.sfo + files), reusing the desktop shadPS4 PKG/Crypto path. Used to
// produce a real decrypted eboot for the HN2 guest-execution bring-up harness and
// for on-device acceptance staging. Not shipped in the APK (the app imports via its
// own JVM/SAF path); this is a developer/CI extraction tool.

#include <cstdio>
#include <filesystem>
#include <string>

#include "core/file_format/pkg.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input.pkg> <output-dir>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path pkg_path = argv[1];
    const std::filesystem::path out_dir = argv[2];

    PKG pkg;
    std::string fail;
    if (!pkg.Open(pkg_path, fail)) {
        std::fprintf(stderr, "PKG::Open failed: %s\n", fail.c_str());
        return 1;
    }
    std::fprintf(stderr, "title=%.*s flags=%s files=%u size=%llu\n",
                 9, pkg.GetTitleID().data(), pkg.GetPkgFlags().c_str(),
                 pkg.GetNumberOfFiles(),
                 static_cast<unsigned long long>(pkg.GetPkgSize()));

    std::filesystem::create_directories(out_dir);
    if (!pkg.Extract(pkg_path, out_dir, fail)) {
        std::fprintf(stderr, "PKG::Extract failed: %s\n", fail.c_str());
        return 1;
    }
    // Extract every file entry (Extract() only stages metadata; ExtractFiles pulls
    // each PFS entry to disk).
    const u32 n = pkg.GetNumberOfFiles();
    for (u32 i = 0; i < n; ++i) {
        pkg.ExtractFiles(static_cast<int>(i));
    }
    std::fprintf(stderr, "extracted %u files to %s\n", n, out_dir.c_str());
    return 0;
}
