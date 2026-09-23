// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include "common/types.h"

namespace VideoCore {

// Lock-free Render Scale coverage counters. The producer (GpuComm) increments with
// relaxed atomics on paths that already run per draw or per rare promotion; the
// StatusLayer and gpu_memory diagnostics only read. Windowed rates are computed by
// the reader from deltas, so a plain snapshot never blocks the producer.
struct ScaleCoverageCounters {
    // Attachment draws/passes and how many of them rendered at the scaled resolution.
    // resumed_passes: instances that re-opened the same attachments after a break, i.e.
    // pass fragments the guest did not express (passes - resumed_passes = guest passes).
    std::atomic<u64> draws{}, scaled_draws{}, passes{}, scaled_passes{}, resumed_passes{};
    // One-way scaled->native promotions (any reason), readbacks served from a
    // temporary upscaled copy, and same-format copies between backings of different
    // scale executed as blits; the latter two keep both plans unchanged.
    std::atomic<u64> native_promotions{}, upscaled_readbacks{}, scaled_blit_copies{};
    // Formatted-buffer reads served by tiling a GPU-modified image back into guest layout
    // (BufferCache::SynchronizeBufferFromImage), and the bytes they moved.
    std::atomic<u64> image_buffer_syncs{}, image_buffer_sync_bytes{};
    // Readbacks that sampled the scaled backing directly (no native temporary/blit/copy).
    std::atomic<u64> fused_readbacks{};
    // Why a render pass was forced native while at least one attachment was scaled.
    std::atomic<u64> native_pass_side_effects{}, native_pass_msaa{}, native_pass_attachment{},
        native_pass_mismatch{};
    // Garbage collector activity: GPU-modified images written back before eviction and
    // images freed under pressure.
    std::atomic<u64> gc_downloads{}, gc_frees{}, gc_pressured_ticks{};
    // Dirty images re-uploaded from guest memory / the buffer cache (TextureCache::
    // RefreshImage, counted once per upload with the guest bytes of the uploaded mips), and
    // images cleared directly instead of re-uploaded after a compute pattern fill.
    std::atomic<u64> image_uploads{}, image_upload_bytes{}, fill_clears{};

    struct Snapshot {
        u64 draws, scaled_draws, passes, scaled_passes, resumed_passes, native_promotions,
            upscaled_readbacks,
            scaled_blit_copies, image_buffer_syncs, image_buffer_sync_bytes, fused_readbacks,
            native_pass_side_effects, native_pass_msaa,
            native_pass_attachment, native_pass_mismatch, gc_downloads, gc_frees,
            gc_pressured_ticks, image_uploads, image_upload_bytes, fill_clears;
    };
    Snapshot Read() const noexcept {
        constexpr auto o = std::memory_order_relaxed;
        return {draws.load(o), scaled_draws.load(o), passes.load(o), scaled_passes.load(o),
                resumed_passes.load(o), native_promotions.load(o), upscaled_readbacks.load(o), scaled_blit_copies.load(o),
                image_buffer_syncs.load(o), image_buffer_sync_bytes.load(o), fused_readbacks.load(o),
                native_pass_side_effects.load(o), native_pass_msaa.load(o),
                native_pass_attachment.load(o), native_pass_mismatch.load(o), gc_downloads.load(o),
                gc_frees.load(o), gc_pressured_ticks.load(o), image_uploads.load(o),
                image_upload_bytes.load(o), fill_clears.load(o)};
    }
};

// Cause bits reported by the rasterizer when a pass with scaled attachments is forced native.
enum NativePassCause : u32 {
    NativePassSideEffects = 1u << 0,
    NativePassMsaa = 1u << 1,
    NativePassAttachment = 1u << 2,
    NativePassMismatch = 1u << 3,
};

} // namespace VideoCore
