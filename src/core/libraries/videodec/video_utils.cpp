// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_utils.h"

#include "common/alignment.h"
#include "common/assert.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <cstring>

namespace Libraries::Videodec {

void CopyNV12Data(u8* dst, u64 max_size, const AVFrame& src) {
    const auto dst_pitch = Common::AlignUp<u32>(src.width, 64);
    const auto dst_height = Common::AlignUp<u32>(src.height, 16);

    ASSERT(((dst_pitch * dst_height * 3) / 2) <= max_size);

    const auto luma_dst = dst;
    const auto chroma_dst = dst + dst_pitch * dst_height;

    // Source line stride is independent of the guest's aligned destination pitch.
    // Even width == dst_pitch can have padding in an FFmpeg AVFrame.
    for (u32 y = 0; y < src.height; ++y) {
        std::memcpy(luma_dst + y * dst_pitch, src.data[0] + y * src.linesize[0], src.width);
    }
    for (u32 y = 0; y < src.height / 2; ++y) {
        std::memcpy(chroma_dst + y * dst_pitch, src.data[1] + y * src.linesize[1], src.width);
    }

    if (src.height != dst_height) {
        // Extend the data vertically to the crop space
        const auto ly = src.height - 1;
        for (u32 y = src.height; y < dst_height; ++y) {
            std::memcpy(luma_dst + y * dst_pitch, src.data[0] + ly * src.linesize[0], src.width);
        }
        const auto cy = (src.height / 2) - 1;
        for (u32 y = src.height / 2; y < dst_height / 2; ++y) {
            std::memcpy(chroma_dst + y * dst_pitch, src.data[1] + cy * src.linesize[1], src.width);
        }
    }
}

} // namespace Libraries::Videodec
