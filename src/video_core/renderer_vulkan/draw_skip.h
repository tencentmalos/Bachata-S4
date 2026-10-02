// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "common/types.h"

namespace Vulkan::DrawSkip {

// DebugBus `draw_skip`: drops the draws whose vertex or fragment shader hash is listed, to find
// which draw writes a given rendering artifact. Empty by default; the check is one relaxed load.
inline std::atomic<u32> count{0};
inline std::mutex mutex;
inline std::vector<u64> hashes;
inline std::atomic<u64> skipped{0};

inline bool Matches(u64 vs_hash, u64 fs_hash) {
    if (count.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    std::scoped_lock lock{mutex};
    const bool match = std::ranges::any_of(
        hashes, [&](u64 hash) { return hash == vs_hash || hash == fs_hash; });
    if (match) {
        skipped.fetch_add(1, std::memory_order_relaxed);
    }
    return match;
}

inline std::string Command(const std::vector<std::string>& args) {
    constexpr size_t MaxHashes = 256;
    const std::string sub = args.empty() ? "status" : args[0];
    std::scoped_lock lock{mutex};
    if (sub == "clear" && args.size() == 1) {
        hashes.clear();
        skipped.store(0, std::memory_order_relaxed);
    } else if ((sub == "add" || sub == "remove") && args.size() >= 2) {
        for (size_t i = 1; i < args.size(); ++i) {
            u64 hash{};
            try {
                size_t used{};
                hash = std::stoull(args[i], &used, 0);
                if (used != args[i].size()) {
                    return "status=bad_arguments hash=" + args[i] + "\n";
                }
            } catch (const std::exception&) {
                return "status=bad_arguments hash=" + args[i] + "\n";
            }
            const auto it = std::ranges::find(hashes, hash);
            if (sub == "add" && it == hashes.end()) {
                if (hashes.size() == MaxHashes) {
                    return "status=full\n";
                }
                hashes.push_back(hash);
            } else if (sub == "remove" && it != hashes.end()) {
                hashes.erase(it);
            }
        }
    } else if (sub != "status" || args.size() != 1) {
        return "status=bad_arguments usage: status | add <hash>... | remove <hash>... | clear\n";
    }
    count.store(static_cast<u32>(hashes.size()), std::memory_order_relaxed);
    std::string out = fmt::format("draw_skip hashes={} skipped_draws={}\n", hashes.size(),
                                  skipped.load(std::memory_order_relaxed));
    for (const u64 hash : hashes) {
        out += fmt::format("  {:#x}\n", hash);
    }
    return out;
}

} // namespace Vulkan::DrawSkip
