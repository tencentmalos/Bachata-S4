// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/guest_cpu/fex/fex_context.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/TypeDefines.h>

#include "Common/HostFeatures.h"

namespace Core::GuestCpu::Fex {
namespace {

// One live context per process (API contract §2). This is a live-instance rule,
// not a once-ever latch: destroying the context must allow a rebuild, which
// acceptance L01 exercises 100 times.
std::atomic<bool> g_context_active{false};

// --- register mapping --------------------------------------------------------
// The public Gpr enum uses the x86-64 encoding order, which is also FEX's gregs
// index order. Assert it rather than trusting the coincidence: a divergence
// would silently swap registers.
static_assert(static_cast<int>(Gpr::Rax) == FEXCore::X86State::REG_RAX);
static_assert(static_cast<int>(Gpr::Rcx) == FEXCore::X86State::REG_RCX);
static_assert(static_cast<int>(Gpr::Rsp) == FEXCore::X86State::REG_RSP);
static_assert(static_cast<int>(Gpr::R15) == FEXCore::X86State::REG_R15);
static_assert(kGprCount == FEXCore::Core::CPUState::NUM_GPRS);

Error BackendError(ErrorCategory category, std::string_view operation, std::string detail,
                   int errno_value = 0) {
    Error error = MakeError(category, operation, std::move(detail));
    if (errno_value != 0) {
        error.system_error = errno_value;
    }
    return error;
}

// Monotonic context identity. Tickets carry it so one issued by a destroyed context cannot be
// mistaken for a valid request against its replacement: thread ids and epochs both restart, this
// does not.
std::uint64_t NextContextId() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

// --- FEXCore embedder obligations -------------------------------------------
// InitCore dereferences the signal delegator to install the dispatcher config,
// so a context without one crashes there. FEXCore::SignalDelegator is concrete
// and only stores what the dispatcher hands it; shadPS4 installs its own signal
// handling, so this supplies the delegator object without taking over delivery.
class FexSignalDelegator final : public FEXCore::SignalDelegator {
public:
    explicit FexSignalDelegator(std::uintptr_t callback_return)
        : callback_return_(callback_return) {}

    uintptr_t GetThunkCallbackRET() const override {
        return callback_return_;
    }

private:
    std::uintptr_t callback_return_{};
};

// --- asynchronous interrupt delivery -----------------------------------------------------------
//
// See docs/fex-async-stop-source-proof.md for the traced FEX path this implements. In short: the
// handler must not longjmp out of the JIT. It rewrites the interrupted PC to a FEX stub that
// spills the static register allocation and calls SleepThread, where the actual wait happens in
// ordinary thread context.

// Real-time signal used to kick an owner out of the JIT.
//
// Linux FEX hardcodes 63. That is not portable here: on bionic SIGRTMIN and SIGRTMAX are function
// calls (__libc_current_sigrtmin/max), because the runtime reserves the lowest real-time signals
// for itself, and the usable range is decided at runtime rather than by the headers. Taking the
// top of the range keeps distance from both bionic's reserved base and the low signals ART uses.
//
// Resolved once. Every call site is ordinary thread context -- registration, restore and tgkill --
// so calling into libc here is fine; the signal handler must never do it.
int InterruptSignal() {
    static const int cached = SIGRTMAX - 1;
    return cached;
}

// Per-thread pointer to the state the handler is allowed to touch.
//
// Thread-local rather than a lookup: the handler cannot take the context lock, and walking a hash
// map another thread might be rehashing is exactly the kind of undefined behaviour that is
// impossible to debug from a signal context.
struct ThreadInterruptBinding;
thread_local ThreadInterruptBinding* t_binding = nullptr;

// The previous disposition, so an unrelated signal is forwarded rather than swallowed. ART installs
// its own handlers; discarding them would break the runtime hosting us.
struct sigaction g_previous_interrupt_action{};
std::atomic<bool> g_interrupt_installed{false};
std::mutex g_interrupt_install_lock;

// What one owner thread's handler needs, reachable without a lock or an allocation.
struct ThreadInterruptBinding final {
    // The control block. shared_ptr is copied here at Run entry so the handler's view stays valid
    // even if the thread map is mutated meanwhile; the handler only reads the raw pointer.
    void* state{};
    // FEXCore context, for IsAddressInCodeBuffer.
    FEXCore::Context::Context* fex{};
    FEXCore::Core::InternalThreadState* native{};
    // Spill entry points, copied out of the delegator config so the handler does no indirection
    // through objects that could be mid-destruction.
    std::uint64_t pause_spill_sra{};
    std::uint64_t pause_no_spill{};
    // The frame CPUState pointer the handler must install in the state register.
    void* frame{};
};

// Set the interrupted context's PC and the state register.
//
// Implemented here rather than reusing FEX's ArchHelpers because those live under
// Source/Tools/LinuxEmulation, which is the Linux frontend an embedder replaces.
#if defined(__aarch64__)
void SetContextPc(void* ucontext, std::uint64_t pc) {
    auto* uc = static_cast<ucontext_t*>(ucontext);
    uc->uc_mcontext.pc = pc;
}
std::uint64_t GetContextPc(void* ucontext) {
    return static_cast<ucontext_t*>(ucontext)->uc_mcontext.pc;
}
void SetContextState(void* ucontext, std::uint64_t value) {
    // x28 is the STATE register in FEX's ARM64 JIT ABI for the audited configuration. This is a
    // property of the pinned FEX layout, not a universal ARM64 convention; it is asserted against
    // the running configuration at registration time rather than assumed here.
    static_cast<ucontext_t*>(ucontext)->uc_mcontext.regs[28] = value;
}
#else
void SetContextPc(void*, std::uint64_t) {}
std::uint64_t GetContextPc(void*) {
    return 0;
}
void SetContextState(void*, std::uint64_t) {}
#endif

void ForwardToPrevious(int signal, siginfo_t* info, void* ucontext) {
    const struct sigaction& previous = g_previous_interrupt_action;
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
        previous.sa_sigaction(signal, info, ucontext);
        return;
    }
    if (previous.sa_handler == SIG_IGN) {
        return;
    }
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != nullptr) {
        previous.sa_handler(signal);
        return;
    }
    // Default disposition for a real-time signal is termination. Restore and re-raise so the
    // process dies the way it would have without us, rather than looping in our handler.
    ::signal(signal, SIG_DFL);
    ::raise(signal);
}

// Declared here, defined after InterruptState is complete.
bool ClaimInterrupt(void* state);

// The kick handler.
//
// Async-signal-safe by construction: atomic loads and stores, two ucontext writes, and nothing
// else. No allocation, no mutex, no logging, no Foundation. The wait it sets up happens later in
// SleepThread, which runs in ordinary thread context.
void InterruptSignalHandler(int signal, siginfo_t* info, void* ucontext) {
    ThreadInterruptBinding* binding = t_binding;
    if (binding == nullptr || binding->state == nullptr) {
        // Not one of our owner threads -- an app or ART thread that happens to share the signal
        // number. Hand it back rather than swallowing it.
        ForwardToPrevious(signal, info, ucontext);
        return;
    }
    if (!ClaimInterrupt(binding->state)) {
        // Ours, but nothing is pending: a stale or duplicate delivery. Consume it silently;
        // forwarding would hand ART a signal it did not send.
        return;
    }

    // Choose the spill entry by where the thread actually is. Getting this wrong is not a
    // performance issue: the no-spill entry assumes the static register allocation is already in
    // memory, so taking it from inside the JIT leaves guest registers in host registers and every
    // subsequent read of CPUState is stale.
    const std::uint64_t pc = GetContextPc(ucontext);
    const bool in_code_buffer =
        binding->fex != nullptr && binding->native != nullptr &&
        binding->fex->IsAddressInCodeBuffer(binding->native, static_cast<uintptr_t>(pc));

    const std::uint64_t target = in_code_buffer ? binding->pause_spill_sra : binding->pause_no_spill;
    if (target == 0) {
        // No usable entry point; leaving the PC alone is the only safe action.
        return;
    }
    SetContextState(ucontext, reinterpret_cast<std::uint64_t>(binding->frame));
    SetContextPc(ucontext, target);
}

