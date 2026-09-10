// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/guest_cpu/fex/fex_context.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cfenv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
#include "Interface/Core/CPUBackend.h"
#include "core/guest_cpu/hle/call_adapter.h"

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

// Map FEX's CPUState to the public register file and back at an HLE boundary. GPR order matches
// by construction (the static_asserts above); xmm low/high words are FEX's packed 128-bit view.
void RegistersFromCpuState(const FEXCore::Core::CPUState& state, RegisterFile& out) {
    for (std::size_t i = 0; i < kGprCount; ++i) {
        out.gpr[i] = state.gregs[i];
    }
    for (std::size_t i = 0; i < kXmmCount && i < FEXCore::Core::CPUState::NUM_XMMS; ++i) {
        out.xmm[i] = Xmm{state.xmm.sse.data[i][0], state.xmm.sse.data[i][1]};
    }
    out.rip = state.rip;
    out.mxcsr = state.mxcsr;
    out.fs_base = state.fs_cached;
    out.gs_base = state.gs_cached;
}
void ApplyRegistersToCpuState(const RegisterFile& in, FEXCore::Core::CPUState& state) {
    for (std::size_t i = 0; i < kGprCount; ++i) {
        state.gregs[i] = in.gpr[i];
    }
    for (std::size_t i = 0; i < kXmmCount && i < FEXCore::Core::CPUState::NUM_XMMS; ++i) {
        state.xmm.sse.data[i][0] = in.xmm[i].low;
        state.xmm.sse.data[i][1] = in.xmm[i].high;
    }
    state.rip = in.rip;
    state.mxcsr = in.mxcsr;
    state.fs_cached = in.fs_base;
    state.gs_cached = in.gs_base;
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

    uintptr_t GetThunkCallbackRET() const override { return callback_return_; }

  private:
    std::uintptr_t callback_return_{};
};

// --- entry-boundary interrupts ------------------------------------------------
// See docs/validation/round2/g1-control-decision.md. The controller protects an
// owner-exclusive fault page; the JIT checks it before a basic block. We never
// redirect an arbitrary host PC out of a C++ call/lock or restore a suspended JIT
// stack after code publication. Pause returns Run to its owner like Cancel.
struct InterruptState final {
    // All fields are protected by FexCpuContext::lock_. No handler accesses this.
    std::map<std::uint64_t, InterruptReason> requests;
    std::uint32_t pending{};
    std::uint64_t request_epoch{};
    std::uint64_t acked_epoch{};
    std::optional<StopReceipt> receipt;
    std::optional<CpuSnapshot> stopped_snapshot;
    StopReason last_reason{StopReason::Returned};
};

struct ThreadInterruptBinding final {
    FEXCore::Context::Context *fex{};
    FEXCore::Core::InternalThreadState *native{};
    std::uintptr_t fault_page{};
    std::uintptr_t stop_spill{};
    std::array<std::uint64_t, 31> gprs{};
    std::uint64_t pstate{};
    std::uint64_t guest_rip{};
    std::atomic<bool> interrupted{false};
};
static_assert(std::atomic<bool>::is_always_lock_free);
thread_local ThreadInterruptBinding *t_binding = nullptr;
struct sigaction g_previous_fault_action {};
std::mutex g_interrupt_install_lock;
bool g_interrupt_installed{};

void ForwardAction(int signal, siginfo_t *info, void *ucontext, const struct sigaction &previous) {
    if (previous.sa_handler == SIG_IGN)
        return;
    if (previous.sa_handler == SIG_DFL || previous.sa_handler == nullptr) {
        ::signal(signal, SIG_DFL);
        ::raise(signal);
    } else if ((previous.sa_flags & SA_SIGINFO) != 0) {
        previous.sa_sigaction(signal, info, ucontext);
    } else {
        previous.sa_handler(signal);
    }
}

void InterruptFaultHandler(int signal, siginfo_t *info, void *raw_context) {
#if defined(__aarch64__)
    auto *binding = t_binding;
    if (binding && info && info->si_code == SEGV_ACCERR &&
        reinterpret_cast<std::uintptr_t>(info->si_addr) == binding->fault_page) {
        auto *uc = static_cast<ucontext_t *>(raw_context);
        const auto pc = uc->uc_mcontext.pc;
        const bool in_jit = binding->fex->IsAddressInCodeBuffer(binding->native, pc);
        if (!in_jit) {
            // FEX's deferred-signal guards also store zero to this exclusively
            // owned page after host work. Skip that probe, NOT its host frame;
            // keep the page protected until the next JIT entry. AArch64 stores
            // are one instruction. This address is never exposed to guest/HLE.
            uc->uc_mcontext.pc += 4;
            return;
        }
        // An IR-internal backward edge (e.g. REP) may also contain this probe,
        // even with MULTIBLOCK disabled. Only the FIRST probe after this block's
        // header is an entry safe point. Skip internal probes without unwinding
        // partially executed guest instructions. This encoding/layout is pinned
        // to the same FEX revision as the adapter, not a generic PC heuristic.
        constexpr auto offset = offsetof(FEXCore::Core::InternalThreadState, InterruptFaultPage) -
                                offsetof(FEXCore::Core::InternalThreadState, BaseFrameState);
        static_assert(offset <= 32760 && offset % 8 == 0);
        constexpr std::uint32_t probe = 0xf9000000u | (offset / 8 << 10) | (28 << 5) | 31;
        const auto header = binding->native->CurrentFrame->State.InlineJITBlockHeader;
        bool entry_probe = false;
        // The entry preamble is short. Refuse rather than guess if a future FEX
        // layout moves it beyond this bounded range.
        for (auto at = header + sizeof(FEXCore::CPU::CPUBackend::JITCodeHeader);
             at <= pc && at < header + 256; at += 4) {
            if (*reinterpret_cast<const std::uint32_t *>(at) == probe) {
                entry_probe = at == pc;
                break;
            }
        }
        if (!entry_probe) {
            uc->uc_mcontext.pc += 4;
            return;
        }
        // MULTIBLOCK is disabled and the fault check is before guest operations.
        // InlineJITBlockHeader was installed by EmitEntryPoint immediately before
        // this store. Preserve NZCV/GPRs for FEX's flag reconstruction on return.
        binding->guest_rip = binding->fex->GetGuestBlockEntry(binding->native);
        binding->pstate = uc->uc_mcontext.pstate;
        for (std::size_t i = 0; i < binding->gprs.size(); ++i)
            binding->gprs[i] = uc->uc_mcontext.regs[i];
        binding->interrupted.store(true, std::memory_order_release);
        uc->uc_mcontext.sp = binding->native->CurrentFrame->ReturningStackLocation;
        uc->uc_mcontext.regs[28] = reinterpret_cast<std::uintptr_t>(binding->native->CurrentFrame);
        uc->uc_mcontext.pc = binding->stop_spill;
        return;
    }
#endif
    ForwardAction(signal, info, raw_context, g_previous_fault_action);
}

