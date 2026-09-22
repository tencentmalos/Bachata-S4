// SPDX-License-Identifier: GPL-2.0-or-later
// Production Scheduler/SubmissionWorker/TimelineCompletion on the real Android
// system driver. An injected queue owner models slow present without a Surface.
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/profiler.h"
#include "frontend/window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <chrono>
#include <cstdio>
#include <future>
#include <semaphore>
#include <vk_mem_alloc.h>

using namespace std::chrono_literals;
struct ProbeWindow : Frontend::Window {
    s32 GetWidth() const override { return 64; }
    s32 GetHeight() const override { return 64; }
    Frontend::WindowSystemInfo GetWindowInfo() const override { return {}; }
    bool RequestKeyboard() override { return false; }
    void ReleaseKeyboard() override {}
};
static unsigned checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
struct Buffer {
    VmaAllocator allocator{};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    uint32_t* data{};
    explicit Buffer(Vulkan::Instance& instance) : allocator(instance.GetAllocator()) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = 8192;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VmaAllocationInfo allocated{};
        Vulkan::Check(vk::Result(vmaCreateBuffer(allocator, &info, &ai, &buffer, &allocation, &allocated)));
        data = static_cast<uint32_t*>(allocated.pMappedData);
    }
    ~Buffer() { vmaDestroyBuffer(allocator, buffer, allocation); }
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    Common::FS::InitializeAndroidUserPaths(argv[1]);
    Common::Log::Setup("submission-probe");
    Common::Profiler::Initialize();
    Common::Profiler::Control({"start"});
    // Lite activates on the first real source frame. This probe owns that
    // source; production uses the guest frame mark instead.
    Common::Profiler::Frame();
    CHECK(Common::Profiler::Enabled());
    auto driver = Vulkan::LoadAndroidSystemDriver();
    ProbeWindow window;
    for (unsigned generation = 0; generation < 3; ++generation) {
        Vulkan::Instance instance(window, 0, false, false, driver);
        CHECK(instance.Submissions() != nullptr);
        if (!instance.Submissions()) return 2;
        Buffer buffer(instance);
        Vulkan::Scheduler draw(instance), present(instance, Vulkan::GpuProfiler::Stage::Present);
        const auto device = instance.GetDevice();
        // Real GPU signal is visible before host submission acknowledgement.
        // Hold that acknowledgement deterministically; neither Wait nor pool
        // retirement may pass the command-buffer ownership boundary early.
        {
            Vulkan::Semaphore master(instance);
            auto pool = Vulkan::Check(device.createCommandPoolUnique({
                .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()}));
            auto cmds = Vulkan::Check(device.allocateCommandBuffersUnique({
                .commandPool = *pool, .level = vk::CommandBufferLevel::ePrimary,
                .commandBufferCount = 1}));
            auto cmd = *cmds.front();
            Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{}));
            Vulkan::Check(cmd.end());
            const auto tick = master.NextTick();
            const auto sem = master.Handle();
            std::binary_semaphore submitted{0}, acknowledge{0};
            auto receipt = instance.Submissions()->Enqueue([&](auto&) {
                const vk::TimelineSemaphoreSubmitInfo timeline{
                    .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &tick};
                const vk::SubmitInfo info{.pNext = &timeline,
                    .commandBufferCount = 1, .pCommandBuffers = &cmd,
                    .signalSemaphoreCount = 1, .pSignalSemaphores = &sem};
                { std::scoped_lock lock(instance.QueueMutex());
                  Vulkan::Check(instance.GetGraphicsQueue().submit(info)); }
                submitted.release();
                acknowledge.acquire();
                master.Submitted(tick);
            });
            CHECK(submitted.try_acquire_for(2s));
            const vk::SemaphoreWaitInfo wait{
                .semaphoreCount = 1, .pSemaphores = &sem, .pValues = &tick};
            CHECK(device.waitSemaphores(wait, 2'000'000'000) == vk::Result::eSuccess);
            auto waiting = std::async(std::launch::async, [&] { master.Wait(tick); });
            CHECK(waiting.wait_for(50ms) == std::future_status::timeout);
            CHECK(!master.IsFree(tick));
            acknowledge.release();
            CHECK(waiting.wait_for(2s) == std::future_status::ready);
            waiting.get();
            instance.Submissions()->Wait(receipt->serial);
            CHECK(master.IsFree(tick));
        }
        const auto signal = Vulkan::Check(device.createSemaphoreUnique({}));
        const auto fence = Vulkan::Check(device.createFenceUnique({}));
        Common::Profiler::GpuTimingControl({"start"});
        // Allocate a query lease after toggling on.
        draw.Flush(); present.Flush();
        draw.WaitSubmitted(); present.WaitSubmitted();
        const auto draw_tick = draw.CurrentTick();
        const auto present_tick = present.CurrentTick();
        draw.CommandBuffer().fillBuffer(buffer.buffer, 0, 4096, 0x12340000 + generation);
        Vulkan::SubmitInfo a{};
        Vulkan::SubmitInfo b{};
        b.AddWait(draw.GetWorkSemaphore()->Handle(), draw_tick);
        b.AddSignal(*signal);
        b.AddSignal(*fence);
        present.CommandBuffer().copyBuffer(buffer.buffer, buffer.buffer,
                                          vk::BufferCopy{0, 4096, 4096});
        std::unique_lock queue_owner(instance.QueueMutex());
        auto produce = std::async(std::launch::async, [&] {
            draw.Flush(a);
            present.Flush(b);
        });
        const bool accepted_while_present_blocked = produce.wait_for(200ms) == std::future_status::ready;
        CHECK(accepted_while_present_blocked);
        if (!accepted_while_present_blocked) queue_owner.unlock();
        produce.get();
        if (accepted_while_present_blocked) {
            CHECK(!draw.GetWorkSemaphore()->IsFree(draw_tick));
            CHECK(!present.GetWorkSemaphore()->IsFree(present_tick));
            CHECK(device.getFenceStatus(*fence) == vk::Result::eNotReady);
            // Destroy caller-owned submit arrays before the worker can read them.
            a = {}; b = {};
            queue_owner.unlock();
        }
        present.WaitSubmitted();
        // Match present's requirement: the binary signal has actually been
        // submitted before its consumer. The consumer need not wait for the GPU.
        Vulkan::SubmitInfo consume{};
        consume.AddWait(*signal);
        present.Flush(consume);
        present.Finish(); draw.Finish();
        CHECK(device.getFenceStatus(*fence) == vk::Result::eSuccess);
        CHECK(draw.GetWorkSemaphore()->IsFree(draw_tick));
        CHECK(present.GetWorkSemaphore()->IsFree(present_tick));
        bool correct = true;
        for (unsigned i = 1024; i < 2048; ++i) correct &= buffer.data[i] == 0x12340000 + generation;
        CHECK(correct);

        // Exercise tick-protected pool/query reuse and bounded GPU backpressure.
        // Every recorded fill is followed by real completion before CPU readback.
        for (unsigned i = 0; i < 96; ++i) {
            if (i == 2) draw.BeginMarker("probe.logical_scope_across_flush");
            if (i == 4) draw.EndMarker();
            draw.CommandBuffer().fillBuffer(buffer.buffer, (i % 256) * 4, 4, 0xabc00000 + i);
            draw.GpuProfile().FrameEnd();
            draw.Flush();
            if (i == 30) Common::Profiler::GpuTimingControl({"stop"});
            if (i == 40) Common::Profiler::GpuTimingControl({"start"});
        }
        draw.Finish(); present.Finish();
        for (unsigned i = 0; i < 96; ++i) CHECK(buffer.data[i] == 0xabc00000 + i);
        const auto stats = instance.GpuTiming()->Read();
        CHECK(stats.supported && stats.errors == 0);
        CHECK(stats.pending == 0);
        CHECK(stats.retired_batches > 32);
        Common::Profiler::GpuTimingControl({"stop"});
        std::printf("generation=%u retired=%llu errors=%llu pending=%llu\n", generation,
                    (unsigned long long)stats.retired_batches, (unsigned long long)stats.errors,
                    (unsigned long long)stats.pending);
        if (generation == 1) {
            // Inject a backend exception, not real device loss. Both host and
            // GPU waiters must observe it without waiting for an unsignalled
            // tick; the following generation must still create and execute.
            auto failed = instance.Submissions()->Enqueue([](auto&) {
                throw std::runtime_error("injected submit failure");
            });
            try { instance.Submissions()->Wait(failed->serial); CHECK(false); }
            catch (const std::runtime_error&) { CHECK(true); }
            const auto start = std::chrono::steady_clock::now();
            try { draw.GetWorkSemaphore()->Wait(draw.CurrentTick()); CHECK(false); }
            catch (const std::runtime_error&) { CHECK(true); }
            CHECK(std::chrono::steady_clock::now() - start < 200ms);
            CHECK(!failed->submitted.load());
        }
    }
    std::printf("%s", Common::Profiler::Control({"dump", "0"}).c_str());
    std::string ring;
    for (unsigned i = 0; i < 100; ++i) {
        ring = Common::Profiler::Control({"status"});
        if (ring.find("ready") != std::string::npos || ring.find("failed") != std::string::npos) break;
        std::this_thread::sleep_for(20ms);
    }
    std::printf("%s", ring.c_str());
    CHECK(ring.find("ready") != std::string::npos);
    std::printf("SUBMISSION_DEVICE checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
