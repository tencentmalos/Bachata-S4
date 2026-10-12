// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <span>
#include <string>
#include <vector>
#include "common/types.h"

// Bounded, default-off evidence for texture re-uploads: which write made an image dirty,
// and whether the guest bytes of the uploaded mips actually changed since that image's
// previous upload. Hot paths only read `armed` when off; everything else runs while armed.
namespace VideoCore::UploadDiagnostics {

enum class DirtySource : u8 {
    None,            // created dirty, or dirtied by a path that is not instrumented
    CpuFault,        // guest write fault on a tracked page (1/8-byte probe) overlapping the image
    HostWrite,       // HLE/host write range overlapping the image
    CpuPageShared,   // single-page image on a written page; the hash check decides
    GpuStorageWrite, // formatted storage buffer written by a shader aliases the image
    GpuCopy,         // CP/DMA buffer copy destination aliases the image
    Count,
};

struct ImageKey {
    VAddr address{};
    u64 size{};
    u32 format{};
    bool operator==(const ImageKey&) const = default;
};

// How an uploaded image was made (Image::created_by).
enum class Created : u8 {
    Fresh,            // FindImage created it: nothing cached matched
    Expand,           // ExpandImage: a larger or reinterpreted copy of a cached image
    ExpandTwin,       // ExpandImage of a pure reinterpretation (e.g. 3D <-> 2D array)
    ExpandTwinReused, // the same, reusing the image parked by the previous conversion
    Revived,          // an image of decommitted pool memory, back after a commit
    DepthCopy,        // ResolveDepthOverlap: depth <-> colour reinterpretation
    Count,
};
// How the last shader that wrote an image bound it (Image::last_write_kind).
enum class WriteKind : u8 { None, Storage, RenderTarget, DepthTarget, Count };

// The guest shader of the draw (pixel shader, else vertex) or dispatch being bound, set by the
// rasterizer while armed: names the action that refreshed or wrote an image.
inline std::atomic<u64> current_action{0};
inline std::atomic<bool> current_action_dispatch{false};

// ExpandImage copies a cached image into the new one. When the copy covers the new image the
// guest upload before it is skipped, and a pure reinterpretation parks the replaced image for
// the conversion back (`upload_diag expand_twin on|off`, on by default).
inline std::atomic<bool> expand_twin{true};
inline std::atomic<u64> expansions{0}, expand_upload_skips{0}, expand_twin_reuses{0},
    expand_stale_sources{0};

// Decommitted pool memory keeps its GPU-written images; a commit of the same range the CPU has
// not written revives the exact image with its contents (`upload_diag pool_revive on|off`, on
// by default). Counters: images kept, revived (and their bytes), deleted unrevived, and
// found but not revived because the range was defined or GPU buffer data was there.
inline std::atomic<bool> pool_revive{true};
inline std::atomic<u64> pool_revive_kept{0}, pool_revive_revived{0}, pool_revive_bytes{0},
    pool_revive_released{0}, pool_revive_stale{0};

struct UploadEvent {
    ImageKey key;
    u32 width{}, height{}, depth{}, layers{}, levels{};
    u32 tile_mode{};
    bool tiled{}, scaled{}, cpu_dirty{}, maybe_cpu_dirty{}, gpu_dirty{}, gpu_modified{};
    // Buffer cache holds newer bytes than guest memory: the upload source is GPU-resident
    // and a guest-memory hash cannot say whether the content changed.
    bool gpu_resident{};
    u32 frame{};
    std::span<const u32> mips;    // uploaded mip levels
    std::span<const u64> hashes;  // guest hash per uploaded mip, empty when gpu_resident
    // Image::usage at the refresh (bits: texture, storage, render target, depth target),
    // Image::created_by, and the action refreshing it (current_action).
    u8 usage{};
    u8 created{};
    u64 action{};
    bool action_dispatch{};
};

// A GPU-written image was freed with its range (unmap, decommit): a later upload of an image
// over the same range is uploading guest memory that never held what the GPU wrote there.
// Recorded while armed, last few per address.
void NoteFreedGpuImage(VAddr address, u64 size, u32 format, WriteKind kind, u64 writer,
                       u32 frame, bool kept);

// One buffer binding of a compute dispatch that writes a formatted storage buffer over an
// image base. `head` holds the first guest dwords when the guest copy is current.
struct DispatchBinding {
    VAddr address{};
    u64 size{};
    u32 stride{}, num_records{};
    u8 data_format{}, num_format{};
    bool written{}, formatted{}, gpu_resident{}, head_valid{};
    std::array<u32, 4> head{};
    std::string images; // overlapping cached images, empty when none
};

inline std::atomic<bool> armed{false};
// Diagnostic A/B only: keep an already rendered image authoritative when a shader binds its
// memory as a written formatted storage buffer (no GpuDirty, no re-upload from the buffer).
inline std::atomic<bool> ignore_storage_dirty{false};
inline std::atomic<u64> ignored_storage_dirty{0};

// Compute fill kernels replaced by image clears (Rasterizer::TryComputeImageFill). The
// counters are always maintained (a few relaxed atomics per fill dispatch); `fill_clear_off`
// is a runtime A/B switch (`upload_diag fill_clear on|off`) that restores the dispatch.
enum class FillOutcome : u8 {
    Cleared,     // every overlapping image was cleared and the dispatch skipped
    NoImage,     // plain buffer fill, dispatched normally
    Partial,     // an overlapping image is not fully covered or not pattern-aligned
    Pattern,     // source is not a repeating pattern of at most 16 dwords (a copy)
    Format,      // texel not uniform for the image format, or format not clearable
    GpuResident, // control or pattern words were written by the GPU in this submission
    Disabled,    // A/B switch off
    Count,
};
inline std::atomic<bool> fill_clear_off{false};

// Mutable-format colour images get a format list (UniqueImage::Create) so Turnip can keep them
// UBWC-compressed: -1 until read from SHADPS4_IMAGE_FORMAT_LIST / debug.shadps4.image_format_list
// (default on); `upload_diag format_list on|off` applies to images created afterwards.
inline std::atomic<int> format_list{-1};
inline std::atomic<u64> format_list_images{0}, format_list_moves{0};

// Raw (unformatted) buffer reads of GPU-written images tile the image back into the buffer cache
// first (BufferCache::SynchronizeMemoryFromGpuImage). `raw_sync_off` is the runtime A/B switch
// (`upload_diag raw_sync on|off`); raw_sync_stale counts images passed over because guest memory
// under them was written through the buffer path after them.
inline std::atomic<bool> raw_sync_off{false};
inline std::atomic<u64> raw_syncs{0}, raw_sync_bytes{0}, raw_sync_stale{0};
// Formatted (texel buffer) reads of an image whose current contents were already tiled back.
inline std::atomic<u64> texel_sync_skips{0};

// DMA synchronization bounds: a shader whose pointer reads all come from known user-data bases
// synchronizes only the 4 GiB window above each base (reads are base + u32 dword offset).
// Unbounded shaders always synchronize every resident range. Off restores that for all
// shaders; it re-tiled MHR's two 1080p render targets into the buffer arena on each of its
// ~350 pointer-reading draws per frame.
inline std::atomic<bool> dma_bounds{true};
// Diagnostic: copy depth<->color twins through the scratch buffer even when the
// driver reports VK_KHR_maintenance8 direct depth/color image copies.
inline std::atomic<bool> depth_copy_buffer{false};
inline std::atomic<u64> depth_copy_direct{};
inline std::atomic<u64> depth_copy_buffered{};
inline std::atomic<u64> dma_bounded_calls{0}, dma_full_calls{0}, dma_ranges_skipped{0};

// Raw dword copy kernels between two same-layout images replaced by an image copy
// (Rasterizer::TryComputeRawImageCopy); `upload_diag raw_copy on|off`.
inline std::atomic<bool> raw_copy_off{false};
// DMA copies of an HTILE template onto a depth target's HTILE treated as a depth clear
// (Rasterizer::TryHtileClear); `upload_diag htile_copy on|off`.
inline std::atomic<bool> htile_copy_off{false};
inline std::atomic<u64> htile_copies{}, htile_copy_clears{}, htile_copy_rejected{},
    htile_copy_gpu_source{};
// Largest read-only buffer copied into the stream buffer instead of bound from the arena
// (`upload_diag stream_max <bytes>`; capped by BufferCache::STREAM_THRESHOLD).
inline std::atomic<u32> stream_max{16384};
// Streamed data is first copied from guest memory into a cached buffer on the stack and written
// to the stream buffer after the memory manager's read lock is released, so the lock release
// does not wait for stores to write-combined memory. Diagnostic (`upload_diag stream_bounce`).
inline std::atomic<bool> stream_bounce{false};
// The streamed copies of one draw or dispatch share one memory manager read lock instead of
// taking it per copy (BufferCache::BeginStreamReads). `upload_diag stream_lease on|off`.
inline std::atomic<bool> stream_lease{true};
// Discrete GPUs: streamed data (small read-only buffers, per-draw constants) goes to a stream
// buffer in host memory instead of device memory the CPU writes through the PCIe BAR. On by
// default there (`upload_diag stream_host on|off`); other GPUs have one stream buffer.
inline std::atomic<bool> stream_host{true};
// Discrete GPUs with a copy-only queue: streamed data written to host memory is copied into
// VRAM on that queue before each submission, so shaders read it from VRAM. Takes precedence
// over stream_host; on by default where available (`upload_diag stream_dma on|off`).
inline std::atomic<bool> stream_dma{true};
inline std::atomic<u64> stream_dma_submits{0}, stream_dma_bytes{0}, stream_dma_regions{0};
// Sampled-image bindings reuse FindImage's result for the same T# while the texture cache's
// image set is unchanged (`upload_diag texture_bind_cache on|off`, on by default).
inline std::atomic<bool> texture_bind_cache{true};
inline std::atomic<u64> texture_bind_hits{0}, texture_bind_misses{0};
// A cached sampled binding whose image, backing, layout state and the texture cache's content
// generation are unchanged reuses its view and layout: no refresh, view lookup or barrier
// computation (`upload_diag texture_bind_fast on|off`, on by default).
inline std::atomic<bool> texture_bind_fast{true};
inline std::atomic<u64> texture_bind_fast_hits{0};
// Reads of streamed buffers also go through the barrier tracker (`upload_diag stream_barriers
// on|off`, off by default): an A/B switch for the earlier behaviour.
inline std::atomic<bool> stream_barriers{false};
// A write fault calls the texture cache only when a page it touches is still write watched after
// the buffer cache released it: images watch their pages, so a page without watchers is tracked
// by no image (`upload_diag fault_textures skip|always`, skip by default).
inline std::atomic<bool> skip_unwatched_fault_textures{true};
// Pool memory the guest commits holds undefined contents until the CPU writes it (the commit
// may map any free physical pages). Images the GPU writes first (render, depth or storage
// targets) created there skip the guest upload (`upload_diag pool_undefined on|off`, on by
// default). Only pages that held such an image before are write protected at a commit.
// Counters: runs protected, commits with no such pages, runs left defined because a page was
// still watched, ranges learned, skipped uploads and their bytes, runs released by a CPU write,
// runs decommitted without any image refreshed in them (forgotten until used again).
inline std::atomic<bool> pool_undefined{true};
inline std::atomic<u64> pool_undefined_marked{0}, pool_undefined_unlearned{0},
    pool_undefined_watched{0}, pool_undefined_learned{0}, pool_undefined_skips{0},
    pool_undefined_skip_bytes{0}, pool_undefined_cpu_writes{0}, pool_undefined_unused{0};
// Why an image the GPU writes first still uploaded: its range was defined, it was not fully
// tracked yet, or the buffer cache held GPU data for it. Sampled: an image read before any GPU
// write found its range undefined (uploaded as before).
enum class UndefinedMiss : u32 { Defined, Untracked, GpuBuffer, Sampled, Count };
inline std::array<std::atomic<u64>, static_cast<size_t>(UndefinedMiss::Count)>
    pool_undefined_misses{};
// Counts the miss and logs the first few with the image range.
void NoteUndefinedMiss(UndefinedMiss miss, VAddr address, u64 size, u32 format, bool uploaded);
inline std::atomic<u64> raw_copies{0}, raw_copy_bytes{0}, raw_copy_fallbacks{0};
void NoteFill(FillOutcome outcome, u32 images_cleared, u64 bytes);

// `writer` is the binding shader's program hash for GPU writes, 0 otherwise.
void NoteDirty(const ImageKey& key, DirtySource source, VAddr write_address, u64 write_size,
               u64 writer = 0);
void NoteUpload(const UploadEvent& event);
void NoteDispatch(u64 pgm_hash, u32 dim_x, u32 dim_y, u32 dim_z, bool indirect,
                  std::span<const DispatchBinding> bindings);

// start [log_lines] | status | stop | ignore_storage_dirty on|off | fill_clear on|off |
// raw_sync on|off | raw_copy on|off
std::string Command(const std::vector<std::string>& args);

} // namespace VideoCore::UploadDiagnostics