// Asynchronous control state for one guest thread.
//
// Reachable from three places with different rules, which is why the fields are atomics rather
// than plain members under the context lock:
//   * a controller thread issuing RequestInterrupt / WaitStopped,
//   * the target's own signal handler, which may take no lock and must allocate nothing,
//   * the target inside SleepThread, which is ordinary thread context and may block.
//
// Kept in a stable heap allocation, separately from ThreadEntry: the signal handler resolves it
// through a thread-local pointer and must not chase a map that another thread could rehash.
struct InterruptState final {
    // Set by RequestInterrupt, read by the signal handler. Bit set of BitOf(InterruptReason).
    std::atomic<std::uint32_t> pending{0};
    // Highest request epoch issued for this thread.
    std::atomic<std::uint64_t> request_epoch{0};
    // Epoch the owner has acknowledged by actually stopping.
    std::atomic<std::uint64_t> acked_epoch{0};
    // Set while the owner is parked inside SleepThread.
    std::atomic<bool> parked{false};
    // Cleared by Resume to let the parked owner continue.
    std::atomic<bool> resume_requested{false};
    // The reason the owner actually stopped for, decided at park time.
    std::atomic<std::uint32_t> stop_reason{0};
    // Native thread id, for tgkill. Written by the owner as it starts running.
    std::atomic<std::uint64_t> native_tid{0};
    // True while the owner is inside ExecuteThread. A request that arrives outside that window
    // needs no signal: the owner will observe it before entering.
    std::atomic<bool> in_jit{false};

    // Park/unpark handshake. The mutex is only ever taken in ordinary thread context -- the
    // signal handler must not touch it.
    std::mutex park_lock;
    std::condition_variable park_changed;
};

// True when there is a request this delivery should act on.
//
// Called from the signal handler, so this is a plain atomic read. It deliberately does not clear
// `pending`: the reason is needed later, in SleepThread, to decide what kind of stop this is and
// whether a Cancel outranks a Pause.
bool ClaimInterrupt(void* state) {
    auto* interrupt = static_cast<InterruptState*>(state);
    return interrupt != nullptr && interrupt->pending.load(std::memory_order_acquire) != 0;
}

// Install the kick handler once per process.
//
// Chains rather than replaces: ART has its own handlers, and an unrelated delivery on this signal
// must reach whatever was there before.
Status InstallInterruptHandler() {
    std::lock_guard guard{g_interrupt_install_lock};
    if (g_interrupt_installed.load(std::memory_order_acquire)) {
        return Ok();
    }
    const int signal_number = InterruptSignal();
    // The usable real-time range is a runtime property on bionic, so check the resolved number
    // against it rather than trusting a header constant.
    if (signal_number < SIGRTMIN || signal_number > SIGRTMAX) {
        return MakeError(ErrorCategory::Unsupported, "InstallInterruptHandler",
                         "the chosen interrupt signal is outside this platform's real-time range");
    }

    struct sigaction action {};
    action.sa_sigaction = InterruptSignalHandler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    ::sigemptyset(&action.sa_mask);

    if (::sigaction(signal_number, &action, &g_previous_interrupt_action) != 0) {
        const int saved = errno;
        auto error = MakeError(ErrorCategory::BackendFailure, "InstallInterruptHandler",
                               "sigaction failed for the interrupt signal");
        error.system_error = saved;
        return error;
    }
    g_interrupt_installed.store(true, std::memory_order_release);
    return Ok();
}

void RestoreInterruptHandler() {
    std::lock_guard guard{g_interrupt_install_lock};
    if (!g_interrupt_installed.load(std::memory_order_acquire)) {
        return;
    }
    // Put back exactly what was there, so a second context -- or ART after we unload -- sees the
    // disposition it installed.
    ::sigaction(InterruptSignal(), &g_previous_interrupt_action, nullptr);
    g_interrupt_installed.store(false, std::memory_order_release);
}

// LookupCache's constructor calls SyscallHandler::MarkOvercommitRange, so
// CreateThread crashes without a handler. Three methods are pure virtual and
// must be supplied even though guest syscalls are not part of V0.
class FexSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
    void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
        // Reaching here means guest code executed a syscall instruction, which
        // V0 has no HLE path for. Record it on the frame's thread so Run can
        // report a defined stop instead of the guest silently continuing with a
        // garbage return value.
        if (Frame != nullptr) {
            unexpected_syscall.store(true, std::memory_order_release);
        }
    }

    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
        FEXCore::Core::InternalThreadState* Thread, uint64_t Address) override {
        std::lock_guard guard{lock};
        for (const auto& range : executable_ranges) {
            if (Address >= range.base && Address < range.base + range.size) {
                return {.Base = range.base, .Size = range.size, .Writable = range.writable};
            }
        }
        // Not a known executable range. Report an empty one rather than
        // claiming the address is valid code.
        return {.Base = Address, .Size = 0, .Writable = false};
    }

    std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(
        FEXCore::Core::InternalThreadState* Thread, uint64_t GuestAddr) override {
        // No file-backed guest mappings in V0; the disk code cache stays off.
        return std::nullopt;
    }

    void PreCompile() override {
        // CompileBlock always calls this, so it is a reliable signal that the dispatcher actually
        // reached translation rather than exiting first.
        compile_count.fetch_add(1, std::memory_order_relaxed);
    }

    // Where an interrupted owner actually waits.
    //
    // FEXCore's dispatcher calls this after the pause stub has spilled the static register
    // allocation, so by the time we are here the thread's CPUState is complete and a snapshot taken
    // from it is authoritative. Returning from this function makes the stub execute its hlt, which
    // faults back in to restore and resume -- see docs/fex-async-stop-source-proof.md.
    //
    // The default implementation in FEXCore is an empty body, so before this override a pause
    // signal would spill and immediately resume: the thread would never actually stop.
    void SleepThread(FEXCore::Context::Context* CTX,
                     FEXCore::Core::CpuStateFrame* Frame) override {
        InterruptState* interrupt = t_binding != nullptr
                                        ? static_cast<InterruptState*>(t_binding->state)
                                        : nullptr;
        if (interrupt == nullptr) {
            return;
        }

        // Publish the acknowledgement only now. This is the distinction the spec draws between
        // "the request was received" and "the thread is stopped": the controller may read state
        // only after this point.
        const std::uint64_t epoch = interrupt->request_epoch.load(std::memory_order_acquire);
        const std::uint32_t reasons = interrupt->pending.load(std::memory_order_acquire);
        interrupt->stop_reason.store(reasons, std::memory_order_release);

        {
            std::unique_lock guard{interrupt->park_lock};
            interrupt->parked.store(true, std::memory_order_release);
            interrupt->acked_epoch.store(epoch, std::memory_order_release);
            interrupt->park_changed.notify_all();

            // A Cancel or Shutdown does not park: the owner has to unwind, not wait to be resumed.
            const std::uint32_t stop_now =
                (1u << static_cast<std::uint32_t>(InterruptReason::Cancel)) |
                (1u << static_cast<std::uint32_t>(InterruptReason::Shutdown));
            if ((reasons & stop_now) == 0) {
                interrupt->park_changed.wait(guard, [&] {
                    return interrupt->resume_requested.load(std::memory_order_acquire);
                });
            }
            interrupt->parked.store(false, std::memory_order_release);
            interrupt->resume_requested.store(false, std::memory_order_release);
        }

        // Clear only the reasons that were satisfied by this stop. A request that arrived while we
        // were parked keeps its bit and will be serviced on the next kick, rather than being lost
        // because a Resume happened to run in between.
        interrupt->pending.fetch_and(~reasons, std::memory_order_acq_rel);
        interrupt->park_changed.notify_all();
    }

    // FEXCore calls this once per guest page it has compiled code from, which is the only
    // outside-visible confirmation of *what* was translated.
    void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start,
                                  uint64_t Length) override {
        if (::getenv("GUEST_CPU_DEBUG") != nullptr) {
            std::fprintf(stderr, "[guest_cpu] compiled code covering 0x%llx +0x%llx\n",
                         static_cast<unsigned long long>(Start),
                         static_cast<unsigned long long>(Length));
        }
    }

    [[nodiscard]] std::uint64_t CompileCount() const {
        return compile_count.load(std::memory_order_relaxed);
    }

    void RegisterExecutableRange(std::uint64_t base, std::uint64_t size, bool writable) {
        std::lock_guard guard{lock};
        executable_ranges.push_back({base, size, writable});
    }

    [[nodiscard]] bool TakeUnexpectedSyscall() {
        return unexpected_syscall.exchange(false, std::memory_order_acq_rel);
    }

