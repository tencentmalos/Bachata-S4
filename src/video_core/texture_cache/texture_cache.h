// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <boost/icl/interval_set.hpp>
#include <boost/icl/separate_interval_set.hpp>
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

namespace Vulkan {
class Runtime;
}

namespace VideoCore {

class BufferCache;
class PageManager;

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;
    // Driver budget headroom below which collection runs under pressure / aggressively.
    static constexpr u64 BUDGET_PRESSURE_HEADROOM = 1_GB;
    static constexpr u64 BUDGET_CRITICAL_HEADROOM = 512_MB;

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
        // A storage write that may follow the image's render scale (see
        // Rasterizer::ScalableStorageWrite); FindImage keeps the scale plan of such an image.
        bool scalable_write{};

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
                 Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache,
                 PageManager& tracker);
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
    /// With `cover_range`, the images must also cover every byte of the range together (the
    /// skipped dispatch would otherwise leave the bytes between them unwritten): Partial if not.
    [[nodiscard]] UploadDiagnostics::FillOutcome ClearImagesForFill(VAddr address, u64 size,
                                                                    std::span<const u32> pattern,
                                                                    u32& images_cleared,
                                                                    bool cover_range = false);

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

    /// Evicts any images that overlap the unmapped range. `pool_commit`: the range is a pool
    /// commit, whose images decommitted earlier may come back (see DecommitMemory).
    void UnmapMemory(VAddr cpu_addr, size_t size, bool pool_commit = false);

    /// Pooled memory in [addr, addr + size) is decommitted. Like UnmapMemory, except that the
    /// images the GPU wrote and the CPU did not, entirely inside the range, are kept alive
    /// (`upload_diag pool_revive`): when the guest commits the range again and the CPU has not
    /// written it, the same image created there takes its old contents instead of uploading
    /// the new pages, as if the commit got the same physical pages back.
    void DecommitMemory(VAddr addr, size_t size);

    /// Pooled memory was committed over [addr, addr + size): its contents are undefined until the
    /// CPU writes it. Images the GPU writes first (render, depth or storage targets) created in
    /// pages that held such images before start without uploading guest memory
    /// (`upload_diag pool_undefined`).
    void MarkUndefinedContent(VAddr addr, size_t size);

    /// Guest memory in [addr, addr + size) is about to hold meaningful bytes from another source
    /// (a GPU write-back, a buffer readback): it is no longer undefined.
    void ReleaseUndefinedContent(VAddr addr, size_t size) {
        ReleaseUndefinedContent(addr, size, UndefinedRelease::Range);
    }

    /// Copies the images queued for write-back to guest memory: at once after waiting for the
    /// GPU (`sync`), else once the GPU completed them (deferred write-backs, see
    /// WaitPendingWritebacks). Returns the number of images.
    size_t ProcessDownloadImages(bool sync = true);

    /// Images are queued for write-back and UploadDiagnostics::async_writeback lets them wait
    /// for GPU completion.
    bool AsyncDownloadsAllowed();

    /// Deferred write-backs that have not written guest memory yet.
    bool HasPendingWritebacks() const {
        return pending_writebacks_count.load(std::memory_order_acquire) != 0;
    }

    /// Blocks until every deferred write-back overlapping [addr, addr + size) wrote guest
    /// memory: host reads of that memory would find the bytes from before the GPU wrote the
    /// image, and host writes would be overwritten by the late write-back. Command processor
    /// thread (it may submit the work the write-backs follow); one atomic load when no
    /// write-back is pending.
    void WaitPendingWritebacks(VAddr addr, u64 size, UploadDiagnostics::WritebackReader reader) {
        if (pending_writebacks_count.load(std::memory_order_acquire) == 0) [[likely]] {
            return;
        }
        // The bounds only shrink off this thread: a stale value is wider, never narrower.
        if (addr >= pending_writebacks_hi.load(std::memory_order_relaxed) ||
            addr + size <= pending_writebacks_lo.load(std::memory_order_relaxed)) {
            return;
        }
        WaitPendingWritebacksSlow(addr, size, reader);
    }
    /// Blocks until every deferred write-back wrote guest memory (fences performed ahead of the
    /// GPU follow them). Command processor thread.
    void WaitAllPendingWritebacks(UploadDiagnostics::WritebackReader reader) {
        if (HasPendingWritebacks()) {
            WaitPendingWritebacksSlow(0, ~VAddr{0}, reader);
        }
    }

    /// GPU replay: writes every image the GPU modified, and the CPU has not written since, back
    /// to guest memory in guest layout. Command processor thread; waits for the GPU.
    void WriteBackGpuModified();
    /// GPU replay: the scale plans that must stay native, as a ScalePlans payload, and back.
    std::vector<u8> SaveScalePlans();
    void LoadScalePlans(std::span<const u8> payload);

    struct ImageContentHash {
        u64 uid;
        VAddr address;
        vk::Extent3D extent;
        u32 levels;
        u32 layers;
        vk::Format format;
        /// XXH3 over every level and layer of the host image (depth aspect for depth images);
        /// 0 with a reason when the image cannot be copied to a buffer.
        u64 hash;
        const char* skipped;
    };
    /// GPU replay diagnostics: hashes the images written since their last hash (their write
    /// epoch changed). Command processor thread; waits for the GPU.
    /// With dump_prefix, level 0 / layer 0 of each hashed image is also written to
    /// "<dump_prefix>_<uid>_<address>_<w>x<h>x<d>_<format>.bin" (tightly packed rows).
    std::vector<ImageContentHash> HashWrittenImages(std::unordered_map<u64, u64>& hashed_epochs,
                                                    const std::string& dump_prefix = {});

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// The image starting at `address` whose GPU-written contents are newer than guest memory
    /// and the buffer cache (written by a draw or dispatch, not since overwritten through
    /// memory), for a raw buffer read of that range. With several such aliases only one of
    /// exactly `size` bytes is taken; otherwise none.
    [[nodiscard]] ImageId FindGpuWrittenImage(VAddr address, size_t size);

    /// A GPU buffer write (fill, copy) to guest memory: GPU-written images overlapping the range
    /// no longer hold the newest contents of that memory.
    void NoteBufferWrite(VAddr address, size_t size);

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
    void PublishImageDiagnostics(u64 before_shader = 0);

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

    /// A newly created image contains cached images the GPU wrote as single mips of it (e.g.
    /// UE4 renders every face of mip 1 of a scratch cube before the cube is first sampled):
    /// copies them in after the guest upload and frees them. Guest memory never received those
    /// GPU writes. Returns the number of images adopted.
    u32 AdoptGpuSubresources(ImageId image_id);

    /// A texture request smaller than the GPU-written image `cache` at the same base address,
    /// reading its top-left texels (same layout, single 2D mip and layer).
    bool IsCornerOf(const ImageInfo& info, BindingType type, const Image& cache) const;

    /// Copies the corner of its source into a corner texture when the source was written since.
    void RefreshCorner(ImageId image_id);

    /// Frees the least recently used corner textures at `address` so a new one keeps at most
    /// MaxCornerImages there. Only copies of their source that the commands being recorded do not
    /// use are freed.
    void EvictCornerImages(VAddr address, const ImageIds& candidates);

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

    enum class UndefinedRelease {
        Range,    ///< Only the pages of the range become defined (decommit, GPU write-back).
        CpuWrite, ///< The CPU wrote the range: its whole commit becomes defined.
    };
    /// True when every page of [addr, addr + size) lies in one committed run the CPU has not
    /// written since the commit.
    bool IsUndefinedContent(VAddr addr, size_t size);
    /// Drops the undefined-content state, and the page watchers holding it.
    void ReleaseUndefinedContent(VAddr addr, size_t size, UndefinedRelease kind);
    /// Records pages that held an image the GPU wrote first; later commits protect only those.
    void LearnGpuWriteRange(VAddr addr, size_t size);
    /// True when `image`, which the GPU writes first, may start with undefined contents.
    bool TrySkipUndefinedUpload(const Image& image);
    /// Marks the undefined runs overlapping [addr, addr + size) as used by an image.
    void NoteUndefinedUse(VAddr addr, size_t size);

    /// Upload diagnostics (default off): record which write dirtied an image and hash the
    /// guest bytes of the mips about to be uploaded. Called only while armed.
    void NoteUploadDirty(const Image& image, UploadDiagnostics::DirtySource source, VAddr address,
                         u64 size, u64 writer = 0);
    void NoteUploadDiagnostics(const Image& image, std::span<const vk::BufferImageCopy> copies);
    /// The image is freed (or kept) with its range: records its GPU contents' writer.
    void NoteFreedGpuImage(const Image& image, bool kept);
    /// The image is bound for writing by the current action (diagnostics only).
    static void NoteGpuWriter(Image& image, UploadDiagnostics::WriteKind kind) {
        image.last_write_kind = static_cast<u8>(kind);
        image.last_writer = UploadDiagnostics::current_action.load(std::memory_order_relaxed);
    }

    /// A color target of at most TinyTargetPixels pixels was bound for rendering: queues its
    /// write-back unless it is rendered every submit (see UploadDiagnostics::tiny_writeback).
    void NoteTinyTargetWrite(ImageId image_id, Image& image);
    /// `upload_diag tiny_writeback dump`: keeps the last image lookups, uploads and draws; the
    /// record is printed when a single-row tiny target (an SH map) is first written back.
    void TinyTrace(std::string entry);
    void DumpTinyTrace(VAddr trigger);

    /// Retrieves the sampler that matches the provided S# descriptor.
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base,
                                         bool is_depth);

    /// GetSampler for the GPU command thread (the thread that runs the garbage collector):
    /// a direct-mapped memo of recent S# lookups in front of the hashed, locked map. A hit
    /// skips the hash and the lock. The first use of an entry in each GC tick still goes
    /// through GetSampler, so the sampler's LRU tick is the same as without the memo, and any
    /// sampler eviction invalidates every entry (upload_diag sampler_memo).
    [[nodiscard]] vk::Sampler GetCommandSampler(const AmdGpu::Sampler& sampler,
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

    /// Changes whenever an image is registered or unregistered, i.e. whenever FindImage could
    /// resolve the same descriptor to another image.
    /// Changes whenever an image is invalidated or untracked: while it is unchanged, a bound
    /// image that was refreshed and tracked needs neither again.
    [[nodiscard]] u64 ContentGeneration() const noexcept {
        return content_generation.load(std::memory_order_acquire);
    }
    [[nodiscard]] u64 ImageSetGeneration() const noexcept {
        return image_set_generation.load(std::memory_order_acquire);
    }

    /// FindImage's per-use bookkeeping, for a sampled image the caller found with FindImage
    /// earlier in the current ImageSetGeneration. False when the image has been freed since
    /// (another thread unmapped it): the caller looks it up again.
    bool TouchFoundTexture(ImageId image_id);
    /// The same for an image of any binding type (render and depth targets).
    bool TouchFoundImage(ImageId image_id, BindingType type);

    /// Changes whenever a metadata surface (CMASK/FMASK/HTILE) is marked cleared: a draw that
    /// found its targets not cleared finds them so again while it is unchanged.
    [[nodiscard]] u64 MetaClearGeneration() const noexcept {
        return meta_clear_generation;
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

    /// The host color image is expanded; single-sample GCN CMASK readers must see
    /// expanded tiles too, not the guest's stale fast-clear metadata. Multisample
    /// CMASK also describes FMASK and is not covered by this representation.
    bool IsSingleSampleCMask(VAddr address) const {
        const auto it = surface_metas.find(address);
        return it != surface_metas.end() && it->second.type == MetaType::CMask &&
               it->second.num_samples == 1;
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
            ++meta_clear_generation;
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
                ++meta_clear_generation;
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

    // Renderer-thread only; a no-op unless a diagnostic snapshot was requested.
    void PublishMemoryDiagnostics();

private:
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

    /// Copies image memory back to CPU. `tracked_only`, `write_end` and `writer` as for
    /// RecordImageDownload.
    void DownloadImageMemory(ImageId image_id, bool sync = false, bool tracked_only = false,
                             VAddr write_end = ~VAddr{0},
                             const char* writer = "image_writeback");

    /// One image write-back: the guest range it writes and the guest bytes there when it was
    /// decided. Bytes that differ from `before` when it lands were written by someone else in
    /// between (the guest reusing the memory, a host copy) and keep their value: the write-back
    /// only replaces what nobody changed.
    struct WritebackState {
        VAddr begin{};
        u64 size{};
        /// Pending write-back sequence; 0 for one written right after a GPU wait.
        u64 sequence{};
        std::vector<u8> before;
        /// False when the range has no backing to snapshot: written whole, as before.
        bool have_before{};
    };

    /// WaitPendingWritebacks past the lock-free checks.
    void WaitPendingWritebacksSlow(VAddr addr, u64 size,
                                   UploadDiagnostics::WritebackReader reader);
    /// Queues a deferred write-back at the current tick: snapshots its range and returns its
    /// sequence. Under the pending lock, so no earlier write-back lands between the two.
    u64 AddPendingWriteback(const std::shared_ptr<WritebackState>& state);
    /// The deferred write-back `sequence` wrote guest memory. Priority thread.
    void CompletePendingWriteback(u64 sequence);
    /// Writes a downloaded image over guest memory, keeping bytes changed since the snapshot.
    /// A pending write-back also updates the snapshots of later pending write-backs of the same
    /// bytes, so they do not take its bytes for a foreign write.
    void LandWriteback(WritebackState& state, const u8* data);

    /// Records the copy of a GPU-modified image into a download buffer and returns the step
    /// that writes it to guest memory, to run once the GPU has executed the copy. With
    /// `tracked_only` only the part whose pages are still write-tracked is written: anywhere
    /// else a CPU write would have gone unnoticed. Nothing at or past `write_end` is written.
    /// `writer` names the path for the guest write watch. Empty when there is nothing to write.
    /// The guest bytes are snapshotted now, or, with `deferred`, when the caller registers the
    /// returned state (AddPendingWriteback).
    std::function<void()> RecordImageDownload(ImageId image_id, bool tracked_only,
                                              VAddr write_end = ~VAddr{0},
                                              const char* writer = "image_writeback",
                                              std::shared_ptr<WritebackState>* deferred = nullptr);

    /// Logs an image retired by a same-address extent change. With the guest write watch
    /// armed it also reads mip 0 into a staging buffer (never into guest memory) and counts
    /// NaN and zero words, to tell never-drawn backings from drawn NaN content.
    void DescribeRetiredImage(ImageId image_id, const ImageInfo& next);

public:
    /// `upload_diag image_stats`: downloads the first level and logs per-layer channel stats.
    void LogImageStats(ImageId image_id);

private:

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

    /// Drops the surface metas registered by the image.
    void ReleaseImageMetas(const Image& image);

    /// A depth <-> color reinterpretation of one surface can alternate every frame (e.g. a
    /// shadow atlas written by compute and then rendered as depth). The replaced image is
    /// parked, unregistered but alive, so the next conversion back reuses it instead of
    /// allocating a new one. Parked contents are never read: reuse overwrites all of them.
    void ParkImage(ImageId image_id);
    ImageId TakeParkedImage(const ImageInfo& info);
    /// Deletes parked images overlapping [address, address + size) or older than max_age ticks.
    void ReleaseParkedImages(VAddr address, u64 size, u64 max_age);

    /// Keeps an image of decommitted pool memory alive, unregistered (see DecommitMemory).
    void KeepDecommittedImage(ImageId image_id);
    /// The kept image with exactly `info`, when its range is still undefined (committed again
    /// and not written by the CPU) and the buffer cache holds no GPU data there. Registered by
    /// the caller.
    ImageId TakeDecommittedImage(const ImageInfo& info);
    /// Deletes kept images overlapping [address, address + size) or older than max_age ticks.
    void ReleaseDecommittedImages(VAddr address, u64 size, u64 max_age);

    void GarbageCollectImages();
    void GarbageCollectIdleAssets();
    void GarbageCollectSamplers();

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
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
    /// Deferred write-backs in the scheduler's priority queue, in queue (and so completion)
    /// order. Added by the command processor thread, removed by the priority thread once the
    /// bytes are in guest memory.
    struct PendingWriteback {
        VAddr begin;
        VAddr end;
        u64 tick;
        u64 sequence;
        std::shared_ptr<WritebackState> state;
    };
    std::mutex pending_writebacks_mutex;
    std::condition_variable pending_writebacks_cv;
    std::vector<PendingWriteback> pending_writebacks;
    u64 pending_writebacks_added{};
    /// Highest sequence written: write-backs complete in sequence order.
    u64 pending_writebacks_done{};
    std::atomic<u32> pending_writebacks_count{};
    /// Union bounds of the pending ranges, for the lock-free overlap test.
    std::atomic<VAddr> pending_writebacks_lo{};
    std::atomic<VAddr> pending_writebacks_hi{};
    u64 total_used_memory = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 gc_budget_overruns = 0;
    bool gc_budget_pressured = false; // Driver budget headroom was short at the last pass.
    u64 total_used_samplers = 0;
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    // GetCommandSampler's memo. GPU command thread only; sampler_generation moves (under
    // samplers_mutex, on that thread) whenever the garbage collector erases a sampler.
    struct SamplerMemoEntry {
        u64 raw0{};
        u64 raw1{};
        u64 tick{~0ULL};
        u64 generation{};
        vk::Sampler handle{};
        bool is_depth{};
    };
    static constexpr u32 SamplerMemoBits = 8;
    std::array<SamplerMemoEntry, 1u << SamplerMemoBits> sampler_memo{};
    u64 sampler_generation{1};
    bool readback_linear_images;
    PageTable page_table;
    std::atomic<u64> image_set_generation{};
    // GPU command thread only (surface_metas is touched there only).
    u64 meta_clear_generation{1};
    std::atomic<u64> content_generation{1};
    /// Runs of committed pool memory not written by the CPU since the commit, one per commit.
    /// Each page holds one write watcher, so a CPU write faults and releases its run.
    boost::icl::separate_interval_set<VAddr> undefined_content;
    /// Pages that held an image the GPU wrote first; only these are protected at a commit.
    boost::icl::interval_set<VAddr> rt_history;
    /// Undefined runs an image was refreshed in since their commit.
    boost::icl::separate_interval_set<VAddr> undefined_used;
    /// Guards the two sets. Taken after `mutex` and before page-manager locks; download
    /// write-backs take it without `mutex`.
    std::mutex undefined_mutex;
    std::mutex mutex;
    std::mutex samplers_mutex;
    std::mutex download_images_mutex;
    std::mutex tiny_trace_mutex;
    std::vector<std::string> tiny_trace;
    size_t tiny_trace_next{};
    u32 tiny_trace_dumps{};
    struct ParkedImage {
        ImageId id;
        u64 tick;
    };
    std::vector<ParkedImage> parked_images; // guarded by mutex
    static constexpr size_t MaxParkedImages = 8;
    static constexpr u64 ParkedImageMaxAge = 240; // submissions
    std::vector<ParkedImage> decommitted_images; // guarded by mutex, oldest first
    u64 decommitted_bytes{};
    static constexpr size_t MaxDecommittedImages = 32;
    static constexpr u64 MaxDecommittedBytes = 256_MB;
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
        u32 num_samples = 1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
};

} // namespace VideoCore
