// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/profiler.h"
#include <xxhash.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <map>
#include <optional>
#include <sstream>

#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/guest_write_watch.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/memory_diagnostics.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

#include <vk_mem_alloc.h>
#include "video_core/vma_diagnostics.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    memory_diagnostics_epoch = MemoryDiagnostics::Begin(instance.ScalePolicy());

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

std::shared_ptr<ResourceScalePlan> TextureCache::AcquireScalePlan(const ImageInfo& info, ScaleUse use) {
    return scale_plans.Acquire({info.guest_address, info.guest_size, u64(info.pixel_format),
        u64(info.type), info.size.width, info.size.height, info.size.depth, info.pitch,
        info.resources.levels, info.resources.layers, info.num_samples,
        u64(info.tile_mode) | (u64(info.bank_swizzle) << 32) | (u64(info.alt_tile) << 40)}, use);
}

TextureCache::~TextureCache() {
    MemoryDiagnostics::End(memory_diagnostics_epoch);
}

void TextureCache::RecordAttachmentDraw(std::span<const ImageId> attachments, u64 fragment_hash,
                                        bool scaled, bool began_rendering, bool resumed) {
    if (attachments.empty()) return;
    {
        constexpr auto o = std::memory_order_relaxed;
        coverage->draws.fetch_add(1, o);
        if (scaled) coverage->scaled_draws.fetch_add(1, o);
        if (began_rendering) {
            coverage->passes.fetch_add(1, o);
            if (scaled) coverage->scaled_passes.fetch_add(1, o);
            if (resumed) coverage->resumed_passes.fetch_add(1, o);
        }
    }
    u32 reasons{}, width{}, height{}, depth{};
    for (const auto id : attachments) {
        const auto& image = slot_images[id];
        const auto& plan = image.ScalePlan();
        reasons |= plan.native_reason_mask | (1u << u32(plan.reason));
        width = std::max(width, image.info.size.width);
        height = std::max(height, image.info.size.height);
        depth |= image.info.props.is_depth;
    }
    const std::array<u64, 7> key{fragment_hash, width, height, reasons, u64(scaled),
                               attachments.size(), depth};
    auto it = attachment_groups.find(key);
    if (it == attachment_groups.end()) {
        if (attachment_groups.size() == 512) { ++attachment_group_overflow; return; }
        it = attachment_groups.emplace(key, AttachmentCounts{}).first;
    }
    ++it->second.draws;
    it->second.passes += began_rendering;
    it->second.resumed += began_rendering && resumed;
}