private:
    struct Range final {
        std::uint64_t base{};
        std::uint64_t size{};
        bool writable{};
    };

    mutable std::mutex lock;
    std::vector<Range> executable_ranges;
    std::atomic<bool> unexpected_syscall{false};
    std::atomic<std::uint64_t> compile_count{0};
};

// Upper bound this backend places on guest addresses.
//
// This is a V0 policy, NOT a demonstrated FEXCore capability limit. An earlier revision claimed
// FEXCore could not address above 1<<36 because LookupCache masks the guest RIP when computing its
// page index. That reasoning does not hold: after indexing, both LookupCache (`LookupCache.h:207`)
// and the dispatcher (`Dispatcher.cpp:211-218`) compare the *full* address and fall through to L3
// or recompile on a mismatch, so an index collision costs a lookup miss rather than executing the
// wrong block. The 2026-09-08 review, finding R8, is correct on this point.
//
// The bound is kept because it makes guest placement deterministic while execution is still being
// brought up, and because Config.VirtualMemSize is 1<<36 so staying inside it avoids exercising the
// aliasing path at the same time as everything else. Removing it needs a same-fixture low-VA vs
// high-VA comparison, not just deleting the constant.
constexpr std::uint64_t kGuestAddressPolicyLimit = std::uint64_t{1} << 36;

// --- return gate -------------------------------------------------------------
// A host page holding a single x86 HLT, mapped executable and registered as a
// guest executable range. Guest code returns to this address; with
// EnableExitOnHLT the HLT makes ExecuteThread return instead of trapping.
//
// This is what makes StopReason::Returned distinguishable from a real guest HLT
// (acceptance D05): only this exact address counts as a normal return, and a
// HLT anywhere else is a fault.
class ReturnGate final {
public:
    ~ReturnGate() {
        if (page != nullptr && page != MAP_FAILED) {
            ::munmap(page, size);
        }
    }

    [[nodiscard]] bool Create(std::string& error_detail, int& error_no) {
        const long host_page = ::sysconf(_SC_PAGESIZE);
        size = host_page > 0 ? static_cast<std::size_t>(host_page) : 4096;

        // The gate is guest-executable code, so it is subject to the same addressing limit as any
        // other guest mapping: above kGuestAddressPolicyLimit the block lookup would alias it.
        //
        // Hinted near the top of the addressable range, because the guest reservation is placed in
        // the lower half and a hint that lands inside it would be rejected and fall back to a high
        // address. A hint is advisory either way, so the result is verified below.
        void* hint = reinterpret_cast<void*>(kGuestAddressPolicyLimit - (std::uint64_t{1} << 30));
        page = ::mmap(hint, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            error_no = errno;
            error_detail = "failed to map the return gate page";
            page = nullptr;
            return false;
        }
        if (reinterpret_cast<std::uint64_t>(page) + size > kGuestAddressPolicyLimit) {
            ::munmap(page, size);
            page = nullptr;
            error_detail = "the kernel placed the return gate above the addressable guest range";
            return false;
        }

        // 0xF4 == HLT.
        *static_cast<std::uint8_t*>(page) = 0xF4;

        // W^X: publish read+execute only after the byte is written. Writing
        // through an RWX mapping would also work on Linux but is refused under
        // stricter policies and is not needed here.
        if (::mprotect(page, size, PROT_READ | PROT_EXEC) != 0) {
            error_no = errno;
            error_detail = "failed to make the return gate executable";
            return false;
        }
        return true;
    }

    [[nodiscard]] std::uint64_t Address() const noexcept {
        return reinterpret_cast<std::uint64_t>(page);
    }
    [[nodiscard]] std::size_t Size() const noexcept {
        return size;
    }

private:
    void* page{};
    std::size_t size{};
};

// --- call-return stack -------------------------------------------------------
// FEXCore reads InternalThreadState::CallRetStackBase and CPUState::callret_sp but never allocates
// the memory behind them: that is the embedder's job, like the signal delegator and syscall
// handler. Leaving it null makes the JIT fault on its first call/ret, which surfaces as an
// immediate exit rather than as an obvious null dereference.
//
// Reserved PROT_NONE with a guard page on each side, then only the middle made writable, so an
// overrun or underrun of the call-return stack faults instead of corrupting a neighbour.
class CallRetStack final {
public:
    ~CallRetStack() {
        if (reservation != nullptr && reservation != MAP_FAILED) {
            ::munmap(reservation, TotalSize());
        }
    }

    [[nodiscard]] bool Create(std::string& error_detail, int& error_no) {
        const long host_page = ::sysconf(_SC_PAGESIZE);
        guard_size = host_page > 0 ? static_cast<std::size_t>(host_page) : 4096;

        reservation = ::mmap(nullptr, TotalSize(), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reservation == MAP_FAILED) {
            error_no = errno;
            error_detail = "failed to reserve the call-return stack";
            reservation = nullptr;
            return false;
        }
        if (::mprotect(Base(), FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE,
                       PROT_READ | PROT_WRITE) != 0) {
            error_no = errno;
            error_detail = "failed to make the call-return stack writable";
            return false;
        }
        return true;
    }

    void InstallInto(FEXCore::Core::InternalThreadState* thread) const {
        thread->CallRetStackBase = Base();
        // Start a quarter in, matching the reference bring-up: the stack grows in both directions
        // depending on call depth, so starting at either end would waste half of it.
        thread->CurrentFrame->State.callret_sp =
            reinterpret_cast<std::uint64_t>(Base()) +
            FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
    }

private:
    [[nodiscard]] std::size_t TotalSize() const {
        return FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * guard_size;
    }
    [[nodiscard]] void* Base() const {
        return static_cast<std::uint8_t*>(reservation) + guard_size;
    }

    void* reservation{};
    std::size_t guard_size{};
};

// --- context -----------------------------------------------------------------
class FexCpuContext final : public CpuContext, public CodeInvalidationSink {
public:
    explicit FexCpuContext(const CpuConfig& config, GuestAddressSpace& space)
        : config_(config), space_(space) {}

    ~FexCpuContext() override {
        // Unregister before anything else: once the backend is going away, the address space must
        // stop routing publications to it rather than calling into a destroyed object.
        space_.ClearCodeInvalidationSink(this);
        // Destroy threads before the context: FEXCore requires it, and a live
        // thread holding a code buffer would otherwise outlive its owner.
        {
            std::lock_guard guard{lock_};
            for (auto& [id, entry] : threads_) {
                if (entry.native != nullptr && context_ != nullptr) {
                    context_->DestroyThread(entry.native);
                    entry.native = nullptr;
                }
            }
            threads_.clear();
        }
        context_.reset();
        RestoreInterruptHandler();
        g_context_active.store(false, std::memory_order_release);
    }

