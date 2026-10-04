// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

// How often each guest page is re-uploaded to the buffer cache from CPU writes ("page heat"), and
// how many resident-buffer lookups cover it. Sizes the set of pages the CPU rewrites every frame,
// for which write protection costs a fault, two mprotects and a copy each time. Default off.
namespace VideoCore::PageHeat {

inline std::atomic<bool> enabled{false};

// GpuComm, after an upload snapshot: `copies` are arena-relative regions of the arena at
// `arena_base` that were just uploaded from guest memory.
void NoteUploads(std::span<const vk::BufferCopy> copies, VAddr arena_base, u64 frame);

// GpuComm, for each resident buffer lookup.
void NoteResident(VAddr address, u64 size, u64 frame);

// on | off | reset | status
std::string Command(const std::vector<std::string>& args);

} // namespace VideoCore::PageHeat
