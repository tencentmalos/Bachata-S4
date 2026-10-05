// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Streaming pages of the production MemoryTracker/RegionManager (region_definitions.h,
// stream_pages) against a stand-in guest memory that keeps a version per page: a CPU write to a
// watched page faults, one to an unwatched page does not. After every upload the GPU copy of each
// page of the binding must equal guest memory. Covers promotion after three write cycles, copies
// only of changed streaming pages, GPU-written bindings, demotion of unchanged pages, the switch,
// fewer promotion cycles and a randomized run. Builds on the desktop and on Android (any
// TRACKER_HIGHER_PAGE_SIZE): only OS protection and settings ownership are stubbed.
#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
#include <unordered_map>
#include "video_core/buffer_cache/memory_tracker.h"

EmulatorSettingsImpl::EmulatorSettingsImpl() = default;
EmulatorSettingsImpl::~EmulatorSettingsImpl() = default;
std::shared_ptr<EmulatorSettingsImpl> EmulatorSettingsImpl::GetInstance() {
    static auto settings = std::make_shared<EmulatorSettingsImpl>();
    return settings;
}
namespace VideoCore {
static std::set<u64> watched_pages; // write-protected pages (page numbers)
struct PageManager::Impl {};
PageManager::PageManager(Vulkan::Rasterizer*) : impl{std::make_unique<Impl>()} {}
PageManager::~PageManager() = default;
template <bool track, bool read>
void PageManager::UpdatePageWatchersForRegion(VAddr base, RegionBits& mask) const {
    if constexpr (!read) {
        for (const auto& [start, end] : mask) {
            for (u64 page = start; page < end; ++page) {
                const u64 number = (base >> TRACKER_PAGE_BITS) + page;
                if constexpr (track) {
                    watched_pages.insert(number);
                } else {
                    watched_pages.erase(number);
                }
            }
        }
    }
}
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr, RegionBits&) const;
} // namespace VideoCore
using namespace VideoCore;

static int checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
        }                                                                                          \
    } while (0)

namespace {
constexpr u64 Page = TRACKER_BYTES_PER_PAGE;

std::unordered_map<u64, u64> guest; // page number -> contents version
std::unordered_map<u64, u64> gpu;   // page number -> version the GPU copy holds

u64 PageOf(VAddr address) {
    return address >> TRACKER_PAGE_BITS;
}

struct Upload {
    u64 reserved{};
    u64 copied{};
};

// One upload of [address, address + size), recording which versions the GPU now holds.
Upload Snapshot(MemoryTracker& tracker, VAddr address, u64 size, bool is_written) {
    Upload result{};
    tracker.SnapshotForUpload(
        address, size, is_written, [&](u64 capacity) { result.reserved = capacity; },
        [&](u64 run, u64 bytes) {
            result.copied += bytes;
            for (u64 offset = 0; offset < bytes; offset += Page) {
                gpu[PageOf(run + offset)] = guest[PageOf(run + offset)];
            }
        },
        [](VAddr run, size_t count, u64* hashes) {
            for (size_t i = 0; i < count; ++i) {
                hashes[i] = 0x9e3779b97f4a7c15ULL * (guest[PageOf(run) + i] + 1);
            }
        });
    return result;
}

// Every page of the range holds on the GPU what guest memory holds.
bool Current(VAddr address, u64 size) {
    for (u64 offset = 0; offset < size; offset += Page) {
        if (gpu[PageOf(address + offset)] != guest[PageOf(address + offset)]) {
            return false;
        }
    }
    return true;
}

// A guest CPU write: faults while the page is watched.
void Write(MemoryTracker& tracker, VAddr address) {
    ++guest[PageOf(address)];
    if (watched_pages.contains(PageOf(address))) {
        tracker.InvalidateRegionFromWriteFault(address, 8, [] { std::abort(); });
    }
}

bool Watched(VAddr address) {
    return watched_pages.contains(PageOf(address));
}

std::array<u64, 5> Counters() {
    const auto& c = stream_page_counters;
    return {c.promoted.load(), c.demoted.load(), c.checked.load(), c.copied.load(),
            c.busy.load()};
}
} // namespace

