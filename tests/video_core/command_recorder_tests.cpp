// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <limits>
#include "video_core/renderer_vulkan/vk_command_recorder.h"

namespace {
std::vector<unsigned> order;
bool copied = true;
unsigned callbacks{};
VKAPI_ATTR void VKAPI_CALL Barrier(VkCommandBuffer, const VkDependencyInfo* info) {
    ++callbacks;
    const unsigned tag = info->pImageMemoryBarriers[0].subresourceRange.baseArrayLayer;
    order.push_back(tag);
    copied &= info->memoryBarrierCount == 1 && info->bufferMemoryBarrierCount == 1 &&
              info->imageMemoryBarrierCount == 2048 &&
              info->pMemoryBarriers[0].srcStageMask == VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
              info->pBufferMemoryBarriers[0].size == tag;
    for (unsigned i = 0; i < info->imageMemoryBarrierCount; ++i) {
        const auto& b = info->pImageMemoryBarriers[i];
        copied &= b.subresourceRange.baseArrayLayer == tag + i &&
                  b.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                  b.newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
}
}

int main() {
    unsigned checks{}, failures{};
    auto check = [&](const char* name, bool ok) {
        ++checks;
        failures += !ok;
        if (!ok) std::printf("FAIL %s\n", name);
    };
    Vulkan::CommandChunk chunk;
    constexpr size_t payload = 221336; // ASTRO BOT texture-upload crash.
    check("normal chunk rejects large payload", !chunk.HasRoom(payload));
    check("size arithmetic cannot wrap", !chunk.HasRoom(std::numeric_limits<size_t>::max()));
    bool overflow = false;
    try {
        chunk.ReservePayload(std::numeric_limits<size_t>::max());
    } catch (const std::length_error&) {
        overflow = true;
    }
    check("overflow rejected before allocation", overflow && chunk.Empty());
    chunk.ReservePayload(payload);
    check("large payload reserved", chunk.HasRoom(payload));
    auto* bytes = static_cast<std::byte*>(chunk.AllocPayload(payload, 16));
    std::memset(bytes, 0x5a, payload);
    bool intact = false;
    chunk.Record([=, &intact](vk::CommandBuffer) {
        intact = bytes[0] == std::byte{0x5a} && bytes[payload - 1] == std::byte{0x5a};
    });
    chunk.Execute({});
    check("payload survives recording", intact);
    chunk.Reset();
    check("reset retains expanded capacity", chunk.Empty() && chunk.HasRoom(payload));

    auto old_barrier = VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2 = Barrier;
    {
        Vulkan::CommandRecorder recorder;
        Vulkan::CommandRecorder::requested_mode = 1;
        recorder.ApplyRequestedMode();
        const vk::CommandBuffer raw{reinterpret_cast<VkCommandBuffer>(uintptr_t{1})};
        recorder.SetTarget(raw);
        Vulkan::RecordingCommandBuffer cb{&recorder, raw};
        auto marker = [&](unsigned n) {
            recorder.Record(0, [=](Vulkan::CommandChunk&) {
                return [=](vk::CommandBuffer) { order.push_back(n); };
            });
        };
        auto barrier = [&](unsigned tag) {
            vk::MemoryBarrier2 memory{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer};
            vk::BufferMemoryBarrier2 buffer{.size = tag};
            std::vector<vk::ImageMemoryBarrier2> images(2048);
            for (unsigned i = 0; i < images.size(); ++i) {
                images[i].newLayout = vk::ImageLayout::eTransferDstOptimal;
                images[i].subresourceRange.baseArrayLayer = tag + i;
            }
            cb.pipelineBarrier2(vk::DependencyInfo{
                .memoryBarrierCount = 1, .pMemoryBarriers = &memory,
                .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &buffer,
                .imageMemoryBarrierCount = u32(images.size()), .pImageMemoryBarriers = images.data(),
            });
            // All three source arrays die before the worker executes held commands.
            memory = {}; buffer = {};
            std::fill(images.begin(), images.end(), vk::ImageMemoryBarrier2{});
        };
        for (unsigned round = 0; round < 3; ++round) {
            order.clear();
            marker(0);
            recorder.BeginPass();
            marker(30);
            recorder.BeginPrePass();
            barrier(100);
            barrier(200);
            recorder.EndPrePass();
            barrier(300);
            marker(31);
            recorder.EndPass();
            recorder.Sync();
            check("oversized pre-pass and held commands remain ordered",
                  order == std::vector<unsigned>{0, 100, 200, 30, 300, 31});
        }
        order.clear();
        marker(40);
        barrier(400);
        marker(41);
        recorder.Sync();
        check("oversized command outside a pass stays ordered",
              order == std::vector<unsigned>{40, 400, 41});
        check("all copied barrier arrays intact across worker/pool reuse", copied && callbacks == 10);
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2 = old_barrier;
    std::printf("command_recorder_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
