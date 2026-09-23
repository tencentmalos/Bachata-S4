// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <queue>
#include <tsl/robin_map.h>

#include "common/lru_cache.h"
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/multi_level_page_table.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/scale_coverage.h"
#include "video_core/texture_cache/tile_manager.h"
#include "video_core/texture_cache/upload_diagnostics.h"

namespace AmdGpu {
struct Liverpool;
}

namespace VideoCore {

class BufferCache;
class PageManager;

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    using ImageIds = boost::container::small_vector<ImageId, 16>;

    struct Traits {
        using Entry = ImageIds;
        static constexpr size_t AddressSpaceBits = 40;
        static constexpr size_t FirstLevelBits = 10;
        static constexpr size_t PageBits = 20;
    };
    using PageTable = MultiLevelPageTable<Traits>;

public:
    using BindingType = ScaleUse;

    struct ImageDesc {
        ImageInfo info;
        ImageViewInfo view_info;
        BindingType type{BindingType::Texture};

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {}
        ImageDesc(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint)
            : info{buffer, hint}, view_info{buffer}, type{BindingType::RenderTarget} {}
        ImageDesc(const AmdGpu::DepthBuffer& buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl, VAddr htile_address, AmdGpu::CbDbExtent hint,
                  bool write_buffer = false)
            : info{buffer, view.NumSlices(), htile_address, hint, write_buffer},
              view_info{buffer, view, ctl}, type{BindingType::DepthTarget} {}
        ImageDesc(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr cpu_address)
            : info{group, cpu_address}, type{BindingType::VideoOut} {}
    };

public:
    TextureCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache, PageManager& tracker);
    ~TextureCache();

    TileManager& GetTileManager() noexcept {
        return tile_manager;
    }

    BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    /// Invalidates any image in the logical page range.
    void InvalidateMemory(VAddr addr, size_t size);

    /// Marks an image as dirty if it exists at the provided address. `source` only labels
    /// upload diagnostics (storage-buffer shader write vs CP/DMA copy).
    void InvalidateMemoryFromGPU(VAddr address, size_t max_size,
                                 UploadDiagnostics::DirtySource source =
                                     UploadDiagnostics::DirtySource::GpuStorageWrite,
                                 u64 writer = 0);

    /// A GPU kernel fills [address, address + size) with `pattern` repeated from `address`.
    /// When every cached image overlapping the range lies fully inside it, starts on a pattern
    /// boundary and sees one uniform texel, clears those images to that texel (they become the
    /// authoritative GPU copy, like render targets) and returns Cleared: the caller skips the
    /// dispatch. Otherwise nothing is recorded and the caller dispatches as before.
    [[nodiscard]] UploadDiagnostics::FillOutcome ClearImagesForFill(VAddr address, u64 size,
                                                                    std::span<const u32> pattern,
                                                                    u32& images_cleared);

    /// One (level, layer) subresource of a non-volume cached image.
    struct ImageSlice {
        ImageId id;
        u32 level{};
        u32 layer{};
    };
    /// The single cached image whose one subresource occupies exactly [address, address + size),
    /// e.g. a cube face cleared on its own. nullopt when no or several cached images overlap the
    /// range, or the range is not one whole subresource.
    [[nodiscard]] std::optional<ImageSlice> FindImageSlice(VAddr address, u64 size);

    /// True when any cached image overlaps [address, address + size).
    [[nodiscard]] bool HasImageInRange(VAddr address, u64 size);

    /// Upload diagnostics only: describes cached images overlapping a range; `base_match` is
    /// set when one of them starts exactly at `address` (the case InvalidateMemoryFromGPU marks).
    [[nodiscard]] std::string DescribeImagesForDiagnostics(VAddr address, u64 size,
                                                           bool& base_match);

    /// Evicts any images that overlap the unmapped range.
    void UnmapMemory(VAddr cpu_addr, size_t size);

    /// Schedules a copy of pending images for download back to CPU memory.
    void ProcessDownloadImages();

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// Retrieves an image view with the properties of the specified image id.
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the render target with specified properties
    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the depth target with specified properties
    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// Updates image contents if it was modified by CPU.
    void UpdateImage(ImageId image_id) {
        std::scoped_lock lock{mutex};
        Image& image = slot_images[image_id];
        TrackImage(image_id);
        TouchImage(image);
        RefreshImage(image);
    }

    // Android SBS bring-up diagnostic.  Forces one GPU image readback so the
    // presenter can distinguish a black guest render target from a bad host
    // composition without changing the normal frame path.
    void ReadbackImageForDiagnostics(ImageId image_id);

    /// Resolves overlap between existing cache image and pending merged image
    [[nodiscard]] std::tuple<ImageId, int, int> ResolveOverlap(const ImageInfo& info,
                                                               BindingType binding,
                                                               ImageId cache_img_id,
                                                               ImageId merged_image_id);

    /// Resolves depth overlap and either re-creates the image or returns existing one
    [[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                              ImageId cache_img_id);

    /// Creates a new image with provided image info and copies subresources from image_id
    [[nodiscard]] ImageId ExpandImage(const ImageInfo& info, ImageId image_id);

    std::shared_ptr<ResourceScalePlan> AcquireScalePlan(const ImageInfo& info, ScaleUse use);
    void RecordAttachmentDraw(std::span<const ImageId> attachments, u64 fragment_hash,
                              bool scaled, bool began_rendering, bool resumed);
    void RecordNativeFallback(ScaleReason reason) {
        ++native_fallbacks[u32(reason)];
        coverage->native_promotions.fetch_add(1, std::memory_order_relaxed);
    }
    /// A pass with at least one scaled attachment was forced native; cause bits from
    /// NativePassCause. Called once per such pass, never on the scaled fast path.
    void RecordNativePassCause(u32 causes) {
        constexpr auto o = std::memory_order_relaxed;
        if (causes & NativePassSideEffects) coverage->native_pass_side_effects.fetch_add(1, o);
        if (causes & NativePassMsaa) coverage->native_pass_msaa.fetch_add(1, o);
        if (causes & NativePassAttachment) coverage->native_pass_attachment.fetch_add(1, o);
        if (causes & NativePassMismatch) coverage->native_pass_mismatch.fetch_add(1, o);
    }
    void RecordUpscaledReadback() { coverage->upscaled_readbacks.fetch_add(1, std::memory_order_relaxed); }
    void RecordFusedReadback() {
        coverage->upscaled_readbacks.fetch_add(1, std::memory_order_relaxed);
        coverage->fused_readbacks.fetch_add(1, std::memory_order_relaxed);
    }
    void RecordImageBufferSync(u64 bytes) {
        coverage->image_buffer_syncs.fetch_add(1, std::memory_order_relaxed);
        coverage->image_buffer_sync_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    void RecordScaledBlitCopy() { coverage->scaled_blit_copies.fetch_add(1, std::memory_order_relaxed); }
    const std::shared_ptr<ScaleCoverageCounters>& Coverage() const { return coverage; }

    /// Reuploads image contents.
    void RefreshImage(Image& image);

    /// Upload diagnostics (default off): record which write dirtied an image and hash the
    /// guest bytes of the mips about to be uploaded. Called only while armed.
    void NoteUploadDirty(const Image& image, UploadDiagnostics::DirtySource source, VAddr address,
                         u64 size, u64 writer = 0);
    void NoteUploadDiagnostics(const Image& image, std::span<const vk::BufferImageCopy> copies);

    /// Retrieves the sampler that matches the provided S# descriptor.
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base,
                                         bool is_depth);

    /// Retrieves the image with the specified id.
    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        TouchImage(image);
        return image;
    }

    /// Retrieves the image view with the specified id.
    [[nodiscard]] ImageView& GetImageView(ImageId id) {
        return slot_image_views[id];
    }

    /// Get the associated depth stencil image if it is still valid.
    ImageId GetAssociatedDepth(Image& image) {
        if (!image.depth_id) {
            return {};
        }
        if (slot_images.is_allocated(image.depth_id)) {
            auto& depth_image = slot_images[image.depth_id];
            if (depth_image.image_uid == image.depth_uid &&
                depth_image.flags & ImageFlagBits::Registered) {
                return image.depth_id;
            }
        }
        // The linked depth image is no longer valid, disassociate it.
        image.DisassociateDepth();
        return {};
    }

    enum class MetaType {
        CMask,
        FMask,
        HTile,
    };

    /// Returns meta type if the specified address is a metadata surface.
    std::optional<MetaType> IsMeta(VAddr address) const {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it->second.type;
        }
        return std::nullopt;
    }

    /// Returns true if a slice of the specified metadata surface has been cleared.
    bool IsMetaCleared(VAddr address, u32 slice) const {
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
    }

    /// Clears all slices of the specified metadata surface.
    bool ClearMeta(VAddr address) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            return true;
        }
        return false;
    }

    /// Updates the state of a slice of the specified metadata surface.
    bool TouchMeta(VAddr address, u32 slice, bool is_clear) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            if (is_clear) {
                it.value().clear_mask |= 1u << slice;
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    /// Runs the garbage collector.
    void RunGarbageCollector();

    template <typename Func>
    void ForEachImageInRegion(VAddr cpu_addr, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        ImageIds images;
        ForEachPage(cpu_addr, size, [this, &images, cpu_addr, size, func](u64 page) {
            const auto it = page_table.find(page);
            if (it == nullptr) {
                if constexpr (BOOL_BREAK) {
                    return false;
                } else {
                    return;
                }
            }
            for (const ImageId image_id : *it) {
                Image& image = slot_images[image_id];
                if (image.flags & ImageFlagBits::Picked) {
                    continue;
                }
                if (!image.Overlaps(cpu_addr, size)) {
                    continue;
                }
                image.flags |= ImageFlagBits::Picked;
                images.push_back(image_id);
                if constexpr (BOOL_BREAK) {
                    if (func(image_id, image)) {
                        return true;
                    }
                } else {
                    func(image_id, image);
                }
            }
            if constexpr (BOOL_BREAK) {
                return false;
            }
        });
        for (const ImageId image_id : images) {
            slot_images[image_id].flags &= ~ImageFlagBits::Picked;
        }
    }

private:
    void PublishMemoryDiagnostics();
    unsigned long long memory_diagnostics_epoch{};
    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachPage(PAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> Traits::PageBits;
        for (u64 page = addr >> Traits::PageBits; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Copies image memory back to CPU.
    void DownloadImageMemory(ImageId image_id, bool sync = false);

    /// Records the copy of a GPU-modified image into a download buffer and returns the step
    /// that writes it to guest memory, to run once the GPU has executed the copy. With
    /// `tracked_only` only the part whose pages are still write-tracked is written: anywhere
    /// else a CPU write would have gone unnoticed. Empty when there is nothing to write.
    std::function<void()> RecordImageDownload(ImageId image_id, bool tracked_only);

    /// Thread function for copying downloaded images out to CPU memory.
    void DownloadedImagesThread(const std::stop_token& token);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, VAddr cpu_addr);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageId image_id);
    void TrackImageHead(ImageId image_id);
    void TrackImageTail(ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageId image_id);
    void UntrackImageHead(ImageId image_id);
    void UntrackImageTail(ImageId image_id);

    void MarkAsMaybeDirty(ImageId image_id, Image& image);

    /// Removes the image and any views/surface metas that reference it.
    void DeleteImage(ImageId image_id);

    /// Touch the image in the LRU cache.
    void TouchImage(const Image& image);

    void FreeImage(ImageId image_id) {
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id);
    }

    void GarbageCollectImages();
    void GarbageCollectIdleAssets();
    void GarbageCollectSamplers();

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    BlitHelper blit_helper;
    TileManager tile_manager;
    ScalePlanTable scale_plans;
    // Shared with the StatusLayer; producer increments are relaxed atomics.
    std::shared_ptr<ScaleCoverageCounters> coverage = std::make_shared<ScaleCoverageCounters>();
    std::array<u64, u32(ScaleReason::Count)> native_fallbacks{};
    // Last logged garbage-collector state (0 idle, 1 pressured, 2 aggressive).
    u32 gc_logged_state{};
    struct AttachmentCounts { u64 draws{}, passes{}, resumed{}; };
    // Bounded aggregate by shader, dimensions, reason mask, scale and attachment count;
    // windowed: cleared each time gpu_memory diagnostics are published.
    std::map<std::array<u64, 7>, AttachmentCounts> attachment_groups;
    u64 attachment_group_overflow{};
    u64 idle_asset_evictions{}, idle_asset_retired_bytes{};
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler> samplers;
    std::unordered_set<ImageId> download_images;
    u64 total_used_memory = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 total_used_samplers = 0;
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    bool readback_linear_images;
    PageTable page_table;
    std::mutex mutex;
    std::mutex samplers_mutex;
    std::mutex download_images_mutex;
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
};

} // namespace VideoCore
