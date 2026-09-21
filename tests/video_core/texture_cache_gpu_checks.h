// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <bitset>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/texture_cache.h"

// Real guest/backing aliases and production caches, with a small VM instead of
// the desktop address-space singleton. Watch faults are avoided by explicit
// invalidation before test writes; mappings/protection remain real.
struct TextureProbeMemory final : Core::GuestMemoryBackend {
    static constexpr u64 Bytes = 64ull << 20;
    int fd{-1};
    u8* backing{};
    u8* base{};
    std::bitset<Bytes / 4096> mapped_pages;
    unsigned unmapped_watch_calls{};
    explicit TextureProbeMemory(const char* directory) {
        std::string path = std::string(directory) + "/texture-cache-XXXXXX";
        fd = mkstemp(path.data());
        if (fd < 0 || ftruncate(fd, Bytes))
            throw std::runtime_error("probe backing file");
        unlink(path.c_str());
        backing = static_cast<u8*>(mmap(nullptr, Bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        base =
            static_cast<u8*>(mmap(nullptr, Bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (backing == MAP_FAILED || base == MAP_FAILED || u64(base) + Bytes >= (1ull << 40))
            throw std::runtime_error("probe guest address");
    }
    ~TextureProbeMemory() {
        munmap(base, Bytes);
        munmap(backing, Bytes);
        close(fd);
    }
    u8* BackingBase() const override {
        return backing;
    }
    boost::icl::interval_set<VAddr> UsableRegions() const override {
        boost::icl::interval_set<VAddr> result;
        result.add(boost::icl::interval<VAddr>::right_open(u64(base), u64(base) + Bytes));
        return result;
    }
    bool OwnsRange(VAddr a, u64 n) const override {
        return a >= u64(base) && a - u64(base) < Bytes && n <= Bytes - (a - u64(base));
    }
    void* Map(VAddr a, u64 n, PAddr physical, bool) override {
        auto* result = mmap(reinterpret_cast<void*>(a), n, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_FIXED, fd, physical);
        if (result == MAP_FAILED)
            throw std::runtime_error("probe map");
        for (u64 offset = a - u64(base); offset < a - u64(base) + n; offset += 4096)
            mapped_pages.set(offset / 4096);
        return result;
    }
    void* MapFile(VAddr, u64, u64, u32, uintptr_t, bool) override {
        throw std::runtime_error("unexpected probe file mapping");
    }
    void Unmap(VAddr a, u64 n) override {
        mprotect(reinterpret_cast<void*>(a), n, PROT_NONE);
        for (u64 offset = a - u64(base); offset < a - u64(base) + n; offset += 4096)
            mapped_pages.reset(offset / 4096);
    }
    void ProtectGpu(VAddr a, u64 n, Core::MemoryPermission p) override {
        for (u64 offset = a - u64(base); offset < a - u64(base) + n; offset += 4096) {
            if (!mapped_pages.test(offset / 4096)) {
                ++unmapped_watch_calls;
                return; // Keep the guard inaccessible in the counterexample too.
            }
        }
        Protect(a, n, p);
    }
    void Protect(VAddr a, u64 n, Core::MemoryPermission p) override {
        if (mprotect(reinterpret_cast<void*>(a), n, int(p)))
            throw std::runtime_error("probe protect");
    }
};

static int TextureCacheGpuChecks(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                                 const char* directory) {
    using namespace VideoCore;
    unsigned checks{}, failures{};
    auto check = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) {
            ++failures;
            std::printf("FAIL CACHE: %s\n", name);
        }
    };
    TextureProbeMemory backend(directory);
    Core::MemoryManager memory(&backend);
    Core::Memory::Binding binding(memory);
    const auto physical = memory.Allocate(0, backend.Bytes, backend.Bytes, 0x4000, 0);
    void* mapped{};
    check(memory.MapMemory(&mapped, u64(backend.base), backend.Bytes,
                           Core::MemoryProt::CpuReadWrite, Core::MemoryMapFlags::Fixed,
                           Core::VMAType::Direct, "texture-probe", false, physical) == 0,
          "guest direct mapping");
    AmdGpu::Liverpool liverpool;
    Vulkan::Rasterizer rasterizer(instance, scheduler, &liverpool);
    auto& textures = rasterizer.GetTextureCache();
    auto desc_for = [&](u32 width, u32 height, vk::Format format = vk::Format::eR8G8B8A8Unorm,
                        u32 bits = 32) {
        TextureCache::ImageDesc desc;
        desc.type = ScaleUse::RenderTarget;
        auto& info = desc.info;
        info.type = AmdGpu::ImageType::Color2D;
        info.size = {width, height, 1};
        info.pitch = width;
        info.pixel_format = format;
        info.num_bits = bits;
        info.guest_address = u64(backend.base);
        info.resources = {1, 1};
        info.UpdateSize();
        desc.view_info.format = format;
        desc.view_info.type = info.type;
        desc.view_info.range = {{0, 0}, {1, 1}};
        return desc;
    };
    auto write_gpu = [&](ImageId id, const TextureCache::ImageDesc& desc) {
        textures.UpdateImage(id);
        (void)textures.FindRenderTarget(id, desc);
        auto& image = textures.GetImage(id);
        image.Clear(
            vk::ClearValue{vk::ClearColorValue{std::array<float, 4>{0.25f, 0.5f, 0.75f, 1.f}}},
            {{0, 0}, {1, 1}});
    };
    std::printf("CACHE extent roundtrip begin\n");
    std::fflush(stdout);
    ImageId previous{};
    for (const auto extent : {Extent2D{1920, 1080}, Extent2D{1600, 900}, Extent2D{1280, 720},
                              Extent2D{1601, 901}, Extent2D{1920, 1080}}) {
        auto desc = desc_for(extent.width, extent.height);
        const auto id = textures.FindImage(desc);
        auto& image = textures.GetImage(id);
        check(image.info.size == desc.info.size, "changed extent gets correct logical identity");
        check(image.HostExtent().width == extent.width / 2 &&
                  image.HostExtent().height == extent.height / 2,
              "changed extent gets current scaled backing");
        if (previous) {
            check(backend.backing[0] == 64 && backend.backing[1] == 128 &&
                      backend.backing[2] == 191 && backend.backing[3] == 255,
                  "extent transition writes latest GPU contents to real guest backing");
        }
        write_gpu(id, desc);
        previous = id;
    }
    textures.ReadbackImageForDiagnostics(previous);
    check(!textures.GetImage(previous).IsScaled(), "readback promotion");
    check(textures.GetImage(previous).ScalePlan().upscaled_readback, "lossy readback label");
    textures.UnmapMemory(u64(backend.base), backend.Bytes);
    scheduler.Finish();
    scheduler.PopPendingOperations();

    std::printf("CACHE Bloodborne alias begin\n");
    std::fflush(stdout);
    auto old = desc_for(128, 64, vk::Format::eR16G16B16A16Sfloat, 64);
    old.type = ScaleUse::Storage;
    const auto old_id = textures.FindImage(old);
    textures.UpdateImage(old_id);
    textures.GetImage(old_id).flags |= ImageFlagBits::GpuModified;
    textures.InvalidateMemory(u64(backend.base), old.info.guest_size);
    auto reused = desc_for(25, 21, vk::Format::eB10G11R11UfloatPack32, 32);
    reused.info.pitch = 128;
    reused.info.UpdateSize();
    const auto reused_id = textures.FindImage(reused);
    check(textures.GetImage(reused_id).info.pixel_format == reused.info.pixel_format,
          "format/extent pool reuse does not enter old-image DRS readback");
    check(textures.GetImage(reused_id).info.size == reused.info.size,
          "small reused target has requested extent");
    textures.UnmapMemory(u64(backend.base), backend.Bytes);
    scheduler.Finish();
    scheduler.PopPendingOperations();

    std::printf("CACHE oversized readback begin\n");
    std::fflush(stdout);
    auto large = desc_for(4096, 2304); // 36 MiB > fixed 32 MiB download ring.
    const auto large_id = textures.FindImage(large);
    write_gpu(large_id, large);
    textures.ReadbackImageForDiagnostics(large_id);
    const auto last = (u64(4096) * 2304 - 1) * 4;
    check(backend.backing[0] == 64 && backend.backing[last] == 64 &&
              backend.backing[last + 1] == 128 && backend.backing[last + 3] == 255,
          "oversized readback reaches both ends of guest backing");
    textures.UnmapMemory(u64(backend.base), backend.Bytes);
    scheduler.Finish();
    scheduler.PopPendingOperations();
    // A sparse buffer update must not coalesce an unwatch across an untouched
    // reservation hole. Exercise the production PageManager with real VM pages.
    const auto sparse = Common::AlignUp(u64(backend.base) + (48ull << 20),
                                       TRACKER_HIGHER_PAGE_SIZE);
    check(memory.UnmapMemory(sparse + 0x4000, 0x4000) == 0, "sparse watch hole unmap");
    {
        PageManager pages(&rasterizer);
        RegionBits mask;
        mask.Clear();
        mask.SetRange(0, 1);
        mask.SetRange(8, 9);
        pages.UpdatePageWatchersForRegion<true, false>(sparse, mask);
        pages.UpdatePageWatchersForRegion<false, false>(sparse, mask);
        check(backend.unmapped_watch_calls == 0, "write unwatch preserves sparse mapping hole");
        backend.unmapped_watch_calls = 0;
        pages.UpdatePageWatchersForRegion<true, false>(sparse, mask);
        pages.UpdatePageWatchersForRegion<true, true>(sparse, mask);
        pages.UpdatePageWatchersForRegion<false, true>(sparse, mask);
        check(backend.unmapped_watch_calls == 0, "read unwatch preserves sparse mapping hole");
        pages.UpdatePageWatchersForRegion<false, false>(sparse, mask);
    }
    backend.unmapped_watch_calls = 0;
    auto& buffers = rasterizer.GetBufferCache();
    const u64 source = sparse + 0x1000;
    constexpr u32 bytes = 0x10000;
    std::memset(backend.backing + (sparse - u64(backend.base)), 0x47, 0x20000);
    Buffer download(instance, scheduler, MemoryUsage::Download, 0,
                    vk::BufferUsageFlagBits::eTransferDst, bytes);
    auto snapshot = [&] {
        auto [buffer, offset] = buffers.ObtainBuffer(source, bytes, false);
        scheduler.EndRendering();
        const vk::MemoryBarrier barrier{
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead};
        auto cmd = scheduler.CommandBuffer();
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                            vk::PipelineStageFlagBits::eTransfer, {}, barrier, {}, {});
        cmd.copyBuffer(buffer->Handle(), download.Handle(), vk::BufferCopy{offset, 0, bytes});
        scheduler.Finish();
        scheduler.PopPendingOperations();
        vmaInvalidateAllocation(instance.GetAllocator(), download.buffer.allocation, 0, bytes);
    };
    snapshot();
    check(backend.unmapped_watch_calls == 0, "sparse upload never protects reservation holes");
    check(std::ranges::all_of(download.mapped_data.first(bytes), [i = u32{}](u8 value) mutable {
        const u32 offset = i++;
        return value == (offset >= 0x3000 && offset < 0x7000 ? 0 : 0x47);
    }), "sparse GPU upload preserves mapped bytes and zero fills hole");
    check(memory.MapMemory(&mapped, sparse + 0x4000, 0x4000, Core::MemoryProt::CpuReadWrite,
                           Core::MemoryMapFlags::Fixed, Core::VMAType::Direct, "recommit",
                           false, physical + sparse - u64(backend.base) + 0x4000) == 0,
          "map former sparse hole");
    std::memset(mapped, 0x6c, 0x4000);
    check(buffers.IsRegionCpuModified(sparse + 0x4000, 0x4000),
          "new mapping invalidates cached zero pages");
    snapshot();
    check(std::ranges::all_of(download.mapped_data.first(bytes), [i = u32{}](u8 value) mutable {
        const u32 offset = i++;
        return value == (offset >= 0x3000 && offset < 0x7000 ? 0x6c : 0x47);
    }), "same cached GPU buffer uploads newly mapped contents");
    check(backend.unmapped_watch_calls == 0, "no protection revived a hole during remap");
    std::printf("TEXTURE_CACHE_GPU %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
