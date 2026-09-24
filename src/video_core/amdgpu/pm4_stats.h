// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <span>
#include <string>
#include <vector>
#include "common/types.h"

namespace AmdGpu::Pm4Stats {

// Default-off structure census of guest command buffers, used to judge whether
// separately built DCBs could be translated concurrently. While armed, each
// submitted DCB is walked once on the submitting thread (following INDIRECT_BUFFER
// calls) and summarised; nothing is executed or modified.
inline std::atomic<bool> armed{false};

// HLE copy shader: merge regions that continue each other in both buffers (default on).
inline std::atomic<bool> hle_merge_off{false};

void NoteDcb(u32 index_in_call, std::span<const u32> dcb, bool has_ccb);
void EndSubmitCall();
void NoteFlip(); // the submit that ends a frame (SubmitAndFlip)

// Read-only small-buffer copies into the stream buffer (UBOs, small index/vertex
// buffers). epoch is the PM4 owner's guest-flip counter; data is the copied bytes.
void NoteStreamCopy(u64 epoch, u64 address, u32 size, const void* data);

// One HLE copy-shader dispatch: region list as (src offset, dst offset, size) triples,
// in dispatch order, before any merging.
struct HleRegion {
    u64 src, dst, size;
};
void NoteHleCopy(u64 epoch, u64 src_base, u64 dst_base, std::span<const HleRegion> regions);

std::string Command(const std::vector<std::string>& args);

} // namespace AmdGpu::Pm4Stats
