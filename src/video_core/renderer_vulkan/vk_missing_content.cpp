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

/// A range last marked this many frames before it is read is history or feedback (temporal
/// filters, ping-pong targets): the frame that reads it draws complete content over it, so the
/// loss fades out by itself. Content marked longer ago is stale: nothing has drawn it since.
constexpr u64 HistoryFrames = 4;

struct Action {
    u32 depth{};
    u64 epoch{};
    bool tainted{};
    u64 taint_origin{};
    VAddr taint_address{};
    bool stale{};             ///< Read content marked more than HistoryFrames frames ago.
    VAddr stale_address{};
    std::vector<std::pair<VAddr, VAddr>> history; ///< Marked ranges read as history.
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
u64 pause_clears{};  ///< Times the marks were dropped when skipping paused.
u64 history_reads{}; ///< Marked ranges read in a later frame as history and cleared.

constexpr std::array EscapeNames = {"CPU readback", "indirect arguments",
                                     "a later frame, stale"};

/// First range that ends after `begin`.
std::vector<Range>::iterator FirstEndingAfter(VAddr begin) {
    return std::ranges::upper_bound(ranges, begin, {}, &Range::end);
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

/// `overwrite`: the range was replaced as a whole (counted in the status).
void Unmark(VAddr begin, VAddr end, bool overwrite = true) {
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
    if (overwrite) {
        ++cleared;
    }
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
    if (PipelineStats::EffectiveCompileMode() != PipelineStats::CompileMode::AsyncGraphicsSkip ||
        PipelineStats::SkippingOff(epoch)) {
        return true;
    }
    if (kind == Escape::CrossFrame && PipelineStats::PauseSkipping(epoch)) {
        // Nothing redraws stale content, so what was lost stays lost; stop losing more for a
        // while, then start over with nothing marked (marks only decide when to stop dropping
        // draws).
        LOG_INFO(Render_Vulkan,
                 "Stale content missing from skipped draws was read at {:#x} (frame {}): draws "
                 "wait for their pipelines for {} frames",
                 address, epoch, PipelineStats::SkipPauseFrames);
        ++pause_clears;
        ranges.clear();
        detail::num_ranges.store(0, std::memory_order_relaxed);
        Common::Profiler::Counter("Pipeline.MissingRanges", 0);
        return true;
    }
    LOG_WARNING(Render_Vulkan,
                "Content missing from skipped draws reached {} at {:#x} (frame {}): draws "
                "wait for their pipelines for the rest of the session",
                EscapeNames[u32(kind)], address, epoch);
    PipelineStats::DisableSkipping(kind == Escape::Readback   ? PipelineStats::SkipOff::Readback
                                   : kind == Escape::Indirect ? PipelineStats::SkipOff::Indirect
                                                              : PipelineStats::SkipOff::CrossFrame);
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
    const VAddr end = address + size;
    for (auto it = FirstEndingAfter(address); it != ranges.end() && it->begin < end; ++it) {
        const VAddr at = std::max(address, it->begin);
        if (it->last_epoch < action.epoch && action.epoch - it->last_epoch <= HistoryFrames) {
            action.history.emplace_back(at, std::min(end, it->end));
            continue;
        }
        if (it->last_epoch < action.epoch && !action.stale) {
            action.stale = true;
            action.stale_address = at;
        }
        if (!action.tainted || it->origin_epoch < action.taint_origin) {
            action.taint_origin = it->origin_epoch;
            action.taint_address = at;
        }
        action.tainted = true;
    }
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
    action.stale = false;
    action.history.clear();
    action.writes.clear();
    // Once skipping is off for the session nothing depends on the marks any more: they stay
    // as they were for the status, and actions stop paying for the lookups.
    detail::action_tracking = Tracking() && !PipelineStats::SkippingOff(epoch);
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
    for (const auto& [begin, end] : action.history) {
        // Drawn over this frame: no longer followed, and what it feeds is not marked for it.
        Unmark(begin, end, false);
        ++history_reads;
    }
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
    if (action.stale) {
        // Content lost long ago and not drawn since (a target made once and kept).
        std::scoped_lock escape_lock{mutex};
        RecordEscape(Escape::CrossFrame, action.stale_address, action.epoch);
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
    if (!Tracking() || PipelineStats::SkippingOff(current_epoch)) {
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
    pause_clears = 0;
    history_reads = 0;
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
                       "draws, {} carried by reads, {} cleared by whole overwrites, {} read as "
                       "history and cleared, all dropped on {} pauses{}{}\n",
                       ranges.size(), double(bytes) / (1024.0 * 1024.0), skip_marks,
                       propagated_marks, cleared, history_reads, pause_clears,
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