void TextureCache::PublishMemoryDiagnostics() {
    const auto request = MemoryDiagnostics::requested.load(std::memory_order_acquire);
    if (request == MemoryDiagnostics::completed.load(std::memory_order_acquire)) return;
    const auto cov = coverage->Read();
    std::ostringstream out;
    out << "status=sampled sample_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()
        << " render_scale_percent=" << instance.ScalePolicy().render_eighths * 12.5f
        << " texture_quality=" << TextureQualityName(instance.ScalePolicy().texture)
        << " legacy=" << instance.ScalePolicy().legacy
        << " attachment_counter_version=2 attachment_draws=" << cov.draws
        << " scaled_attachment_draws=" << cov.scaled_draws
        << " attachment_passes=" << cov.passes << " scaled_attachment_passes=" << cov.scaled_passes
        << " resumed_attachment_passes=" << cov.resumed_passes
        << " plan_entries=" << scale_plans.Size() << " budget_native=" << scale_plans.BudgetNative() << "\n";
    out << "native_pass_causes side_effects=" << cov.native_pass_side_effects
        << " msaa=" << cov.native_pass_msaa << " attachment=" << cov.native_pass_attachment
        << " mismatch=" << cov.native_pass_mismatch << " promotions=" << cov.native_promotions
        << " upscaled_readbacks=" << cov.upscaled_readbacks
        << " scaled_blit_copies=" << cov.scaled_blit_copies
        << " image_buffer_syncs=" << cov.image_buffer_syncs
        << " image_buffer_sync_bytes=" << cov.image_buffer_sync_bytes
        << " fused_readbacks=" << cov.fused_readbacks << '\n';
    out << "texture_uploads count=" << cov.image_uploads << " bytes=" << cov.image_upload_bytes
        << " fill_clears=" << cov.fill_clears << '\n';
    out << "gc downloads=" << cov.gc_downloads << " frees=" << cov.gc_frees
        << " pressured_ticks=" << cov.gc_pressured_ticks << " used_mib=" << (total_used_memory >> 20)
        << " trigger_mib=" << (trigger_gc_memory >> 20) << " pressure_mib=" << (pressure_gc_memory >> 20)
        << " critical_mib=" << (critical_gc_memory >> 20)
        << " budget_reported=" << instance.CanReportMemoryUsage() << '\n';
    {
        const auto& breaks = scheduler.RenderBreaks();
        out << "render_pass begins=" << scheduler.RenderBegins() << " breaks:";
        for (size_t i = 0; i < breaks.size(); ++i)
            out << ' ' << Vulkan::Scheduler::RenderBreakNames[i] << '=' << breaks[i];
        out << '\n';
        // natural = guest-expressed target changes; resumed = same targets re-opened
        // after a break, keyed by the break that split them (emulator-imposed fragments).
        const auto& resumes = scheduler.RenderResumes();
        out << "render_pass natural=" << scheduler.RenderNatural() << " resumed:";
        for (size_t i = 0; i < resumes.size(); ++i)
            out << ' ' << Vulkan::Scheduler::RenderBreakNames[i] << '=' << resumes[i];
        out << '\n';
        const auto tiles = Vulkan::render_pass_stats.Read();
        out << "tile_traffic passes=" << tiles.passes << " loads=" << tiles.loads
            << " clears=" << tiles.clears << " stores=" << tiles.stores
            << " load_pixels=" << tiles.load_pixels << " store_pixels=" << tiles.store_pixels
            << " draws_per_pass empty=" << tiles.empty << " single=" << tiles.single
            << " few=" << tiles.few << " many=" << tiles.many << '\n';
        out << "pass_hoist hoisted=" << tiles.hoisted << " conflicts=" << tiles.hoist_conflicts
            << " unavailable=" << tiles.hoist_unavailable
            << " interrupted=" << tiles.hoist_interrupted << '\n';
        // Re-arm the bounded pass-break/resume log so the next frames after a request
        // describe which images and transitions split passes in the current scene.
        scheduler.ArmPassBreakLog(400);
    }
    out << "idle_asset_evictions=" << idle_asset_evictions
        << " idle_asset_retired_allocation_bytes=" << idle_asset_retired_bytes << '\n';
    out << "attachment_group_overflow_draws=" << attachment_group_overflow << '\n';
    for (u32 i = 0; i < u32(ScaleReason::Count); ++i)
        out << "native_fallback=" << ScaleReasonName(ScaleReason(i)) << " transitions="
            << native_fallbacks[i] << '\n';
    for (const auto& [key, counts] : attachment_groups)
        out << "attachment_group fragment=" << key[0] << " logical=" << key[1] << 'x' << key[2]
            << " reasons=" << key[3] << " scaled=" << key[4] << " attachments=" << key[5]
            << " depth=" << key[6] << " draws=" << counts.draws << " passes=" << counts.passes
            << " resumed=" << counts.resumed << '\n';
    // The group table is a window between requests, so a scene sampled after a long
    // loading phase is not hidden behind groups that overflowed the table earlier.
    out << "attachment_groups_window=1 groups=" << attachment_groups.size() << '\n';
    attachment_groups.clear();
    attachment_group_overflow = 0;
    VmaDiagnostics::Append(instance.GetAllocator(), instance.GetMemoryProperties(), instance.CanReportMemoryUsage(), out);
    struct Group { u64 images{}, backings{}, allocation_bytes{}, guest_layout_bytes{}; };
    std::map<std::string, Group> groups;
    {
        std::scoped_lock lock{mutex};
        for (auto& image : slot_images) {
            if (!image.backing) continue;
            const std::string kind = std::string(ScaleDomainName(image.ScalePlan().domain)) + "/" +
                std::string(ScaleReasonName(image.ScalePlan().reason));
            const std::string format = image.IsReencoded() ? std::string(BlockCodecName(image.Reencoding()))
                                       : image.info.props.is_block ? "BC" : "uncompressed";
            auto& group = groups[kind + "/" + format + (image.IsScaled() ? "/scaled" : "/native")];
            if (group.images < 4) {
                const auto& plan = image.ScalePlan();
                const auto extent = image.HostExtent();
                out << "resource=" << kind << " address=" << image.info.guest_address
                    << " generation=" << plan.mapping_generation << " logical=" << image.info.size.width << 'x' << image.info.size.height
                    << " physical=" << extent.width << 'x' << extent.height << " drop=" << image.DroppedMips()
                    << " content_version=" << plan.content_version << " plan_version=" << plan.plan_version
                    << " upscaled_readback=" << plan.upscaled_readback << " uploads=" << plan.uploads << "\n";
            }
            ++group.images;
            group.guest_layout_bytes += image.info.guest_size;
            for (auto& backing : image.backing_images) {
                if (!backing.image.allocation) continue;
                VmaAllocationInfo info{};
                vmaGetAllocationInfo(instance.GetAllocator(), backing.image.allocation, &info);
                group.allocation_bytes += info.size;
                ++group.backings;
            }
        }
    }
    u64 image_bytes{};
    for (const auto& [key, group] : groups) {
        image_bytes += group.allocation_bytes;
        out << "images=" << key << " count=" << group.images << " backings=" << group.backings
            << " allocation_bytes=" << group.allocation_bytes
            << " guest_layout_bytes=" << group.guest_layout_bytes << "\n";
    }
    out << "cached_image_allocation_bytes=" << image_bytes
        << "\nscale_upload_image_bytes=" << MemoryDiagnostics::upload_image_bytes.load(std::memory_order_relaxed)
        << " scale_upload_created_bytes=" << MemoryDiagnostics::upload_image_created_bytes.load(std::memory_order_relaxed)
        << " scale_upload_created_count=" << MemoryDiagnostics::upload_image_created_count.load(std::memory_order_relaxed)
        << "\nnotes=guest_layout_bytes_is_not_resident_RAM; VMA_includes_buffers_and_pending_resources;"
           " cached_images_include_pending_cache_deletion; host_snapshot_is_not_GPU_idle\n";
    buffer_cache.AppendMemoryDiagnostics(out);
    MemoryDiagnostics::Publish(memory_diagnostics_epoch, request, out.str());
}