Status InstallInterruptHandler() {
    std::lock_guard guard{g_interrupt_install_lock};
    if (g_interrupt_installed)
        return Ok();
    struct sigaction action {};
    action.sa_sigaction = InterruptFaultHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGSEGV, &action, &g_previous_fault_action) != 0)
        return BackendError(ErrorCategory::BackendFailure, "InstallInterruptHandler",
                            "sigaction(SIGSEGV) failed", errno);
    g_interrupt_installed = true;
    return Ok();
}

void RestoreInterruptHandler() {
    std::lock_guard guard{g_interrupt_install_lock};
    if (!g_interrupt_installed)
        return;
    ::sigaction(SIGSEGV, &g_previous_fault_action, nullptr);
    g_interrupt_installed = false;
}

// An owner restores its pre-existing altstack on every Run exit. Allocated before
// execution, never in the handler. An existing sufficiently sized stack is reused.
class OwnerSignalStack final {
  public:
    Status Install(std::size_t page_size) {
        if (::sigaltstack(nullptr, &previous_) != 0)
            return BackendError(ErrorCategory::BackendFailure, "Run", "query altstack", errno);
        if ((previous_.ss_flags & SS_ONSTACK) != 0)
            return BackendError(ErrorCategory::WrongState, "Run", "cannot Run from a signal stack");
        if (!(previous_.ss_flags & SS_DISABLE) && previous_.ss_size >= 64 * 1024)
            return Ok();
        size_ = 128 * 1024 + 2 * page_size;
        memory_ = ::mmap(nullptr, size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory_ == MAP_FAILED) {
            memory_ = nullptr;
            return BackendError(ErrorCategory::OutOfMemory, "Run", "allocate altstack", errno);
        }
        auto *stack = static_cast<std::byte *>(memory_) + page_size;
        if (::mprotect(stack, 128 * 1024, PROT_READ | PROT_WRITE) != 0)
            return BackendError(ErrorCategory::BackendFailure, "Run", "protect altstack", errno);
        stack_t replacement{};
        replacement.ss_sp = stack;
        replacement.ss_size = 128 * 1024;
        if (::sigaltstack(&replacement, nullptr) != 0)
            return BackendError(ErrorCategory::BackendFailure, "Run", "install altstack", errno);
        installed_ = true;
        return Ok();
    }
    ~OwnerSignalStack() {
        if (installed_)
            ::sigaltstack(&previous_, nullptr);
        if (memory_)
            ::munmap(memory_, size_);
    }

  private:
    stack_t previous_{};
    void *memory_{};
    std::size_t size_{};
    bool installed_{};
};

// LookupCache's constructor calls SyscallHandler::MarkOvercommitRange, so
// CreateThread crashes without a handler. Three methods are pure virtual and
// must be supplied even though guest syscalls are not part of V0.
class FexSyscallHandler final : public FEXCore::HLE::SyscallHandler {
  public:
    explicit FexSyscallHandler(GuestAddressSpace& space) : space_(space) {}

    // Real guest->host HLE gate (R2-H01). The guest places the operation number in rax before the
    // syscall instruction; its other GPRs/xmm hold the SysV arguments. We switch FP environment,
    // build a frame from the spilled state and dispatch to a registered native function; the return
    // value is encoded back into the frame and the guest continues after the syscall. An
    // unregistered operation is a per-thread fault, never an implicit host syscall.
    void HandleSyscall(FEXCore::Core::CpuStateFrame *Frame) override {
        if (Frame == nullptr) {
            unknown_thread_syscall_.store(true, std::memory_order_release);
            return;
        }

        // Per-thread attribution first: a fault flag unique to this thread.
        std::shared_ptr<std::atomic<bool>> fault_flag;
        {
            std::lock_guard guard{threads_lock_};
            auto it = syscall_fault_by_frame_.find(Frame);
            if (it != syscall_fault_by_frame_.end()) {
                fault_flag = it->second.lock();
            }
        }

        const std::uint64_t operation = Frame->State.gregs[FEXCore::X86State::REG_RAX];
        if (auto adapter = registry_.Find(operation)) {
            Hle::HleCallFrame hle_frame{};
            hle_frame.operation = operation;
            hle_frame.space = &space_;
            RegistersFromCpuState(Frame->State, hle_frame.registers);
            // syscall callgate: the 4th integer argument arrived in r10; the adapter decodes rcx
            // positionally, so normalise r10 -> rcx before dispatch.
            hle_frame.registers.Set(Gpr::Rcx, Frame->State.gregs[FEXCore::X86State::REG_R10]);
            hle_frame.rcx_normalised_from_r10 = true;

            // Save host FP across the crossing; the native function may use SSE. Restore guest FP
            // before the guest resumes. This is per-crossing, not just the outermost Run save.
            fenv_t host_fp{};
            ::fegetenv(&host_fp);
            Status call_status = adapter->Invoke(hle_frame);
            ::fesetenv(&host_fp);

            if (call_status) {
                // Encode the return values back into the guest state the dispatcher continues with.
                ApplyRegistersToCpuState(hle_frame.registers, Frame->State);
                return;
            }
            // A registered-but-rejected call (bad pointer/signature) is this thread's fault.
            last_hle_error_.store(call_status.GetError().category, std::memory_order_release);
        }

        if (fault_flag) {
            fault_flag->store(true, std::memory_order_release);
        } else {
            unknown_thread_syscall_.store(true, std::memory_order_release);
        }
    }

    Hle::HleCallRegistry& Registry() { return registry_; }

    // Registers the per-thread syscall-fault flag for the duration of an ExecuteThread call. The
    // weak_ptr entry is dropped when the owner unregisters, so a stale frame never faults the
    // wrong thread and a destroyed thread's flag does not outlive it.
    void RegisterThreadFrame(FEXCore::Core::CpuStateFrame* frame,
                             std::weak_ptr<std::atomic<bool>> flag) {
        std::lock_guard guard{threads_lock_};
        syscall_fault_by_frame_[frame] = std::move(flag);
    }
    void UnregisterThreadFrame(FEXCore::Core::CpuStateFrame* frame) {
        std::lock_guard guard{threads_lock_};
        syscall_fault_by_frame_.erase(frame);
    }

