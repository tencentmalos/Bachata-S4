// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <future>
#include <stdexcept>
#include <thread>
#include "compiled_guest/abi.h"
#include "core/guest_cpu/fex/fex_context.h"
#include "core/guest_cpu/hle/call_adapter.h"
#include "core/guest_cpu/hle/scope.h"
#include "core/guest_cpu/hle/veneer_allocator.h"
#include "guest_payload.h"
using namespace Core::GuestCpu;
using namespace Core::GuestCpu::Hle;
using namespace std::chrono_literals;
namespace {
unsigned checks{}, failures{};
void Check(const char* name, bool ok) {
    ++checks;
    failures += !ok;
    std::printf("[CG%02u] %s %s\n", checks, ok ? "PASS" : "FAIL", name);
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

// Fixed backing for this isolated mechanism test. No VM mutation is performed
// until all guest/host waiters join. Production must add mapping identity and
// retirement coordination before using a similar wait primitive.
struct SlowPath final : HleCallAdapter {
    GuestAddressSpace& space;
    const uint64_t state_va;
    std::atomic<uint64_t> calls{}, waits{}, wakes{}, probes{}, entered{};
    mutable std::mutex mutex;
    mutable std::condition_variable_any changed;
    SlowPath(GuestAddressSpace& s, uint64_t va) : space(s), state_va(va) {}
    bool SignatureSupported() const noexcept override {
        return true;
    }
    std::string SignatureDescription() const override {
        return "compiled guest wait/wake probe";
    }
    Status Invoke(HleCallFrame& frame) const override {
        auto& self = *const_cast<SlowPath*>(this);
        const auto address = frame.registers.Get(Gpr::Rdi);
        const auto op = frame.registers.Get(Gpr::Rsi);
        ++self.calls;
        if (op == GuestCrossingProbe) {
            ++self.probes;
        } else {
            if (address != state_va ||
                !space.ValidateRange({GuestAddress{address}, 4},
                                     GuestPermission::Read | GuestPermission::Write))
                return MakeError(ErrorCategory::InvalidArgument, "CompiledGuest.Wait",
                                 "invalid state address");
            std::unique_lock lock(mutex);
            if (op == GuestWait) {
                ++self.waits;
                ++self.entered;
                auto* scope = HleScope::Current();
                if (!scope)
                    return MakeError(ErrorCategory::WrongState, "CompiledGuest.Wait",
                                     "missing HLE scope");
                // Expected-value check under the same gate as Wake prevents a
                // release between checking the word and sleeping being lost.
                const auto state = std::atomic_ref(*reinterpret_cast<uint32_t*>(state_va));
                changed.wait(lock, scope->CancellationToken(),
                             [&] { return state.load(std::memory_order_acquire) != 2; });
            } else if (op == GuestWake) {
                ++self.wakes;
                changed.notify_one();
            } else {
                return MakeError(ErrorCategory::InvalidArgument, "CompiledGuest.Wait",
                                 "invalid operation");
            }
        }
        frame.registers.Set(Gpr::Rax, 0);
        return Ok();
    }
};

struct Harness {
    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> cpu;
    uint64_t base{}, code{}, data{}, veneer{};
    std::shared_ptr<SlowPath> slow;
    std::mutex handles_mutex;
    ThreadHandle handles[2]{};
    Harness() {
        AddressSpaceConfig cfg{};
        cfg.reservation_size = 1ull << 28;
        cfg.max_address = QueryBackendCapabilities().max_guest_address;
        space = Must(GuestAddressSpace::Create(cfg));
        base = space->ReservationBase().value;
        code = base + 0x10000;
        data = base + 0x20000;
        for (auto va : {code, data, base + 0x30000, base + 0x40000})
            Must(space->Map({GuestAddress{va}, 0x4000},
                            GuestPermission::Read | GuestPermission::Write));
        if (sizeof(CompiledGuest::Image) > 0x4000)
            throw std::runtime_error("compiled image too large");
        Must(space->Write(GuestAddress{code}, std::as_bytes(std::span{CompiledGuest::Image})));
        Must(space->Protect({GuestAddress{code}, 0x4000},
                            GuestPermission::Read | GuestPermission::Execute));
        cpu = Must(CreateContext({}, *space));
        slow = std::make_shared<SlowPath>(*space, data);
        auto* registry = static_cast<HleCallRegistry*>(Fex::FexHleRegistryPointer(*cpu));
        if (!registry)
            throw std::runtime_error("FEX HLE registry unavailable");
        auto op = Must(registry->Adopt(slow, "CompiledGuest.WaitWake"));
        auto allocator =
            Must(HleVeneerAllocator::Create(*space, {GuestAddress{base + 0x50000}, 0x4000}));
        veneer = Must(allocator.Allocate(op)).value;
        Must(allocator.Seal());
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
    void Request(unsigned slot, uint64_t mode, uint64_t count) {
        GuestRequest request{mode,
                             count,
                             reinterpret_cast<GuestMutexProbe*>(data),
                             reinterpret_cast<GuestSlowCall>(veneer),
                             0,
                             0};
        Put(data + 0x100 + slot * 0x100, request);
    }
    GuestCallResult Run(unsigned slot, std::barrier<>* ready = nullptr) {
        ThreadInit init{};
        init.entry_rip = GuestCodeAddress{code + CompiledGuest::EntryOffset};
        init.initial_rsp = GuestAddress{base + 0x30000 + slot * 0x10000 + 0x3ff0};
        init.guest_tid = 100 + slot;
        auto thread = Must(cpu->CreateThread(init));
        {
            std::lock_guard lock(handles_mutex);
            handles[slot] = thread;
        }
        if (ready)
            ready->arrive_and_wait();
        GuestCallArgs args{};
        args.count = 1;
        args.values[0] = data + 0x100 + slot * 0x100;
        auto result = cpu->InvokeGuest(thread, init.entry_rip, args, {});
        {
            std::lock_guard lock(handles_mutex);
            handles[slot] = {};
        }
        Must(cpu->DestroyThread(thread));
        return Must(std::move(result));
    }
    void Cancel(unsigned slot) {
        std::lock_guard lock(handles_mutex);
        if (handles[slot].IsValid())
            Must(cpu->RequestInterrupt(handles[slot], InterruptReason::Cancel));
    }
    GuestCallResult Join(std::future<GuestCallResult>& future) {
        if (future.wait_for(20s) != std::future_status::ready) {
            Check("bounded completion", false);
            Cancel(0);
            Cancel(1);
            if (future.wait_for(3s) != std::future_status::ready)
                std::_Exit(3);
        }
        return future.get();
    }
};
} // namespace

int main() {
    try {
        Harness h;
        std::printf("compiled x86-64 image=%zu entry=%llu rebased_va=%llx\n",
                    sizeof(CompiledGuest::Image), (unsigned long long)CompiledGuest::EntryOffset,
                    (unsigned long long)h.code);
        h.Request(0, 0, 8);
        auto math = h.Run(0);
        auto result = h.Read<GuestRequest>(h.data + 0x100);
        Check("C++ entry -> C function with 8 SysV arguments",
              math.reason == StopReason::Returned && math.return_value == 204);
        Check("C++ writes through guest pointer and C SSE2 double result",
              result.output == 204 && result.fp_output == 6.25);
        h.Request(0, 0, 9);
        Check("compiled function reused with new input", h.Run(0).return_value == 212);

        // Warm both paths; benchmark includes InvokeGuest but no translation of
        // new instructions. This measures crossing overhead, not game FPS.
        h.Request(0, 2, 100);
        (void)h.Run(0);
        for (unsigned mode : {1u, 2u}) {
            h.Put(h.data, GuestMutexProbe{});
            h.Request(0, mode, 10000);
            const auto before = h.slow->calls.load();
            const auto begin = std::chrono::steady_clock::now();
            const auto run = h.Run(0);
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - begin)
                                .count();
            const auto crossings = h.slow->calls.load() - before;
            Check(mode == 1 ? "guest atomic lock + RAII unlock"
                            : "same work with two HLE calls per iteration",
                  run.reason == StopReason::Returned && run.return_value == 10000 &&
                      h.Read<GuestMutexProbe>(h.data).value == 10000);
            Check(mode == 1 ? "uncontended mutex makes zero HLE calls"
                            : "crossing control makes exactly 20000 HLE calls",
                  crossings == (mode == 1 ? 0u : 20000u));
            std::printf("BENCH mode=%u iterations=10000 elapsed_ns=%lld hle_calls=%llu\n", mode,
                        (long long)ns, (unsigned long long)crossings);
        }

        h.Put(h.data, GuestMutexProbe{});
        for (unsigned i = 0; i < 2; ++i)
            h.Request(i, 1, 50000);
        std::barrier ready(2);
        auto a = std::async(std::launch::async, [&] { return h.Run(0, &ready); });
        auto b = std::async(std::launch::async, [&] { return h.Run(1, &ready); });
        const auto ar = h.Join(a), br = h.Join(b);
        const auto shared = h.Read<GuestMutexProbe>(h.data);
        Check("two real FEX owners preserve 100000 protected increments",
              ar.reason == StopReason::Returned && br.reason == StopReason::Returned &&
                  shared.value == 100000 && shared.state == 0);
        Check("contended path reached host wait/wake", h.slow->waits > 0 && h.slow->wakes > 0);

        // Expected-value wait must not park if the guest has already released.
        HleCallFrame frame{};
        frame.space = h.space.get();
        frame.registers.Set(Gpr::Rdi, h.data + 4);
        frame.registers.Set(Gpr::Rsi, GuestWait);
        Check("slow path rejects a foreign word", !h.slow->Invoke(frame));

        h.Put(h.data, GuestMutexProbe{1, 0, 0});
        h.Request(0, 1, 1);
        const auto entered = h.slow->entered.load();
        auto waiting = std::async(std::launch::async, [&] { return h.Run(0); });
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (h.slow->entered == entered && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        Check("compiled mutex actually entered cancellable WaitingHle", h.slow->entered > entered);
        h.Cancel(0);
        Check("Stop cancels compiled guest's host slow wait",
              h.Join(waiting).reason == StopReason::Cancelled);
        h.Put(h.data, GuestMutexProbe{});
        h.Request(0, 1, 100);
        Check("fresh owner can execute compiled payload after cancellation",
              h.Run(0).reason == StopReason::Returned &&
                  h.Read<GuestMutexProbe>(h.data).value == 100);
    } catch (const std::exception& e) {
        Check(e.what(), false);
    }
    std::printf("COMPILED_GUEST checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