    [[nodiscard]] Result<void> Initialize() {
        std::string detail;
        int error_no = 0;
        if (!return_gate_.Create(detail, error_no)) {
            return BackendError(ErrorCategory::OutOfMemory, "CreateContext", detail, error_no);
        }

        // FEXCore has no sysconf call in its link scope, so the host page size
        // must be injected or it silently assumes 4096.
        const long host_page = ::sysconf(_SC_PAGESIZE);
        if (host_page <= 0) {
            return BackendError(ErrorCategory::BackendFailure, "CreateContext",
                                "sysconf(_SC_PAGESIZE) did not report a usable page size");
        }
        host_page_size_ = static_cast<std::uint64_t>(host_page);
        if (!FEXCore::Utils::SetHostPageSize(host_page_size_)) {
            return BackendError(ErrorCategory::Unsupported, "CreateContext",
                                "this FEXCore build does not support the host page size");
        }

        FEXCore::Config::Initialize();
        // Order matters: ReloadMetaLayer rebuilds the meta layer from the registered config layers
        // and drops anything Set beforehand. Setting after it is what makes the value stick.
        //
        // Getting this backwards left Is64BitMode unset, which ContextImpl reads as 32-bit: it then
        // clamps VirtualMemSize to 1<<32 and the decoder builds 32-bit blocks from 64-bit guest
        // bytes. That was the cause of guest fixtures having no architectural effect.
        FEXCore::Config::ReloadMetaLayer();
        FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
        config_initialized_ = true;

        {
            // Read it back rather than assuming the write landed; a silently 32-bit context
            // mistranslates every guest instruction while still appearing to run.
            auto mode = FEXCore::Config::Get(FEXCore::Config::CONFIG_IS64BIT_MODE);
            if (!mode || **mode != "1") {
                return BackendError(ErrorCategory::BackendFailure, "CreateContext",
                                    "FEXCore did not accept 64-bit guest mode");
            }
        }

        // Optional JIT disassembly. Must be set here, before the Context is constructed, because
        // FEX_CONFIG_OPT caches the value at construction; setting the FEX_DISASSEMBLE environment
        // variable has no effect on an embedder that does not load FEX's environment config layer.
        // Requires a FEXCore built with -DENABLE_VIXL_DISASSEMBLER=ON.
        if (const char* disasm = ::getenv("GUEST_CPU_DISASSEMBLE"); disasm != nullptr) {
            FEXCore::Config::Set(FEXCore::Config::CONFIG_DISASSEMBLE, disasm);
        }

        // Surface FEXCore's own diagnostics. Without a handler these are dropped, and a JIT-side
        // refusal looks identical to a guest that simply did nothing.
        if (::getenv("GUEST_CPU_DEBUG") != nullptr) {
            LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char* message) {
                std::fprintf(stderr, "[fex %s] %s\n", LogMan::DebugLevelStr(level), message);
            });
            LogMan::Throw::InstallHandler([](const char* message) {
                std::fprintf(stderr, "[fex assert] %s\n", message);
            });
        }

        FEXCore::Allocator::SetupHooks(host_page_size_);
        allocator_hooked_ = true;

        // Reads the real ID registers rather than assuming a feature set.
        host_features_ = FEX::FetchHostFeatures();
        context_ = FEXCore::Context::Context::CreateNewContext(host_features_);
        if (!context_) {
            return BackendError(ErrorCategory::BackendFailure, "CreateContext",
                                "FEXCore refused to create a context");
        }

        signal_delegator_ = std::make_unique<FexSignalDelegator>(return_gate_.Address());
        syscall_handler_ = std::make_unique<FexSyscallHandler>();
        context_->SetSignalDelegator(signal_delegator_.get());
        context_->SetSyscallHandler(syscall_handler_.get());

        // Makes the return gate's HLT exit ExecuteThread rather than trap.
        // Makes the return gate's HLT exit ExecuteThread rather than trap.
        //
        // GUEST_CPU_NO_EXIT_ON_HLT disables it for diagnosis: without it the gate's HLT takes the
        // SIGILL path, which deliberately faults, so a crash there is positive evidence that guest
        // execution actually reached the gate.
        if (::getenv("GUEST_CPU_NO_EXIT_ON_HLT") == nullptr) {
            context_->EnableExitOnHLT();
        }

        if (!context_->InitCore()) {
            return BackendError(ErrorCategory::BackendFailure, "CreateContext",
                                "FEXCore InitCore failed");
        }

        syscall_handler_->RegisterExecutableRange(return_gate_.Address(), return_gate_.Size(),
                                                  false);

        // The async kick handler. Installed after InitCore so the dispatcher config -- and with it
        // the spill entry points the handler needs -- already exists.
        if (auto installed = InstallInterruptHandler(); !installed) {
            return installed.GetError();
        }
        if (signal_delegator_->GetConfig().ThreadPauseHandlerAddressSpillSRA == 0) {
            // Without this the handler has nowhere safe to redirect an in-JIT thread, and a stop
            // request would either do nothing or corrupt register state. Fail loudly at init
            // instead of at the first interrupt.
            return BackendError(ErrorCategory::BackendFailure, "CreateContext",
                                "FEXCore did not publish a pause spill entry point");
        }

        // Route the address space's publication transactions here. Done last, so a context that
        // failed to initialise is never registered as the thing that owns translated code.
        if (auto registered = space_.SetCodeInvalidationSink(this); !registered) {
            return registered;
        }
        return Result<void>{};
    }

    [[nodiscard]] BackendCapabilities Capabilities() const override {
        auto caps = QueryFexCapabilities();
        caps.host_page_size = host_page_size_;
        caps.memory_mode = config_.memory_mode;
        caps.smc_mode = config_.smc_mode;
        caps.return_gate_address = return_gate_.Address();
        return caps;
    }

    [[nodiscard]] Result<ThreadHandle> CreateThread(const ThreadInit& init) override {
        // Refuse while a publication transaction is active or code is poisoned. Creating a thread
        // is not executing yet, but it allocates backend state against a code image that is
        // mid-change, and admitting it here would let a Run follow immediately. Taking the lease
        // and dropping it at the end of this function is the admission check.
        auto admission = space_.AcquireExecutionLease();
        if (!admission) {
            return admission.GetError();
        }

        // The entry and stack must already be mapped in this context's address
        // space. Checking here turns a guest crash into a caller-side error.
        auto entry_mapping = space_.Query(GuestAddress{init.entry_rip.value});
        if (!entry_mapping) {
            return BackendError(ErrorCategory::InvalidArgument, "CreateThread",
                                "entry_rip is not mapped in this address space");
        }
        if (!HasPermission(entry_mapping.Value().permission, GuestPermission::Execute)) {
            return BackendError(ErrorCategory::PermissionDenied, "CreateThread",
                                "entry_rip is mapped without execute permission");
        }
        auto stack_mapping = space_.Query(init.initial_rsp);
        if (!stack_mapping) {
            return BackendError(ErrorCategory::InvalidArgument, "CreateThread",
                                "initial_rsp is not mapped in this address space");
        }

        FEXCore::Core::CPUState state{};
        state.rip = init.entry_rip.value;
        state.gregs[FEXCore::X86State::REG_RSP] = init.initial_rsp.value;
        ApplyPatchToState(init.initial_state, state);

        std::lock_guard guard{lock_};

        // Allocate the GDT before the thread so the pointer stored in CPUState
        // stays valid for the thread's whole life.
        auto gdt = std::make_unique<std::array<FEXCore::Core::CPUState::gdt_segment, 32>>();
        InitializeSegments(state, *gdt);

        auto* native = context_->CreateThread(&state);
        if (native == nullptr) {
            return BackendError(ErrorCategory::OutOfMemory, "CreateThread",
                                "FEXCore could not create a thread state");
        }

        // CreateThread copies the CPUState by value, so the live thread's copy
        // needs the segment pointers pointed at the same GDT again -- the copy
        // holds the address, but re-running this keeps the two in step if the
        // backend ever starts adjusting descriptors after creation.
        InitializeSegments(native->CurrentFrame->State, *gdt);

        // Must happen before the first Run: the JIT dereferences callret_sp on
        // its first call or ret.
        auto callret = std::make_unique<CallRetStack>();
        std::string detail;
        int error_no = 0;
        if (!callret->Create(detail, error_no)) {
            context_->DestroyThread(native);
            return BackendError(ErrorCategory::OutOfMemory, "CreateThread", std::move(detail),
                                error_no);
        }
        callret->InstallInto(native);

        if (init.initial_state.fields != RegisterValidity::None) {
            ApplyXmmPatch(init.initial_state, native);
        }

        const std::uint64_t id = next_thread_id_++;
        ThreadEntry entry{};
        entry.native = native;
        entry.generation = ++generation_counter_;
        entry.owner = std::this_thread::get_id();
        entry.guest_tid = init.guest_tid;
        entry.entry_rip = init.entry_rip.value;
        entry.gdt = std::move(gdt);
        entry.callret = std::move(callret);

        // Register the entry's mapping so the JIT can look it up as executable
        // code rather than refusing to compile from it.
        syscall_handler_->RegisterExecutableRange(entry_mapping.Value().range.base.value,
                                                  entry_mapping.Value().range.size, false);

        threads_.emplace(id, std::move(entry));
        return ThreadHandle{.id = id, .generation = entry.generation};
    }

    [[nodiscard]] Result<RunResult> Run(ThreadHandle thread, const RunOptions& options) override {
        // Admission first, before claiming the thread. The lease is what makes a publication
        // transaction exclude execution for its whole duration: previously a token only stopped
        // *other writers*, and the backend checked for running threads inside DiscardTranslations,
        // so a Run could enter the JIT on either side of that check (2026-09-08 poison review, R3).
        // It also refuses while code is poisoned, since the backend's translation no longer matches
        // the bytes on the guest page.
        //
        // Asked of the space, never the reverse: the lock order is context -> space.
        auto lease = space_.AcquireExecutionLease();
        if (!lease) {
            return lease.GetError();
        }

        // Claim the thread under the lock and only then leave it: resolving the handle and marking
        // it running have to be one step, or two callers can both pass the "not running" check.
        // The earlier version returned a raw entry pointer from a helper that released the lock on
        // return, then dereferenced it -- the 2026-09-08 review flagged that window as R5.
        FEXCore::Core::InternalThreadState* native = nullptr;
        std::uint64_t invocation = 0;
        std::shared_ptr<InterruptState> interrupt;
        {
            std::lock_guard guard{lock_};

            auto* entry = FindOwnedLocked(thread);
            if (entry == nullptr) {
                return OwnershipError(thread, "Run");
            }
            if (entry->running) {
                return BackendError(ErrorCategory::AlreadyRunning, "Run",
                                    "this thread is already executing");
            }

            // Consume exactly the acknowledged epoch. A request newer than the one the caller says
            // it handled stays pending, so a Resume racing a second Pause cannot swallow it.
            if (auto status = ConsumeResumeLocked(*entry, options.resume_after_epoch); !status) {
                return status.GetError();
            }
            if (options.deadline_ns != 0) {
                // Refuse before execution rather than silently ignoring it, which is what the
                // previous version did. Round 2 allows either implementing it or an explicit
                // refusal; a deadline that is quietly dropped is the one outcome not allowed.
                return BackendError(ErrorCategory::Unsupported, "Run",
                                    "RunOptions::deadline_ns is not implemented by this backend; "
                                    "use RequestInterrupt for bounded execution");
            }

            entry->running = true;
            invocation = ++entry->invocation_counter;
            native = entry->native;
            interrupt = entry->interrupt;
        }

        // Bind this thread for the duration of the run, so the signal handler can find its state
        // without a lock. Cleared on every exit path below.
        ThreadInterruptBinding binding{};
        binding.state = interrupt.get();
        binding.fex = context_.get();
        binding.native = native;
        binding.frame = native != nullptr ? static_cast<void*>(native->CurrentFrame) : nullptr;
        if (auto* delegator = signal_delegator_.get(); delegator != nullptr) {
            const auto& config = delegator->GetConfig();
            binding.pause_spill_sra = config.ThreadPauseHandlerAddressSpillSRA;
            binding.pause_no_spill = config.ThreadPauseHandlerAddress;
        }
        t_binding = &binding;
        interrupt->native_tid.store(static_cast<std::uint64_t>(::gettid()),
                                    std::memory_order_release);
        interrupt->in_jit.store(true, std::memory_order_release);

        // Executed with the lock released: a guest block runs for an unbounded time, and holding
        // the context lock across it would block every other thread's handle operations. The
        // running flag set above is what keeps this thread from being run or destroyed meanwhile.
        context_->ExecuteThread(native);

        interrupt->in_jit.store(false, std::memory_order_release);
        t_binding = nullptr;

        std::lock_guard guard{lock_};
        auto* entry = FindOwnedLocked(thread);
        if (entry == nullptr) {
            // Should be impossible: running threads are refused by DestroyThread. Report it rather
            // than dereferencing whatever is left.
            return BackendError(ErrorCategory::BackendFailure, "Run",
                                "the thread disappeared while it was executing");
        }
        entry->running = false;
        return BuildRunResultLocked(thread, *entry, invocation, /*step=*/std::nullopt);
    }

    [[nodiscard]] Result<RunResult> Step(ThreadHandle thread, const StepOptions&) override {
        std::lock_guard guard{lock_};
        if (FindOwnedLocked(thread) == nullptr) {
            return OwnershipError(thread, "Step");
        }
        // Single-instruction stepping needs a JIT block-length limit that this
        // backend does not yet drive. Refusing before execution leaves guest
        // state untouched, which T07 requires; returning a whole block as if it
        // were one instruction would be worse than refusing.
        return BackendError(ErrorCategory::Unsupported, "Step",
                            "single-instruction step is not implemented by this backend yet");
    }

    [[nodiscard]] Result<CpuSnapshot> ReadRegisters(ThreadHandle thread) const override {
        std::lock_guard guard{lock_};
        const ThreadEntry* entry = Find(thread);
        if (entry == nullptr) {
            return BackendError(ErrorCategory::InvalidHandle, "ReadRegisters",
                                "no live thread with this id and generation");
        }
        if (entry->running) {
            // Reading while the JIT owns the registers would report stale
            // memory as if it were current state (D03).
            return BackendError(ErrorCategory::AlreadyRunning, "ReadRegisters",
                                "thread is executing; registers are not at a safe point");
        }
        return CaptureSnapshot(thread, *entry, SnapshotKind::SafePoint);
    }

    [[nodiscard]] Result<void> WriteRegisters(ThreadHandle thread, const RegisterPatch& patch,
                                              std::uint64_t stop_epoch) override {
        std::lock_guard guard{lock_};
        auto* entry = FindOwnedLocked(thread);
        if (entry == nullptr) {
            return OwnershipError(thread, "WriteRegisters");
        }
        if (entry->running) {
            return BackendError(ErrorCategory::AlreadyRunning, "WriteRegisters",
                                "cannot write registers of an executing thread");
        }
        if (stop_epoch != entry->stop_epoch) {
            return BackendError(ErrorCategory::StaleEpoch, "WriteRegisters",
                                "the supplied stop epoch is not the thread's current one");
        }

        ApplyPatchToState(patch, entry->native->CurrentFrame->State);
        ApplyXmmPatch(patch, entry->native);
        return Result<void>{};
    }

    [[nodiscard]] Result<void> DestroyThread(ThreadHandle thread) override {
        std::lock_guard guard{lock_};
        auto* entry = FindOwnedLocked(thread);
        if (entry == nullptr) {
            return OwnershipError(thread, "DestroyThread");
        }
        if (entry->running) {
            return BackendError(ErrorCategory::AlreadyRunning, "DestroyThread",
                                "cannot destroy a thread that is executing");
        }

        context_->DestroyThread(entry->native);
        threads_.erase(thread.id);
        return Result<void>{};
    }

    [[nodiscard]] std::size_t LiveThreadCount() const override {
        std::lock_guard guard{lock_};
        return threads_.size();
    }

    // --- asynchronous control ---------------------------------------------------------------

    [[nodiscard]] std::uint64_t ContextId() const noexcept override {
        return context_id_;
    }

    [[nodiscard]] Result<InterruptTicket> RequestInterrupt(ThreadHandle thread,
                                                           InterruptReason reason) override {
        std::shared_ptr<InterruptState> interrupt;
        std::uint64_t generation = 0;
        {
            std::lock_guard guard{lock_};
            // Deliberately not FindOwnedLocked: any thread may request an interrupt, including one
            // that does not own the target. That is the whole point of an out-of-band kick.
            auto found = threads_.find(thread.id);
            if (found == threads_.end() || found->second.generation != thread.generation) {
                return BackendError(ErrorCategory::InvalidHandle, "RequestInterrupt",
                                    "no live thread with this id and generation");
            }
            interrupt = found->second.interrupt;
            generation = found->second.generation;
        }

        const std::uint64_t epoch = next_request_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
        interrupt->pending.fetch_or(1u << static_cast<std::uint32_t>(reason),
                                    std::memory_order_acq_rel);
        // Monotonic: a concurrent request with a higher epoch must not be walked backwards.
        std::uint64_t previous = interrupt->request_epoch.load(std::memory_order_acquire);
        while (previous < epoch &&
               !interrupt->request_epoch.compare_exchange_weak(previous, epoch,
                                                               std::memory_order_acq_rel)) {
        }

        // Only signal a thread that is actually inside the JIT. One that is not will observe the
        // pending bit at its next entry, and signalling it anyway would deliver to a thread that
        // might be inside malloc or the runtime.
        if (interrupt->in_jit.load(std::memory_order_acquire)) {
            const auto tid = static_cast<pid_t>(interrupt->native_tid.load(std::memory_order_acquire));
            if (tid != 0) {
                // tgkill rather than pthread_kill: the target is identified by the tid recorded at
                // Run entry, and a pthread_t could belong to a thread that has since exited.
                ::syscall(SYS_tgkill, ::getpid(), tid, InterruptSignal());
            }
        }

        InterruptTicket ticket{};
        ticket.context_id = context_id_;
        ticket.thread_id = thread.id;
        ticket.thread_generation = generation;
        ticket.epoch = epoch;
        ticket.reason = reason;
        return ticket;
    }

    [[nodiscard]] Result<StopReceipt> WaitStopped(const InterruptTicket& ticket,
                                                  std::uint64_t timeout_ns) override {
        if (!ticket.IsValid() || ticket.context_id != context_id_) {
            return BackendError(ErrorCategory::InvalidArgument, "WaitStopped",
                                "the ticket was not issued by this context");
        }
        std::shared_ptr<InterruptState> interrupt;
        {
            std::lock_guard guard{lock_};
            auto found = threads_.find(ticket.thread_id);
            if (found == threads_.end() || found->second.generation != ticket.thread_generation) {
                return BackendError(ErrorCategory::InvalidHandle, "WaitStopped",
                                    "no live thread with this id and generation");
            }
            if (found->second.owner == std::this_thread::get_id() && found->second.running) {
                // The owner is the thread that has to reach the safe point, so waiting here would
                // deadlock on itself.
                return BackendError(ErrorCategory::WrongThread, "WaitStopped",
                                    "a thread cannot wait for its own stop");
            }
            interrupt = found->second.interrupt;
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::nanoseconds(timeout_ns == 0 ? 1'000'000'000 : timeout_ns);
        {
            std::unique_lock guard{interrupt->park_lock};
            const bool acked = interrupt->park_changed.wait_until(guard, deadline, [&] {
                return interrupt->acked_epoch.load(std::memory_order_acquire) >= ticket.epoch;
            });
            if (!acked) {
                // Leave the request pending. Reporting a stop that did not happen, or tearing down
                // a thread that is still running, are the two outcomes the spec forbids here.
                return BackendError(ErrorCategory::Timeout, "WaitStopped",
                                    "the owner did not reach a safe point before the deadline");
            }
        }

        // The owner is stopped and its state is published, so a snapshot is now authoritative.
        std::lock_guard guard{lock_};
        auto found = threads_.find(ticket.thread_id);
        if (found == threads_.end() || found->second.generation != ticket.thread_generation) {
            return BackendError(ErrorCategory::InvalidHandle, "WaitStopped",
                                "the thread was destroyed while stopping");
        }
        StopReceipt receipt{};
        receipt.context_id = context_id_;
        receipt.thread_id = ticket.thread_id;
        receipt.request_epoch = ticket.epoch;
        receipt.stop_epoch = ++found->second.stop_epoch;
        const std::uint32_t reasons = interrupt->stop_reason.load(std::memory_order_acquire);
        // Precedence is fixed by the spec: Cancel outranks Pause, and a fault would outrank both.
        receipt.reason =
            (reasons & (1u << static_cast<std::uint32_t>(InterruptReason::Cancel))) != 0
                ? StopReason::Cancelled
                : StopReason::PauseRequested;
        receipt.pending_reasons = interrupt->pending.load(std::memory_order_acquire);
        receipt.snapshot = CaptureSnapshot(
            ThreadHandle{ticket.thread_id, ticket.thread_generation}, found->second,
            SnapshotKind::SafePoint);
        return receipt;
    }

    [[nodiscard]] Result<void> Resume(ThreadHandle thread,
                                      std::uint64_t acknowledged_epoch) override {
        std::shared_ptr<InterruptState> interrupt;
        {
            std::lock_guard guard{lock_};
            auto found = threads_.find(thread.id);
            if (found == threads_.end() || found->second.generation != thread.generation) {
                return BackendError(ErrorCategory::InvalidHandle, "Resume",
                                    "no live thread with this id and generation");
            }
            interrupt = found->second.interrupt;
        }
        if (interrupt->acked_epoch.load(std::memory_order_acquire) < acknowledged_epoch) {
            return BackendError(ErrorCategory::WrongState, "Resume",
                                "that epoch has not been acknowledged by a stop yet");
        }
        {
            std::lock_guard guard{interrupt->park_lock};
            interrupt->resume_requested.store(true, std::memory_order_release);
        }
        interrupt->park_changed.notify_all();
        return Result<void>{};
    }

    [[nodiscard]] Result<void> InvalidateCode(const QuiescenceToken& token, GuestRange range,
                                              InvalidationReason reason) override {
        if (!token.IsValid()) {
            return BackendError(ErrorCategory::WrongState, "InvalidateCode",
                                "a valid quiescence token is required to discard translated code");
        }
        // Identity, not just validity. A token from another address space is structurally valid
        // and would otherwise authorise discarding translations for a space it says nothing about.
        if (!token.IsFrom(&space_)) {
            return BackendError(ErrorCategory::InvalidArgument, "InvalidateCode",
                                "the token belongs to a different address space, or its space has "
                                "been destroyed");
        }
        return DiscardTranslationsImpl(range, reason, "InvalidateCode");
    }

    // --- CodeInvalidationSink ---------------------------------------------------
    //
    // GuestAddressSpace::PublishCode / InvalidateCode reach the same code below. Before this
    // existed the public memory API only bumped a generation, so a caller could publish new bytes,
    // see both calls succeed, and still execute the previous translation -- reproduced as P1-C in
    // the 2026-09-08 publication review. The address space has already checked the token by the
    // time it calls this, so there is no token argument to re-check here.

    [[nodiscard]] std::string_view Name() const override {
        return "FEXCore";
    }

    [[nodiscard]] Status DiscardTranslations(GuestRange range, InvalidationReason reason) override {
        return DiscardTranslationsImpl(range, reason, "DiscardTranslations");
    }

private:
    [[nodiscard]] Status DiscardTranslationsImpl(GuestRange range, InvalidationReason reason,
                                                 const char* operation) {
        (void)reason;
        std::lock_guard guard{lock_};
        for (auto& [id, entry] : threads_) {
            if (entry.running) {
                return BackendError(ErrorCategory::AlreadyRunning, operation,
                                    "a guest thread is still executing");
            }
        }

        auto checked = GuestRange::Checked(range.base, range.size);
        if (!checked) {
            return checked.GetError();
        }

        // CodeBuffer/L3 mappings survive the last guest thread. Clearing only
        // threads therefore did nothing between destroy/recreate cycles: the
        // next thread reused the previous fixture's translation at the same VA.
        // Match FEX's frontend protocol: invalidate shared translations even
        // with zero threads, then invalidate each live thread's local caches.
        // Both operations require FEX's exclusive code-invalidation lock.
        std::scoped_lock code_guard{context_->GetCodeInvalidationMutex()};
        context_->InvalidateCodeBuffersCodeRange(range.base.value, range.size);
        for (auto& [id, entry] : threads_) {
            if (entry.native != nullptr) {
                context_->InvalidateThreadCachedCodeRange(entry.native, range.base.value,
                                                          range.size);
            }
        }
        return Result<void>{};
    }

public:

    [[nodiscard]] std::uint64_t ReturnGateAddress() const noexcept {
        return return_gate_.Address();
    }

private:
    struct ThreadEntry final {
        FEXCore::Core::InternalThreadState* native{};
        std::uint64_t generation{};
        std::thread::id owner{};
        std::uint64_t guest_tid{};
        std::uint64_t entry_rip{};
        std::uint64_t invocation_counter{};
        std::uint64_t stop_epoch{1};
        bool running{false};
        // Stable address, so the signal handler can hold a pointer to it while the map changes.
        std::shared_ptr<InterruptState> interrupt{std::make_shared<InterruptState>()};

        // The guest descriptor table. CPUState only holds a pointer to it, and
        // the decoder dereferences that pointer on the very first block to read
        // CS.L and decide 64-bit mode -- so this must exist before any code
        // runs and must outlive the FEX thread. Owned per thread rather than
        // per context so one thread's segment state cannot disturb another's.
        std::unique_ptr<std::array<FEXCore::Core::CPUState::gdt_segment, 32>> gdt;

        // Also embedder-owned and also referenced by raw pointer from CPUState,
        // so it has the same lifetime requirement as the GDT.
        std::unique_ptr<CallRetStack> callret;
    };

    // Requires lock_. Applies RunOptions::resume_after_epoch before a run starts.
    //
    // Consumes exactly the named epoch. A request that arrived after it keeps its pending bit, so
    // resuming from an older stop cannot swallow a newer Pause -- which is the race the spec calls
    // out for resume_after_epoch.
    [[nodiscard]] Status ConsumeResumeLocked(ThreadEntry& entry, std::uint64_t acknowledged_epoch) {
        auto& interrupt = *entry.interrupt;
        if (acknowledged_epoch == 0) {
            // No resume requested. Refuse to enter the JIT while a stop is still outstanding rather
            // than running through it.
            if (interrupt.pending.load(std::memory_order_acquire) != 0) {
                return BackendError(ErrorCategory::WrongState, "Run",
                                    "an interrupt is pending; resume the acknowledged epoch first");
            }
            return Ok();
        }
        if (interrupt.acked_epoch.load(std::memory_order_acquire) < acknowledged_epoch) {
            return BackendError(ErrorCategory::StaleEpoch, "Run",
                                "resume_after_epoch has not been acknowledged by a stop");
        }
        if (interrupt.request_epoch.load(std::memory_order_acquire) > acknowledged_epoch) {
            return BackendError(ErrorCategory::WrongState, "Run",
                                "a newer interrupt is pending; it must be serviced before running");
        }
        interrupt.pending.store(0, std::memory_order_release);
        return Ok();
    }

    // Points CPUState at this thread's GDT and installs a flat 64-bit code
    // segment. Without it FEXCore::Frontend::Decoder dereferences a null
    // segment_arrays entry as soon as it compiles the first block.
    static void InitializeSegments(FEXCore::Core::CPUState& state,
                                   std::array<FEXCore::Core::CPUState::gdt_segment, 32>& gdt) {
        state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt.data();
        state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt.data();
        state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;

        auto* code_segment = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
        FEXCore::Core::CPUState::SetGDTBase(code_segment, 0);
        FEXCore::Core::CPUState::SetGDTLimit(code_segment, 0xF'FFFFU);
        state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*code_segment);
        // L=1, D=0 is 64-bit mode. The decoder asserts this matches the
        // context's Is64BitMode, so a mismatch fails loudly rather than
        // decoding 32-bit instructions from 64-bit code.
        code_segment->L = 1;
        code_segment->D = 0;
    }

    [[nodiscard]] const ThreadEntry* Find(ThreadHandle handle) const {
        auto it = threads_.find(handle.id);
        if (it == threads_.end() || it->second.generation != handle.generation) {
            return nullptr;
        }
        return &it->second;
    }

    // Resolves a handle and enforces that the caller owns it. Owner-thread checking is what keeps
    // two host threads from driving one guest thread.
    //
    // Requires lock_ to already be held, and the returned pointer is only valid while the caller
    // keeps holding it. An earlier version took the lock inside a helper and returned this pointer
    // to a caller that then used it unlocked, which is the window the 2026-09-08 review recorded as
    // part of R5.
    [[nodiscard]] ThreadEntry* FindOwnedLocked(ThreadHandle handle) {
        auto it = threads_.find(handle.id);
        if (it == threads_.end() || it->second.generation != handle.generation) {
            return nullptr;
        }
        if (it->second.owner != std::this_thread::get_id()) {
            return nullptr;
        }
        return &it->second;
    }

    // Distinguishes "no such thread" from "not your thread" for the caller, which FindOwnedLocked
    // deliberately collapses so it can return a single pointer.
    [[nodiscard]] Error OwnershipError(ThreadHandle handle, std::string_view operation) {
        auto it = threads_.find(handle.id);
        if (it == threads_.end() || it->second.generation != handle.generation) {
            return BackendError(ErrorCategory::InvalidHandle, operation,
                                "no live thread with this id and generation");
        }
        return BackendError(ErrorCategory::WrongThread, operation,
                            "only the creating thread may drive this guest thread");
    }

    void ApplyPatchToState(const RegisterPatch& patch, FEXCore::Core::CPUState& state) const {
        if (HasAll(patch.fields, RegisterValidity::Gpr)) {
            for (std::size_t i = 0; i < kGprCount; ++i) {
                if ((patch.gpr_mask & (1u << i)) != 0) {
                    state.gregs[i] = patch.values.gpr[i];
                }
            }
        }
        if (HasAll(patch.fields, RegisterValidity::Rip)) {
            state.rip = patch.values.rip;
        }
        if (HasAll(patch.fields, RegisterValidity::Mxcsr)) {
            state.mxcsr = patch.values.mxcsr;
        }
        if (HasAll(patch.fields, RegisterValidity::SegmentBases)) {
            state.fs_cached = patch.values.fs_base;
            state.gs_cached = patch.values.gs_base;
        }
    }

    // RFLAGS and XMM go through context calls rather than raw CPUState fields:
    // FEX stores flags decomposed across a byte array and may hold XMM in an
    // AVX layout, so writing the struct directly would corrupt them.
    void ApplyXmmPatch(const RegisterPatch& patch, FEXCore::Core::InternalThreadState* native) {
        if (HasAll(patch.fields, RegisterValidity::Rflags)) {
            context_->SetFlagsFromCompactedEFLAGS(native,
                                                  static_cast<std::uint32_t>(patch.values.rflags));
        }
        if (!HasAll(patch.fields, RegisterValidity::Xmm)) {
            return;
        }

        std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> low{};
        std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> high{};
        // Read-modify-write: the mask selects individual registers, so the
        // unselected ones must keep their current values.
        context_->ReconstructXMMRegisters(native, low.data(),
                                          host_features_.SupportsAVX ? high.data() : nullptr);
        for (std::size_t i = 0; i < kXmmCount && i < low.size(); ++i) {
            if ((patch.xmm_mask & (1u << i)) != 0) {
                // Xmm is two explicit halves rather than a byte array, so build
                // the 128-bit value from them instead of memcpy'ing a struct
                // whose layout could drift.
                low[i] = (static_cast<__uint128_t>(patch.values.xmm[i].high) << 64) |
                         patch.values.xmm[i].low;
            }
        }
        context_->SetXMMRegistersFromState(native, low.data(),
                                           host_features_.SupportsAVX ? high.data() : nullptr);
    }

    [[nodiscard]] CpuSnapshot CaptureSnapshot(ThreadHandle handle, const ThreadEntry& entry,
                                              SnapshotKind kind) const {
        CpuSnapshot snapshot{};
        snapshot.thread_id = handle.id;
        snapshot.thread_generation = handle.generation;
        snapshot.stop_epoch = entry.stop_epoch;
        snapshot.invocation_id = entry.invocation_counter;
        snapshot.kind = kind;
        snapshot.mapping_generation = space_.MappingGeneration();
        snapshot.code_generation = space_.CodeGeneration();

        const auto& state = entry.native->CurrentFrame->State;
        for (std::size_t i = 0; i < kGprCount; ++i) {
            snapshot.registers.gpr[i] = state.gregs[i];
        }
        snapshot.registers.rip = state.rip;
        snapshot.registers.mxcsr = state.mxcsr;
        snapshot.registers.fs_base = state.fs_cached;
        snapshot.registers.gs_base = state.gs_cached;

        // WasInJIT=false: this is a safe point, so flags are reconstructed from
        // the published state rather than from host registers.
        snapshot.registers.rflags =
            context_->ReconstructCompactedEFLAGS(entry.native, /*WasInJIT=*/false, nullptr, 0);

        std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> low{};
        std::array<__uint128_t, FEXCore::Core::CPUState::NUM_XMMS> high{};
        context_->ReconstructXMMRegisters(entry.native, low.data(),
                                          host_features_.SupportsAVX ? high.data() : nullptr);
        for (std::size_t i = 0; i < kXmmCount && i < low.size(); ++i) {
            snapshot.registers.xmm[i].low = static_cast<std::uint64_t>(low[i]);
            snapshot.registers.xmm[i].high = static_cast<std::uint64_t>(low[i] >> 64);
        }

        // Only claim the fields actually captured. AVX high state is not
        // reported, so it stays out of the validity mask rather than appearing
        // as valid zeros.
        snapshot.registers.validity = RegisterValidity::Gpr | RegisterValidity::Rip |
                                      RegisterValidity::Rflags | RegisterValidity::Xmm |
                                      RegisterValidity::Mxcsr | RegisterValidity::SegmentBases;
        return snapshot;
    }

    // Requires lock_ to be held: it reads and advances the entry's stop epoch.
    [[nodiscard]] Result<RunResult> BuildRunResultLocked(ThreadHandle handle, ThreadEntry& entry,
                                                   std::uint64_t invocation,
                                                   std::optional<StepInfo> step) {
        entry.stop_epoch++;

        RunResult result{};
        result.thread_id = handle.id;
        result.thread_generation = handle.generation;
        result.invocation_id = invocation;
        result.stop_epoch = entry.stop_epoch;
        result.step = step;

        const std::uint64_t rip = entry.native->CurrentFrame->State.rip;
        const std::uint64_t gate = return_gate_.Address();

        if (syscall_handler_->TakeUnexpectedSyscall()) {
            // A syscall with no HLE path is a fault, not a normal return.
            result.primary_reason = StopReason::GuestFault;
            result.pending_reasons |= BitOf(StopReason::GuestFault);
            GuestFaultInfo fault{};
            fault.guest_rip = rip;
            fault.access = GuestAccessKind::Execute;
            fault.recoverable = false;
            result.fault = fault;
        } else if (rip >= gate && rip < gate + return_gate_.Size()) {
            // Stopped at the registered gate: this is the only RIP that counts
            // as a normal return (D05).
            result.primary_reason = StopReason::Returned;
            result.pending_reasons |= BitOf(StopReason::Returned);
            result.guest_pc = rip;
        } else {
            // Execution left the JIT somewhere unregistered -- a real HLT, an
            // illegal instruction, or a fault. Reporting Returned here would be
            // exactly the conflation D05 forbids.
            result.primary_reason = StopReason::GuestFault;
            result.pending_reasons |= BitOf(StopReason::GuestFault);
            GuestFaultInfo fault{};
            fault.guest_rip = rip;
            fault.access = GuestAccessKind::Execute;
            fault.recoverable = false;
            result.fault = fault;
        }

        result.snapshot = CaptureSnapshot(handle, entry,
                                          result.primary_reason == StopReason::Returned
                                              ? SnapshotKind::SafePoint
                                              : SnapshotKind::Faulted);
        return result;
    }

    CpuConfig config_{};
    GuestAddressSpace& space_;

    // Identity for tickets and receipts. Monotonic across the process, so a ticket from a
    // destroyed context cannot match its replacement even though thread ids restart.
    const std::uint64_t context_id_{NextContextId()};
    std::atomic<std::uint64_t> next_request_epoch_{0};

    mutable std::mutex lock_;
    std::unordered_map<std::uint64_t, ThreadEntry> threads_;
    std::uint64_t next_thread_id_{1};
    std::uint64_t generation_counter_{0};

    fextl::unique_ptr<FEXCore::Context::Context> context_;
    std::unique_ptr<FexSignalDelegator> signal_delegator_;
    std::unique_ptr<FexSyscallHandler> syscall_handler_;
    FEXCore::HostFeatures host_features_{};
    ReturnGate return_gate_;
    std::uint64_t host_page_size_{};
    bool config_initialized_{false};
    bool allocator_hooked_{false};
};

} // namespace

