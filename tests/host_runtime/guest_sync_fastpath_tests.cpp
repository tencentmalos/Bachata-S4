// SPDX-License-Identifier: GPL-2.0-or-later
// Real FEX execution of the production guest synchronization payload
// (guest/runtime/sync/mutex.c) against the production GuestMutexDomain.
// Verifies the uncontended path makes zero HLE calls, that guest and host
// callers interoperate on one object through the shared word protocol, and
// that contention, recursion, errors and cancellation behave as on the HLE path.
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include "common/fex_tsc_scale.h"
#include "common/native_clock.h"
#include "core/guest_cpu/fex/fex_context.h"
#include "core/guest_cpu/hle/call_adapter.h"
#include "core/guest_cpu/hle/scope.h"
#include "core/guest_cpu/hle/veneer_allocator.h"
#include "core/host_runtime/guest_mutex.h"
#include "core/host_runtime/guest_rwlock.h"
#include "core/host_runtime/guest_sync_abi.h"
#include "core/host_runtime/guest_sync_arena.h"
#include "guest_sync_fastpath_payload.h"
using namespace Core::GuestCpu;
using namespace Core::GuestCpu::Hle;
using namespace Core::HostRuntime;
using namespace std::chrono_literals;
namespace {
unsigned checks{}, failures{};
void Check(const char* name, bool ok) {
    ++checks;
    failures += !ok;
    std::printf("[SF%02u] %s %s\n", checks, ok ? "PASS" : "FAIL", name);
    std::fflush(stdout);
}
template <class T>
T Must(Result<T> r) {
    if (!r)
        throw std::runtime_error(Describe(r.GetError()));
    return std::move(r).Value();
}
void Must(Status r) {
    if (!r)
        throw std::runtime_error(Describe(r.GetError()));
}

// Same layout as the guest driver (tests/guest_cpu/sync_fastpath_guest/entry.c).
struct GuestRequest {
    uint64_t mode, iterations, slot, counter;
};
enum { ModePosixLoop, ModeTrylock, ModeLock, ModeUnlock, ModeSelf, ModeSceLoop, ModeSceTrylock,
       ModeRecursive, ModeGetspecific, ModeGetthreadid, ModeSetspecific, ModeEqual, ModeProcessTime,
       ModeReadTsc, ModeTscFrequency, ModeRwReadLoop, ModeRwWriteLoop, ModeRwRead, ModeRwTryRead,
       ModeRwWrite, ModeRwTryWrite, ModeRwUnlock, ModeRwSceLoop, ModeRwReadMany, ModeRwUnlockMany,
       ModeRwMixedLoop };

struct Harness;
// One adapter per import slot; each forwards to the production domain exactly
// as the runtime's HLE bindings do, with the owner taken from the calling
// guest thread's handle (the value the payload reads from fs:[0x10]).
struct Import final : HleCallAdapter {
    Harness& h;
    unsigned kind;
    std::atomic<uint64_t> calls{};
    Import(Harness& h, unsigned kind) : h(h), kind(kind) {}
    bool SignatureSupported() const noexcept override { return true; }
    std::string SignatureDescription() const override { return "sync fast path import"; }
    Status Invoke(HleCallFrame& frame) const override;
};

// The harness's own arena window; the payload learns it through the window table.
constexpr uint64_t kArenaBase = 0x1000000000ull;
constexpr uint64_t kArenaLimit = kArenaBase + SHAD_SYNC_ARENA_WINDOW_SIZE;
// The rwlock objects' own window, right above the mutex window.
constexpr uint64_t kRwBase = kArenaLimit;
constexpr uint64_t kRwLimit = kRwBase + SHAD_SYNC_ARENA_WINDOW_SIZE;

struct Harness {
    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> cpu;
    std::unique_ptr<GuestSyncArena> arena;
    std::unique_ptr<GuestMutexDomain> domain;
    std::unique_ptr<GuestSyncArena> rw_arena;
    std::unique_ptr<GuestRwlockDomain> rw;
    uint64_t base{}, code{}, data{}, tcb{}, stacks{}, handles_block{}, keys{}, arena_next{},
        rw_next{};
    // The runtime's GuestClock: host counter shifted by the FEX SMALLTSCSCALE amount.
    Common::NativeClock host_clock;
    const uint64_t clock_origin = host_clock.GetUptime();
    const Common::FexTscScale tsc_scale{host_clock.GetTscFrequency()};
    std::array<std::shared_ptr<Import>, ShadSyncImportCount> imports;
    std::array<uint64_t, ShadSyncImportCount> veneers{};
    std::mutex handles_mutex;
    std::map<uint64_t, uint64_t> handle_by_thread; // thread id -> guest handle value
    ThreadHandle handles[4]{};
    static constexpr unsigned Slots = 4;
    static uint64_t Offset(std::string_view name) {
        for (const auto& s : SyncFastPathGuest::Symbols)
            if (name == s.name) return s.offset;
        throw std::runtime_error("payload symbol missing: " + std::string(name));
    }
    Harness() {
        AddressSpaceConfig cfg{};
        // Place the reservation so the arena window [kArenaBase, kArenaLimit)
        // lies inside it: the payload only takes its fast path for objects
        // inside the window the host publishes.
        cfg.reservation_size = 0x30000000; // mutex window, then the rwlock window
        cfg.preferred_base = kArenaBase - 0x10000000;
        cfg.max_address = QueryBackendCapabilities().max_guest_address;
        space = Must(GuestAddressSpace::Create(cfg));
        base = space->ReservationBase().value;
        if (base != cfg.preferred_base)
            throw std::runtime_error("reservation did not land at the preferred base");
        code = base + 0x10000;
        data = base + 0x20000;
        tcb = base + 0x30000;
        stacks = base + 0x40000;
        // Thread handles with the runtime layout: SHAD_SYNC_TLS_KEYS records end at
        // each handle, then the 64-byte handle object (thread id at +0x10).
        handles_block = base + 0xa0000;
        keys = data + 0x2000;
        arena_next = kArenaBase;
        for (auto va : {code, data, tcb})
            Must(space->Map({GuestAddress{va}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
        Must(space->Map({GuestAddress{stacks}, 0x10000 * Slots},
                        GuestPermission::Read | GuestPermission::Write));
        Must(space->Map({GuestAddress{handles_block}, 0x2000 * Slots},
                        GuestPermission::Read | GuestPermission::Write));
        if (sizeof(SyncFastPathGuest::Image) > 0x4000)
            throw std::runtime_error("payload image too large");
        cpu = Must(CreateContext({}, *space));
        arena = std::make_unique<GuestSyncArena>([this] {
            const auto block = arena_next;
            arena_next += GuestSyncArena::BlockSize;
            Must(space->Map({GuestAddress{block}, GuestSyncArena::BlockSize},
                            GuestPermission::Read | GuestPermission::Write));
            return block;
        });
        domain = std::make_unique<GuestMutexDomain>(*space, [this] { return arena->Allocate(); });
        rw_next = kRwBase;
        rw_arena = std::make_unique<GuestSyncArena>([this] {
            const auto block = rw_next;
            rw_next += GuestSyncArena::BlockSize;
            Must(space->Map({GuestAddress{block}, GuestSyncArena::BlockSize},
                            GuestPermission::Read | GuestPermission::Write));
            return block;
        });
        // Rwlock waits park on the mutex domain's waiters: the instance behind the wake
        // import, as in the runtime.
        rw = std::make_unique<GuestRwlockDomain>(*space, [this] { return rw_arena->Allocate(); },
                                                 &domain->Waiters());
        auto* registry = static_cast<HleCallRegistry*>(Fex::FexHleRegistryPointer(*cpu));
        if (!registry)
            throw std::runtime_error("FEX HLE registry unavailable");
        auto allocator = Must(HleVeneerAllocator::Create(*space, {GuestAddress{base + 0x90000}, 0x4000}));
        for (unsigned i = 0; i < ShadSyncImportCount; ++i) {
            imports[i] = std::make_shared<Import>(*this, i);
            veneers[i] = Must(allocator.Allocate(Must(registry->Adopt(imports[i], "SyncFastPath.Import")))).value;
        }
        Must(allocator.Seal());
        // Publish the payload with its import table filled, exactly as the runtime does.
        std::vector<std::byte> image(sizeof(SyncFastPathGuest::Image));
        std::memcpy(image.data(), SyncFastPathGuest::Image, sizeof(SyncFastPathGuest::Image));
        const auto table = Offset(SHAD_SYNC_IMPORT_TABLE);
        for (unsigned i = 0; i < ShadSyncImportCount; ++i)
            std::memcpy(image.data() + table + 8 * i, &veneers[i], 8);
        const auto window = Offset(SHAD_SYNC_WINDOW_TABLE);
        // Clock slots exactly as GuestRuntime::InstallSyncFastPath fills them.
        const uint64_t bounds[ShadSyncWindowCount] = {kArenaBase, kArenaLimit, keys,
                                                      tsc_scale.ToGuest(clock_origin),
                                                      tsc_scale.Frequency(), kRwBase, kRwLimit};
        for (unsigned i = 0; i < ShadSyncWindowCount; ++i)
            std::memcpy(image.data() + window + 8 * i, &bounds[i], 8);
        Must(space->Write(GuestAddress{code}, image));
        Must(space->Protect({GuestAddress{code}, 0x4000}, GuestPermission::Read | GuestPermission::Execute));
    }
    template <class T>
    void Put(uint64_t va, const T& x) {
        Must(space->Write(GuestAddress{va}, std::as_bytes(std::span{&x, 1})));
    }
    template <class T>
    T Read(uint64_t va) {
        T x{};
        Must(space->Read(GuestAddress{va}, std::as_writable_bytes(std::span{&x, 1})));
        return x;
    }
    uint64_t Slot(unsigned index) { return data + 8 * index; }        // mutex handle slots
    uint64_t Counter(unsigned index) { return data + 0x80 + 8 * index; }
    // [rwlock read holds][thread-specific records][handle] per slot, as in the runtime.
    uint64_t Handle(unsigned slot) { return handles_block + slot * 0x2000 + SHAD_SYNC_RW_HOLDS_BELOW_HANDLE; }
    uint64_t Holds(unsigned slot) { return Handle(slot) - SHAD_SYNC_RW_HOLDS_BELOW_HANDLE; }
    uint64_t RwSlot(unsigned index) { return data + 0x200 + 8 * index; } // rwlock handle slots
    void RwRequest(unsigned slot, uint64_t mode, uint64_t iterations, unsigned rw_index = 0,
                   unsigned counter_index = 0) {
        Put(data + 0x100 + slot * 0x40, GuestRequest{mode, iterations, RwSlot(rw_index), Counter(counter_index)});
    }
    uint64_t SpecificRecord(unsigned slot, unsigned key) {
        return Handle(slot) - SHAD_SYNC_TLS_VALUES_BELOW_HANDLE + 16 * key;
    }
    void SetKey(unsigned key, uint64_t sequence, bool allocated) {
        Put<uint64_t>(keys + 8 * key, (sequence << 1) | (allocated ? 1 : 0));
    }
    void SetSpecific(unsigned slot, unsigned key, uint64_t sequence, uint64_t value) {
        Put(SpecificRecord(slot, key), std::array<uint64_t, 2>{sequence, value});
    }
    void Request(unsigned slot, uint64_t mode, uint64_t iterations, unsigned mutex_index = 0,
                 unsigned counter_index = 0) {
        Put(data + 0x100 + slot * 0x40, GuestRequest{mode, iterations, Slot(mutex_index), Counter(counter_index)});
    }
    GuestCallResult Run(unsigned slot, std::barrier<>* ready = nullptr) {
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{code + SyncFastPathGuest::EntryOffset};
        init.initial_rsp = GuestAddress{stacks + slot * 0x10000 + 0xff0};
        init.guest_tid = 100 + slot;
        // Minimal TCB: fs:[0x10] holds the thread handle, like Tcb::tcb_thread.
        const auto tcb_va = tcb + slot * 0x100;
        Put(tcb_va + 0x10, Handle(slot));
        init.initial_state.fields = RegisterValidity::SegmentBases;
        init.initial_state.values.fs_base = tcb_va;
        init.initial_state.values.gs_base = 0;
        auto thread = Must(cpu->CreateThread(init));
        {
            std::lock_guard lock(handles_mutex);
            handles[slot] = thread;
            handle_by_thread[thread.id] = Handle(slot);
        }
        if (ready)
            ready->arrive_and_wait();
        GuestCallArgs args{};
        args.count = 1;
        args.values[0] = data + 0x100 + slot * 0x40;
        auto result = cpu->InvokeGuest(thread, init.entry_rip, args, {});
        {
            std::lock_guard lock(handles_mutex);
            handles[slot] = {};
        }
        Must(cpu->DestroyThread(thread));
        return Must(std::move(result));
    }
    uint64_t OwnerOf(ThreadHandle thread) {
        std::lock_guard lock(handles_mutex);
        return handle_by_thread.at(thread.id);
    }
    void Cancel(unsigned slot) {
        std::lock_guard lock(handles_mutex);
        if (handles[slot].IsValid())
            Must(cpu->RequestInterrupt(handles[slot], InterruptReason::Cancel));
    }
    GuestCallResult Join(std::future<GuestCallResult>& future) {
        if (future.wait_for(20s) != std::future_status::ready) {
            Check("bounded completion", false);
            for (unsigned i = 0; i < Slots; ++i) Cancel(i);
            if (future.wait_for(3s) != std::future_status::ready)
                std::_Exit(3);
        }
        return future.get();
    }
    uint64_t HleCalls() const {
        uint64_t total{};
        for (const auto& i : imports) total += i->calls.load();
        return total;
    }
};

Status Import::Invoke(HleCallFrame& frame) const {
    auto& self = *const_cast<Import*>(this);
    ++self.calls;
    auto* scope = HleScope::Current();
    if (!scope)
        return MakeError(ErrorCategory::WrongState, "SyncFastPath.Import", "missing HLE scope");
    const auto a0 = frame.registers.Get(Gpr::Rdi), a1 = frame.registers.Get(Gpr::Rsi),
               a2 = frame.registers.Get(Gpr::Rdx);
    uint64_t result{};
    switch (kind) {
    case ShadSyncImportWait:
        result = h.domain->Waiters().Wait(a0, a1, unsigned(a2), scope->CancellationToken());
        break;
    case ShadSyncImportWake:
        result = h.domain->Waiters().Wake(a0, a1);
        break;
    case ShadSyncImportMutexLock:
        result = h.domain->Lock(a0, h.OwnerOf(scope->Thread()), false, scope->CancellationToken());
        break;
    case ShadSyncImportMutexTrylock:
        result = h.domain->Lock(a0, h.OwnerOf(scope->Thread()), true, scope->CancellationToken());
        break;
    case ShadSyncImportMutexUnlock:
        result = h.domain->Unlock(a0, h.OwnerOf(scope->Thread()));
        break;
    case ShadSyncImportRwlockRdlock:
    case ShadSyncImportRwlockTryrdlock:
    case ShadSyncImportRwlockWrlock:
    case ShadSyncImportRwlockTrywrlock:
        result = uint64_t(h.rw->Lock(a0, h.OwnerOf(scope->Thread()),
                                     kind == ShadSyncImportRwlockWrlock || kind == ShadSyncImportRwlockTrywrlock,
                                     kind == ShadSyncImportRwlockTryrdlock || kind == ShadSyncImportRwlockTrywrlock,
                                     scope->CancellationToken()));
        break;
    case ShadSyncImportRwlockUnlock:
        result = uint64_t(h.rw->Unlock(a0, h.OwnerOf(scope->Thread())));
        break;
    default:
        return MakeError(ErrorCategory::InvalidArgument, "SyncFastPath.Import", "unknown import");
    }
    frame.registers.Set(Gpr::Rax, result);
    return Ok();
}
} // namespace

int main() {
    try {
        Harness h;
        auto& d = *h.domain;
        std::printf("payload image=%zu entry=%llu code=%llx arena=%llx\n",
                    sizeof(SyncFastPathGuest::Image), (unsigned long long)SyncFastPathGuest::EntryOffset,
                    (unsigned long long)h.code, (unsigned long long)kArenaBase);
        // Slots start as static initializers (0): the first lock goes through
        // the HLE fallback, which creates the arena object, and everything
        // afterwards stays inside the guest.
        h.Put<uint64_t>(h.Slot(0), 0);
        h.Put<uint64_t>(h.Counter(0), 0);
        h.Request(0, ModeSelf, 0);
        Check("pthread_self returns the fs:[0x10] handle without HLE",
              h.Run(0).return_value == h.Handle(0) && h.HleCalls() == 0);
        h.Request(0, ModePosixLoop, 1);
        auto first = h.Run(0);
        const auto object = h.Read<uint64_t>(h.Slot(0));
        Check("static initializer takes one HLE lock that creates the object",
              first.reason == StopReason::Returned && first.return_value == 0 &&
                  h.imports[ShadSyncImportMutexLock]->calls == 1 && object >= kArenaBase &&
                  object < kArenaLimit && h.Read<uint64_t>(h.Counter(0)) == 1);
        const auto before = h.HleCalls();
        h.Request(0, ModePosixLoop, 10000);
        const auto begin = std::chrono::steady_clock::now();
        auto loop = h.Run(0);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - begin).count();
        Check("10000 uncontended lock/unlock pairs make zero HLE calls",
              loop.reason == StopReason::Returned && loop.return_value == 0 &&
                  h.Read<uint64_t>(h.Counter(0)) == 10001 && h.HleCalls() == before);
        std::printf("BENCH uncontended pairs=10000 elapsed_ns=%lld\n", (long long)ns);
        Check("guest release leaves the ABI prefix free",
              h.Read<uint64_t>(object + SHAD_SYNC_MUTEX_OWNER) == 0 &&
                  h.Read<uint32_t>(object + SHAD_SYNC_MUTEX_STATE) == SHAD_SYNC_STATE_FREE &&
                  h.Read<uint32_t>(object + SHAD_SYNC_MUTEX_COUNT) == 0);
        h.Request(0, ModeSceLoop, 100);
        Check("scePthread entries share the same object and protocol",
              h.Run(0).return_value == 0 && h.Read<uint64_t>(h.Counter(0)) == 10101 &&
                  h.HleCalls() == before);

        // Host HLE view of guest ownership and vice versa.
        h.Request(0, ModeLock, 0);
        Check("guest lock is visible to host IsOwned",
              h.Run(0).return_value == 0 && d.IsOwned(h.Slot(0), h.Handle(0)));
        Check("host trylock sees the guest-held object busy",
              d.Lock(h.Slot(0), 55, true, {}) == POSIX_EBUSY);
        Check("host unlock by a non-owner is refused", d.Unlock(h.Slot(0), 55) == POSIX_EPERM);
        h.Request(1, ModeTrylock, 0);
        Check("guest trylock from another thread is busy", h.Run(1).return_value == POSIX_EBUSY);
        h.Request(1, ModeUnlock, 0);
        Check("guest unlock from a non-owner is refused", h.Run(1).return_value == POSIX_EPERM);
        auto host_waiter = std::async(std::launch::async, [&] { return d.Lock(h.Slot(0), 55, false, {}); });
        const auto until = std::chrono::steady_clock::now() + 3s;
        while (d.PendingWaits(h.Slot(0)) == 0 && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        Check("host contender parks on the guest-held word", d.PendingWaits(h.Slot(0)) == 1);
        const auto wakes = h.imports[ShadSyncImportWake]->calls.load();
        h.Request(0, ModeUnlock, 0);
        Check("guest unlock wakes the host contender through the wake import",
              h.Run(0).return_value == 0 && host_waiter.get() == 0 &&
                  h.imports[ShadSyncImportWake]->calls == wakes + 1 && d.IsOwned(h.Slot(0), 55));
        // Now the host owns it: a guest lock must park on the wait import.
        h.Request(0, ModeLock, 0);
        const auto waits = h.imports[ShadSyncImportWait]->calls.load();
        auto guest_waiter = std::async(std::launch::async, [&] { return h.Run(0); });
        while (h.imports[ShadSyncImportWait]->calls == waits && std::chrono::steady_clock::now() < until + 3s)
            std::this_thread::yield();
        Check("guest contender parks on the host-held word", h.imports[ShadSyncImportWait]->calls > waits &&
                                                              d.PendingWaits(h.Slot(0)) == 1);
        Check("host unlock hands the object to the parked guest",
              d.Unlock(h.Slot(0), 55) == 0 && h.Join(guest_waiter).return_value == 0 &&
                  d.IsOwned(h.Slot(0), h.Handle(0)));
        h.Request(0, ModeUnlock, 0);
        Check("guest releases after host handoff", h.Run(0).return_value == 0 && !d.IsOwned(h.Slot(0), h.Handle(0)));

        // Contention between real FEX owners: every increment is protected.
        h.Put<uint64_t>(h.Counter(1), 0);
        for (unsigned i = 0; i < 4; ++i) h.Request(i, ModePosixLoop, 25000, 0, 1);
        std::barrier ready(4);
        std::array<std::future<GuestCallResult>, 4> runs;
        for (unsigned i = 0; i < 4; ++i)
            runs[i] = std::async(std::launch::async, [&, i] { return h.Run(i, &ready); });
        bool all_returned = true;
        for (auto& r : runs) {
            const auto result = h.Join(r);
            all_returned &= result.reason == StopReason::Returned && result.return_value == 0;
        }
        Check("four real FEX owners preserve 100000 protected increments",
              all_returned && h.Read<uint64_t>(h.Counter(1)) == 100000 &&
                  h.Read<uint32_t>(object + SHAD_SYNC_MUTEX_STATE) == SHAD_SYNC_STATE_FREE);
        Check("contended path used host wait/wake and no other HLE",
              h.imports[ShadSyncImportWait]->calls > waits && h.imports[ShadSyncImportWake]->calls > wakes &&
                  h.imports[ShadSyncImportMutexLock]->calls == 1 &&
                  h.imports[ShadSyncImportMutexUnlock]->calls == 0);
        std::printf("contended waits=%llu wakes=%llu\n",
                    (unsigned long long)h.imports[ShadSyncImportWait]->calls.load(),
                    (unsigned long long)h.imports[ShadSyncImportWake]->calls.load());

        // Thread-specific reads and the thread id never leave the guest.
        {
            const auto before = h.HleCalls();
            const auto get = [&](unsigned slot, uint64_t key) {
                h.Request(slot, ModeGetspecific, key);
                return h.Run(slot).return_value;
            };
            h.SetKey(3, 7, true);
            h.SetSpecific(0, 3, 7, 0x1111);
            h.SetSpecific(1, 3, 7, 0x2222);
            Check("getspecific returns the calling thread's current value",
                  get(0, 3) == 0x1111 && get(1, 3) == 0x2222);
            Check("getspecific ignores the undefined upper half of an int key",
                  get(0, 0xdead000000000003ull) == 0x1111);
            h.SetSpecific(2, 3, 6, 0x3333);
            Check("value stored under an older key sequence reads as 0", get(2, 3) == 0);
            h.SetKey(3, 7, false);
            Check("deleted key reads as 0", get(0, 3) == 0);
            h.SetKey(255, 1, true);
            h.SetSpecific(0, 255, 1, 0x4444);
            Check("last key in range", get(0, 255) == 0x4444);
            Check("out-of-range and negative keys read as 0",
                  get(0, 256) == 0 && get(0, 0xffffffffull) == 0);
            h.Put<uint64_t>(h.Handle(1) + SHAD_SYNC_HANDLE_THREAD_ID, 42);
            h.Request(1, ModeGetthreadid, 0);
            Check("getthreadid reads the id stored in the handle", h.Run(1).return_value == 42);
            Check("thread-specific and thread-id paths made no HLE call", h.HleCalls() == before);
        }

        // Leaf calls: setspecific, pthread_equal and the clocks never leave the guest.
        {
            const auto before = h.HleCalls();
            const auto set = [&](unsigned slot, uint64_t key, uint64_t value) {
                h.Put(h.data + 0x100 + slot * 0x40, GuestRequest{ModeSetspecific, key, value, 0});
                return h.Run(slot).return_value;
            };
            const auto get = [&](unsigned slot, uint64_t key) {
                h.Request(slot, ModeGetspecific, key);
                return h.Run(slot).return_value;
            };
            h.SetKey(5, 9, true);
            Check("setspecific stores the value under the key's current sequence",
                  set(0, 5, 0xabcd) == 0 && get(0, 5) == 0xabcd &&
                      h.Read<std::array<uint64_t, 2>>(h.SpecificRecord(0, 5)) ==
                          std::array<uint64_t, 2>{9, 0xabcd});
            Check("setspecific writes only the calling thread's record", get(1, 5) == 0);
            Check("setspecific ignores the undefined upper half of an int key",
                  set(1, 0xfeed000000000005ull, 0x77) == 0 && get(1, 5) == 0x77);
            h.SetKey(6, 2, false);
            Check("setspecific on an unallocated or out-of-range key is EINVAL",
                  set(0, 6, 1) == POSIX_EINVAL && set(0, 256, 1) == POSIX_EINVAL &&
                      set(0, 0xffffffffull, 1) == POSIX_EINVAL);
            const auto equal = [&](uint64_t a, uint64_t b) {
                h.Put(h.data + 0x100, GuestRequest{ModeEqual, a, b, 0});
                return h.Run(0).return_value;
            };
            Check("pthread_equal compares the full handles",
                  equal(h.Handle(0), h.Handle(0)) == 1 && equal(h.Handle(0), h.Handle(1)) == 0 &&
                      equal(0x100000001ull, 1) == 0);
            const auto call = [&](uint64_t mode) {
                h.Request(0, mode, 0);
                return h.Run(0).return_value;
            };
            const auto host_process = [&] { return h.tsc_scale.ToGuest(h.host_clock.GetUptime() - h.clock_origin); };
            const auto host_tsc = [&] { return h.tsc_scale.ToGuest(h.host_clock.GetUptime()); };
            bool process_ok = true, tsc_ok = true;
            for (int i = 0; i < 200; ++i) {
                const auto lo = host_process();
                const auto value = call(ModeProcessTime);
                process_ok &= lo <= value && value <= host_process();
                const auto tlo = host_tsc();
                const auto tsc = call(ModeReadTsc);
                tsc_ok &= tlo <= tsc && tsc <= host_tsc();
            }
            std::printf("tsc scale shift=%u frequency=%llu\n", h.tsc_scale.Shift(),
                        (unsigned long long)h.tsc_scale.Frequency());
            Check("guest process counter lies between host GuestClock reads (200 samples)", process_ok);
            Check("guest rdtsc lies between host ReadTsc reads (200 samples)", tsc_ok);
            Check("TSC frequency is the scaled host frequency",
                  call(ModeTscFrequency) == h.tsc_scale.Frequency());
            Check("leaf paths made no HLE call", h.HleCalls() == before);
        }

        // Recursive type through the attribute path; depth handled in the guest.
        const auto attr = h.data + 0x70;
        h.Put<uint64_t>(attr, 0);
        h.Put<uint64_t>(h.Slot(1), 0);
        Check("recursive attribute init", d.AttributeInit(attr) == 0 && d.Attribute(attr, SHAD_SYNC_TYPE_RECURSIVE, 1) == 0 &&
                                              d.Init(h.Slot(1), attr) == 0);
        const auto recursive_hle = h.HleCalls();
        h.Put<uint64_t>(h.Counter(2), 0);
        h.Request(0, ModeRecursive, 0, 1, 2);
        Check("recursive lock twice/unlock twice stays in the guest",
              h.Run(0).return_value == 0 && h.Read<uint64_t>(h.Counter(2)) == 1 && h.HleCalls() == recursive_hle &&
                  h.Read<uint32_t>(h.Read<uint64_t>(h.Slot(1)) + SHAD_SYNC_MUTEX_COUNT) == 0);
        // Errorcheck (default type 1) object on slot 0, free now: self-lock errors.
        h.Request(0, ModeLock, 0, 0);
        Check("errorcheck first lock", h.Run(0).return_value == 0);
        h.Request(0, ModeLock, 0, 0);
        Check("errorcheck self-lock returns EDEADLK without HLE",
              h.Run(0).return_value == POSIX_EDEADLK && h.HleCalls() == recursive_hle);
        h.Request(0, ModeSceTrylock, 0, 0);
        Check("sce trylock on own errorcheck object reports SCE EBUSY",
              h.Run(0).return_value == (0x80020000ull | POSIX_EBUSY));
        h.Request(0, ModeUnlock, 0, 0);
        Check("errorcheck release", h.Run(0).return_value == 0);

        // Destroyed handle: slot value 2 leaves the arena window -> HLE EINVAL.
        h.Put<uint64_t>(h.Slot(2), 2);
        h.Request(0, ModeLock, 0, 2);
        Check("destroyed handle falls back to HLE and reports EINVAL",
              h.Run(0).return_value == POSIX_EINVAL && h.imports[ShadSyncImportMutexLock]->calls == 2);

        // Cancellation while parked on a host-held object.
        Check("host takes object for cancellation scenario", d.Lock(h.Slot(0), 66, false, {}) == 0);
        h.Request(0, ModeLock, 0, 0);
        const auto parked = h.imports[ShadSyncImportWait]->calls.load();
        auto cancelled = std::async(std::launch::async, [&] { return h.Run(0); });
        const auto limit = std::chrono::steady_clock::now() + 3s;
        while (d.PendingWaits(h.Slot(0)) == 0 && std::chrono::steady_clock::now() < limit)
            std::this_thread::yield();
        Check("guest parked in the cancellable wait import",
              h.imports[ShadSyncImportWait]->calls > parked && d.PendingWaits(h.Slot(0)) == 1);
        h.Cancel(0);
        Check("cancellation ends the guest wait", h.Join(cancelled).reason == StopReason::Cancelled &&
                                                    d.PendingWaits(h.Slot(0)) == 0);
        Check("host still owns and can release after the cancelled contender",
              d.IsOwned(h.Slot(0), 66) && d.Unlock(h.Slot(0), 66) == 0);
        h.Request(0, ModePosixLoop, 10);
        Check("fresh owner uses the object normally after cancellation",
              h.Run(0).return_value == 0 && d.Destroy(h.Slot(0)) == 0 && d.Destroy(h.Slot(1)) == 0);

        // ---- rwlocks: same window/word scheme, per-thread read-hold tables ----
        {
            auto& rw = *h.rw;
            const auto word = [&](uint64_t object) { return h.Read<uint32_t>(object + SHAD_SYNC_RW_WORD); };
            const auto writer = [&](uint64_t object) { return h.Read<uint64_t>(object + SHAD_SYNC_RW_WRITER); };
            const auto hold = [&](unsigned slot, unsigned entry) {
                return h.Read<std::array<uint64_t, 2>>(h.Holds(slot) + 16 + 16 * entry);
            };
            const auto run = [&](unsigned slot) { return h.Run(slot).return_value; };
            const auto wait_pending = [&](uint64_t slot, unsigned n) {
                const auto limit = std::chrono::steady_clock::now() + 3s;
                while (rw.Pending(slot) < n && std::chrono::steady_clock::now() < limit)
                    std::this_thread::yield();
                return rw.Pending(slot) == n;
            };
            const auto calls = [&](unsigned kind) { return h.imports[kind]->calls.load(); };

            h.Put<uint64_t>(h.RwSlot(0), 0);
            h.RwRequest(0, ModeRwReadLoop, 1, 0);
            const auto first = run(0);
            const auto object = h.Read<uint64_t>(h.RwSlot(0));
            Check("rwlock static initializer: one HLE rdlock creates the object in the rwlock window",
                  first == 0 && calls(ShadSyncImportRwlockRdlock) == 1 &&
                      calls(ShadSyncImportRwlockUnlock) == 0 && object >= kRwBase && object < kRwLimit &&
                      word(object) == 0 && hold(0, 0)[0] == 0);
            const auto before = h.HleCalls();
            h.RwRequest(0, ModeRwReadLoop, 10000, 0);
            auto begin = std::chrono::steady_clock::now();
            const auto reads = run(0);
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
            Check("10000 uncontended rdlock/unlock pairs make zero HLE calls",
                  reads == 0 && h.HleCalls() == before && word(object) == 0);
            std::printf("BENCH rwlock read pairs=10000 elapsed_ns=%lld\n", (long long)ns);
            h.Put<uint64_t>(h.Counter(3), 0);
            h.RwRequest(0, ModeRwWriteLoop, 10000, 0, 3);
            begin = std::chrono::steady_clock::now();
            const auto writes = run(0);
            ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
            Check("10000 uncontended wrlock/unlock pairs make zero HLE calls",
                  writes == 0 && h.Read<uint64_t>(h.Counter(3)) == 10000 && h.HleCalls() == before &&
                      word(object) == 0 && writer(object) == 0);
            std::printf("BENCH rwlock write pairs=10000 elapsed_ns=%lld\n", (long long)ns);
            h.RwRequest(0, ModeRwSceLoop, 100, 0, 3);
            Check("scePthreadRwlock entries share the object without HLE",
                  run(0) == 0 && h.Read<uint64_t>(h.Counter(3)) == 10100 && h.HleCalls() == before);

            // Read holds and recursion live in the thread's table; the host sees them.
            h.RwRequest(0, ModeRwRead, 0, 0);
            Check("guest rdlock counts once in the word and records the hold",
                  run(0) == 0 && word(object) == 1 && hold(0, 0) == std::array<uint64_t, 2>{object, 1});
            h.RwRequest(0, ModeRwRead, 0, 0);
            Check("guest recursive rdlock only deepens the table entry",
                  run(0) == 0 && word(object) == 1 && hold(0, 0)[1] == 2 && h.HleCalls() == before);
            h.RwRequest(0, ModeRwTryWrite, 0, 0);
            Check("reader trywrlock on its own lock is EBUSY from the HLE",
                  run(0) == POSIX_EBUSY && calls(ShadSyncImportRwlockTrywrlock) == 1);
            Check("host writer trylock sees the guest read hold",
                  rw.Lock(h.RwSlot(0), 55, true, true, {}) == POSIX_EBUSY);
            auto host_writer = std::async(std::launch::async, [&] { return rw.Lock(h.RwSlot(0), 55, true, false, {}); });
            Check("host writer parks and publishes WRITE_WAITERS",
                  wait_pending(h.RwSlot(0), 1) && (word(object) & SHAD_SYNC_RW_WRITE_WAITERS));
            h.RwRequest(0, ModeRwUnlock, 0, 0);
            Check("inner guest unlock keeps the hold",
                  run(0) == 0 && host_writer.wait_for(0ms) != std::future_status::ready &&
                      (word(object) & SHAD_SYNC_RW_READERS) == 1);
            const auto wakes = calls(ShadSyncImportWake);
            h.RwRequest(0, ModeRwUnlock, 0, 0);
            Check("last guest unlock wakes the parked host writer through the wake import",
                  run(0) == 0 && host_writer.get() == 0 && calls(ShadSyncImportWake) == wakes + 1 &&
                      word(object) == SHAD_SYNC_RW_WRITE_LOCKED && writer(object) == 55 && hold(0, 0)[0] == 0);
            h.RwRequest(1, ModeRwRead, 0, 0);
            auto guest_reader = std::async(std::launch::async, [&] { return h.Run(1); });
            Check("guest rdlock parks in the HLE behind the host writer",
                  wait_pending(h.RwSlot(0), 1) && calls(ShadSyncImportRwlockRdlock) == 2);
            Check("host write unlock hands the lock to the parked guest reader",
                  rw.Unlock(h.RwSlot(0), 55) == 0 && h.Join(guest_reader).return_value == 0 &&
                      word(object) == 1 && hold(1, 0)[0] == object);
            h.RwRequest(1, ModeRwUnlock, 0, 0);
            Check("guest reader recorded by the HLE releases in the guest",
                  run(1) == 0 && word(object) == 0 && calls(ShadSyncImportRwlockUnlock) == 0);

            // Errors come from the HLE with the same codes as before.
            h.RwRequest(0, ModeRwWrite, 0, 0);
            Check("guest wrlock stores the owner handle",
                  run(0) == 0 && word(object) == SHAD_SYNC_RW_WRITE_LOCKED && writer(object) == h.Handle(0));
            h.RwRequest(0, ModeRwRead, 0, 0);
            const auto self_read = run(0);
            h.RwRequest(0, ModeRwWrite, 0, 0);
            const auto self_write = run(0);
            Check("writer rdlock/wrlock on its own lock: EDEADLK",
                  self_read == POSIX_EDEADLK && self_write == POSIX_EDEADLK);
            h.RwRequest(1, ModeRwUnlock, 0, 0);
            Check("unlock by a non-holder: EPERM", run(1) == POSIX_EPERM);
            h.RwRequest(0, ModeRwUnlock, 0, 0);
            Check("writer unlock clears word and owner", run(0) == 0 && word(object) == 0 && writer(object) == 0);

            // Type 1: a parked writer keeps new readers out; the thread already reading
            // may read again.
            const auto attr = h.data + 0x78;
            h.Put<uint64_t>(attr, 0);
            h.Put<uint64_t>(h.RwSlot(1), 0);
            Check("writer-preferring rwlock init",
                  rw.Attribute(attr, 0) == 0 && rw.Attribute(attr, 5, 1) == 0 && rw.Init(h.RwSlot(1), attr) == 0);
            const auto pref = h.Read<uint64_t>(h.RwSlot(1));
            const auto pref_hle = h.HleCalls();
            h.RwRequest(0, ModeRwRead, 0, 1);
            Check("type 1 guest rdlock in the guest", run(0) == 0 && h.HleCalls() == pref_hle && word(pref) == 1);
            auto pref_writer = std::async(std::launch::async, [&] { return rw.Lock(h.RwSlot(1), 77, true, false, {}); });
            Check("type 1 host writer parks", wait_pending(h.RwSlot(1), 1));
            h.RwRequest(1, ModeRwTryRead, 0, 1);
            Check("new reader is kept out by the parked writer (EBUSY)", run(1) == POSIX_EBUSY);
            const auto recursion_hle = h.HleCalls();
            h.RwRequest(0, ModeRwRead, 0, 1);
            Check("the thread already reading reads again in the guest",
                  run(0) == 0 && h.HleCalls() == recursion_hle && hold(0, 0)[1] == 2);
            h.RwRequest(0, ModeRwUnlock, 0, 1);
            run(0);
            h.RwRequest(0, ModeRwUnlock, 0, 1);
            Check("releasing both levels hands the lock to the writer",
                  run(0) == 0 && pref_writer.get() == 0 && writer(pref) == 77);
            Check("writer releases", rw.Unlock(h.RwSlot(1), 77) == 0 && word(pref) == 0);

            // A full table: the 16th distinct hold goes to the host and flags the thread.
            bool created = true;
            for (unsigned i = 0; i < 16; ++i) {
                h.Put<uint64_t>(h.RwSlot(8 + i), 0);
                created &= rw.Init(h.RwSlot(8 + i), 0) == 0;
            }
            h.RwRequest(2, ModeRwReadMany, 16, 8);
            const auto many_rd = calls(ShadSyncImportRwlockRdlock);
            Check("16 distinct read holds: 15 in the guest table, only the 16th through the HLE",
                  created && run(2) == 0 && h.Read<uint64_t>(h.Holds(2)) == 1 && hold(2, 14)[0] != 0 &&
                      calls(ShadSyncImportRwlockRdlock) == many_rd + 1);
            bool counted = true;
            for (unsigned i = 0; i < 16; ++i) counted &= word(h.Read<uint64_t>(h.RwSlot(8 + i))) == 1;
            Check("every lock counts its holder once", counted);
            // Reverse order: the overflow hold goes first (HLE), the flag drops, and the
            // other 15 release in the guest.
            h.RwRequest(2, ModeRwUnlockMany, 16, 8);
            const auto many_unlocks = calls(ShadSyncImportRwlockUnlock);
            bool released = run(2) == 0 && h.Read<uint64_t>(h.Holds(2)) == 0 &&
                            calls(ShadSyncImportRwlockUnlock) == many_unlocks + 1;
            for (unsigned i = 0; i < 16; ++i) released &= word(h.Read<uint64_t>(h.RwSlot(8 + i))) == 0;
            Check("unlocking all clears the overflow flag and every word, one HLE unlock", released);
            const auto after_overflow = h.HleCalls();
            h.RwRequest(2, ModeRwReadLoop, 100, 8);
            Check("the thread is back on the guest path afterwards",
                  run(2) == 0 && h.HleCalls() == after_overflow);

            // Contention between real FEX owners: readers never see a writer's update.
            h.Put<uint64_t>(h.RwSlot(2), 0);
            h.Put<uint64_t>(h.Counter(4), 0);
            for (unsigned i = 0; i < 4; ++i) h.RwRequest(i, ModeRwMixedLoop, 20000, 2, 4);
            std::barrier ready(4);
            std::array<std::future<GuestCallResult>, 4> runs;
            for (unsigned i = 0; i < 4; ++i)
                runs[i] = std::async(std::launch::async, [&, i] { return h.Run(i, &ready); });
            bool all_ok = true;
            for (auto& r : runs) {
                const auto result = h.Join(r);
                all_ok &= result.reason == StopReason::Returned && result.return_value == 0;
            }
            const auto mixed = h.Read<uint64_t>(h.RwSlot(2));
            Check("four FEX owners: 10000 writer increments, readers never see one, word ends free",
                  all_ok && h.Read<uint64_t>(h.Counter(4)) == 10000 && word(mixed) == 0);
            std::printf("rwlock contended: rdlock HLE=%llu wrlock HLE=%llu wakes=%llu\n",
                        (unsigned long long)calls(ShadSyncImportRwlockRdlock),
                        (unsigned long long)calls(ShadSyncImportRwlockWrlock),
                        (unsigned long long)calls(ShadSyncImportWake));

            // Destroyed: the slot leaves the window and the HLE reports EINVAL.
            Check("destroy free locks", rw.Destroy(h.RwSlot(0)) == 0 && rw.Destroy(h.RwSlot(2)) == 0 &&
                                            word(object) == SHAD_SYNC_RW_DESTROYED);
            h.RwRequest(0, ModeRwRead, 0, 0);
            Check("destroyed rwlock falls back to the HLE: EINVAL", run(0) == POSIX_EINVAL);
        }
    } catch (const std::exception& e) {
        Check(e.what(), false);
    }
    std::printf("GUEST_SYNC_FASTPATH checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
