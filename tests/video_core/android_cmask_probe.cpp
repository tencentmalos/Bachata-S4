// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <sys/mman.h>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "frontend/window.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/vma_diagnostics.h"
struct Window : Frontend::Window {
    s32 GetWidth() const override {
        return 64;
    }
    s32 GetHeight() const override {
        return 64;
    }
    Frontend::WindowSystemInfo GetWindowInfo() const override {
        return {};
    }
    bool RequestKeyboard() override {
        return false;
    }
    void ReleaseKeyboard() override {}
};
struct Backend : Core::GuestMemoryBackend {
    static constexpr u64 Size = 4ULL << 20;
    void* allocation =
        mmap(std::getenv("DMA_PROBE_LOW_VA") ? reinterpret_cast<void*>(0x220000000ULL) : nullptr,
             Size + 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    VAddr base = (u64(allocation) + 0x3fff) & ~0x3fffULL;
    ~Backend() {
        munmap(allocation, Size + 0x4000);
    }
    u8* BackingBase() const override {
        return reinterpret_cast<u8*>(base);
    }
    boost::icl::interval_set<VAddr> UsableRegions() const override {
        boost::icl::interval_set<VAddr> out;
        out.add(boost::icl::interval<VAddr>::right_open(base, base + Size));
        return out;
    }
    bool OwnsRange(VAddr a, u64 n) const override {
        return a >= base && a - base < Size && n <= Size - (a - base);
    }
    void* Map(VAddr a, u64 n, PAddr, bool) override {
        if (!OwnsRange(a, n) || mprotect(reinterpret_cast<void*>(a), n, PROT_READ | PROT_WRITE))
            throw std::runtime_error("map");
        return reinterpret_cast<void*>(a);
    }
    void* MapFile(VAddr, u64, u64, u32, uintptr_t, bool) override {
        throw std::runtime_error("unused");
    }
    void Unmap(VAddr a, u64 n) override {
        mprotect(reinterpret_cast<void*>(a), n, PROT_NONE);
    }
    void Protect(VAddr a, u64 n, Core::MemoryPermission p) override {
        mprotect(reinterpret_cast<void*>(a), n, int(p));
    }
};

int Run(int argc, char** argv) {
    if (argc != 3)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("cmask-probe");
    Common::ElfInfo::Instance().InitializeGuestMetadata(argv[2], 0x11000000, "CMASK-PROBE");
    Backend backend;
    if (backend.allocation == MAP_FAILED)
        return 2;
    Core::MemoryManager memory(&backend);
    Core::Memory::Binding binding(memory);
    void* mapped{};
    if (memory.MapMemory(&mapped, backend.base, Backend::Size, Core::MemoryProt::CpuReadWrite,
                         Core::MemoryMapFlags::NoFlags, Core::VMAType::File, "dma-probe") != 0)
        return 2;
    std::memset(mapped, 0, Backend::Size);
    Window window;
    Vulkan::Instance instance(window, 0, false, false, Vulkan::LoadAndroidTurnip(argv[1], argv[2]));
    std::printf("Upload probe base=%llx maxStorageBufferRange=%u\n",
                static_cast<unsigned long long>(backend.base),
                instance.GetPhysicalDevice().getProperties().limits.maxStorageBufferRange);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    AmdGpu::Liverpool liverpool;
    Vulkan::Rasterizer rasterizer(instance, scheduler, runtime, &liverpool);
    rasterizer.MapMemory(backend.base, Backend::Size);
    auto& bc = rasterizer.GetBufferCache();
    const u64 page_size = 1ULL << bc.GetSparsePageShift();
    if (page_size != 0x4000)
        throw std::runtime_error("test requires 16 KiB sparse pages");
    unsigned checks{}, failures{};
    auto check = [&](bool ok) {
        ++checks;
        if (!ok)
            ++failures;
    };
    VideoCore::Buffer download(instance, 0, Backend::Size, VideoCore::MemoryType::HostCached);
    auto finish = [&] {
        runtime.FlushBarriers();
        const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                         .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        scheduler.CommandBuffer().pipelineBarrier2(
            vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        scheduler.Finish();
        scheduler.PopPendingOperations();
        download.Invalidate(0, download.SizeBytes());
    };

    auto& tc = rasterizer.GetTextureCache();
    auto verify = [&](VAddr address, u32 expected) {
        const auto [arena, offset] = bc.ObtainResidentBuffer(address, 4096, false, true);
        const vk::BufferCopy copy{.srcOffset = offset, .dstOffset = 0, .size = 4096};
        runtime.CopyBuffer(arena, &download, {&copy, 1});
        finish();
        const auto* actual = reinterpret_cast<const u32*>(download.mapped_data.data());
        for (u32 n = 0; n < 1024; ++n)
            check(actual[n] == expected);
    };
    for (u32 samples : {1u, 2u, 4u}) {
        VideoCore::TextureCache::ImageDesc desc;
        desc.type = VideoCore::TextureCache::BindingType::RenderTarget;
        desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
        desc.info.type = samples == 1 ? AmdGpu::ImageType::Color2D : AmdGpu::ImageType::Color2DMsaa;
        desc.info.size = {64, 64, 1};
        desc.info.pitch = 64;
        desc.info.num_bits = 32;
        desc.info.num_samples = samples;
        desc.info.guest_address = backend.base + samples * 0x40000;
        desc.info.meta_info.cmask_addr = backend.base + 0x200000 + samples * 0x4000;
        desc.info.UpdateSize();
        desc.view_info.type = desc.info.type;
        // Ordinary memory must stay unchanged before it is registered as metadata.
        verify(desc.info.meta_info.cmask_addr, 0);
        const auto id = tc.FindImage(desc);
        (void)tc.FindRenderTarget(id, desc);
        verify(desc.info.meta_info.cmask_addr, samples == 1 ? 0xffffffffu : 0u);
        // A repeated query must still observe the host's expanded representation.
        verify(desc.info.meta_info.cmask_addr, samples == 1 ? 0xffffffffu : 0u);
    }
    scheduler.Finish();
    scheduler.PopPendingOperations();
    scheduler.SetSubmitCallback({});
    memory.Protect(backend.base, Backend::Size, Core::MemoryProt::CpuReadWrite);
    printf("CMASK %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int result = Run(argc, argv);
    Common::Log::Shutdown();
    return result;
}