void TextureCache::ProcessDownloadImages() {
    std::unique_lock lk{download_images_mutex};
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id, true, false, ~VAddr{0}, "image_writeback_queue");
    }
    download_images.clear();
}

void TextureCache::ReadbackImageForDiagnostics(ImageId image_id) {
    DownloadImageMemory(image_id, true, false, ~VAddr{0}, "image_writeback_diagnostic");
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync, bool tracked_only,
                                       VAddr write_end, const char* writer) {
    auto write_back = RecordImageDownload(image_id, tracked_only, write_end, writer);
    if (!write_back) {
        return;
    }
    if (sync) {
        scheduler.Finish();
        write_back();
    } else {
        scheduler.DeferPriorityOperation(std::move(write_back));
    }
}

std::function<void()> TextureCache::RecordImageDownload(ImageId image_id, bool tracked_only,
                                                       VAddr write_limit, const char* writer) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return {};
    }
    const VAddr image_begin = image.info.guest_address;
    VAddr write_begin = image_begin;
    VAddr write_end = std::min(image_begin + image.info.guest_size, write_limit);
    if (tracked_only) {
        if (!image.IsTracked()) {
            return {};
        }
        write_begin = std::max(write_begin, image.track_addr);
        write_end = std::min(write_end, image.track_addr_end);
    }
    if (write_begin >= write_end) {
        return {};
    }
    // A readback no longer promotes the identity to native: a scaled backing is
    // transferred through a temporary native-extent copy inside Image::Download and
    // the plan keeps its scale (see scale_plan->upscaled_readback).
    const u32 download_size = image.info.guest_size;
    // Held until the write-back below ran, which may be deferred past later requests.
    const auto staging = runtime.GetStagingPool().Request(download_size, MemoryType::HostCached,
                                                          16, /*deferred=*/true);
    std::vector<vk::BufferImageCopy> copies;
    for (u32 mip = 0; mip < image.info.resources.levels; ++mip) {
        const auto& part = image.info.mips_layout[mip];
        copies.push_back({.bufferOffset = part.offset, .bufferRowLength = part.pitch,
            .bufferImageHeight = part.height,
            .imageSubresource = {image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                mip, 0, image.info.resources.layers},
            .imageExtent = {std::max(image.info.size.width >> mip, 1u),
                std::max(image.info.size.height >> mip, 1u),
                image.info.props.is_volume ? std::max(image.info.size.depth >> mip, 1u) : 1u}});
    }
    // Preserve the guest layout, including tiled surfaces and retained mip history.
    // A plain VkImageToBuffer copy would write linear pixels into tiled guest RAM.
    tile_manager.TileImage(image, copies, staging.buffer->Handle(), staging.offset,
                           download_size);
    const vk::MemoryBarrier2 host_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead};
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1, .pMemoryBarriers = &host_barrier});
    const u64 skip = write_begin - image_begin;
    return [this, staging, image_begin, writer, write_begin, write_size = write_end - write_begin,
            source = staging.mapped + skip] {
        staging.Invalidate();
        const Core::GuestWriteWatch::Scope watch_scope{writer, image_begin};
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(write_begin), source,
                                                  write_size);
        runtime.GetStagingPool().FreeDeferred(staging);
    };
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    const bool upload_diag = UploadDiagnostics::armed.load(std::memory_order_relaxed);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
            if (upload_diag) {
                // Page-fault probes report 1 or 8 bytes; HLE/host writes report their range.
                NoteUploadDirty(image, size <= 8 ? UploadDiagnostics::DirtySource::CpuFault
                                                 : UploadDiagnostics::DirtySource::HostWrite,
                                addr, size);
            }
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
            if (upload_diag) {
                NoteUploadDirty(image, UploadDiagnostics::DirtySource::CpuPageShared, addr, size);
            }
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size,
                                           UploadDiagnostics::DirtySource source, u64 writer) {
    std::scoped_lock lock{mutex};
    const bool upload_diag = UploadDiagnostics::armed.load(std::memory_order_relaxed);
    const bool keep_rendered = source == UploadDiagnostics::DirtySource::GpuStorageWrite &&
        UploadDiagnostics::ignore_storage_dirty.load(std::memory_order_relaxed);
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        if (keep_rendered && True(image.flags & ImageFlagBits::GpuModified)) {
            // Diagnostic A/B: the rendered image stays authoritative.
            UploadDiagnostics::ignored_storage_dirty.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Ensure image is reuploaded when accessed again.
        image.flags |= ImageFlagBits::GpuDirty;
        if (upload_diag) {
            NoteUploadDirty(image, source, address, max_size, writer);
        }
    });
}

