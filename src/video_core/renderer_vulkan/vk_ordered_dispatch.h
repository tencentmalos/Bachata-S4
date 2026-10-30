// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

// For shaders admitted as one guest wave per workgroup. Serial submission avoids
// relying on workgroup scheduling order or forward progress of GPU spin loops.
inline void DispatchOrderedGroups(auto cmd, u32 nx, u32 ny, u32 nz) {
    for (u32 z = 0; z < nz; ++z) {
        for (u32 y = 0; y < ny; ++y) {
            for (u32 x = 0; x < nx; ++x) {
                if (x || y || z) {
                    cmd.pipelineBarrier(
                        vk::PipelineStageFlagBits::eComputeShader,
                        vk::PipelineStageFlagBits::eComputeShader, {},
                        vk::MemoryBarrier{
                            .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                            .dstAccessMask = vk::AccessFlagBits::eShaderRead |
                                             vk::AccessFlagBits::eShaderWrite}, {}, {});
                }
                cmd.dispatchBase(x, y, z, 1, 1, 1);
            }
        }
    }
}

} // namespace Vulkan
