// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include "common/alignment.h"
#include "common/types.h"
#include "video_core/buffer_cache//region_definitions.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

struct UffdImpl;
struct SignalImpl;

class PageManager {
    // PAGE_SIZE and PAGE_BITS conflicts with machine/param.h definitions on freebsd!
    // Use the same page size as the tracker.
    static constexpr size_t PM_PAGE_BITS = TRACKER_PAGE_BITS;
    static constexpr size_t PM_PAGE_SIZE = TRACKER_BYTES_PER_PAGE;

    // Keep the lock granularity the same as region granularity. (since each regions has
    // itself a lock)
    static constexpr size_t PAGES_PER_LOCK = NUM_PAGES_PER_REGION;

public:
    explicit PageManager(Vulkan::Rasterizer* rasterizer);
    ~PageManager();

    /// Register a range of mapped gpu memory.
    void OnGpuMap(VAddr address, size_t size);

    /// Unregister a range of gpu memory that was unmapped.
    void OnGpuUnmap(VAddr address, size_t size);

    /// Updates watches in the pages touching the specified region.
    template <bool track>
    void UpdatePageWatchers(VAddr addr, u64 size) const;

    /// Updates watches in the pages touching the specified region using a mask.
    template <bool track, bool is_read = false>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const;

    /// Whether a page touching the specified region has a write watcher.
    bool HasWriteWatchers(VAddr addr, u64 size) const;

    /// GPU replay recorder (docs/specs/gpu-replay-20261004.md, section 2.4). While active, the
    /// recorder can write-protect pages on top of the caches' watchers: the first CPU write to
    /// such a page faults once, which clears its recorder bit and leaves the page protected only
    /// as the caches need. Deactivate only after every bit is cleared.
    static void SetRecorderActive(bool active);
    /// Sets the recorder bits of [addr, addr + size) and write-protects those pages.
    void RecordWrites(VAddr addr, u64 size) const;
    /// Clears the recorder bits of [addr, addr + size). With restore_protection the pages get
    /// the caches' protection back; without it their host protection is left as it is, for
    /// ranges whose protection the guest has just set itself.
    void StopRecordingWrites(VAddr addr, u64 size, bool restore_protection) const;
    /// Appends the pages of [addr, addr + size) whose recorder bit is clear: pages written since
    /// they were last protected. Reads the bits without locking; a page cleared later is found by
    /// the next call.
    void CollectRecorderDirty(VAddr addr, u64 size, std::vector<u64>& pages) const;
    /// Write faults the recorder handled.
    static u64 RecorderWriteFaults();

    /// Who releases page watchers on this thread, named in the report when a release finds a
    /// page without a watcher left: some earlier release of that page did not match a track.
    struct WatchContext {
        const char* what{};
        VAddr address{};
        u64 size{};
        u32 format{};
    };
    class WatchContextScope {
    public:
        WatchContextScope(const char* what, VAddr address, u64 size, u32 format = 0);
        ~WatchContextScope();
        WatchContextScope(const WatchContextScope&) = delete;
        WatchContextScope& operator=(const WatchContextScope&) = delete;

    private:
        WatchContext saved;
    };
    /// Releases that found a page without a watcher. The count stays at zero instead of
    /// aborting; the first ones are logged with the page and the WatchContext.
    static u64 WatcherUnderflows();
    /// Tracks that found a page at the most watchers it counts (127 write, 1 read): the count
    /// stays there, so a later release can leave the page unwatched early.
    static u64 WatcherOverflows();

    /// Returns page aligned address.
    static constexpr VAddr GetPageAddr(VAddr addr) {
        return Common::AlignDown(addr, PM_PAGE_SIZE);
    }

    /// Returns address of the next page.
    static constexpr VAddr GetNextPageAddr(VAddr addr) {
        return Common::AlignUp(addr + 1, PM_PAGE_SIZE);
    }

private:
    friend struct UffdImpl;
    friend struct SignalImpl;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCore
