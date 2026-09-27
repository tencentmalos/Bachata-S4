// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/profiler.h"
#include "video_core/renderer_vulkan/vk_missing_content.h"
#include "video_core/renderer_vulkan/vk_pipeline_stats.h"

namespace Vulkan::MissingContent {

namespace {

struct Range {
    VAddr begin;
    VAddr end;
    u64 origin_epoch; ///< Frame whose dropped draw the content is missing from.
    u64 last_epoch;   ///< Frame this range was last marked.
    bool propagated;  ///< Marked through reads, not only by dropped draws.
};

/// Marks beyond this stop being recorded, and skipping stops for the session.
constexpr std::size_t MaxRanges = 1024;

// Sorted by begin, disjoint. Written by the GPU command thread; the mutex is for the status.
std::mutex mutex;
std::vector<Range> ranges;

struct Action {
    u32 depth{};
    u64 epoch{};
    bool tainted{};
    u64 taint_origin{};
    VAddr taint_address{};
    struct Written {
        VAddr begin;
        VAddr end;
        bool whole;
    };
    std::vector<Written> writes;
};
Action action;

u64 current_epoch{}; ///< Frame of the latest action or dropped draw.

struct EscapeRecord {
    u64 count{};
    VAddr first_address{};
    u64 first_epoch{};
};
std::array<EscapeRecord, u32(Escape::Count)> escapes{};
u64 skip_marks{};
u64 propagated_marks{};
u64 cleared{};
u64 dropped_marks{}; ///< Marks the full record could not keep.

constexpr std::array EscapeNames = {"CPU readback", "indirect arguments", "a later frame"};

/// First range that ends after `begin`.
std::vector<Range>::iterator FirstEndingAfter(VAddr begin) {
    return std::ranges::upper_bound(ranges, begin, {}, &Range::end);
}

/// Earliest origin among marked ranges overlapping [begin, end), or ~0 when none do.
u64 OriginOf(VAddr begin, VAddr end) {
    u64 origin = ~0ULL;
    for (auto it = FirstEndingAfter(begin); it != ranges.end() && it->begin < end; ++it) {
        origin = std::min(origin, it->origin_epoch);
    }
    return origin;
}

bool Mark(VAddr begin, VAddr end, u64 origin, u64 epoch, bool propagated) {
    // Ranges that overlap or touch [begin, end) merge into one.
    auto first = std::ranges::lower_bound(ranges, begin, {}, &Range::end);
    auto last = first;
    Range merged{begin, end, origin, epoch, propagated};
    for (; last != ranges.end() && last->begin <= end; ++last) {
        merged.begin = std::min(merged.begin, last->begin);
        merged.end = std::max(merged.end, last->end);
        merged.origin_epoch = std::min(merged.origin_epoch, last->origin_epoch);
        merged.propagated |= last->propagated;
    }
    if (first == last && ranges.size() >= MaxRanges) {
        ++dropped_marks;
        return false;
    }
    const auto at = ranges.erase(first, last);
    ranges.insert(at, merged);
    detail::num_ranges.store(u32(ranges.size()), std::memory_order_relaxed);
    Common::Profiler::Counter("Pipeline.MissingRanges", s64(ranges.size()));
    return true;
}

void Unmark(VAddr begin, VAddr end) {
    auto it = FirstEndingAfter(begin);
    if (it == ranges.end() || it->begin >= end) {
        return;
    }
    // A range that sticks out on both sides splits in two; keep the mark if there is no room.
    if (it->begin < begin && it->end > end) {
        if (ranges.size() >= MaxRanges) {
            return;
        }
        Range tail = *it;
        tail.begin = end;
        it->end = begin;
        ranges.insert(it + 1, tail);
    } else {
        while (it != ranges.end() && it->begin < end) {
            if (it->begin < begin) {
                it->end = begin;
                ++it;
            } else if (it->end > end) {
                it->begin = end;
                ++it;
            } else {
                it = ranges.erase(it);
            }
        }
    }
    ++cleared;
    detail::num_ranges.store(u32(ranges.size()), std::memory_order_relaxed);
    Common::Profiler::Counter("Pipeline.MissingRanges", s64(ranges.size()));
}

bool RecordEscape(Escape kind, VAddr address, u64 epoch) {
    auto& record = escapes[u32(kind)];
    if (record.count++ == 0) {
        record.first_address = address;
        record.first_epoch = epoch;
    }
    if (record.count <= 8) {
        const auto it = FirstEndingAfter(address);
        if (it != ranges.end()) {
            LOG_INFO(Render_Vulkan,
                     "Missing content reached {} at {:#x} in frame {}: range {:#x}..{:#x} lost in "
                     "frame {}, last marked in frame {}{}",
                     EscapeNames[u32(kind)], address, epoch, it->begin, it->end, it->origin_epoch,
                     it->last_epoch, it->propagated ? ", carried by reads" : "");
        }
    }
    if (PipelineStats::EffectiveCompileMode() == PipelineStats::CompileMode::AsyncGraphicsSkip &&
        !PipelineStats::SkipDisabledForSession()) {
        LOG_WARNING(Render_Vulkan,
                    "Content missing from skipped draws reached {} at {:#x} (frame {}): draws "
                    "wait for their pipelines for the rest of the session",
                    EscapeNames[u32(kind)], address, epoch);
        PipelineStats::DisableSkipping(kind == Escape::Readback   ? PipelineStats::SkipOff::Readback
                                       : kind == Escape::Indirect ? PipelineStats::SkipOff::Indirect
                                                                  : PipelineStats::SkipOff::CrossFrame);
    }
    return true;
}

} // namespace

namespace detail {

std::atomic<u32> num_ranges{};
bool action_tracking{};

void Read(VAddr address, u64 size) {
    if (!size) {
        return;
    }
    std::scoped_lock lock{mutex};
    const u64 origin = OriginOf(address, address + size);
    if (origin == ~0ULL) {
        return;
    }
    if (!action.tainted || origin < action.taint_origin) {
        action.taint_origin = origin;
        action.taint_address = address;
    }
    action.tainted = true;
}

void Write(VAddr address, u64 size, bool whole) {
    if (size) {
        action.writes.push_back({address, address + size, whole});
    }
}

} // namespace detail

bool MarkSkipped(VAddr address, u64 size, u64 epoch) {
    if (!size) {
        return true;
    }
    std::scoped_lock lock{mutex};
    current_epoch = epoch;
    if (++skip_marks <= 32) {
        LOG_INFO(Render_Vulkan, "Missing content: {:#x}..{:#x} ({} KiB) not written in frame {}",
                 address, address + size, size / 1024, epoch);
    }
    return Mark(address, address + size, epoch, epoch, false);
}

void BeginAction(u64 epoch) {
    if (action.depth++ != 0) {
        return;
    }
    action.epoch = epoch;
    current_epoch = epoch;
    action.tainted = false;
    action.writes.clear();
    // Once skipping is off for the session nothing depends on the marks any more: they stay
    // as they were for the status, and actions stop paying for the lookups.
    detail::action_tracking = Tracking() && !PipelineStats::SkipDisabledForSession();
}

void EndAction() {
    if (--action.depth != 0) {
        return;
    }
    if (!detail::action_tracking) {
        return;
    }
    detail::action_tracking = false;
    std::unique_lock lock{mutex};
    if (!action.tainted) {
        for (const auto& write : action.writes) {
            if (write.whole) {
                Unmark(write.begin, write.end);
            }
        }
        return;
    }
    bool kept = true;
    for (const auto& write : action.writes) {
        kept &= Mark(write.begin, write.end, action.taint_origin, action.epoch, true);
        ++propagated_marks;
    }
    lock.unlock();
    if (!kept) {
        LOG_WARNING(Render_Vulkan, "Missing-content record is full; draws wait for their "
                                   "pipelines for the rest of the session");
        PipelineStats::DisableSkipping(PipelineStats::SkipOff::TableFull);
    }
    if (action.taint_origin < action.epoch) {
        // Content lost in an earlier frame is still being consumed: history or feedback.
        std::scoped_lock escape_lock{mutex};
        RecordEscape(Escape::CrossFrame, action.taint_address, action.epoch);
    }
}

void Overwrite(VAddr address, u64 size) {
    if (!size) {
        return;
    }
    if (action.depth != 0) {
        if (detail::action_tracking) {
            detail::Write(address, size, true);
        }
        return;
    }
    if (!Tracking() || PipelineStats::SkipDisabledForSession()) {
        return;
    }
    std::scoped_lock lock{mutex};
    Unmark(address, address + size);
}

bool CheckEscape(Escape kind, VAddr address, u64 size) {
    if (!Tracking() || !size) {
        return false;
    }
    std::scoped_lock lock{mutex};
    const auto it = FirstEndingAfter(address);
    if (it == ranges.end() || it->begin >= address + size) {
        return false;
    }
    return RecordEscape(kind, std::max(address, it->begin), current_epoch);
}

bool Overlaps(VAddr address, u64 size) {
    if (!Tracking() || !size) {
        return false;
    }
    std::scoped_lock lock{mutex};
    const auto it = FirstEndingAfter(address);
    return it != ranges.end() && it->begin < address + size;
}

void Reset() {
    std::scoped_lock lock{mutex};
    ranges.clear();
    detail::num_ranges = 0;
    escapes = {};
    current_epoch = 0;
    skip_marks = 0;
    propagated_marks = 0;
    cleared = 0;
    dropped_marks = 0;
}

void Describe(std::string& out) {
    std::scoped_lock lock{mutex};
    if (ranges.empty() && skip_marks == 0) {
        return;
    }
    u64 bytes = 0;
    for (const auto& range : ranges) {
        bytes += range.end - range.begin;
    }
    out += fmt::format("  missing content: {} ranges, {:.1f} MiB marked; {} marks from skipped "
                       "draws, {} carried by reads, {} cleared by whole overwrites{}{}\n",
                       ranges.size(), double(bytes) / (1024.0 * 1024.0), skip_marks,
                       propagated_marks, cleared,
                       dropped_marks ? fmt::format(", {} not recorded (full)", dropped_marks)
                                     : std::string{},
                       PipelineStats::SkipDisabledForSession()
                           ? "; no longer followed (skipping is off)"
                           : "");
    for (u32 i = 0; i < u32(Escape::Count); ++i) {
        const auto& record = escapes[i];
        if (record.count) {
            out += fmt::format("    reached {}: {} times, first at {:#x} in frame {}\n",
                               EscapeNames[i], record.count, record.first_address,
                               record.first_epoch);
        }
    }
    auto sorted = ranges;
    std::ranges::sort(sorted, std::greater{}, [](const Range& r) { return r.end - r.begin; });
    for (std::size_t i = 0; i < std::min<std::size_t>(sorted.size(), 16); ++i) {
        const auto& r = sorted[i];
        out += fmt::format("    {:#x}..{:#x} ({} KiB): lost in frame {}, last marked in frame {}"
                           "{}\n",
                           r.begin, r.end, (r.end - r.begin) / 1024, r.origin_epoch, r.last_epoch,
                           r.propagated ? ", carried by reads" : "");
    }
}

} // namespace Vulkan::MissingContent