BackendCapabilities QueryFexCapabilities() {
    BackendCapabilities caps{};
    caps.backend_name = "FEXCore";
    caps.upstream_revision = "50e6eee95ae95d3257672727a9302a30b4a60a9a";
    caps.downstream_revision = "feature/malos/host-page-size";
    // AVX is deliberately absent: C04 is a conditional MUST, and declaring a
    // feature without an execution and state-restore test would be a false
    // claim. C03 verifies that asking for it is refused.
    caps.features = GuestFeature::BaseInteger | GuestFeature::Sse2;
    // Step is refused before execution until the block-limit path is driven, so
    // no scope is claimed.
    caps.step_scope = StepScope::None;
    caps.host_page_size = FEXCore::Utils::HostPageSize();
    caps.max_guest_address = kGuestAddressPolicyLimit;
    return caps;
}

Result<std::unique_ptr<CpuContext>> CreateFexContext(const CpuConfig& config,
                                                     GuestAddressSpace& space) {
    if (config.api_version != CpuConfig::kApiVersion) {
        return BackendError(ErrorCategory::InvalidArgument, "CreateContext",
                            "api_version does not match this build");
    }
    if (config.memory_mode != MemoryMode::DirectMapped) {
        return BackendError(ErrorCategory::UnsupportedMemoryMode, "CreateContext",
                            "V0 implements DirectMapped only");
    }
    if (config.smc_mode != SmcMode::ExplicitPublication) {
        return BackendError(ErrorCategory::Unsupported, "CreateContext",
                            "V0 implements ExplicitPublication only");
    }
    if (Contains(config.requested_features, GuestFeature::Avx)) {
        return BackendError(ErrorCategory::Unsupported, "CreateContext",
                            "AVX is not implemented; capabilities do not declare it");
    }

    bool expected = false;
    if (!g_context_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return BackendError(ErrorCategory::AlreadyActive, "CreateContext",
                            "a guest CPU context is already live in this process");
    }

    auto context = std::make_unique<FexCpuContext>(config, space);
    auto init = context->Initialize();
    if (!init) {
        // Reset the flag here rather than only in the destructor: a failed
        // Initialize must not leave the process unable to try again.
        g_context_active.store(false, std::memory_order_release);
        return init.GetError();
    }
    return std::unique_ptr<CpuContext>{std::move(context)};
}

} // namespace Core::GuestCpu::Fex

namespace Core::GuestCpu {

Result<std::unique_ptr<CpuContext>> CreateContext(const CpuConfig& config,
                                                  GuestAddressSpace& space) {
    return Fex::CreateFexContext(config, space);
}

BackendCapabilities QueryBackendCapabilities() {
    return Fex::QueryFexCapabilities();
}

} // namespace Core::GuestCpu
