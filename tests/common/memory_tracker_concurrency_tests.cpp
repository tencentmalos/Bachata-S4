// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include "video_core/buffer_cache/memory_tracker.h"

// Only OS protection and settings ownership are substituted. The production
// MemoryTracker, RegionManager, bitmaps and Android lock run unchanged.
EmulatorSettingsImpl::EmulatorSettingsImpl() = default;
EmulatorSettingsImpl::~EmulatorSettingsImpl() = default;
std::shared_ptr<EmulatorSettingsImpl> EmulatorSettingsImpl::GetInstance() {
    static auto settings = std::make_shared<EmulatorSettingsImpl>();
    return settings;
}
namespace VideoCore {
static std::atomic<s64> write_watchers{}, read_watchers{};
struct PageManager::Impl {};
PageManager::PageManager(Vulkan::Rasterizer*) : impl{std::make_unique<Impl>()} {}
PageManager::~PageManager() = default;
template <bool track, bool read>
void PageManager::UpdatePageWatchersForRegion(VAddr, RegionBits& mask) const {
    s64 pages = 0;
    for (const auto& [start, end] : mask) {
        pages += end - start;
    }
    auto& watchers = read ? read_watchers : write_watchers;
    watchers.fetch_add(track ? pages : -pages);
}
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr, RegionBits&) const;
} // namespace VideoCore
using namespace VideoCore;
using namespace std::chrono_literals;
// These addresses are not mapped: hash nothing, report every page as all zeros.
static void ZeroHashes(VAddr, size_t count, u64* hashes) {
    std::fill_n(hashes, count, u64{0});
}
static u64 Snapshot(MemoryTracker& tracker, VAddr address, u64 size, bool is_written,
                    auto&& prepare, auto&& copy_range) {
    return tracker.SnapshotForUpload(address, size, is_written, prepare, copy_range, ZeroHashes);
}
static int checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
        }                                                                                          \
    } while (0)
