// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <boost/container/small_vector.hpp>
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/futex_mutex.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifndef _WIN64
#include <sys/mman.h>
#include "common/adaptive_mutex.h"
#else
#include <windows.h>
#endif

#if defined(__linux__) && !defined(__ANDROID__)
#define SHADPS4_USERFAULTFD_TRACKING 1
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#else
#include "common/spin_lock.h"
#endif
#include "common/spin_lock.h"

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

struct PageManager::Impl {
    struct PageState {
        u8 num_write_watchers : 7;
        // At the moment only buffer cache can request read watchers.
        // And buffers cannot overlap, thus only 1 can exist per page.
        u8 num_read_watchers : 1;

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                           : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                          : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return ReadPerm() | WritePerm();
        }

        template <s32 delta, bool is_read>
        u8 AddDelta() {
            if constexpr (is_read) {
                if constexpr (delta == 1) {
                    return ++num_read_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_read_watchers > 0, "Not enough watchers");
                    return --num_read_watchers;
                } else {
                    return num_read_watchers;
                }
            } else {
                if constexpr (delta == 1) {
                    return ++num_write_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                    return --num_write_watchers;
                } else {
                    return num_write_watchers;
                }
            }
        }
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;
    inline static Impl* instance;

    // GPU replay recorder: one bit per page it write-protects on top of the watchers. A bit only
    // changes under its page's lock, like cached_pages, so protections computed under the lock
    // see both. The bitmap spans the tracked address space (32 MiB, committed as touched), is
    // allocated by the first capture and never freed, so a reader can never see it go away.
    inline static std::atomic<bool> recorder_active{};
    inline static std::atomic<u64*> recorder_bits{};
    inline static std::atomic<u64> recorder_faults{};

    enum class RecorderFault {
        None,     ///< Not a recorder page.
        Released, ///< The recorder released the page; the write can retry.
        Watched,  ///< The bit is cleared; the caches still watch the page and handle the fault.
    };

    Impl() = default;
    virtual ~Impl() = default;

    static bool RecorderBit(size_t page) {
        u64* bits = recorder_bits.load(std::memory_order_relaxed);
        return bits && ((std::atomic_ref<u64>{bits[page >> 6]}.load(std::memory_order_relaxed) >>
                         (page & 63)) &
                        1);
    }

    template <bool value>
    static void SetRecorderBit(size_t page) {
        std::atomic_ref<u64> word{recorder_bits.load(std::memory_order_relaxed)[page >> 6]};
        if constexpr (value) {
            word.fetch_or(u64{1} << (page & 63), std::memory_order_relaxed);
        } else {
            word.fetch_and(~(u64{1} << (page & 63)), std::memory_order_relaxed);
        }
    }

    /// The protection a page needs: the watchers' plus the recorder's write protection.
    Core::MemoryPermission PagePerms(size_t page) const {
        auto perms = cached_pages[page].Perms();
        if (recorder_active.load(std::memory_order_relaxed) && RecorderBit(page)) [[unlikely]] {
            perms &= ~Core::MemoryPermission::Write;
        }
        return perms;
    }

    static void SetRecorderActive(bool active) {
        if (active && !recorder_bits.load()) {
            recorder_bits = static_cast<u64*>(std::calloc(NUM_ADDRESS_PAGES / 64, sizeof(u64)));
            ASSERT_MSG(recorder_bits.load(), "Cannot allocate the GPU replay recorder bitmap");
        }
        recorder_active = active;
    }

    /// A write fault, before the caches see it.
    RecorderFault OnRecorderWriteFault(VAddr addr) {
        const size_t page = addr >> PM_PAGE_BITS;
        std::scoped_lock lk{locks[page / PAGES_PER_LOCK]};
        if (!RecorderBit(page)) {
            return RecorderFault::None;
        }
        SetRecorderBit<false>(page);
        recorder_faults.fetch_add(1, std::memory_order_relaxed);
        if (cached_pages[page].num_write_watchers != 0) {
            return RecorderFault::Watched;
        }
        Protect(page << PM_PAGE_BITS, PM_PAGE_SIZE, PagePerms(page));
        return RecorderFault::Released;
    }

    /// Sets or clears the recorder bits of a range. Pages whose bit changes are protected as
    /// they now need (unless protect is false), in runs of equal protection. Works through one
    /// lock region at a time: a range of several GiB must not hold every lock in it at once.
    template <bool record>
    void UpdateRecorderBits(VAddr addr, u64 size, bool protect) {
        const size_t first = addr >> PM_PAGE_BITS;
        const size_t last = Common::DivCeil(addr + size, PM_PAGE_SIZE);
        for (size_t begin = first; begin < last;) {
            const size_t end =
                std::min<size_t>(last, (begin / PAGES_PER_LOCK + 1) * PAGES_PER_LOCK);
            std::scoped_lock lk{locks[begin / PAGES_PER_LOCK]};
            size_t run_begin = 0;
            size_t run_end = 0;
            Core::MemoryPermission run_perms{};
            const auto release_run = [&] {
                if (run_end > run_begin) {
                    Protect(run_begin << PM_PAGE_BITS, (run_end - run_begin) << PM_PAGE_BITS,
                            run_perms);
                }
                run_begin = run_end = 0;
            };
            for (size_t page = begin; page < end; ++page) {
                if (RecorderBit(page) == record) {
                    release_run();
                    continue;
                }
                SetRecorderBit<record>(page);
                if (!protect) {
                    continue;
                }
                const auto perms = PagePerms(page);
                if (run_end > run_begin && run_end == page && perms == run_perms) {
                    ++run_end;
                    continue;
                }
                release_run();
                run_begin = page;
                run_end = page + 1;
                run_perms = perms;
            }
            release_run();
            begin = end;
        }
    }

    static void CollectRecorderDirty(VAddr addr, u64 size, std::vector<u64>& pages) {
        u64* bits = recorder_bits.load(std::memory_order_relaxed);
        if (!bits || size == 0) {
            return;
        }
        const size_t end = Common::DivCeil(addr + size, PM_PAGE_SIZE);
        for (size_t page = addr >> PM_PAGE_BITS; page < end;) {
            const size_t word = page >> 6;
            const size_t word_end = (word + 1) << 6;
            u64 clear = ~std::atomic_ref<u64>{bits[word]}.load(std::memory_order_relaxed);
            clear &= ~u64{0} << (page & 63);
            if (end < word_end) {
                clear &= (u64{1} << (end & 63)) - 1;
            }
            while (clear) {
                pages.push_back((word << 6) + std::countr_zero(clear));
                clear &= clear - 1;
            }
            page = word_end;
        }
    }

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        auto perms = PagePerms(page);
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped "
                        "(track={} read={}).",
                        aligned_addr, aligned_end, track, is_read);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = state.AddDelta<track ? 1 : -1, is_read>();

            if (auto new_perms = PagePerms(page); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        auto perms = PagePerms(base_page + start_range.first);
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect((range_begin << PM_PAGE_BITS), range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>() : state.AddDelta<0, is_read>();

            if (auto new_perms = PagePerms(base_page + page); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    bool HasWriteWatchers(VAddr addr, u64 size) {
        const size_t page_begin = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);
        const auto lock_start = locks.begin() + (page_begin / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);
        for (size_t page = page_begin; page != page_end; ++page) {
            if (cached_pages[page].num_write_watchers != 0) {
                return true;
            }
        }
        return false;
    }

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#if defined(__ANDROID__) || defined(_WIN32)
    // Held across page protection changes: waiters sleep rather than spin.
    using LockType = Common::FutexMutex;
#elif defined(PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP)
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
};

#ifdef SHADPS4_USERFAULTFD_TRACKING
struct UffdImpl : public PageManager::Impl {
private:
    std::jthread ufd_thread;
    int uffd;

public:
    UffdImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd == -1) {
            LOG_ERROR(Common_Memory,
                      "userfaultfd syscall failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }

        // Request uffdio features from kernel.
        uffdio_api api;
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_THREAD_ID;
        const int ret = ioctl(uffd, UFFDIO_API, &api);
        if (ret != 0) {
            LOG_ERROR(Common_Memory,
                      "uffdio_api call failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("uffdio_api");
        }

        // Create uffd handler thread
        ufd_thread = std::jthread([&](std::stop_token token) { UffdHandler(token); });
    }

    ~UffdImpl() = default;

    void OnMap(VAddr address, size_t size) override {
        uffdio_register reg;
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_REGISTER, &reg);
        ASSERT_MSG(ret != -1, "Uffdio register failed");
    }

    void OnUnmap(VAddr address, size_t size) override {
        uffdio_range range;
        range.start = address;
        range.len = size;
        const int ret = ioctl(uffd, UFFDIO_UNREGISTER, &range);
        ASSERT_MSG(ret != -1, "Uffdio unregister failed");
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        bool allow_write = True(perms & Core::MemoryPermission::Write);
        uffdio_writeprotect wp;
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
        ASSERT_MSG(ret != -1, "Uffdio writeprotect failed with error: {}",
                   Common::GetLastErrorMsg());
    }

    void UffdHandler(std::stop_token token) {
        while (!token.stop_requested()) {
            pollfd pollfd;
            pollfd.fd = uffd;
            pollfd.events = POLLIN;

            // Block until the descriptor is ready for data reads.
            const int pollres = poll(&pollfd, 1, -1);
            switch (pollres) {
            case -1:
                perror("Poll userfaultfd");
                continue;
                break;
            case 0:
                continue;
            case 1:
                break;
            default:
                UNREACHABLE_MSG("Unexpected number of descriptors {} out of poll", pollres);
            }

            // We don't want an error condition to have occured.
            ASSERT_MSG(!(pollfd.revents & POLLERR), "POLLERR on userfaultfd");

            // We waited until there is data to read, we don't care about anything else.
            if (!(pollfd.revents & POLLIN)) {
                continue;
            }

            // Read message from kernel.
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            if (readret == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                LOG_ERROR(Common_Memory, "Unexpected result of uffd read: {}",
                          Common::GetLastErrorMsg());
                break;
            }
            ASSERT_MSG(readret == sizeof(msg), "Unexpected short read, exiting");
            ASSERT(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP);

            // Notify rasterizer about the fault.
            const VAddr addr = msg.arg.pagefault.address;
            rasterizer->InvalidateMemory(addr, 1);

            // Some calls to InvalidateMemory never reach the UFFDIO_WRITEPROTECT ioctl in
            // ::Protect, therefore we use MODE_DONTWAKE and wake the thread with UFFDIO_WAKE here
            const auto ptid = msg.arg.pagefault.feat.ptid;
            uffdio_range wake;
            wake.start = msg.arg.pagefault.address;
            wake.len = PM_PAGE_SIZE;
            const int ret = ioctl(uffd, UFFDIO_WAKE, &wake);
            ASSERT_MSG(ret != -1, "Waking thread {} failed with: {}", ptid,
                       Common::GetLastErrorMsg());
        }
    }
};
#endif // SHADPS4_USERFAULTFD_TRACKING

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        auto* memory = Core::Memory::Instance();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        // Through the memory manager: on Android the guest runtime owns page protection and
        // coalesces runs of pages into one mprotect.
        memory->ProtectGpu(address, size, perms);
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        // Probe at most to the end of the faulting page: a spill into the next page can fail
        // IsMapped at the end of a GPU mapping, or take over GPU bytes of a page the CPU did
        // not write.
        const u64 size = std::min<u64>(8, PageManager::GetNextPageAddr(addr) - addr);
        auto& space = Core::Memory::Instance()->GetAddressSpace();
        const bool write = Common::IsWriteError(context);
        // Faults on guest memory the GPU does not watch belong to the guest runtime.
        if (space.IsGuestBackend() && !space.IsGpuWatchFault(addr, write))
            return false;
        if (write) {
            if (recorder_active.load(std::memory_order_relaxed)) [[unlikely]] {
                if (instance->OnRecorderWriteFault(addr) == RecorderFault::Released) {
                    return true;
                }
            }
            return rasterizer->InvalidateMemoryFromWriteFault(addr, size);
        } else {
            return rasterizer->ReadMemory(addr, size);
        }
    }
};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
#ifdef SHADPS4_USERFAULTFD_TRACKING
    if (EmulatorSettings.IsUserfaultfdTracking()) {
        try {
            impl = std::make_unique<UffdImpl>(rasterizer_);
            LOG_INFO(Config, "Memory tracking method: userfaultfd");
            return;
        } catch (const std::runtime_error& e) {
            // if uffd is unsupported, falls back to SignalImpl
        }
    }
    LOG_INFO(Config, "Memory tracking method: signals");
#endif
    impl = std::make_unique<SignalImpl>(rasterizer_);
    Impl::instance = impl.get();
}

PageManager::~PageManager() {
    Impl::instance = nullptr;
}

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

bool PageManager::HasWriteWatchers(VAddr addr, u64 size) const {
    return impl->HasWriteWatchers(addr, size);
}

void PageManager::SetRecorderActive(bool active) {
    Impl::SetRecorderActive(active);
}

void PageManager::RecordWrites(VAddr addr, u64 size) const {
    impl->UpdateRecorderBits<true>(addr, size, true);
}

void PageManager::StopRecordingWrites(VAddr addr, u64 size, bool restore_protection) const {
    impl->UpdateRecorderBits<false>(addr, size, restore_protection);
}

void PageManager::CollectRecorderDirty(VAddr addr, u64 size, std::vector<u64>& pages) const {
    Impl::CollectRecorderDirty(addr, size, pages);
}

u64 PageManager::RecorderWriteFaults() {
    return Impl::recorder_faults.load(std::memory_order_relaxed);
}

template <bool track>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, false>(addr, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore
