// SPDX-License-Identifier: GPL-2.0-or-later
// Real FEX execution of the production guest synchronization payload
// (guest/runtime/sync/mutex.c) against the production GuestMutexDomain.
// Verifies the uncontended path makes zero HLE calls, that guest and host
// callers interoperate on one object through the shared word protocol, and
// that contention, recursion, errors and cancellation behave as on the HLE path.
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
#include "core/guest_cpu/fex/fex_context.h"
#include "core/guest_cpu/hle/call_adapter.h"
#include "core/guest_cpu/hle/scope.h"
#include "core/guest_cpu/hle/veneer_allocator.h"
#include "core/host_runtime/guest_mutex.h"
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
       ModeRecursive };

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

struct Harness {
    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> cpu;
    std::unique_ptr<GuestSyncArena> arena;
    std::unique_ptr<GuestMutexDomain> domain;
    uint64_t base{}, code{}, data{}, tcb{}, stacks{}, arena_next{};
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
        cfg.reservation_size = 0x20000000;
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
        arena_next = kArenaBase;
        for (auto va : {code, data, tcb})
            Must(space->Map({GuestAddress{va}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
        Must(space->Map({GuestAddress{stacks}, 0x10000 * Slots},
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
        const uint64_t bounds[ShadSyncWindowCount] = {kArenaBase, kArenaLimit};
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
    uint64_t Handle(unsigned slot) { return 0x7700 + slot; }          // guest handle values
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
    } catch (const std::exception& e) {
        Check(e.what(), false);
    }
    std::printf("GUEST_SYNC_FASTPATH checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
