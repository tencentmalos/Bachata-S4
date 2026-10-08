// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/types.h"

namespace Core::FileFormat {

// Read-only, seekable FPKG filesystem. Opening reads the directory/index metadata;
// file payload is decrypted and inflated only when requested. All methods may
// throw on malformed/truncated input. No extracted files or keys are written.
class PkgReader {
public:
    struct Entry {
        std::string path;
        u64 size{};
        bool directory{};
    };
    struct Statistics {
        u64 source_bytes{};
        u64 inflated_blocks{};
        u64 cache_hits{};
    };

    static std::shared_ptr<PkgReader> Open(const std::filesystem::path& path);
    ~PkgReader();
    const std::vector<Entry>& Entries() const;
    size_t Find(std::string_view path) const;
    size_t Read(size_t entry, u64 offset, std::span<u8> output);
    Statistics GetStatistics() const;
    static constexpr size_t Missing = ~size_t{0};

private:
    struct Impl;
    explicit PkgReader(const std::filesystem::path& path);
    std::unique_ptr<Impl> impl;
};

} // namespace Core::FileFormat
