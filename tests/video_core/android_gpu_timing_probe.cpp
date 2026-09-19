// SPDX-License-Identifier: GPL-2.0-or-later
// Real private-Turnip tests of the production collector. Only this test waits
// for test fences; GpuProfiler itself never waits. No system driver fallback.
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "frontend/window.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/profiler.h"

#include <cstdio>
#include <filesystem>

struct Window : Frontend::Window {
    s32 GetWidth() const override { return 64; }
    s32 GetHeight() const override { return 64; }
    Frontend::WindowSystemInfo GetWindowInfo() const override { return {}; }
    bool RequestKeyboard() override { return false; }
    void ReleaseKeyboard() override {}
};
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    unsigned checks{}, failures{};
#define TEST(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while(0)
    using namespace Common::Profiler;
    Common::FS::InitializeAndroidUserPaths(argv[3]);
    Common::Log::Setup("gpu-timing");
    Common::Profiler::Initialize();
    Common::Profiler::Frame();
    Window window;
    auto driver = Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
    auto device = instance.GetDevice();
    auto command_pool = Vulkan::Check(device.createCommandPoolUnique({.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()}));
    auto cmds = Vulkan::Check(device.allocateCommandBuffers({.commandPool = *command_pool,
        .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 40}));
    auto fence = Vulkan::Check(device.createFenceUnique({}));
    Vulkan::GpuProfiler profile(instance, GpuStage::DrawBatch);
    auto stats = instance.GpuTiming();
    GpuTimingControl({"stop"});
    Vulkan::Check(cmds[0].begin(vk::CommandBufferBeginInfo{})); profile.BeginBatch(cmds[0], 0); profile.EndBatch(cmds[0]);
    Vulkan::Check(cmds[0].end());
    TEST(!stats->Read().supported); // off path never creates a query pool
    GpuTimingControl({"start"});
    // Fill every lease with recorded but not submitted command buffers. Even a
    // bogus high completion watermark must not inspect or recycle those queries.
    for (uint32_t i = 0; i < Vulkan::GpuProfiler::BatchCount; ++i) {
        auto cmd = cmds[i + 1]; Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{})); profile.BeginBatch(cmd, UINT64_MAX);
        if (i == 0) {
            for (unsigned j = 0; j < Vulkan::GpuProfiler::ZonesPerBatch + 2; ++j) { auto z = profile.Begin(cmd, GpuStage::PostProcess); profile.End(cmd, z); }
        }
        profile.Prepare(cmd); profile.FrameEnd(); profile.EndBatch(cmd); Vulkan::Check(cmd.end());
    }
    TEST(stats->Read().supported);
    TEST(stats->Read().retired_batches == 0);
    TEST(stats->Read().dropped_zones > 0);
    Vulkan::Check(cmds[33].begin(vk::CommandBufferBeginInfo{})); profile.BeginBatch(cmds[33], UINT64_MAX); profile.EndBatch(cmds[33]); Vulkan::Check(cmds[33].end());
    TEST(stats->Read().dropped_batches == 1); // bounded exhaustion, no wait/no reuse
    // This first object contains unsubmitted leases: legal to destroy after the
    // command pool is reset. A fresh collector below checks normal retirement.
    Vulkan::Check(device.resetCommandPool(*command_pool));
    {
        Vulkan::GpuProfiler live(instance, GpuStage::DrawBatch);
        auto execute = [&](uint64_t tick, bool toggle) {
            auto cmd = cmds[0]; Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{})); live.BeginBatch(cmd, tick - 1);
            live.Prepare(cmd); live.FrameEnd(); live.EndBatch(cmd); Vulkan::Check(cmd.end());
            auto before = stats->Read().retired_batches;
            live.Collect(UINT64_MAX); TEST(stats->Read().retired_batches == before);
            vk::SubmitInfo info{.commandBufferCount = 1, .pCommandBuffers = &cmd};
            live.Submitting();
            Vulkan::Check(instance.GetGraphicsQueue().submit(info, *fence)); live.Submitted(tick);
            live.Collect(tick - 1); TEST(stats->Read().retired_batches == before);
            if (toggle) { GpuTimingControl({"stop"}); GpuTimingControl({"start"}); }
            // Bounded test-only wait, never used by the collector.
            auto result = device.waitForFences(*fence, true, 2000000000ull);
            if (result != vk::Result::eSuccess) { puts("GPU fence timeout/error"); std::exit(3); }
            live.Collect(tick); TEST(stats->Read().retired_batches == before + 1);
            Vulkan::Check(device.resetFences(*fence)); Vulkan::Check(device.resetCommandPool(*command_pool));
        };
        execute(1, false); execute(2, false);
        TEST(stats->Read().stages[size_t(GpuStage::Guest)].count == 2);
        TEST(stats->Read().stages[size_t(GpuStage::GuestFrame)].count == 1);
        auto before = stats->Read().stages[size_t(GpuStage::DrawBatch)].count;
        execute(3, true); TEST(stats->Read().discarded == 1);
        TEST(stats->Read().stages[size_t(GpuStage::DrawBatch)].count == before);
        execute(4, false); execute(5, false);
        TEST(stats->Read().pending == 0); TEST(stats->Read().errors == 0);
    }
    puts(GpuTimingControl({}).c_str());
    GpuTimingControl({"stop"});
    printf("GPU_TIMING_DEVICE %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
