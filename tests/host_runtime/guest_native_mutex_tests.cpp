// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <barrier>
#include <cstdio>
#include <future>
#include <string_view>
#include <thread>
#include "core/host_runtime/guest_mutex.h"
#include "core/host_runtime/guest_sync_arena.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
using namespace std::chrono_literals;
namespace {
unsigned checks{}, failures{};
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", name); }
}
template <class F> void Await(F predicate) {
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) {
            std::fprintf(stderr, "FAIL bounded handshake timeout\n");
            std::_Exit(2);
        }
        std::this_thread::yield();
    }
}
struct Fixture {
    std::unique_ptr<GuestAddressSpace> space;
    u64 base{}, next{};
    std::unique_ptr<GuestMutexDomain> locks;
    Fixture() {
        auto made = GuestAddressSpace::Create({.reservation_size = 16ULL << 20});
        if (!made) std::_Exit(2);
        space = std::move(made).Value();
        base = space->ReservationBase().value;
        if (!space->Map({GuestAddress{base}, 0x800000},
                        GuestPermission::Read | GuestPermission::Write)) std::_Exit(2);
        next = base + HostPageSize();
        locks = std::make_unique<GuestMutexDomain>(*space, [&] {
            const auto address = next;
            next += HostPageSize();
            return address;
        });
    }
};
void Bench(bool native = false) {
    Fixture f;
    std::array<std::mutex, 4> native_locks;
    for (unsigned i = 0; i < 4; ++i) Check(f.locks->Init(f.base + 8*i, 0) == 0, "bench init");
    for (const auto shared : {false, true}) for (unsigned threads : {1, 4}) {
        std::barrier start(threads + 1);
        std::atomic<unsigned> errors{};
        std::array<u64, 4> totals{};
        std::vector<std::jthread> workers;
        constexpr unsigned pairs = 20000;
        for (unsigned i = 0; i < threads; ++i) workers.emplace_back([&, i] {
            start.arrive_and_wait();
            for (unsigned j = 0; j < pairs; ++j) {
                const auto slot = shared ? 0 : i;
                if (native) {
                    std::lock_guard held(native_locks[slot]);
                    ++totals[slot];
                    continue;
                }
                if (f.locks->Lock(f.base + 8*slot, i+1, false, {})) { ++errors; continue; }
                ++totals[slot];
                if (f.locks->Unlock(f.base + 8*slot, i+1)) ++errors;
            }
        });
        const auto begin = std::chrono::steady_clock::now();
        start.arrive_and_wait();
        workers.clear();
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
        u64 total{}; for (const auto n : totals) total += n;
        Check(errors == 0 && total == threads*pairs, "bench protected increments");
        std::printf("%s shared=%d threads=%u pairs=%u ms=%.3f\n",
                    native ? "HOST_BENCH" : "BENCH", shared, threads, pairs*threads, ms);
    }
}
}
int main(int argc, char** argv) {
    if (argc > 1) {
        if (std::string_view(argv[1]) == "--host-compare") {
            // Same protected increment, counts and thread launch protocol. This
            // measures the native domain wrapper, not FEX/HLE crossing overhead.
            for (unsigned round = 0; round < 5; ++round) {
                std::printf("ROUND %u\n", round);
                Bench(round % 2 == 0);
                Bench(round % 2 != 0);
            }
        } else {
            Bench();
        }
        return failures ? 1 : 0;
    }
    {
        Fixture large;
        large.locks.reset();
        large.next = large.base + 0x40000;
        unsigned blocks{};
        GuestSyncArena arena([&] {
            const auto address = large.next;
            large.next += GuestSyncArena::BlockSize;
            ++blocks;
            return address;
        });
        GuestMutexDomain many(*large.space, [&] { return arena.Allocate(); });
        constexpr unsigned count = 20000; // exceeds both old quota and directory capacity
        for (unsigned i = 0; i < count; ++i)
            Check(many.Init(large.base + i * 8, 0) == 0, "large live population");
        for (unsigned i = 0; i < count; ++i) {
            const auto slot = large.base + i * 8;
            Check(many.Lock(slot, i + 1, false, {}) == 0 && many.Unlock(slot, i + 1) == 0,
                  "collision chain preserves independent ownership");
            Check(many.Destroy(slot) == 0 && many.Lock(slot, i + 1, true, {}) == POSIX_EINVAL,
                  "large population retirement stays stale");
        }
        Check(blocks == (count + 63) / 64, "64 ABI objects per guest mapping");
        for (unsigned i = 0; i < 5000; ++i) {
            Check(many.AttributeInit(large.base) == 0 && many.Attribute(large.base, 0, 0) == 0 &&
                  many.CondInit(large.base, 0) == 0 && many.CondDestroy(large.base) == 0,
                  "attribute and condition churn has no cumulative quota");
        }
        bool exhausted = true;
        GuestSyncArena retry([&] { return exhausted ? 0 : large.next; });
        GuestMutexDomain oom(*large.space, [&] { return retry.Allocate(); });
        const u64 zero{};
        Check(bool(large.space->WriteData(GuestAddress{large.base},
                                         std::as_bytes(std::span{&zero, 1}))), "clear OOM slot");
        Check(oom.Init(large.base, 0) == POSIX_ENOMEM &&
                  oom.AttributeInit(large.base) == POSIX_ENOMEM &&
                  oom.CondInit(large.base, 0) == POSIX_ENOMEM,
              "real allocator exhaustion remains ENOMEM");
        exhausted = false;
        Check(oom.Init(large.base, 0) == 0 && oom.Destroy(large.base) == 0,
              "failed block allocation does not poison retry");
    }
    Fixture f;
    auto& d = *f.locks;
    const auto mutex = f.base, other = f.base+8, cond = f.base+16;
    Check(d.Init(mutex, 0) == 0 && d.Init(other, 0) == 0, "init");
    // Logical guest ownership may outlive a physical host invocation.
    Check(std::async(std::launch::async, [&] { return d.Lock(other, 91, false, {}); }).get() == 0,
          "logical owner acquisition on host A");
    Check(std::async(std::launch::async, [&] { return d.Unlock(other, 91); }).get() == 0,
          "same logical owner release on host B");
    Check(d.Lock(other, 91, false, {}) == 0, "other retained owner");
    {
        u64 object{};
        if (!f.space->Read(GuestAddress{mutex}, std::as_writable_bytes(std::span{&object,1}))) std::_Exit(2);
        auto retained = f.space->AcquireDataSpan({GuestAddress{object},HostPageSize()},true);
        if (!retained) std::_Exit(2);
        auto edit = std::async(std::launch::async,[&] {
            return f.space->UpdateDataMapping(GuestAddressSpace::VmOperation::Protect,
                {GuestAddress{object},HostPageSize()},GuestPermission::Read|GuestPermission::Write);
        });
        Await([&]{return f.space->Counts().retiring_ranges == 1;});
        std::promise<void> entered;
        auto blocked = std::async(std::launch::async, [&] {
            entered.set_value(); return d.Lock(mutex, 1, false, {});
        });
        entered.get_future().wait();
        Check(blocked.wait_for(100ms) == std::future_status::timeout, "range retirement blocks one object");
        auto independent = std::async(std::launch::async, [&] { return d.Lock(other, 92, true, {}); });
        const bool independent_ready = independent.wait_for(500ms) == std::future_status::ready;
        // Release before inspecting futures: a failing baseline must also clean up.
        retained = MakeError(ErrorCategory::InvalidArgument, "test", "release");
        Check(bool(edit.get()), "retire only the affected object range");
        Check(independent_ready && independent.get() == POSIX_EBUSY,
              "unrelated busy trylock bypasses blocked object");
        Await([&] { return blocked.wait_for(0ms) == std::future_status::ready; });
        Check(blocked.get() == 0 && d.Unlock(mutex, 1) == 0, "blocked object resumes");
    }
    Check(d.Unlock(other, 91) == 0, "other release");
    for (unsigned round = 0; round < 64; ++round) {
        Check(d.Lock(mutex, 1, false, {}) == 0, "cancellation owner");
        std::stop_source stop;
        auto a = std::async(std::launch::async, [&] {
            const auto r = d.Lock(mutex, 2, false, stop.get_token());
            if (!r && d.Unlock(mutex, 2)) std::_Exit(3);
            return r;
        });
        auto b = std::async(std::launch::async, [&] {
            const auto r = d.Lock(mutex, 3, false, {});
            if (!r && d.Unlock(mutex, 3)) std::_Exit(3);
            return r;
        });
        Await([&] { return d.PendingWaits(mutex) == 2; });
        auto stopping = std::async(std::launch::async, [&] { stop.request_stop(); });
        Check(d.Unlock(mutex, 1) == 0, "cancel/unlock race");
        Await([&] { return a.wait_for(0ms) == std::future_status::ready &&
                          b.wait_for(0ms) == std::future_status::ready &&
                          stopping.wait_for(0ms) == std::future_status::ready; });
        const auto ar = a.get();
        Check((ar == 0 || ar == POSIX_EINTR) && b.get() == 0 && d.PendingWaits(mutex) == 0,
              "cancelled selection cannot strand remaining waiter");
    }
    Check(d.CondInit(cond, 0) == 0, "condition init");
    std::array<std::future<int>, 8> waiters;
    for (unsigned i = 0; i < waiters.size(); ++i) {
        Check(d.Lock(mutex, i+1, false, {}) == 0, "condition owner handoff");
        waiters[i] = std::async(std::launch::async, [&, i] {
            const int r = d.CondWait(cond, mutex, i+1, {});
            if (!r && d.Unlock(mutex, i+1)) std::_Exit(3);
            return r;
        });
        Await([&] { return d.PendingWaits(mutex) == i+1; });
    }
    Check(d.CondDestroy(cond) == POSIX_EBUSY && d.Destroy(mutex) == POSIX_EBUSY,
          "queued condition retains both objects");
    Check(d.CondNotify(cond, false, 6) == 0, "target sixth owner");
    Await([&] { return waiters[5].wait_for(0ms) == std::future_status::ready; });
    Check(waiters[5].get() == 0, "selected owner resumed");
    for (unsigned i = 0; i < waiters.size(); ++i) if (i != 5)
        Check(waiters[i].wait_for(0ms) == std::future_status::timeout, "unselected owners remain waiting");
    Check(d.CondNotify(cond, true) == 0, "broadcast remaining owners");
    for (unsigned i = 0; i < waiters.size(); ++i) if (i != 5) {
        Await([&] { return waiters[i].wait_for(0ms) == std::future_status::ready; });
        Check(waiters[i].get() == 0, "broadcast completion");
    }
    // Pre-requested cancellation invokes stop_callback synchronously. Neither
    // registration nor destruction may take a guard already held by this call.
    std::stop_source stopped; stopped.request_stop();
    Check(d.Lock(mutex, 1, false, {}) == 0, "pre-cancel owner");
    Check(d.Lock(mutex, 2, false, stopped.get_token()) == POSIX_EINTR, "pre-cancel lock");
    Check(d.CondWait(cond, mutex, 1, stopped.get_token()) == POSIX_EINTR && d.IsOwned(mutex, 1),
          "pre-cancel condition retains free mutex reacquire semantics");
    Check(d.Unlock(mutex, 1) == 0, "pre-cancel release");
    for (unsigned round = 0; round < 64; ++round) {
        Check(d.Lock(mutex, 1, false, {}) == 0, "condition race owner");
        std::stop_source stop;
        auto waiter = std::async(std::launch::async, [&] {
            const auto r = d.CondWait(cond, mutex, 1, stop.get_token());
            if (d.IsOwned(mutex, 1) && d.Unlock(mutex, 1)) std::_Exit(3);
            return r;
        });
        Await([&] { return d.PendingWaits(mutex) == 1; });
        auto stopping = std::async(std::launch::async, [&] { stop.request_stop(); });
        Check(d.CondNotify(cond, false) == 0, "signal races cancellation");
        Await([&] { return waiter.wait_for(0ms) == std::future_status::ready &&
                          stopping.wait_for(0ms) == std::future_status::ready; });
        const auto r = waiter.get();
        Check((r == 0 || r == POSIX_EINTR) && d.PendingWaits(mutex) == 0 &&
                  d.PendingReacquires(cond) == 0, "condition callback and queue retire together");
    }
    const auto attr = f.base + 24;
    Check(d.AttributeInit(attr) == 0 && d.Attribute(attr, 2, 1) == 0 &&
              d.Destroy(other) == 0 && d.Init(other, attr) == 0, "recursive condition init");
    Check(d.Lock(other, 19, false, {}) == 0 && d.Lock(other, 19, false, {}) == 0,
          "recursive depth two");
    Check(d.CondWait(cond, other, 19, {}, {.relative_us = 0}) == POSIX_ETIMEDOUT &&
              d.Unlock(other, 19) == 0 && d.IsOwned(other, 19) && d.Unlock(other, 19) == 0,
          "condition timeout restores complete recursive depth");
    Check(d.Attribute(attr, 0, 0) == 0, "recursive attribute retirement");
    const auto recycled = f.base + 32;
    Check(d.Init(recycled, 0) == 0, "retirement race init");
    std::atomic<bool> done{};
    std::atomic<unsigned> errors{};
    std::jthread lookup([&] {
        while (!done.load()) {
            const auto r = d.Lock(recycled, 77, true, {});
            if (!r) { if (d.Unlock(recycled, 77)) ++errors; }
            else if (r != POSIX_EINVAL && r != POSIX_EBUSY) ++errors;
        }
    });
    for (unsigned i = 0; i < 64; ++i) {
        Await([&] { const auto r = d.Destroy(recycled); return r == 0; });
        Check(d.Lock(recycled, 1, true, {}) == POSIX_EINVAL && d.Init(recycled, 0) == 0,
              "retired lookup cannot acquire obsolete object");
    }
    done.store(true); lookup.join();
    Check(errors == 0 && d.Destroy(recycled) == 0, "concurrent lookup lifetime");
    Check(d.CondDestroy(cond) == 0 && d.Destroy(mutex) == 0 && d.Destroy(other) == 0,
          "all objects destroy without dangling waiters");
    std::printf("GUEST_NATIVE_MUTEX checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