namespace {

float HalfToFloat(u16 h) {
    const u32 sign = u32(h & 0x8000) << 16;
    const u32 exponent = (h >> 10) & 0x1f;
    u32 mantissa = h & 0x3ff;
    if (exponent == 31) return std::bit_cast<float>(sign | 0x7f800000u | mantissa << 13);
    if (exponent != 0) return std::bit_cast<float>(sign | (exponent + 112) << 23 | mantissa << 13);
    if (mantissa == 0) return std::bit_cast<float>(sign);
    u32 shift = 0; // subnormal half: normalize into a float
    do {
        mantissa <<= 1;
        ++shift;
    } while (!(mantissa & 0x400));
    return std::bit_cast<float>(sign | (113 - shift) << 23 | (mantissa & 0x3ff) << 13);
}

// Clear value that makes vkCmdClearColorImage write exactly `texel` (guest byte order) into a
// `format` image, or nullopt when no exact clear value is known for this format/texel.
std::optional<vk::ClearColorValue> ExactClearValue(vk::Format format, std::span<const u8> texel) {
    vk::ClearColorValue value{};
    const auto size_is = [&](size_t bytes) { return texel.size() == bytes; };
    const auto unorm8 = [&](u32 channels, bool bgra, bool srgb) -> bool {
        if (!size_is(channels)) return false;
        for (u32 c = 0; c < channels; ++c) {
            const u8 byte = texel[c];
            // sRGB clears are linear and re-encoded by the driver; only 0 and 1 round-trip
            // exactly. Alpha is always linear.
            if (srgb && c < 3 && byte != 0 && byte != 0xff) return false;
            const u32 dst = bgra && c < 3 ? 2 - c : c;
            value.float32[dst] = byte / 255.0f;
        }
        return true;
    };
    const auto unorm16 = [&](u32 channels) -> bool {
        if (!size_is(channels * 2)) return false;
        for (u32 c = 0; c < channels; ++c)
            value.float32[c] = (texel[c * 2] | texel[c * 2 + 1] << 8) / 65535.0f;
        return true;
    };
    const auto sfloat16 = [&](u32 channels) -> bool {
        if (!size_is(channels * 2)) return false;
        for (u32 c = 0; c < channels; ++c)
            value.float32[c] = HalfToFloat(u16(texel[c * 2] | texel[c * 2 + 1] << 8));
        return true;
    };
    const auto raw32 = [&](u32 channels) -> bool {
        if (!size_is(channels * 4)) return false;
        std::memcpy(value.uint32.data(), texel.data(), channels * 4);
        return true;
    };
    const auto uint_n = [&](u32 channels, u32 bytes) -> bool {
        if (!size_is(channels * bytes)) return false;
        for (u32 c = 0; c < channels; ++c) {
            u32 v = 0;
            std::memcpy(&v, texel.data() + c * bytes, bytes);
            value.uint32[c] = v;
        }
        return true;
    };
    bool exact = false;
    switch (format) {
    case vk::Format::eR8Unorm: exact = unorm8(1, false, false); break;
    case vk::Format::eR8G8Unorm: exact = unorm8(2, false, false); break;
    case vk::Format::eR8G8B8A8Unorm: exact = unorm8(4, false, false); break;
    case vk::Format::eB8G8R8A8Unorm: exact = unorm8(4, true, false); break;
    case vk::Format::eR8Srgb: exact = unorm8(1, false, true); break;
    case vk::Format::eR8G8Srgb: exact = unorm8(2, false, true); break;
    case vk::Format::eR8G8B8A8Srgb: exact = unorm8(4, false, true); break;
    case vk::Format::eB8G8R8A8Srgb: exact = unorm8(4, true, true); break;
    case vk::Format::eR16Unorm: exact = unorm16(1); break;
    case vk::Format::eR16G16Unorm: exact = unorm16(2); break;
    case vk::Format::eR16G16B16A16Unorm: exact = unorm16(4); break;
    case vk::Format::eR16Sfloat: exact = sfloat16(1); break;
    case vk::Format::eR16G16Sfloat: exact = sfloat16(2); break;
    case vk::Format::eR16G16B16A16Sfloat: exact = sfloat16(4); break;
    // 32-bit float/integer clear values are stored as-is: copy the bits.
    case vk::Format::eR32Sfloat:
    case vk::Format::eR32Uint:
    case vk::Format::eR32Sint: exact = raw32(1); break;
    case vk::Format::eR32G32Sfloat:
    case vk::Format::eR32G32Uint:
    case vk::Format::eR32G32Sint: exact = raw32(2); break;
    case vk::Format::eR32G32B32A32Sfloat:
    case vk::Format::eR32G32B32A32Uint:
    case vk::Format::eR32G32B32A32Sint: exact = raw32(4); break;
    case vk::Format::eR8Uint: exact = uint_n(1, 1); break;
    case vk::Format::eR8G8Uint: exact = uint_n(2, 1); break;
    case vk::Format::eR8G8B8A8Uint: exact = uint_n(4, 1); break;
    case vk::Format::eR16Uint: exact = uint_n(1, 2); break;
    case vk::Format::eR16G16Uint: exact = uint_n(2, 2); break;
    case vk::Format::eR16G16B16A16Uint: exact = uint_n(4, 2); break;
    default:
        // All-zero bits read as zero in every other non-depth, uncompressed color format.
        exact = std::ranges::all_of(texel, [](u8 byte) { return byte == 0; });
        break;
    }
    return exact ? std::optional{value} : std::nullopt;
}

} // namespace