int main() {
    alarm(20);
    PageManager pages(nullptr);
    auto tracker = std::make_unique<MemoryTracker>(pages);
    constexpr VAddr base = 0x1000000000ULL;
    constexpr u64 region = TRACKER_HIGHER_PAGE_SIZE;
    constexpr u64 independent_offset = 256 * 1024;
    CHECK(region == 256 * 1024);
    CHECK(sizeof(MemoryTracker) < 3 * 1024 * 1024);
    // Allocate both locations on the renderer before faulting owners start.
    CHECK(tracker->IsRegionCpuModified(base, 4096));
    CHECK(tracker->IsRegionCpuModified(base + independent_offset, 4096));
    std::promise<void> in_upload, release;
    auto go = release.get_future().share();
    auto renderer = std::async(std::launch::async, [&] {
        Snapshot(*tracker, 
            base, 4096, true, [](u64) {},
            [&](u64, u64) {
                in_upload.set_value();
                go.wait();
            });
    });
    in_upload.get_future().wait();
    auto different = std::async(std::launch::async, [&] {
        tracker->InvalidateRegion(base + independent_offset, 8, [] { std::abort(); });
    });
    auto same = std::async(std::launch::async,
                           [&] { tracker->InvalidateRegion(base, 8, [] { std::abort(); }); });
    // Still in the SAME 4 MiB directory, but no longer the same lock.
    CHECK(different.wait_for(200ms) == std::future_status::ready);
    CHECK(same.wait_for(20ms) == std::future_status::timeout);
    release.set_value();
    renderer.get();
    different.get();
    same.get();
    CHECK(tracker->IsRegionCpuModified(base, 4096));
    CHECK(tracker->IsRegionCpuModified(base + independent_offset, 4096));
    // GPU/CPU transitions retain exact covered pages across a shard boundary.
    const VAddr cross = base + 2 * region - 4096;
    std::uint64_t copied{};
    Snapshot(*tracker, 
        cross, 8192, true, [](u64) {},
        [&](u64 addr, u64 bytes) {
            if (addr < cross || addr + bytes > cross + 8192)
                std::abort();
            copied += bytes;
        });
    CHECK(copied == 8192);
    CHECK(!tracker->IsRegionCpuModified(cross, 8192));
    CHECK(tracker->IsRegionGpuModified(cross, 8192));
    tracker->UnmarkRegionAsGpuModified(cross, 8192);
    CHECK(!tracker->IsRegionGpuModified(cross, 8192));
    tracker->MarkRegionAsCpuModified(cross + 4096, 8);
    CHECK(!tracker->IsRegionCpuModified(cross, 4096));
    CHECK(tracker->IsRegionCpuModified(cross + 4096, 4096));
    // Directory edges and final supported address must preserve full coverage.
    for (const auto addr : {base + (1ULL << 22) - 4096, (1ULL << 40) - 8192}) {
        copied = 0;
        Snapshot(*tracker, addr, 8192, false, [](u64) {}, [&](u64, u64 n) { copied += n; });
        CHECK(copied == 8192);
        CHECK(!tracker->IsRegionCpuModified(addr, 8192));
    }
    // Staging waits must not hold even the SAME region's tracking lock. Start
    // from one dirty page; a writer adds another during prepare and forces a
    // bounded capacity retry. No source bytes may be lost or copied twice.
    const VAddr transaction = base + (1ULL << 24);
    Snapshot(*tracker, transaction, 3 * 4096, false, [](u64) {}, [](u64, u64) {});
    tracker->MarkRegionAsCpuModified(transaction, 8);
    std::promise<void> in_prepare, release_prepare;
    auto resume_prepare = release_prepare.get_future().share();
    unsigned prepares = 0;
    u64 reserved = 0, uploaded = 0, snapshotted = 0;
    auto uploading = std::async(std::launch::async, [&] {
        uploaded = Snapshot(*tracker, 
            transaction, 3 * 4096, true,
            [&](u64 capacity) {
                reserved = capacity;
                if (++prepares == 1) {
                    in_prepare.set_value();
                    resume_prepare.wait();
                }
            },
            [&](u64, u64 bytes) { snapshotted += bytes; });
    });
    in_prepare.get_future().wait();
    auto new_writer = std::async(std::launch::async, [&] {
        tracker->InvalidateRegion(transaction + 4096, 8, [] { std::abort(); });
    });
    CHECK(new_writer.wait_for(200ms) == std::future_status::ready);
    release_prepare.set_value();
    uploading.get();
    new_writer.get();
    CHECK(prepares == 2);
    CHECK(reserved == 3 * 4096);
    CHECK(uploaded == 2 * 4096 && snapshotted == uploaded);
    CHECK(!tracker->IsRegionCpuModified(transaction, 3 * 4096));
    CHECK(tracker->IsRegionGpuModified(transaction, 3 * 4096));

    // Aborted reservation leaves dirtiness intact, and no lock is leaked.
    tracker->MarkRegionAsCpuModified(transaction, 8);
    bool failed = false;
    try {
        Snapshot(*tracker, 
            transaction, 4096, false, [](u64) { throw std::runtime_error("allocation cancelled"); },
            [](u64, u64) { std::abort(); });
    } catch (const std::runtime_error&) {
        failed = true;
    }
    CHECK(failed);
    CHECK(tracker->IsRegionCpuModified(transaction, 4096));

    // Roll back an interrupted multi-region copy exactly. The middle page was
    // clean before the attempt and must remain clean; publish no GPU ownership.
    const VAddr rollback = transaction + 2 * region - 4096;
    Snapshot(*tracker, rollback, 3 * 4096, false, [](u64) {}, [](u64, u64) {});
    tracker->MarkRegionAsCpuModified(rollback, 8);
    tracker->MarkRegionAsCpuModified(rollback + 2 * 4096, 8);
    const auto watchers_before_rollback = write_watchers.load();
    unsigned copies = 0;
    failed = false;
    try {
        Snapshot(*tracker, 
            rollback, 3 * 4096, true, [](u64) {},
            [&](u64, u64) {
                if (++copies == 2)
                    throw std::runtime_error("copy cancelled");
            });
    } catch (const std::runtime_error&) {
        failed = true;
    }
    CHECK(failed && copies == 2);
    CHECK(tracker->IsRegionCpuModified(rollback, 4096));
    CHECK(!tracker->IsRegionCpuModified(rollback + 4096, 4096));
    CHECK(tracker->IsRegionCpuModified(rollback + 2 * 4096, 4096));
    CHECK(!tracker->IsRegionGpuModified(rollback, 3 * 4096));
    CHECK(write_watchers.load() == watchers_before_rollback);
    snapshotted = 0;
    CHECK(Snapshot(*tracker, 
              rollback, 3 * 4096, true, [](u64) {},
              [&](u64, u64 bytes) { snapshotted += bytes; }) == 8192);
    CHECK(snapshotted == 8192);
    CHECK(!tracker->IsRegionCpuModified(rollback, 3 * 4096));

    // A reservation may retire callbacks which already uploaded this range.
    // Recheck must skip the stale outer copy instead of clearing a newer write.
    const VAddr reentrant = transaction + 4 * region;
    prepares = 0;
    uploaded = Snapshot(*tracker, 
        reentrant, 4096, false,
        [&](u64) {
            ++prepares;
            Snapshot(*tracker, reentrant, 4096, false, [](u64) {}, [](u64, u64) {});
        },
        [](u64, u64) { std::abort(); });
    CHECK(prepares == 1 && uploaded == 0);
    CHECK(!tracker->IsRegionCpuModified(reentrant, 4096));
    CHECK(Snapshot(*tracker, 
              reentrant, 0, true, [](u64) { std::abort(); }, [](u64, u64) { std::abort(); }) == 0);
    // Faults for texture-only/untracked ranges never create regions or flush.
    tracker->InvalidateRegion(base + (1ULL << 28), 8, [] { std::abort(); });
    CHECK(!tracker->IsRegionGpuModified(base + (1ULL << 28), 8));

    // Precise readback ownership remains unpublished until the snapshot is
    // complete, and its blocking flush callback runs with no tracking lock.
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Precise);
    const VAddr precise = transaction + 8 * region;
    const auto reads_before = read_watchers.load();
    Snapshot(*tracker, precise, 4096, true, [](u64) {}, [](u64, u64) {});
    CHECK(read_watchers.load() == reads_before + 1);
    bool flushed = false;
    tracker->InvalidateRegion(precise, 8, [&] {
        // Re-entry would deadlock if on_flush retained the range's lock.
        tracker->UnmarkRegionAsGpuModified(precise, 4096);
        tracker->MarkRegionAsCpuModified(precise, 8);
        flushed = true;
    });
    CHECK(flushed);
    CHECK(read_watchers.load() == reads_before);
    CHECK(tracker->IsRegionCpuModified(precise, 4096));
    CHECK(!tracker->IsRegionGpuModified(precise, 4096));
    Snapshot(*tracker, precise, 8192, true, [](u64) {}, [](u64, u64) {});
    CHECK(read_watchers.load() == reads_before + 2);
    tracker->InvalidateMapping(precise, 4096);
    CHECK(read_watchers.load() == reads_before + 1);
    CHECK(tracker->IsRegionCpuModified(precise, 4096));
    CHECK(!tracker->IsRegionGpuModified(precise, 4096));
    CHECK(tracker->IsRegionGpuModified(precise + 4096, 4096));
    CHECK(!tracker->IsRegionCpuModified(precise + 4096, 4096));
    u64 remapped_bytes{};
    Snapshot(*tracker, precise, 8192, false, [](u64) {},
                               [&](u64 a, u64 n) { CHECK(a == precise); remapped_bytes += n; });
    CHECK(remapped_bytes == 4096);
    tracker->InvalidateMapping(precise, 8192);
    CHECK(read_watchers.load() == reads_before);
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Disabled);

    // Write prediction: a fault releases the confident pages after it in its window. A page
    // released ahead and uploaded without a fault loses one step of confidence per cycle while
    // pages decay, and keeps it without decay; contents are then not hashed.
    {
        const VAddr ring = transaction + 16 * region;
        unsigned hashed = 0;
        const auto upload = [&] {
            tracker->SnapshotForUpload(
                ring, 4 * 4096, false, [](u64) {}, [](u64, u64) {},
                [&](VAddr, size_t count, u64* hashes) {
                    hashed += static_cast<unsigned>(count);
                    std::fill_n(hashes, count, u64{0});
                });
        };
        const auto fault = [&](u64 page) {
            return tracker->InvalidateRegionFromWriteFault(ring + page * 4096, 8,
                                                           [] { std::abort(); });
        };
        const auto train = [&] {
            for (u64 page = 0; page < 4; ++page) {
                fault(page);
            }
            upload();
        };
        CHECK(tracker->IsRegionCpuModified(ring, 4 * 4096));
        upload(); // watch the four pages; nothing is confident yet
        CHECK(!tracker->IsRegionCpuModified(ring, 4 * 4096));
        CHECK(fault(0) == 0);
        upload();

        predict_from_contents.store(false);
        decay_unchanged.store(true);
        train();
        std::size_t released[4]{};
        for (auto& count : released) {
            count = fault(0);
            upload();
        }
        // Confidence 3 lasts three cycles of being released without a write.
        CHECK(released[0] == 3 && released[1] == 3 && released[2] == 3 && released[3] == 0);

        predict_from_contents.store(true);
        decay_unchanged.store(false);
        train();
        hashed = 0;
        const u64 kept_before = content_prediction_counters.kept_pages.load();
        bool always_released = true;
        for (int cycle = 0; cycle < 6; ++cycle) {
            always_released &= fault(0) == 3;
            upload();
        }
        CHECK(always_released);
        CHECK(content_prediction_counters.kept_pages.load() - kept_before == 6 * 3);
        CHECK(hashed == 0);

        // With decay, hashed contents keep a rewritten released-ahead page confident: here the
        // hash of every page stays zero, so the released pages decay as without hashes.
        decay_unchanged.store(true);
        train();
        hashed = 0;
        for (auto& count : released) {
            count = fault(0);
            upload();
        }
        CHECK(hashed > 0);
        CHECK(released[0] == 3 && released[1] == 3 && released[2] == 3 && released[3] == 0);
        decay_unchanged.store(false);
    }

    std::printf("checks=%d failures=%d region_bytes=%llu directory_object_bytes=%zu\n", checks,
                failures, static_cast<unsigned long long>(region), sizeof(MemoryTracker));
    return failures != 0;
}
