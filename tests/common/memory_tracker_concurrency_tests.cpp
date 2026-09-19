// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <cstdio>
#include <future>
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
struct PageManager::Impl {};
PageManager::PageManager(Vulkan::Rasterizer*) : impl{std::make_unique<Impl>()} {}
PageManager::~PageManager() = default;
template <bool track, bool read>
void PageManager::UpdatePageWatchersForRegion(VAddr, RegionBits&) const {}
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr, RegionBits&) const;
} // namespace VideoCore
using namespace VideoCore;
using namespace std::chrono_literals;
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
        tracker->ForEachUploadRange(
            base, 4096, true, [](u64, u64) {},
            [&] {
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
    tracker->ForEachUploadRange(
        cross, 8192, true,
        [&](u64 addr, u64 bytes) {
            if (addr < cross || addr + bytes > cross + 8192)
                std::abort();
            copied += bytes;
        },
        [] {});
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
        tracker->ForEachUploadRange(addr, 8192, false, [&](u64, u64 n) { copied += n; }, [] {});
        CHECK(copied == 8192);
        CHECK(!tracker->IsRegionCpuModified(addr, 8192));
    }
    std::printf("checks=%d failures=%d region_bytes=%llu directory_object_bytes=%zu\n", checks,
                failures, static_cast<unsigned long long>(region), sizeof(MemoryTracker));
    return failures != 0;
}