int main() {
    PageManager pages(nullptr);
    auto tracker = std::make_unique<MemoryTracker>(pages);
    constexpr u64 region = TRACKER_HIGHER_PAGE_SIZE;
    constexpr VAddr base = 0x2000000000ULL;
    constexpr u64 ring_pages = 4;
    constexpr u64 ring_bytes = ring_pages * Page;
    predict_write_faults.store(false); // one fault per page: write cycles are explicit
    stream_pages.store(true);

    // Promotion: the third consecutive cycle ended by CPU writes promotes the pages.
    const VAddr ring = base;
    const auto cycle = [&](VAddr address, u64 count, u64 size) {
        for (u64 page = 0; page < count; ++page) {
            Write(*tracker, address + page * Page);
        }
        return Snapshot(*tracker, address, size, false);
    };
    CHECK(Snapshot(*tracker, ring, ring_bytes, false).copied == ring_bytes); // initially dirty
    CHECK(Current(ring, ring_bytes) && Watched(ring));
    const auto before_promotion = Counters();
    for (int i = 0; i < 2; ++i) {
        const Upload upload = cycle(ring, ring_pages, ring_bytes);
        CHECK(upload.copied == ring_bytes && Current(ring, ring_bytes));
        CHECK(Counters()[0] == before_promotion[0]);
        CHECK(Watched(ring) && Watched(ring + 3 * Page)); // watched again after each upload
    }
    const Upload third = cycle(ring, ring_pages, ring_bytes);
    CHECK(third.copied == ring_bytes && third.reserved >= third.copied);
    CHECK(Current(ring, ring_bytes));
    CHECK(Counters()[0] - before_promotion[0] == ring_pages);
    CHECK(!Watched(ring) && !Watched(ring + 3 * Page)); // promoted: no longer watched

    // Unchanged streaming pages are checked, not copied.
    auto before = Counters();
    CHECK(Snapshot(*tracker, ring, ring_bytes, false).copied == 0);
    CHECK(Counters()[2] - before[2] == ring_pages && Counters()[3] == before[3]);
    CHECK(tracker->IsRegionCpuModified(ring, ring_bytes)); // they stay CPU modified

    // A write between two uploads, without a fault, is copied by the next upload.
    Write(*tracker, ring + Page);
    CHECK(!Watched(ring + Page));
    before = Counters();
    const Upload changed = Snapshot(*tracker, ring, ring_bytes, false);
    CHECK(changed.copied == Page && changed.reserved >= changed.copied);
    CHECK(Current(ring, ring_bytes));
    CHECK(Counters()[3] - before[3] == 1);
    // Twice in a row, and by an upload of only part of the ring.
    Write(*tracker, ring + Page);
    Write(*tracker, ring + 2 * Page);
    CHECK(Snapshot(*tracker, ring + Page, Page, false).copied == Page);
    CHECK(Current(ring + Page, Page) && !Current(ring + 2 * Page, Page));
    CHECK(Snapshot(*tracker, ring, ring_bytes, false).copied == Page);
    CHECK(Current(ring, ring_bytes));

    // Marking a streaming page CPU modified explicitly keeps it streaming: its contents decide.
    tracker->MarkRegionAsCpuModified(ring + 2 * Page, 8);
    CHECK(Snapshot(*tracker, ring, ring_bytes, false).copied == 0);
    ++guest[PageOf(ring + 2 * Page)]; // the host wrote it (as with InvalidateMemory)
    tracker->MarkRegionAsCpuModified(ring + 2 * Page, 8);
    CHECK(Snapshot(*tracker, ring, ring_bytes, false).copied == Page);
    CHECK(Current(ring, ring_bytes));

    // A GPU-written binding demotes the page and copies it: watched again, GPU modified, no
    // longer CPU modified.
    Write(*tracker, ring + 3 * Page);
    before = Counters();
    const Upload written = Snapshot(*tracker, ring + 3 * Page, Page, true);
    CHECK(written.copied == Page && written.reserved >= written.copied);
    CHECK(Current(ring + 3 * Page, Page));
    CHECK(Counters()[1] - before[1] == 1);
    CHECK(Watched(ring + 3 * Page));
    CHECK(tracker->IsRegionGpuModified(ring + 3 * Page, Page));
    CHECK(!tracker->IsRegionCpuModified(ring + 3 * Page, Page));
    tracker->UnmarkRegionAsGpuModified(ring + 3 * Page, Page);
    // Even when unchanged since its last copy.
    const Upload written_same = Snapshot(*tracker, ring + 2 * Page, Page, true);
    CHECK(written_same.copied == Page && Watched(ring + 2 * Page));
    tracker->UnmarkRegionAsGpuModified(ring + 2 * Page, Page);

    // Pages unchanged over StreamDemoteChecks checks in a row are demoted (page 0 first: page 1
    // changed later), then copied and watched at their next upload.
    before = Counters();
    u32 uploads = 0;
    while (Counters()[1] - before[1] < 2 && uploads <= StreamDemoteChecks) {
        Snapshot(*tracker, ring, 2 * Page, false);
        ++uploads;
        CHECK(Current(ring, 2 * Page));
    }
    CHECK(Counters()[1] - before[1] == 2 && uploads <= StreamDemoteChecks);
    CHECK(Watched(ring) && !Watched(ring + Page)); // CPU modified until its next upload
    CHECK(Snapshot(*tracker, ring, 2 * Page, false).copied == Page);
    CHECK(Watched(ring) && Watched(ring + Page) && Current(ring, 2 * Page));
    // A page the CPU still writes is not demoted: the count restarts at each change.
    for (int i = 0; i < 3; ++i) {
        cycle(ring, 2, ring_bytes); // promoted again
    }
    CHECK(!Watched(ring));
    before = Counters();
    for (u32 i = 0; i < 3 * StreamDemoteChecks / 2; ++i) {
        if (i % (StreamDemoteChecks / 2) == 0) {
            Write(*tracker, ring);
        }
        Snapshot(*tracker, ring, Page, false);
        CHECK(Current(ring, Page));
    }
    CHECK(Counters()[1] == before[1] && !Watched(ring));

    // Switching the mode off demotes at the next upload, which copies and watches the pages.
    stream_pages.store(false);
    Write(*tracker, ring);
    const Upload off = Snapshot(*tracker, ring, ring_bytes, false);
    CHECK(Current(ring, ring_bytes));
    CHECK(Watched(ring) && Watched(ring + Page));
    CHECK(off.reserved >= off.copied);
    for (int i = 0; i < 4; ++i) {
        cycle(ring, ring_pages, ring_bytes);
    }
    CHECK(Watched(ring)); // off: never promoted
    stream_pages.store(true);

    // Large and written bindings do not promote; large ones still check streaming pages.
    const VAddr large = base + region;
    const u64 large_bytes = StreamMaxBinding + Page;
    Snapshot(*tracker, large, large_bytes, false);
    for (int i = 0; i < 4; ++i) {
        CHECK(cycle(large, 1, large_bytes).copied == Page);
    }
    CHECK(Watched(large));
    for (int i = 0; i < 3; ++i) {
        cycle(large, 1, Page); // promoted through a small binding
    }
    CHECK(!Watched(large));
    Write(*tracker, large);
    CHECK(Snapshot(*tracker, large, large_bytes, false).copied == Page);
    CHECK(Current(large, large_bytes));
    const VAddr gpu_written = base + 2 * region;
    for (int i = 0; i < 4; ++i) {
        Write(*tracker, gpu_written);
        Snapshot(*tracker, gpu_written, Page, true);
        tracker->UnmarkRegionAsGpuModified(gpu_written, Page);
    }
    CHECK(Watched(gpu_written));

    // A page bound more than StreamMaxBinds times per write cycle is not promoted (busy).
    const VAddr busy = base + 3 * region + 8 * Page;
    Snapshot(*tracker, busy, Page, false);
    before = Counters();
    for (int i = 0; i < 5; ++i) {
        Write(*tracker, busy);
        for (u32 bind = 0; bind <= StreamMaxBinds; ++bind) {
            Snapshot(*tracker, busy, Page, false);
            CHECK(Current(busy, Page));
        }
    }
    CHECK(Watched(busy));
    CHECK(Counters()[0] == before[0] && Counters()[4] > before[4]);
    // Bound StreamMaxBinds times per cycle, it is promoted.
    for (int i = 0; i < 3; ++i) {
        Write(*tracker, busy);
        for (u32 bind = 0; bind < StreamMaxBinds; ++bind) {
            Snapshot(*tracker, busy, Page, false);
        }
    }
    CHECK(!Watched(busy) && Counters()[0] - before[0] == 1);

    // A cycle without a CPU write restarts the count.
    const VAddr gap = base + 3 * region;
    Snapshot(*tracker, gap, Page, false);
    for (int i = 0; i < 2; ++i) {
        cycle(gap, 1, Page);
    }
    tracker->MarkRegionAsCpuModified(gap, 8); // dirty, but not by a write fault
    Snapshot(*tracker, gap, Page, false);
    for (int i = 0; i < 2; ++i) {
        cycle(gap, 1, Page);
    }
    CHECK(Watched(gap));
    cycle(gap, 1, Page);
    CHECK(!Watched(gap));

    // Fewer cycles (stream_promote_cycles): with 1 the first cycle that ends with a CPU write
    // promotes the page, with 2 the second (regions holding streaming pages have the hashes).
    for (const u32 cycles : {1u, 2u}) {
        stream_promote_cycles.store(cycles);
        const VAddr fast = gap + Page * cycles;
        Snapshot(*tracker, fast, Page, false); // initially dirty: no write cycle yet
        for (u32 i = 1; i < cycles; ++i) {
            cycle(fast, 1, Page);
            CHECK(Watched(fast));
        }
        CHECK(cycle(fast, 1, Page).copied == Page);
        CHECK(!Watched(fast) && Current(fast, Page));
    }
    stream_promote_cycles.store(3);

    // Randomized: writes, uploads of random ranges, GPU-written bindings, one promotion cycle.
    stream_promote_cycles.store(1);
    std::mt19937 random{12345};
    const VAddr field = base + 4 * region;
    constexpr u64 field_pages = 24;
    int stale = 0;
    for (int step = 0; step < 200000; ++step) {
        const u32 action = random() % 8;
        const u64 first = random() % field_pages;
        if (action < 4) {
            Write(*tracker, field + first * Page + (random() % (Page / 8)) * 8);
            continue;
        }
        const u64 count = 1 + random() % std::min<u64>(field_pages - first, 6);
        const bool is_written = action == 7;
        const Upload upload = Snapshot(*tracker, field + first * Page, count * Page, is_written);
        if (!Current(field + first * Page, count * Page) || upload.reserved < upload.copied) {
            ++stale;
        }
        if (is_written) {
            tracker->UnmarkRegionAsGpuModified(field + first * Page, count * Page);
        }
    }
    CHECK(stale == 0);
    CHECK(Counters()[0] > 0 && Counters()[1] > 0);
    stream_promote_cycles.store(3);

    std::printf("checks=%d failures=%d region_bytes=%llu promoted=%llu demoted=%llu checked=%llu "
                "copied=%llu busy=%llu\n",
                checks, failures, static_cast<unsigned long long>(region),
                static_cast<unsigned long long>(Counters()[0]),
                static_cast<unsigned long long>(Counters()[1]),
                static_cast<unsigned long long>(Counters()[2]),
                static_cast<unsigned long long>(Counters()[3]),
                static_cast<unsigned long long>(Counters()[4]));
    return failures != 0;
}