    [[nodiscard]] bool TakeUnknownThreadSyscall() {
        return unknown_thread_syscall_.exchange(false, std::memory_order_acq_rel);
    }

    FEXCore::HLE::ExecutableRangeInfo
    QueryGuestExecutableRange(FEXCore::Core::InternalThreadState *Thread,
                              uint64_t Address) override {
        std::lock_guard guard{lock};
        for (const auto &range : executable_ranges) {
            if (Address >= range.base && Address < range.base + range.size) {
                return {.Base = range.base, .Size = range.size, .Writable = range.writable};
            }
        }
        auto mapping = space_.Query(GuestAddress{Address});
        if (mapping && HasPermission(mapping.Value().permission, GuestPermission::Execute))
            return {.Base = mapping.Value().range.base.value, .Size = mapping.Value().range.size,
                    .Writable = HasPermission(mapping.Value().permission, GuestPermission::Write)};
        // Not a known executable range. Report an empty one rather than
        // claiming the address is valid code.
        return {.Base = Address, .Size = 0, .Writable = false};
    }

    std::optional<FEXCore::ExecutableFileSectionInfo>
    LookupExecutableFileSection(FEXCore::Core::InternalThreadState *Thread,
                                uint64_t GuestAddr) override {
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
    // FEXCore calls this once per guest page it has compiled code from, which is the only
    // outside-visible confirmation of *what* was translated.
    void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState *Thread, uint64_t Start,
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

    [[nodiscard]] bool TakeUnknownThreadSyscallGlobal() {
        return TakeUnknownThreadSyscall();
    }

  private:
    struct Range final {
        std::uint64_t base{};
        std::uint64_t size{};
        bool writable{};
    };

    mutable std::mutex lock;
    std::vector<Range> executable_ranges; // Immutable backend return gate only.
    GuestAddressSpace& space_;

    // Per-thread syscall-fault attribution (R2-H05). Keyed by the frame FEXCore passes to
    // HandleSyscall; weak so a thread that has exited never keeps the map entry alive.
    mutable std::mutex threads_lock_;
    std::unordered_map<FEXCore::Core::CpuStateFrame*, std::weak_ptr<std::atomic<bool>>>
        syscall_fault_by_frame_;
    std::atomic<bool> unknown_thread_syscall_{false};
    std::atomic<ErrorCategory> last_hle_error_{ErrorCategory::None};

    // Registered native HLE functions. An embedder installs typed adapters here; a guest syscall
    // whose rax names a registered operation is dispatched instead of faulting.
    Hle::HleCallRegistry registry_;
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

    [[nodiscard]] bool Create(std::string &error_detail, int &error_no) {
        const long host_page = ::sysconf(_SC_PAGESIZE);
        size = host_page > 0 ? static_cast<std::size_t>(host_page) : 4096;

        // The gate is guest-executable code, so it is subject to the same addressing limit as any
        // other guest mapping: above kGuestAddressPolicyLimit the block lookup would alias it.
        //
        // Hinted near the top of the addressable range, because the guest reservation is placed in
        // the lower half and a hint that lands inside it would be rejected and fall back to a high
        // address. A hint is advisory either way, so the result is verified below.
        void *hint = reinterpret_cast<void *>(kGuestAddressPolicyLimit - (std::uint64_t{1} << 30));
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
        *static_cast<std::uint8_t *>(page) = 0xF4;

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
    [[nodiscard]] std::size_t Size() const noexcept { return size; }

  private:
    void *page{};
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

    [[nodiscard]] bool Create(std::string &error_detail, int &error_no) {
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

    void InstallInto(FEXCore::Core::InternalThreadState *thread) const {
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
    [[nodiscard]] void *Base() const {
        return static_cast<std::uint8_t *>(reservation) + guard_size;
    }

    void *reservation{};
    std::size_t guard_size{};
};

// --- context -----------------------------------------------------------------
#if defined(GUEST_CPU_TEST_HOOKS)
// Deterministic Run-entry delay for coordinator tests. Test-only; compiled out of release builds.
//
// The owner calls WaitAtEntry while it holds no coordinator/context/address-space lock and before
// entering the JIT. It blocks while an arm for its thread id is live; the controller arms it, waits
// for arrival, exercises the coordinator (which sends the Pause to a running thread that cannot yet
// stop), observes the second owner stop independently in real JIT, then Release(). A strict
// arrival/arm/exit generation handshake means the controller can never arm a hold before the prior
// one has fully left, and the owner never parks inside a signal handler or with a FEX lock held.
class FexTestRunGateImpl final : public FexTestRunGate {
  public:
    bool WaitAtEntry(std::uint64_t context_id, std::uint64_t thread_id, std::uint64_t invocation,
                     std::uint64_t timeout_ms) override {
        std::unique_lock lock{mutex_};
        if (!armed_ || arm_thread_ != thread_id) {
            // Not the held owner (or nothing armed): pass straight through, do not touch gate state.
            return true;
        }
        present_context_ = context_id;
        present_thread_ = thread_id;
        present_invocation_ = invocation;
        arrived_ = true;
        cv_.notify_all();
        const bool released = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms ? timeout_ms : 1000),
                                          [&] { return !armed_; });
        arrived_ = false;
        ++exited_generation_;
        cv_.notify_all();
        return released;
    }

    bool Arm(std::uint64_t thread_id) override {
        std::unique_lock lock{mutex_};
        // Refuse to arm while the previous generation has not fully passed through/left the gate.
        if (armed_)
            return false;
        armed_ = true;
        arrived_ = false;
        arm_thread_ = thread_id;
        cv_.notify_all();
        return true;
    }

    bool Arrived() override {
        std::unique_lock lock{mutex_};
        return armed_ && arrived_ && arm_thread_ == present_thread_;
    }

    bool Release(std::uint64_t timeout_ms) override {
        std::unique_lock lock{mutex_};
        const std::uint64_t before = exited_generation_;
        armed_ = false;
        cv_.notify_all();
        return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms ? timeout_ms : 1000),
                            [&] { return exited_generation_ > before; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool armed_{false};
    bool arrived_{false};
    std::uint64_t arm_thread_{0};
    std::uint64_t present_context_{0};
    std::uint64_t present_thread_{0};
    std::uint64_t present_invocation_{0};
    std::uint64_t exited_generation_{0};
};
#endif

class FexCpuContext final : public CpuContext, public CodeInvalidationSink {
  public:
    explicit FexCpuContext(const CpuConfig &config, GuestAddressSpace &space)
        : config_(config), space_(space) {}

    ~FexCpuContext() override {
        // Unregister before anything else: once the backend is going away, the address space must
        // stop routing publications to it rather than calling into a destroyed object.
        space_.ClearCodeInvalidationSink(this);
        // Destroy threads before the context: FEXCore requires it, and a live
        // thread holding a code buffer would otherwise outlive its owner.
        {
            std::lock_guard guard{lock_};
            for (auto &[id, entry] : threads_) {
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
        // Core-only GDBSERVER enables the entry interrupt-page store, not a server.
        // Single basic blocks make its fault a restartable architectural boundary.
        FEXCore::Config::Set(FEXCore::Config::CONFIG_GDBSERVER, "1");
        FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "0");

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
        if (const char *disasm = ::getenv("GUEST_CPU_DISASSEMBLE"); disasm != nullptr) {
            FEXCore::Config::Set(FEXCore::Config::CONFIG_DISASSEMBLE, disasm);
        }

        // Surface FEXCore's own diagnostics. Without a handler these are dropped, and a JIT-side
        // refusal looks identical to a guest that simply did nothing.
        if (::getenv("GUEST_CPU_DEBUG") != nullptr) {
            LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char *message) {
                std::fprintf(stderr, "[fex %s] %s\n", LogMan::DebugLevelStr(level), message);
            });
            LogMan::Throw::InstallHandler(
                [](const char *message) { std::fprintf(stderr, "[fex assert] %s\n", message); });
        }

        // FEX's allocator owns process-wide VA reservations and static objects.
        // ClearHooks intentionally leaks that arena (ReleaseAllocatorWorkaround),
        // so reinstalling it per context cannot reclaim the old reservation and
        // asserts on context rebuild. Keep one allocator for the process lifetime.
        static std::once_flag allocator_once;
        std::call_once(allocator_once, [this] { FEXCore::Allocator::SetupHooks(host_page_size_); });

        // Reads the real ID registers rather than assuming a feature set.
        host_features_ = FEX::FetchHostFeatures();
        context_ = FEXCore::Context::Context::CreateNewContext(host_features_);
        if (!context_) {
            return BackendError(ErrorCategory::BackendFailure, "CreateContext",
                                "FEXCore refused to create a context");
        }

        signal_delegator_ = std::make_unique<FexSignalDelegator>(return_gate_.Address());
        syscall_handler_ = std::make_unique<FexSyscallHandler>(space_);
        context_->SetSignalDelegator(signal_delegator_.get());
        context_->SetSyscallHandler(syscall_handler_.get());

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
        if (signal_delegator_->GetConfig().ThreadStopHandlerAddressSpillSRA == 0) {
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

    [[nodiscard]] Result<ThreadHandle> CreateThread(const ThreadInit &init) override {
        // Refuse while a publication transaction is active or code is poisoned. Creating a thread
        // is not executing yet, but it allocates backend state against a code image that is
        // mid-change, and admitting it here would let a Run follow immediately. Taking the lease
        // and dropping it at the end of this function is the admission check.
        std::lock_guard guard{lock_};
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


        // Allocate the GDT before the thread so the pointer stored in CPUState
        // stays valid for the thread's whole life.
        auto gdt = std::make_unique<std::array<FEXCore::Core::CPUState::gdt_segment, 32>>();
        InitializeSegments(state, *gdt);

        auto *native = context_->CreateThread(&state);
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
        entry.generation = NextContextId();
        entry.owner = std::this_thread::get_id();
        entry.guest_tid = init.guest_tid;
        entry.entry_rip = init.entry_rip.value;
        entry.gdt = std::move(gdt);
        entry.callret = std::move(callret);

        const auto generation = entry.generation;
        auto [it, inserted] = threads_.emplace(id, std::move(entry));
        const ThreadHandle handle{id, generation};
        it->second.interrupt->stopped_snapshot =
            CaptureSnapshot(handle, it->second, SnapshotKind::SafePoint);
        return handle;
    }

    [[nodiscard]] Result<RunResult> Run(ThreadHandle thread, const RunOptions &options) override {
        if (options.deadline_ns != 0)
            return BackendError(ErrorCategory::Unsupported, "Run", "deadline_ns is unsupported");
        auto lease = space_.AcquireExecutionLease();
        if (!lease)
            return lease.GetError();
        OwnerSignalStack signal_stack;
        if (auto status = signal_stack.Install(host_page_size_); !status)
            return status.GetError();

        ThreadInterruptBinding binding{};
        std::uint64_t invocation{};
        std::shared_ptr<std::atomic<bool>> syscall_fault;
        {
            std::lock_guard guard{lock_};
            // A lease may have been acquired just before BeginDrain. Do not enter after
            // the coordinator's owner snapshot; the lease will drain on this refusal.
            if (space_.IsQuiescent())
                return BackendError(ErrorCategory::Busy, "Run", "coordinated drain is active");
            auto *entry = FindOwnedLocked(thread);
            if (!entry)
                return OwnershipError(thread, "Run");
            if (entry->running)
                return BackendError(ErrorCategory::AlreadyRunning, "Run", "thread already running");
            if (entry->interrupt->last_reason == StopReason::GuestFault ||
                entry->interrupt->last_reason == StopReason::BackendFailure)
                return BackendError(ErrorCategory::WrongState, "Run",
                                    "destroy and recreate a faulted thread");
            if (options.resume_after_epoch != 0) {
                if (auto status = ConsumeResumeLocked(*entry, options.resume_after_epoch); !status)
                    return status.GetError();
            }
            invocation = ++entry->invocation_counter;
            if (entry->interrupt->pending != 0) {
                // A request before entry is handled by the owner without executing
                // an instruction; it cannot disappear in the entering-JIT window.
                return FinishRunLocked(thread, *entry, invocation, true);
            }
            entry->running = true;
            entry->interrupt->receipt.reset();
            entry->interrupt->stopped_snapshot.reset();
            binding.fex = context_.get();
            binding.native = entry->native;
            binding.fault_page =
                reinterpret_cast<std::uintptr_t>(entry->native->InterruptFaultPage);
            binding.stop_spill = signal_delegator_->GetConfig().ThreadStopHandlerAddressSpillSRA;
            syscall_fault = entry->syscall_fault;
        }
#if defined(GUEST_CPU_TEST_HOOKS)
        // Test-only deterministic owner delay. Running is set, the execution lease is held and every
        // coordinator/context/address-space lock has been released; we are not inside a signal handler
        // and have touched no JIT state yet. Parking here cannot hold a lock QuiesceContext needs, so
        // a test can hold one owner deterministically while a second, genuinely-JIT owner drains and
        // stops independently. On release execution proceeds and the already-pending Pause/Cancel is
        // serviced at the block-entry fault page exactly as for a normally running owner.
        if (test_run_gate_ &&
            !test_run_gate_->WaitAtEntry(context_id_, thread.id, invocation, 5000)) {
            std::lock_guard fail_guard{lock_};
            if (auto* fail_entry = FindOwnedLocked(thread)) fail_entry->running = false;
            return BackendError(ErrorCategory::BackendFailure, "Run", "test entry gate timed out");
        }
#endif
        // The controller may protect the page at any point from claiming the
        // entry above onwards. No tgkill/TID reuse or late unbound signal exists.
        t_binding = &binding;
        fenv_t host_fp{};
        ::fegetenv(&host_fp);
        // Writing CPUState.mxcsr alone does not update the executing owner's
        // FPCR. Mirror FEX's SetRoundingMode mapping: x86 down/up are reversed
        // relative to ARM64. Start from guest defaults, not host trap/DN/FZ bits.
        // FEX FillSpecialRegs configures AFP/FIZ from mxcsr when supported.
        const auto mxcsr = binding.native->CurrentFrame->State.mxcsr;
        const std::uint64_t rounding = (mxcsr >> 13) & 3;
        const std::uint64_t guest_fpcr = (((rounding & 1) << 1) | ((rounding & 2) >> 1)) << 22 |
                                         (static_cast<std::uint64_t>((mxcsr >> 15) & 1) << 24);
        asm volatile("msr fpcr, %0\n\tmsr fpsr, xzr" : : "r"(guest_fpcr) : "memory");
        syscall_handler_->RegisterThreadFrame(binding.native->CurrentFrame, syscall_fault);
        context_->ExecuteThread(binding.native);
        syscall_handler_->UnregisterThreadFrame(binding.native->CurrentFrame);
        ::fesetenv(&host_fp);
        t_binding = nullptr;

        std::lock_guard guard{lock_};
        auto *entry = FindOwnedLocked(thread);
        if (!entry)
            return BackendError(ErrorCategory::BackendFailure, "Run", "thread disappeared");
        entry->running = false;
        if (::mprotect(entry->native->InterruptFaultPage, host_page_size_,
                       PROT_READ | PROT_WRITE) != 0) {
            entry->interrupt->last_reason = StopReason::BackendFailure;
            return BackendError(ErrorCategory::BackendFailure, "Run", "restore interrupt page",
                                errno);
        }
        const bool interrupted = binding.interrupted.load(std::memory_order_acquire);
        if (interrupted) {
            const auto flags = context_->ReconstructCompactedEFLAGS(
                entry->native, true, binding.gprs.data(), binding.pstate);
            context_->SetFlagsFromCompactedEFLAGS(entry->native, flags);
            entry->native->CurrentFrame->State.rip = binding.guest_rip;
        }
        return FinishRunLocked(thread, *entry, invocation, interrupted);
    }

    [[nodiscard]] Result<RunResult> Step(ThreadHandle thread, const StepOptions &) override {
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
        const ThreadEntry *entry = Find(thread);
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
        if (!entry->interrupt->stopped_snapshot)
            return BackendError(ErrorCategory::WrongState, "ReadRegisters",
                                "no published snapshot");
        return *entry->interrupt->stopped_snapshot;
    }

    [[nodiscard]] Result<void> WriteRegisters(ThreadHandle thread, const RegisterPatch &patch,
                                              std::uint64_t stop_epoch) override {
        std::lock_guard guard{lock_};
        auto *entry = FindOwnedLocked(thread);
        if (entry == nullptr) {
            return OwnershipError(thread, "WriteRegisters");
        }
        if (entry->running) {
            return BackendError(ErrorCategory::AlreadyRunning, "WriteRegisters",
                                "cannot write registers of an executing thread");
        }
        if (entry->interrupt->last_reason == StopReason::GuestFault ||
            entry->interrupt->last_reason == StopReason::BackendFailure)
            return BackendError(ErrorCategory::WrongState, "WriteRegisters",
                                "unrecoverable fault state");
        if (stop_epoch != entry->stop_epoch) {
            return BackendError(ErrorCategory::StaleEpoch, "WriteRegisters",
                                "the supplied stop epoch is not the thread's current one");
        }

        ApplyPatchToState(patch, entry->native->CurrentFrame->State);
        ApplyXmmPatch(patch, entry->native);
        ++entry->stop_epoch;
        entry->interrupt->stopped_snapshot =
            CaptureSnapshot(thread, *entry, SnapshotKind::SafePoint);
        PublishReceiptLocked(thread, *entry);
        return Result<void>{};
    }

    [[nodiscard]] Result<void> DestroyThread(ThreadHandle thread) override {
        std::lock_guard guard{lock_};
        if (space_.IsQuiescent())
            return BackendError(ErrorCategory::Busy, "DestroyThread", "coordinated drain is active");
        auto *entry = FindOwnedLocked(thread);
        if (entry == nullptr) {
            return OwnershipError(thread, "DestroyThread");
        }
        if (entry->running) {
            return BackendError(ErrorCategory::AlreadyRunning, "DestroyThread",
                                "cannot destroy a thread that is executing");
        }

        context_->DestroyThread(entry->native);
        threads_.erase(thread.id);
        stopped_changed_.notify_all();
        return Result<void>{};
    }

    [[nodiscard]] std::size_t LiveThreadCount() const override {
        std::lock_guard guard{lock_};
        return threads_.size();
    }

    // --- asynchronous control ---------------------------------------------------------------

    [[nodiscard]] std::uint64_t ContextId() const noexcept override { return context_id_; }

    // Backend-internal: install a typed native HLE function and get the guest operation number to
    // place in rax before the syscall. Exposed via the fex backend header, not the backend-free API.
    template <typename Function>
    [[nodiscard]] Result<std::uint64_t> RegisterHle(Function function, std::string name) {
        return syscall_handler_->Registry().Register(function, std::move(name));
    }

    [[nodiscard]] Hle::HleCallRegistry* HleRegistryPointer() { return &syscall_handler_->Registry(); }

#if defined(GUEST_CPU_TEST_HOOKS)
    [[nodiscard]] FexTestRunGate* TestRunGatePointer() {
        if (!test_run_gate_)
            test_run_gate_ = std::make_unique<FexTestRunGateImpl>();
        return test_run_gate_.get();
    }
#endif

    [[nodiscard]] Result<QuiescenceToken> QuiesceContext(std::uint64_t timeout_ns) override {
        std::unique_lock coordinator{coordinator_lock_, std::try_to_lock};
        if (!coordinator.owns_lock())
            return BackendError(ErrorCategory::Busy, "QuiesceContext", "another coordinator is active");
        const auto budget = std::min<std::uint64_t>(timeout_ns ? timeout_ns : 1'000'000'000,
                                                   60'000'000'000);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(budget);
        auto remaining = [&]() -> std::uint64_t {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            return ns > 0 ? ns : 0;
        };
        std::size_t stopped_count{};
        {
            std::lock_guard guard{lock_};
            if (!drain_) {
                auto admission = space_.BeginDrain();
                if (!admission) return admission.GetError();
                drain_.emplace(std::move(admission).Value());
            }
            stopped_count = threads_.size();
            // Issue ALL stop requests before waiting. A retry keeps its original tickets.
            for (auto& [id, entry] : threads_) {
                if (!entry.running) continue;
                const bool requested = std::any_of(drain_tickets_.begin(), drain_tickets_.end(),
                    [&](const auto& t) { return t.thread_id == id; });
                if (requested) continue;
                auto ticket = RequestInterruptLocked({id, entry.generation}, InterruptReason::Pause);
                if (!ticket) return ticket.GetError();
                drain_tickets_.push_back(ticket.Value());
            }
        }
        for (const auto& ticket : drain_tickets_) {
            const auto left = remaining();
            if (!left)
                return BackendError(ErrorCategory::Timeout, "QuiesceContext", "drain deadline expired; retry to recover");
            if (auto receipt = WaitStopped(ticket, left); !receipt) return receipt.GetError();
        }
        auto token = space_.FinishDrain(*drain_, remaining(), stopped_count);
        if (!token) return token.GetError(); // Retain admission on every failure.
        {
            std::lock_guard guard{lock_};
            for (const auto& ticket : drain_tickets_) {
                auto* entry = Find({ticket.thread_id, ticket.thread_generation});
                if (!entry) continue;
                auto& state = *entry->interrupt;
                state.requests.erase(ticket.epoch); // Consume only our own Pause.
                state.pending = 0;
                for (const auto& [epoch, reason] : state.requests)
                    state.pending |= 1u << static_cast<std::uint32_t>(reason);
                // An internal ticket may be newer than a user's Cancel. Retiring it must
                // leave that user's latest ticket resumable, using the same frozen snapshot.
                state.request_epoch = state.requests.empty() ? 0 : state.requests.rbegin()->first;
                state.acked_epoch = state.request_epoch;
                if (state.requests.empty()) state.receipt.reset();
                else PublishReceiptLocked({ticket.thread_id, ticket.thread_generation}, *entry);
            }
            drain_tickets_.clear();
            drain_.reset(); // Reservation was moved into the successful token.
        }
        return token;
    }

    [[nodiscard]] Result<void> ClearCodeCache(const QuiescenceToken& token) override {
        // Route through the memory transaction guard (identity, epoch, callback exclusion and
        // poison recovery). FullFlush covers historical RW/unmapped ranges and the host gate.
        return space_.InvalidateCode(token, {space_.ReservationBase(), space_.ReservationSize()},
                                     InvalidationReason::FullFlush);
    }

    [[nodiscard]] Result<InterruptTicket> RequestInterrupt(ThreadHandle thread,
                                                           InterruptReason reason) override {
        std::lock_guard guard{lock_};
        return RequestInterruptLocked(thread, reason);
    }

  private:
    [[nodiscard]] Result<InterruptTicket> RequestInterruptLocked(ThreadHandle thread,
                                                                InterruptReason reason) {
        if (reason != InterruptReason::Pause && reason != InterruptReason::Cancel &&
            reason != InterruptReason::Shutdown)
            return BackendError(ErrorCategory::InvalidArgument, "RequestInterrupt",
                                "unknown reason");
        auto *entry = Find(thread);
        if (!entry)
            return BackendError(ErrorCategory::InvalidHandle, "RequestInterrupt", "stale handle");
        if (entry->running &&
            ::mprotect(entry->native->InterruptFaultPage, host_page_size_, PROT_NONE) != 0)
            return BackendError(ErrorCategory::BackendFailure, "RequestInterrupt",
                                "arm interrupt page", errno);
        auto &state = *entry->interrupt;
        const auto epoch = ++next_request_epoch_;
        state.requests.emplace(epoch, reason);
        state.request_epoch = epoch;
        state.pending |= 1u << static_cast<std::uint32_t>(reason);
        // Already stopped: a new request is covered by the existing owner-published
        // snapshot. Reuse its stop epoch; never re-read state or fabricate a stop.
        if (!entry->running && state.stopped_snapshot)
            PublishReceiptLocked(thread, *entry);
        return InterruptTicket{context_id_, thread.id, thread.generation, epoch, reason};
    }

  public:
    [[nodiscard]] Result<StopReceipt> WaitStopped(const InterruptTicket &ticket,
                                                  std::uint64_t timeout_ns) override {
        if (!ticket.IsValid() || ticket.context_id != context_id_)
            return BackendError(ErrorCategory::InvalidArgument, "WaitStopped",
                                "wrong context ticket");
        std::unique_lock guard{lock_};
        const ThreadHandle handle{ticket.thread_id, ticket.thread_generation};
        auto valid = [&]() -> bool {
            auto *entry = Find(handle);
            if (!entry)
                return false;
            const auto &requests = entry->interrupt->requests;
            auto request = requests.find(ticket.epoch);
            return request != requests.end() && request->second == ticket.reason;
        };
        if (!valid())
            return BackendError(ErrorCategory::InvalidHandle, "WaitStopped",
                                "stale or unknown ticket");
        if (auto *entry = Find(handle);
            entry->running && entry->owner == std::this_thread::get_id())
            return BackendError(ErrorCategory::WrongThread, "WaitStopped",
                                "owner cannot wait for itself");
        const auto bounded_timeout =
            std::min<std::uint64_t>(timeout_ns ? timeout_ns : 1'000'000'000, 60'000'000'000);
        if (!stopped_changed_.wait_for(guard, std::chrono::nanoseconds(bounded_timeout), [&] {
                if (!valid())
                    return true;
                auto *entry = Find(handle);
                return !entry->running && entry->interrupt->receipt &&
                       entry->interrupt->acked_epoch >= ticket.epoch;
            }))
            return BackendError(ErrorCategory::Timeout, "WaitStopped", "owner has not stopped");
        if (!valid())
            return BackendError(ErrorCategory::StaleEpoch, "WaitStopped",
                                "ticket retired while waiting");
        auto result = *Find(handle)->interrupt->receipt;
        result.request_epoch = ticket.epoch;
        return result;
    }

    [[nodiscard]] Result<void> Resume(ThreadHandle thread,
                                      std::uint64_t acknowledged_epoch) override {
        std::lock_guard guard{lock_};
        // A coordinated transaction pauses owners on its own behalf. The space's quiescence closes
        // new execution; keep Resume from reopening an owner while memory is being changed.
        if (space_.IsQuiescent()) {
            return BackendError(ErrorCategory::Busy, "Resume",
                                "a coordinated quiescence holds this context");
        }
        auto *entry = Find(thread);
        if (!entry)
            return BackendError(ErrorCategory::InvalidHandle, "Resume", "stale handle");
        return ConsumeResumeLocked(*entry, acknowledged_epoch);
    }

    [[nodiscard]] Result<void> InvalidateCode(const QuiescenceToken &token, GuestRange range,
                                              InvalidationReason reason) override {
        return space_.InvalidateCode(token, range, reason);
    }

    // --- CodeInvalidationSink ---------------------------------------------------
    //
    // GuestAddressSpace::PublishCode / InvalidateCode reach the same code below. Before this
    // existed the public memory API only bumped a generation, so a caller could publish new bytes,
    // see both calls succeed, and still execute the previous translation -- reproduced as P1-C in
    // the 2026-09-08 publication review. The address space has already checked the token by the
    // time it calls this, so there is no token argument to re-check here.

    [[nodiscard]] std::string_view Name() const override { return "FEXCore"; }

    [[nodiscard]] Status DiscardTranslations(GuestRange range, InvalidationReason reason) override {
        return DiscardTranslationsImpl(range, reason, "DiscardTranslations");
    }

  private:
    [[nodiscard]] Status DiscardTranslationsImpl(GuestRange range, InvalidationReason reason,
                                                 const char *operation) {
        std::lock_guard guard{lock_};
        for (auto &[id, entry] : threads_) {
            if (entry.running) {
                return BackendError(ErrorCategory::AlreadyRunning, operation,
                                    "a guest thread is still executing");
            }
        }

        auto checked = GuestRange::Checked(range.base, range.size);
        if (!checked) {
            return checked.GetError();
        }

        // FEX requires this exclusive lock for BOTH shared and per-owner cache retirement.
        // InvalidateRange walks guest CodePages keys, not host memory; the entire owned guest
        // reservation also covers mappings removed or temporarily made non-executable.
        std::scoped_lock code_guard{context_->GetCodeInvalidationMutex()};
        auto discard = [&](GuestRange affected) {
            context_->InvalidateCodeBuffersCodeRange(affected.base.value, affected.size);
            for (auto& [id, entry] : threads_)
                if (entry.native)
                    context_->InvalidateThreadCachedCodeRange(entry.native, affected.base.value,
                                                              affected.size);
        };
        discard(range);
        if (reason == InvalidationReason::FullFlush)
            discard({GuestAddress{return_gate_.Address()}, return_gate_.Size()});
        return Result<void>{};
    }

  public:
    [[nodiscard]] std::uint64_t ReturnGateAddress() const noexcept {
        return return_gate_.Address();
    }

  private:
    struct ThreadEntry final {
        FEXCore::Core::InternalThreadState *native{};
        std::uint64_t generation{};
        std::thread::id owner{};
        std::uint64_t guest_tid{};
        std::uint64_t entry_rip{};
        std::uint64_t invocation_counter{};
        std::uint64_t stop_epoch{1};
        bool running{false};
        // Stable address, so the signal handler can hold a pointer to it while the map changes.
        std::shared_ptr<InterruptState> interrupt{std::make_shared<InterruptState>()};

        // Set when this specific guest thread executes a syscall with no HLE path. Attributed per
        // thread (via the frame) so one owner's unregistered-entry fault is never consumed by a
        // different owner's Run (R2-H05).
        std::shared_ptr<std::atomic<bool>> syscall_fault{std::make_shared<std::atomic<bool>>(false)};

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

    [[nodiscard]] Status ConsumeResumeLocked(ThreadEntry &entry, std::uint64_t epoch) {
        auto &state = *entry.interrupt;
        if (entry.running || !state.receipt || epoch == 0 || epoch != state.acked_epoch ||
            epoch != state.request_epoch)
            return BackendError(ErrorCategory::StaleEpoch, "Resume",
                                "resume must name the current stopped request epoch");
        if (state.last_reason == StopReason::GuestFault ||
            state.last_reason == StopReason::BackendFailure)
            return BackendError(ErrorCategory::WrongState, "Resume",
                                "faulted execution cannot be resumed");
        state.pending = 0;
        state.requests.clear();
        state.receipt.reset();
        stopped_changed_.notify_all();
        return Ok();
    }

    void PublishReceiptLocked(ThreadHandle handle, ThreadEntry &entry) {
        auto &state = *entry.interrupt;
        if (!state.stopped_snapshot || state.pending == 0)
            return;
        const auto cancel = (1u << static_cast<unsigned>(InterruptReason::Cancel)) |
                            (1u << static_cast<unsigned>(InterruptReason::Shutdown));
        auto reason = (state.pending & cancel) ? StopReason::Cancelled : StopReason::PauseRequested;
        if (state.last_reason == StopReason::GuestFault ||
            state.last_reason == StopReason::BackendFailure)
            reason = state.last_reason;
        state.acked_epoch = state.request_epoch;
        state.receipt =
            StopReceipt{context_id_, handle.id,     state.request_epoch,    entry.stop_epoch,
                        reason,      state.pending, *state.stopped_snapshot};
        stopped_changed_.notify_all();
    }

    Result<RunResult> FinishRunLocked(ThreadHandle handle, ThreadEntry &entry,
                                      std::uint64_t invocation, bool interrupted) {
        auto result = BuildRunResultLocked(handle, entry, invocation, std::nullopt);
        if (interrupted) {
            const auto cancel = (1u << static_cast<unsigned>(InterruptReason::Cancel)) |
                                (1u << static_cast<unsigned>(InterruptReason::Shutdown));
            auto &value = result.Value();
            value.primary_reason = (entry.interrupt->pending & cancel) ? StopReason::Cancelled
                                                                       : StopReason::PauseRequested;
            value.pending_reasons = BitOf(value.primary_reason);
            if (entry.interrupt->pending & (1u << static_cast<unsigned>(InterruptReason::Pause)))
                value.pending_reasons |= BitOf(StopReason::PauseRequested);
            value.guest_pc = value.snapshot.registers.rip;
            value.snapshot.kind = SnapshotKind::SafePoint;
            value.fault.reset();
        }
        if (entry.interrupt->pending & (1u << static_cast<unsigned>(InterruptReason::Pause)))
            result.Value().pending_reasons |= BitOf(StopReason::PauseRequested);
        if (entry.interrupt->pending & ((1u << static_cast<unsigned>(InterruptReason::Cancel)) |
                                        (1u << static_cast<unsigned>(InterruptReason::Shutdown))))
            result.Value().pending_reasons |= BitOf(StopReason::Cancelled);
        result.Value().primary_reason = SelectPrimaryReason(result.Value().pending_reasons);
        entry.interrupt->last_reason = result.Value().primary_reason;
        entry.interrupt->stopped_snapshot = result.Value().snapshot;
        PublishReceiptLocked(handle, entry);
        return result;
    }

    // Points CPUState at this thread's GDT and installs a flat 64-bit code
    // segment. Without it FEXCore::Frontend::Decoder dereferences a null
    // segment_arrays entry as soon as it compiles the first block.
    static void InitializeSegments(FEXCore::Core::CPUState &state,
                                   std::array<FEXCore::Core::CPUState::gdt_segment, 32> &gdt) {
        state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt.data();
        state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt.data();
        state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;

        auto *code_segment = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
        FEXCore::Core::CPUState::SetGDTBase(code_segment, 0);
        FEXCore::Core::CPUState::SetGDTLimit(code_segment, 0xF'FFFFU);
        state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*code_segment);
        // L=1, D=0 is 64-bit mode. The decoder asserts this matches the
        // context's Is64BitMode, so a mismatch fails loudly rather than
        // decoding 32-bit instructions from 64-bit code.
        code_segment->L = 1;
        code_segment->D = 0;
    }

    [[nodiscard]] ThreadEntry *Find(ThreadHandle handle) {
        auto found = threads_.find(handle.id);
        return found != threads_.end() && found->second.generation == handle.generation
                   ? &found->second
                   : nullptr;
    }

    [[nodiscard]] const ThreadEntry *Find(ThreadHandle handle) const {
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
    [[nodiscard]] ThreadEntry *FindOwnedLocked(ThreadHandle handle) {
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

    void ApplyPatchToState(const RegisterPatch &patch, FEXCore::Core::CPUState &state) const {
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
    void ApplyXmmPatch(const RegisterPatch &patch, FEXCore::Core::InternalThreadState *native) {
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

    [[nodiscard]] CpuSnapshot CaptureSnapshot(ThreadHandle handle, const ThreadEntry &entry,
                                              SnapshotKind kind) const {
        CpuSnapshot snapshot{};
        snapshot.thread_id = handle.id;
        snapshot.thread_generation = handle.generation;
        snapshot.stop_epoch = entry.stop_epoch;
        snapshot.invocation_id = entry.invocation_counter;
        snapshot.kind = kind;
        snapshot.mapping_generation = space_.MappingGeneration();
        snapshot.code_generation = space_.CodeGeneration();

        const auto &state = entry.native->CurrentFrame->State;
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
    [[nodiscard]] Result<RunResult> BuildRunResultLocked(ThreadHandle handle, ThreadEntry &entry,
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

        if (entry.syscall_fault->exchange(false, std::memory_order_acq_rel) ||
            syscall_handler_->TakeUnknownThreadSyscallGlobal()) {
            // This guest thread executed a syscall with no HLE path: a fault attributed to it, not
            // a context-wide boolean another owner could consume.
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

        result.snapshot =
            CaptureSnapshot(handle, entry,
                            result.primary_reason == StopReason::Returned ? SnapshotKind::SafePoint
                                                                          : SnapshotKind::Faulted);
        return result;
    }

    CpuConfig config_{};
    GuestAddressSpace &space_;

    // Identity for tickets and receipts. Monotonic across the process, so a ticket from a
    // destroyed context cannot match its replacement even though thread ids restart.
    const std::uint64_t context_id_{NextContextId()};
    std::uint64_t next_request_epoch_{0};

    mutable std::mutex lock_;
    std::condition_variable stopped_changed_;
    std::unordered_map<std::uint64_t, ThreadEntry> threads_;
    std::mutex coordinator_lock_;
    std::optional<QuiescenceDrain> drain_;
    std::vector<InterruptTicket> drain_tickets_;
    std::uint64_t next_thread_id_{1};

    fextl::unique_ptr<FEXCore::Context::Context> context_;
    std::unique_ptr<FexSignalDelegator> signal_delegator_;
    std::unique_ptr<FexSyscallHandler> syscall_handler_;
#if defined(GUEST_CPU_TEST_HOOKS)
    std::unique_ptr<FexTestRunGate> test_run_gate_;
#endif
    FEXCore::HostFeatures host_features_{};
    ReturnGate return_gate_;
    std::uint64_t host_page_size_{};
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

Result<std::unique_ptr<CpuContext>> CreateFexContext(const CpuConfig &config,
                                                     GuestAddressSpace &space) {
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

void* FexHleRegistryPointer(CpuContext& context) {
    // Only FexCpuContext is constructed in this TU.
    return static_cast<FexCpuContext*>(&context)->HleRegistryPointer();
}

#if defined(GUEST_CPU_TEST_HOOKS)
void* FexTestRunGatePointer(CpuContext& context) {
    return static_cast<FexCpuContext*>(&context)->TestRunGatePointer();
}
#endif

} // namespace Core::GuestCpu::Fex

namespace Core::GuestCpu {

Result<std::unique_ptr<CpuContext>> CreateContext(const CpuConfig &config,
                                                  GuestAddressSpace &space) {
    return Fex::CreateFexContext(config, space);
}

BackendCapabilities QueryBackendCapabilities() { return Fex::QueryFexCapabilities(); }

} // namespace Core::GuestCpu