UploadDiagnostics::FillOutcome TextureCache::ClearImagesForFill(VAddr address, u64 size,
                                                                std::span<const u32> pattern,
                                                                u32& images_cleared) {
    using UploadDiagnostics::FillOutcome;
    images_cleared = 0;
    const u64 period = pattern.size_bytes();
    if (!size || !period || period > 64) return FillOutcome::Pattern;
    std::array<u8, 64> bytes{};
    std::memcpy(bytes.data(), pattern.data(), period);

    std::scoped_lock lock{mutex};
    struct Target {
        Image* image;
        vk::ClearColorValue value;
    };
    boost::container::small_vector<Target, 4> targets;
    FillOutcome outcome = FillOutcome::Cleared;
    ForEachImageInRegion(address, size, [&](ImageId, Image& image) {
        if (outcome != FillOutcome::Cleared) return;
        const auto& info = image.info;
        const u64 offset = info.guest_address - address;
        if (info.guest_address < address || offset + info.guest_size > size ||
            offset % period != 0) {
            outcome = FillOutcome::Partial;
            return;
        }
        const u32 texel_bytes = info.num_bits / 8;
        if (info.props.is_depth || info.props.is_block || info.num_samples > 1 ||
            image.IsReencoded() || !texel_bytes || info.num_bits % 8 != 0 ||
            (period % texel_bytes != 0 && texel_bytes % period != 0)) {
            outcome = FillOutcome::Format;
            return;
        }
        // Every texel starts at a multiple of its size from the image base in any tiling, so
        // a byte stream with period `period` gives each texel the same bytes iff all texel-sized
        // chunks of the pattern are equal (or the texel spans whole periods).
        std::array<u8, 16> texel{};
        if (texel_bytes > texel.size()) {
            outcome = FillOutcome::Format;
            return;
        }
        for (u32 b = 0; b < texel_bytes; ++b) texel[b] = bytes[b % period];
        for (u64 at = texel_bytes; at < period; ++at) {
            if (bytes[at] != texel[at % texel_bytes]) {
                outcome = FillOutcome::Format;
                return;
            }
        }
        const auto value = ExactClearValue(image.backing->image.image_ci.format,
                                           std::span{texel.data(), texel_bytes});
        if (!value) {
            outcome = FillOutcome::Format;
            return;
        }
        targets.push_back({&image, *value});
    });
    if (outcome != FillOutcome::Cleared) return outcome;
    if (targets.empty()) return FillOutcome::NoImage;

    for (auto& [image, value] : targets) {
        const SubresourceRange range{.base = {.level = 0, .layer = 0},
                                     .extent = image->info.resources};
        image->Clear(vk::ClearValue{.color = value}, range);
        // Same state as after a draw into the image: the GPU copy is authoritative and the
        // fill replaces any pending CPU/GPU-side update of its whole range.
        image->flags |= ImageFlagBits::GpuModified;
        image->flags &= ~ImageFlagBits::Dirty;
    }
    images_cleared = static_cast<u32>(targets.size());
    coverage->fill_clears.fetch_add(images_cleared, std::memory_order_relaxed);
    return FillOutcome::Cleared;
}

std::optional<TextureCache::ImageSlice> TextureCache::FindImageSlice(VAddr address, u64 size) {
    std::scoped_lock lock{mutex};
    std::optional<ImageSlice> slice;
    u32 overlaps = 0;
    ForEachImageInRegion(address, size, [&](ImageId id, Image& image) {
        if (++overlaps > 1) return;
        const auto& info = image.info;
        const u32 layers = info.resources.layers;
        if (info.props.is_volume || !layers || address < info.guest_address) return;
        const u64 offset = address - info.guest_address;
        for (u32 level = 0; level < info.resources.levels; ++level) {
            // A mip level stores its layers back to back (ImageInfo::UpdateSize).
            const auto& mip = info.mips_layout[level];
            if (offset < mip.offset || offset - mip.offset >= mip.size) continue;
            const u64 layer_size = mip.size / layers;
            if (mip.size % layers || size != layer_size || (offset - mip.offset) % layer_size)
                return;
            slice = ImageSlice{id, level, u32((offset - mip.offset) / layer_size)};
            return;
        }
    });
    return overlaps == 1 ? slice : std::nullopt;
}

bool TextureCache::HasImageInRange(VAddr address, u64 size) {
    std::scoped_lock lock{mutex};
    bool found = false;
    ForEachImageInRegion(address, size, [&](ImageId, Image&) { found = true; });
    return found;
}

std::string TextureCache::DescribeImagesForDiagnostics(VAddr address, u64 size,
                                                       bool& base_match) {
    std::scoped_lock lock{mutex};
    std::string out;
    u32 count = 0;
    ForEachImageInRegion(address, size, [&](ImageId, Image& image) {
        base_match |= image.info.guest_address == address;
        if (++count > 4) return;
        out += fmt::format("[{:#x}+{:#x} {} {}x{} L{} M{} tile{} {}{}{}{}{}]",
                           image.info.guest_address, image.info.guest_size,
                           vk::to_string(image.info.pixel_format), image.info.size.width,
                           image.info.size.height, image.info.resources.layers,
                           image.info.resources.levels, static_cast<u32>(image.info.tile_mode),
                           image.IsScaled() ? "scaled" : "native",
                           True(image.flags & ImageFlagBits::GpuModified) ? " R" : "",
                           True(image.flags & ImageFlagBits::GpuDirty) ? " G" : "",
                           True(image.flags & ImageFlagBits::CpuDirty) ? " C" : "",
                           image.info.guest_address == address ? " base" : "");
    });
    if (count > 4) out += fmt::format("(+{} more)", count - 4);
    return out;
}

