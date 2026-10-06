// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <string>

#include "common/types.h"

/// Guest memory whose content is incomplete because async_graphics_skip dropped a draw that
/// would have written it, followed through the GPU work that reads it.
///
/// A dropped draw marks the attachment ranges it would have written. A later draw, dispatch or
/// copy that reads a marked range marks everything it writes. A mark goes away only when its
/// range is replaced as a whole without reading the old content (a whole-image clear, a clear
/// load of the whole attachment, a copy of unmarked data, a fill); an ordinary draw can never
/// prove it covered a target. When marked content reaches the CPU (a readback) or steers the GPU
/// (indirect arguments), skipping is turned off for the session: later draws wait for their
/// pipelines. Marked content read within a few frames of its last mark is history or feedback
/// (temporal filters): the reading frame draws complete content over it, so the mark is dropped
/// and nothing is marked for it. Marked content read longer after it was last marked is stale
/// (a target made once and kept): skipping pauses for PipelineStats::SkipPauseFrames frames and
/// the marks are dropped; after PipelineStats::MaxSkipPauses pauses it is off for the session.
/// Either way only later commands are protected; content already lost stays lost.
///
/// Ranges are guest addresses, so aliasing views are covered. Everything runs on the GPU command
/// thread except the status report.
namespace Vulkan::MissingContent {

namespace detail {
extern std::atomic<u32> num_ranges;
extern bool action_tracking;
void Read(VAddr address, u64 size);
void Write(VAddr address, u64 size, bool whole);
} // namespace detail

/// True while any range is marked. One relaxed load; everything below is free when false.
inline bool Tracking() noexcept {
    return detail::num_ranges.load(std::memory_order_relaxed) != 0;
}

/// True inside an action that follows reads and writes (something was marked when it began).
inline bool TrackingAction() noexcept {
    return detail::action_tracking;
}

/// A dropped draw would have written [address, address + size). False when the record is full.
bool MarkSkipped(VAddr address, u64 size, u64 epoch);

/// Brackets one GPU action: what it reads decides whether what it writes is marked. Nested
/// brackets join the outermost one.
void BeginAction(u64 epoch);
void EndAction();

struct ActionScope {
    explicit ActionScope(u64 epoch) {
        BeginAction(epoch);
    }
    ~ActionScope() {
        EndAction();
    }
    ActionScope(const ActionScope&) = delete;
    ActionScope& operator=(const ActionScope&) = delete;
};

/// The current action reads this range.
inline void Read(VAddr address, u64 size) {
    if (detail::action_tracking) {
        detail::Read(address, size);
    }
}
/// The current action writes part of this range, or all of it without proof.
inline void Write(VAddr address, u64 size) {
    if (detail::action_tracking) {
        detail::Write(address, size, false);
    }
}
/// A buffer or image the action reads, or writes. Shader resources that are written are also
/// read (the shader may update them); callers note that separately.
inline void Access(VAddr address, u64 size, bool written) {
    if (detail::action_tracking) {
        if (written) {
            detail::Write(address, size, false);
        } else {
            detail::Read(address, size);
        }
    }
}
/// The whole range is replaced without reading its old content. Outside an action it takes
/// effect at once.
void Overwrite(VAddr address, u64 size);

/// Where marked content left the GPU.
enum class Escape : u32 { Readback, Indirect, CrossFrame, Count };
/// Checks a range that is leaving the GPU in this way; true when it holds marked content.
bool CheckEscape(Escape kind, VAddr address, u64 size);
/// True when any part of the range is marked.
bool Overlaps(VAddr address, u64 size);

void Reset();
/// Status lines for `pipeline_cache status`.
void Describe(std::string& out);

} // namespace Vulkan::MissingContent
