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
    // Keep a session-owned /proc/self/fd alias inside its overlay directory.
    // Resolving it to the provider's private pathname loses the granted capability.
    static std::filesystem::path ResolvePath(const std::filesystem::path& path);
    // A separate, uncached reader that decrypts one 4 KiB sector at a time with
    // LibreSSL and serializes reads: the original behavior, for comparisons in tests.
    static std::shared_ptr<PkgReader> OpenReference(const std::filesystem::path& path);
    ~PkgReader();
    const std::vector<Entry>& Entries() const;
    size_t Find(std::string_view path) const;
    size_t Read(size_t entry, u64 offset, std::span<u8> output);
    Statistics GetStatistics() const;
    static constexpr size_t Missing = ~size_t{0};

private:
    struct Impl;
    PkgReader(const std::filesystem::path& path, bool reference = false);
    std::unique_ptr<Impl> impl;
};

} // namespace Core::FileFormat
