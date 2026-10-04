// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <unordered_map>
#include <fmt/format.h>
#include <xxhash.h>

#include "core/address_space.h"
#include "core/memory.h"
#include "video_core/buffer_cache/page_heat.h"
#include "video_core/buffer_cache/region_definitions.h"

namespace VideoCore::PageHeat {
namespace {

struct Page {
    u32 upload_frames{};
    u32 uploads{};
    // Uploads whose page contents differ from the previous upload of the page.
    u32 changed{};
    u32 spans{};
    u64 last_upload_frame{~0ULL};
    u64 hash{};
    bool hashed{};
};

struct Counters {
    u64 watch_calls{}, watch_pages{}, release_calls{}, release_pages{}, syscalls{}, predicted{};
    u64 hashed{}, rewritten{};
};

Counters ReadCounters() {
    const auto& c = Core::gpu_watch_counters;
    const auto& p = content_prediction_counters;
    constexpr auto o = std::memory_order_relaxed;
    return {c.watch_calls.load(o),   c.watch_pages.load(o),    c.release_calls.load(o),
            c.release_pages.load(o), c.syscalls.load(o),       c.predicted_pages.load(o),
            p.hashed_pages.load(o),  p.rewritten_pages.load(o)};
}

std::mutex mutex;
std::unordered_map<u64, Page> pages;
u64 first_frame{~0ULL};
u64 last_frame{};
u64 resident_calls{};
u64 resident_pages{};
Counters start{};

void Reset() {
    pages.clear();
    first_frame = ~0ULL;
    last_frame = 0;
    resident_calls = 0;
    resident_pages = 0;
    start = ReadCounters();
}

void NoteFrame(u64 frame) {
    first_frame = std::min(first_frame, frame);
    last_frame = std::max(last_frame, frame);
}

} // namespace

void NoteUploads(std::span<const vk::BufferCopy> copies, VAddr arena_base, u64 frame) {
    std::array<u8, TRACKER_BYTES_PER_PAGE> contents{};
    auto* memory = Core::Memory::Instance();
    std::scoped_lock lock{mutex};
    NoteFrame(frame);
    for (const auto& copy : copies) {
        const VAddr begin = arena_base + copy.dstOffset;
        const VAddr end = begin + copy.size;
        for (u64 page = begin >> TRACKER_PAGE_BITS; page <= (end - 1) >> TRACKER_PAGE_BITS;
             ++page) {
            Page& entry = pages[page];
            ++entry.uploads;
            if (entry.last_upload_frame != frame) {
                entry.last_upload_frame = frame;
                ++entry.upload_frames;
            }
            if (memory->TryCopySparseMemory(page << TRACKER_PAGE_BITS, contents.data(),
                                            contents.size())) {
                const u64 hash = XXH3_64bits(contents.data(), contents.size());
                entry.changed += !entry.hashed || entry.hash != hash;
                entry.hash = hash;
                entry.hashed = true;
            }
        }
    }
}

void NoteResident(VAddr address, u64 size, u64 frame) {
    if (size == 0) {
        return;
    }
    std::scoped_lock lock{mutex};
    NoteFrame(frame);
    ++resident_calls;
    const u64 first = address >> TRACKER_PAGE_BITS;
    const u64 last = (address + size - 1) >> TRACKER_PAGE_BITS;
    resident_pages += last - first + 1;
    for (u64 page = first; page <= last; ++page) {
        // Only pages that were uploaded at least once matter for the hot set.
        if (const auto it = pages.find(page); it != pages.end()) {
            ++it->second.spans;
        }
    }
}

std::string Command(const std::vector<std::string>& args) {
    const std::string sub = args.empty() ? "status" : args[0];
    if (sub == "on" || sub == "reset") {
        std::scoped_lock lock{mutex};
        Reset();
        enabled.store(true, std::memory_order_relaxed);
        return "page_heat: on\n";
    }
    if (sub == "off") {
        enabled.store(false, std::memory_order_relaxed);
        return "page_heat: off\n";
    }
    if (sub != "status") {
        return "status: invalid_arguments\n";
    }
    std::scoped_lock lock{mutex};
    const Counters now = ReadCounters();
    const u64 frames = first_frame == ~0ULL ? 0 : last_frame - first_frame + 1;
    std::string out = fmt::format("page_heat: {} frames={} (flips {}..{})\n",
                                  enabled.load(std::memory_order_relaxed) ? "on" : "off", frames,
                                  first_frame == ~0ULL ? 0 : first_frame, last_frame);
    if (frames == 0) {
        return out;
    }
    const double per = 1.0 / static_cast<double>(frames);
    out += fmt::format(
        "watch per frame: release_calls={:.1f} release_pages={:.1f} predicted_pages={:.1f} "
        "watch_calls={:.1f} watch_pages={:.1f} mprotect={:.1f}\n",
        (now.release_calls - start.release_calls) * per,
        (now.release_pages - start.release_pages) * per,
        (now.predicted - start.predicted) * per, (now.watch_calls - start.watch_calls) * per,
        (now.watch_pages - start.watch_pages) * per, (now.syscalls - start.syscalls) * per);
    out += fmt::format("content prediction per frame: {} hashed={:.1f} rewritten={:.1f}\n",
                       predict_from_contents.load(std::memory_order_relaxed) ? "on" : "off",
                       (now.hashed - start.hashed) * per, (now.rewritten - start.rewritten) * per);

    struct Bucket {
        const char* name;
        double low;
        u64 pages{}, uploads{}, changed{}, spans{};
    };
    std::array<Bucket, 4> buckets{{{">=90%", 0.9}, {"50-90%", 0.5}, {"10-50%", 0.1}, {"<10%", 0.0}}};
    u64 uploads{}, changed{};
    // 64 KiB groups of pages uploaded in at least 90% of frames.
    std::map<u64, std::pair<u64, u64>> hot_groups;
    for (const auto& [page, entry] : pages) {
        uploads += entry.uploads;
        changed += entry.changed;
        const double share = static_cast<double>(entry.upload_frames) * per;
        for (Bucket& bucket : buckets) {
            if (share >= bucket.low) {
                ++bucket.pages;
                bucket.uploads += entry.uploads;
                bucket.changed += entry.changed;
                bucket.spans += entry.spans;
                break;
            }
        }
        if (share >= 0.9) {
            auto& group = hot_groups[page >> 4];
            ++group.first;
            group.second += entry.uploads;
        }
    }
    out += fmt::format("uploads per frame: pages={:.1f} changed={:.1f} distinct_pages={}\n",
                       uploads * per, changed * per, pages.size());
    out += fmt::format("resident lookups per frame: calls={:.1f} pages={:.1f}\n",
                       resident_calls * per, resident_pages * per);
    out += "share of frames with an upload: pages, uploads/frame, changed/frame, "
           "lookups covering them/frame\n";
    for (const Bucket& bucket : buckets) {
        out += fmt::format("  {:>7}: {} pages, {:.1f}, {:.1f}, {:.1f}\n", bucket.name,
                           bucket.pages, bucket.uploads * per, bucket.changed * per,
                           bucket.spans * per);
    }
    std::vector<std::pair<u64, std::pair<u64, u64>>> groups(hot_groups.begin(), hot_groups.end());
    std::sort(groups.begin(), groups.end(),
              [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    out += "hottest 64 KiB groups (>=90%): address pages uploads/frame\n";
    for (size_t i = 0; i < std::min<size_t>(groups.size(), 16); ++i) {
        out += fmt::format("  {:#x} {} {:.1f}\n", groups[i].first << (TRACKER_PAGE_BITS + 4),
                           groups[i].second.first, groups[i].second.second * per);
    }
    return out;
}

} // namespace VideoCore::PageHeat