void TextureCache::NoteUploadDirty(const Image& image, UploadDiagnostics::DirtySource source,
                                   VAddr address, u64 size, u64 writer) {
    UploadDiagnostics::NoteDirty({image.info.guest_address, image.info.guest_size,
                                  static_cast<u32>(image.info.pixel_format)},
                                 source, address, size, writer);
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    scale_plans.Unmap(cpu_addr, size);
    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        const auto new_image_id =
            slot_images.insert(instance, scheduler, blit_helper, slot_image_views, new_info, this);
        RegisterImage(new_image_id);
        // The insertion may have grown slot_images, invalidating the cache_image reference.
        auto& source_image = slot_images[cache_image_id];

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = source_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;

        runtime.CopyColorAndDepth(&source_image, &new_image);

        // Free the cache image.
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    // An allocation extent is part of the identity. DRS must not keep the
    // previous larger backing merely because the requested bytes fit inside it.
    if (image_info.guest_address == cache_image.info.guest_address &&
        image_info.size != cache_image.info.size &&
        image_info.pixel_format == cache_image.info.pixel_format &&
        image_info.type == cache_image.info.type &&
        image_info.num_bits == cache_image.info.num_bits &&
        image_info.num_samples == cache_image.info.num_samples &&
        image_info.resources == cache_image.info.resources &&
        image_info.tile_mode == cache_image.info.tile_mode &&
        image_info.bank_swizzle == cache_image.info.bank_swizzle &&
        image_info.alt_tile == cache_image.info.alt_tile &&
        (binding == BindingType::RenderTarget || binding == BindingType::DepthTarget || binding == BindingType::VideoOut)) {
        // The descriptor changed, but the backing may still own the newest
        // history pixels. Preserve them through the existing guest-layout
        // download before rebuilding from the new layout. This is a conservative
        // DRS fallback, not a clear or a reinterpretation of stale guest RAM.
        // Incompatible format/layout reuse belongs to the existing alias/pool
        // path below; a matching address alone is not evidence of DRS.
        // Only the pages still write-tracked and inside the new descriptor are written. The
        // guest may have reused the rest (Bloodborne frees its render targets on a scene
        // change and its heap reallocates the range): re-tracking first, as before, dropped
        // the record of those CPU writes and the old pixels overwrote the heap's block headers.
        TouchImage(cache_image);
        if (True(cache_image.flags & ImageFlagBits::GpuModified)) {
            cache_image.DetachScalePlanForRetirement();
            DownloadImageMemory(cache_image_id, true, true,
                                image_info.guest_address + image_info.guest_size,
                                "image_writeback_drs");
        }
        FreeImage(cache_image_id);
        return {merged_image_id, -1, -1};
    }
    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        const bool pow2_padding_only =
            image_info.props.is_pow2 != cache_image.info.props.is_pow2 &&
            image_info.tile_mode == cache_image.info.tile_mode &&
            image_info.size == cache_image.info.size &&
            image_info.pitch == cache_image.info.pitch && image_info.resources.levels == 1 &&
            cache_image.info.resources.levels == 1 && image_info.resources.layers == 1 &&
            cache_image.info.resources.layers == 1;

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size || pow2_padding_only) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Same pow2:             {}\n"
                  "  Same alt tile:         {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,
                  bool(cache_image.info.props.is_pow2), cache_image.info.alt_tile,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,
                  bool(image_info.props.is_pow2), image_info.alt_tile,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (image_info.props.is_pow2 == cache_image.info.props.is_pow2),
                  (image_info.alt_tile == cache_image.info.alt_tile),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    merged_image.CopyMip(cache_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info, this);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    new_image.CopyImage(src_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);

    TrackImage(new_image_id);
    new_image.flags &= ~ImageFlagBits::Dirty;
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};
    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info, this, desc.type);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Texture && image.info.guest_address == info.guest_address &&
        image.info.resources.levels != info.resources.levels && !instance.ScalePolicy().legacy)
        image.ForceNative("streaming identity");
    image.ObserveUsage(desc.type);
    if (desc.type == BindingType::Storage) {
        image.ForceNative("storage or format alias");
    }
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Storage) {
        image.MarkGpuWrite(true);
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
        }
    }
    UpdateImage(image_id);
    if (desc.type == BindingType::Texture) image.MarkSampled();
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.MarkGpuWrite();
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaType::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaType::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.MarkGpuWrite();
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaType::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id =
                slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info, this);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        // Inserting the stencil image may have grown slot_images: `image` can no longer be used.
        stencil_image.AssociateDepth(image_id, slot_images[image_id].image_uid);
    }

    return slot_images[image_id].FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }

    image.CheckUploadBudget();
    // A codec fallback may have synchronously refreshed the replacement backing.
    if (False(image.flags & ImageFlagBits::Dirty)) return;
    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        // The image size should be less than page size to be considered MaybeCpuDirty
        // So this calculation should be very uncommon and reasonably fast
        // For now we'll just check up to 64 first pixels
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u32 w = std::min(image.info.size.width, u32(8));
        const u32 h = std::min(image.info.size.height, u32(8));

        const u32 s_w = image.info.props.is_block ? Common::DivCeil(w, 4u) : w;
        const u32 s_h = image.info.props.is_block ? Common::DivCeil(h, 4u) : h;
        const u32 size = s_w * s_h * (image.info.num_bits / 8);
        const u64 hash = XXH3_64bits(addr, size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    Common::Profiler::Scope refresh_scope{"Texture.Refresh"};
    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);

    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = image.DroppedMips(); m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    if (UploadDiagnostics::armed.load(std::memory_order_relaxed)) {
        NoteUploadDiagnostics(image, image_copies);
    }
    u64 upload_bytes = 0;
    for (const auto& copy : image_copies)
        upload_bytes += image.info.mips_layout[copy.imageSubresource.mipLevel].size;
    coverage->image_uploads.fetch_add(1, std::memory_order_relaxed);
    coverage->image_upload_bytes.fetch_add(upload_bytes, std::memory_order_relaxed);

    scheduler.EndRendering(Vulkan::RenderBreak::ImageUpload);

    // Crop the upload source before staging and detiling. A low-quality backing
    // never reserves or transfers guest mip0, including on later dirty updates.
    const auto upload_info = image.info.RetainedMipChain(image.DroppedMips());
    const auto skipped_bytes = upload_info.guest_address - image.info.guest_address;
    for (auto& copy : image_copies) copy.bufferOffset -= skipped_bytes;
    const auto [in_buffer, in_offset] = [&] {
        Common::Profiler::Scope scope{"Texture.Stage"};
        return buffer_cache.ObtainBufferForImage(upload_info.guest_address,
                                                 upload_info.guest_size);
    }();
    if (runtime.IsBufferAccessed(in_buffer, in_offset, upload_info.guest_size)) {
        runtime.FlushBarriers();
    }

    const auto [buffer, offset] = [&] {
        Common::Profiler::Scope scope{"Texture.Detile"};
        return tile_manager.DetileImage(in_buffer->Handle(), in_offset, upload_info);
    }();
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    Common::Profiler::Scope upload_scope{"Texture.Upload"};
    image.Upload(image_copies, buffer, offset, upload_info.guest_size);
    runtime.AccessBuffer(in_buffer, in_offset, upload_info.guest_size,
                         vk::PipelineStageFlagBits2::eAllCommands,
                         vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead);
}

