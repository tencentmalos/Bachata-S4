// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <fmt/format.h>
#include <xxhash.h>
#include "shader_recompiler/srt_gate_diag.h"

namespace Shader::SrtGateDiag {
namespace {

constexpr u64 PageBits = 12;

struct Key {
    const void* info;
    u64 user_data_hash;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    size_t operator()(const Key& key) const noexcept {
        return std::hash<const void*>{}(key.info) ^ (key.user_data_hash * 0x9e3779b97f4a7c15ull);
    }
};
struct Entry {
    std::vector<u32> user_data;
    u64 output_hash{};
    u64 walk_seq{};
    std::vector<u64> pages;
};

struct State {
    std::mutex mutex;
    u64 seq{};
    std::unordered_map<u64, u64> page_write_seq;
    std::unordered_map<Key, Entry, KeyHash> cache;
    std::unordered_map<const void*, u64> last_user_data; // per permutation: hash of last walk
    std::unordered_set<u64> frame_pages;
    std::unordered_set<u64> frame_written;

    u64 walks{};
    u64 same_as_last{};   // user data equal to this permutation's previous walk
    u64 seen_before{};    // (permutation, user data) walked earlier
    u64 could_skip{};     // seen before and none of its pages written since
    u64 wrong_skips{};    // could skip but the real output differs
    u64 pages_written{};  // seen before but a page it read was written since
    u64 output_changed{}; // seen before, a page written, and the output really changed
    u64 read_ranges{};
    u64 frames{};
    u64 frame_pages_total{};
    u64 frame_pages_written_total{};
};

State& Get() {
    static State state;
    return state;
}

} // namespace

void NoteWalk(const void* info, std::span<const u32> user_data, std::span<const u32> output,
              std::span<const Read> reads) {
    auto& s = Get();
    std::scoped_lock lock{s.mutex};
    const u64 seq = ++s.seq;
    ++s.walks;
    s.read_ranges += reads.size();
    const u64 ud_hash = XXH3_64bits(user_data.data(), user_data.size_bytes());
    const u64 out_hash = XXH3_64bits(output.data(), output.size_bytes());
    if (auto [it, inserted] = s.last_user_data.try_emplace(info, ud_hash); !inserted) {
        s.same_as_last += it->second == ud_hash;
        it->second = ud_hash;
    }
    std::vector<u64> pages;
    for (const auto& read : reads) {
        if (!read.size)
            continue;
        for (u64 page = read.address >> PageBits; page <= (read.address + read.size - 1) >> PageBits;
             ++page) {
            pages.push_back(page);
            s.frame_pages.insert(page);
        }
    }
    std::ranges::sort(pages);
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());

    if (s.cache.size() > (1u << 20))
        s.cache.clear();
    auto& entry = s.cache[Key{info, ud_hash}];
    const bool seen = entry.walk_seq != 0 &&
                      std::ranges::equal(entry.user_data, user_data);
    if (seen) {
        ++s.seen_before;
        const bool written = std::ranges::any_of(entry.pages, [&](u64 page) {
            const auto it = s.page_write_seq.find(page);
            return it != s.page_write_seq.end() && it->second > entry.walk_seq;
        });
        if (!written) {
            ++s.could_skip;
            s.wrong_skips += entry.output_hash != out_hash;
        } else {
            ++s.pages_written;
            s.output_changed += entry.output_hash != out_hash;
        }
    }
    entry.user_data.assign(user_data.begin(), user_data.end());
    entry.output_hash = out_hash;
    entry.walk_seq = seq;
    entry.pages = std::move(pages);
}

void NoteWrite(u64 page_address) {
    if (!enabled.load(std::memory_order_relaxed))
        return;
    auto& s = Get();
    std::scoped_lock lock{s.mutex};
    const u64 page = page_address >> PageBits;
    s.page_write_seq[page] = ++s.seq;
    s.frame_written.insert(page);
}

void NoteFlip() {
    if (!enabled.load(std::memory_order_relaxed))
        return;
    auto& s = Get();
    std::scoped_lock lock{s.mutex};
    ++s.frames;
    s.frame_pages_total += s.frame_pages.size();
    for (const u64 page : s.frame_pages)
        s.frame_pages_written_total += s.frame_written.contains(page);
    s.frame_pages.clear();
    s.frame_written.clear();
}

void Reset() {
    auto& s = Get();
    std::scoped_lock lock{s.mutex};
    s.seq = 0;
    s.page_write_seq.clear();
    s.cache.clear();
    s.last_user_data.clear();
    s.frame_pages.clear();
    s.frame_written.clear();
    s.walks = s.same_as_last = s.seen_before = s.could_skip = s.wrong_skips = 0;
    s.pages_written = s.output_changed = s.read_ranges = 0;
    s.frames = s.frame_pages_total = s.frame_pages_written_total = 0;
}

std::string Status() {
    auto& s = Get();
    std::scoped_lock lock{s.mutex};
    return fmt::format(
        "srt_gate enabled={} walks={} read_ranges={} same_as_last={} seen_before={} "
        "could_skip={} wrong_skips={} pages_written={} output_changed={} frames={} "
        "frame_pages={} frame_pages_written={} cache_entries={}\n",
        enabled.load(), s.walks, s.read_ranges, s.same_as_last, s.seen_before, s.could_skip,
        s.wrong_skips, s.pages_written, s.output_changed, s.frames, s.frame_pages_total,
        s.frame_pages_written_total, s.cache.size());
}

} // namespace Shader::SrtGateDiag
