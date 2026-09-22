// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace Vulkan {
// Why a dynamic-rendering instance was ended. Only real breaks (a pass was open) are
// counted by the scheduler; StateChange is the natural attachment/area switch, the
// others split a guest pass and cost a tile load/store round trip on a tiler.
enum class RenderBreak : uint8_t {
    StateChange, Dispatch, Barrier, ImageBarrier, Attachment, SampledImage, BufferUpload,
    ImageUpload, Detile, ImageCopy, Download, Flush, CpSync, Hle, Present, Other, Count
};
inline constexpr std::array<const char*, size_t(RenderBreak::Count)> RenderBreakNames{
    "state_change", "dispatch", "buffer_barrier", "image_barrier", "attachment",
    "sampled_image", "buffer_upload", "image_upload", "detile", "image_copy", "download",
    "flush", "cp_sync", "hle", "present", "other"};
} // namespace Vulkan