void TextureCache::NoteUploadDiagnostics(const Image& image,
                                         std::span<const vk::BufferImageCopy> copies) {
    // Diagnostics only: the buffer cache holds newer bytes than guest memory when a GPU write
    // aliases the image, so guest hashes would not describe the uploaded content.
    const bool gpu_resident =
        buffer_cache.IsRegionGpuModified(image.info.guest_address, image.info.guest_size);
    boost::container::small_vector<u32, 16> mips;
    boost::container::small_vector<u64, 16> hashes;
    const u8* base = std::bit_cast<const u8*>(image.info.guest_address);
    for (const auto& copy : copies) {
        const u32 mip = copy.imageSubresource.mipLevel;
        mips.push_back(mip);
        if (!gpu_resident) {
            const auto& layout = image.info.mips_layout[mip];
            hashes.push_back(XXH3_64bits(base + layout.offset, layout.size));
        }
    }
    UploadDiagnostics::NoteUpload({
        .key = {image.info.guest_address, image.info.guest_size,
                static_cast<u32>(image.info.pixel_format)},
        .width = image.info.size.width,
        .height = image.info.size.height,
        .depth = image.info.size.depth,
        .layers = image.info.resources.layers,
        .levels = image.info.resources.levels,
        .tile_mode = static_cast<u32>(image.info.tile_mode),
        .tiled = bool(image.info.props.is_tiled),
        .scaled = image.IsScaled(),
        .cpu_dirty = True(image.flags & ImageFlagBits::CpuDirty),
        .maybe_cpu_dirty = True(image.flags & ImageFlagBits::MaybeCpuDirty),
        .gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty),
        .gpu_modified = True(image.flags & ImageFlagBits::GpuModified),
        .gpu_resident = gpu_resident,
        .frame = DebugState.GetFrameNum(),
        .mips = mips,
        .hashes = hashes,
    });
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth) {
    // Compare and plain uses of one S# need separate samplers.
    const u64 hash = HashCombine(XXH3_64bits(&sampler, sizeof(sampler)), is_depth);

    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] =
        samplers.try_emplace(hash, instance, sampler, border_color_base, is_depth);
    if (new_sampler) {
        samplers.at(hash).lru_id = sampler_lru_cache.Insert(hash, gc_tick);
    } else {
        sampler_lru_cache.Touch(it->second.lru_id, gc_tick);
    }

    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) try {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
} catch (const std::exception& error) {
    const auto& info = slot_images[image_id].info;
    LOG_ERROR(Render, "Image watch failed: address={:#x} bytes={:#x} extent={}x{}x{} "
                     "pitch={} levels={} layers={} tile={} bpp={}: {}",
              info.guest_address, info.guest_size, info.size.width, info.size.height,
              info.size.depth, info.pitch, info.resources.levels, info.resources.layers,
              static_cast<u32>(info.tile_mode), info.num_bits, error.what());
    throw;
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::GarbageCollectIdleAssets() {
    const auto policy = instance.MemoryPolicy();
    if (!policy.idle_asset_submits || gc_tick < policy.idle_asset_submits || gc_tick % 16 != 0)
        return;
    std::scoped_lock lock{mutex};
    u32 remaining = policy.idle_evictions_per_sweep;
    // Only uploaded assets whose current contents remain reconstructible from
    // guest RAM. GPU-produced/aliased/render targets retain their existing rules.
    lru_cache.ForEachItemBelow(gc_tick - policy.idle_asset_submits, [&](ImageId id) {
        if (!remaining) return true;
        auto& image = slot_images[id];
        const auto& plan = image.ScalePlan();
        if (plan.domain != ScaleDomain::Asset || plan.origin != ScaleOrigin::Upload ||
            True(image.flags & (ImageFlagBits::GpuModified | ImageFlagBits::GpuDirty))) return false;
        --remaining;
        ++idle_asset_evictions;
        for (const auto& backing : image.backing_images) {
            VmaAllocationInfo allocation{};
            vmaGetAllocationInfo(instance.GetAllocator(), backing.image.allocation, &allocation);
            idle_asset_retired_bytes += allocation.size;
        }
        FreeImage(id); // Existing GPU + host-submit deferred destruction.
        return false;
    });
}

void TextureCache::GarbageCollectImages() {
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
        const u32 state = aggresive ? 2 : pressured ? 1 : 0;
        if (state != gc_logged_state) {
            // Once per transition: pressure decisions evict GPU-modified images and
            // are otherwise invisible in the trace.
            LOG_INFO(Render_Vulkan,
                     "Texture GC {}: used {} MiB (trigger {} / pressure {} / critical {} MiB, budget {})",
                     state == 2 ? "aggressive" : state == 1 ? "pressured" : "idle",
                     total_used_memory >> 20, trigger_gc_memory >> 20, pressure_gc_memory >> 20,
                     critical_gc_memory >> 20, instance.CanReportMemoryUsage() ? "driver" : "default");
            gc_logged_state = state;
        }
        if (pressured) coverage->gc_pressured_ticks.fetch_add(1, std::memory_order_relaxed);
    };
    // Evicted GPU-modified images are written back to guest memory before they stop being
    // tracked, and only where their pages are still tracked. A write-back deferred until the
    // GPU copy completes landed on memory the guest may have reused in between -- freeing the
    // image untracks its pages, so CPU writes there were no longer seen -- and a freed image's
    // memory reused as heap got its block headers overwritten with old pixels. This costs one
    // GPU wait per GC pass that evicts GPU-modified images, which only happens under pressure.
    std::vector<ImageId> evicted_modified;
    const auto write_back_evicted = [&] {
        if (evicted_modified.empty()) {
            return;
        }
        std::vector<std::function<void()>> writes;
        for (const ImageId image_id : evicted_modified) {
            if (auto write = RecordImageDownload(image_id, true, ~VAddr{0},
                                                 "image_writeback_gc")) {
                writes.push_back(std::move(write));
                static std::atomic<u32> logged{};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                    const auto& image = slot_images[image_id];
                    LOG_INFO(Render_Vulkan,
                             "Texture GC write-back: {:#x}-{:#x} tracked {:#x}-{:#x} {}x{} {}",
                             image.info.guest_address,
                             image.info.guest_address + image.info.guest_size, image.track_addr,
                             image.track_addr_end, image.info.size.width, image.info.size.height,
                             vk::to_string(image.info.pixel_format));
                }
            }
        }
        if (!writes.empty()) {
            scheduler.Finish();
            for (auto& write : writes) {
                write();
            }
        }
        for (const ImageId image_id : evicted_modified) {
            FreeImage(image_id);
        }
        evicted_modified.clear();
    };
    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        auto& image = slot_images[image_id];
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        if (tiled && download) {
            // This is a workaround for now. We can't handle non-linear image downloads.
            return false;
        }
        if (download && !pressured) {
            return false;
        }
        coverage->gc_frees.fetch_add(1, std::memory_order_relaxed);
        if (download) {
            coverage->gc_downloads.fetch_add(1, std::memory_order_relaxed);
            evicted_modified.push_back(image_id);
        } else {
            FreeImage(image_id);
        }
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    write_back_evicted();

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
        write_back_evicted();
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](u64 hash) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        const size_t lru_id = samplers.at(hash).lru_id;
        samplers.erase(hash);
        sampler_lru_cache.Free(lru_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::RunGarbageCollector() {
    PublishMemoryDiagnostics();
    SCOPE_EXIT {
        ++gc_tick;
    };

    GarbageCollectIdleAssets();
    GarbageCollectImages();
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(const Image& image) {
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    for (auto& backing : image.backing_images)
        VmaDiagnostics::Tag(instance.GetAllocator(), backing.image.allocation, nullptr, true);
    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
}

} // namespace VideoCore
