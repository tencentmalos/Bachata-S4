// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/guest_cpu/fex/fex_context.h"

#include <array>
#include <atomic>
#include <cerrno>
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
class FexCpuContext final : public CpuContext {
public:
    explicit FexCpuContext(const CpuConfig& config, GuestAddressSpace& space)
        : config_(config), space_(space) {}

    ~FexCpuContext() override {
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
        FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
        FEXCore::Config::ReloadMetaLayer();
        config_initialized_ = true;

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
        // Claim the thread under the lock and only then leave it: resolving the handle and marking
        // it running have to be one step, or two callers can both pass the "not running" check.
        // The earlier version returned a raw entry pointer from a helper that released the lock on
        // return, then dereferenced it -- the 2026-09-08 review flagged that window as R5.
        FEXCore::Core::InternalThreadState* native = nullptr;
        std::uint64_t invocation = 0;
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
            entry->running = true;
            invocation = ++entry->invocation_counter;
            native = entry->native;
        }

        // Executed with the lock released: a guest block runs for an unbounded time, and holding
        // the context lock across it would block every other thread's handle operations. The
        // running flag set above is what keeps this thread from being run or destroyed meanwhile.
        context_->ExecuteThread(native);

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

    [[nodiscard]] Result<void> InvalidateCode(const QuiescenceToken& token, GuestRange range,
                                              InvalidationReason reason) override {
        if (!token.IsValid()) {
            // Without the token there is no proof that no thread is executing
            // inside the code about to be discarded.
            return BackendError(ErrorCategory::WrongState, "InvalidateCode",
                                "a valid quiescence token is required to discard translated code");
        }

        std::lock_guard guard{lock_};
        for (auto& [id, entry] : threads_) {
            if (entry.running) {
                return BackendError(ErrorCategory::AlreadyRunning, "InvalidateCode",
                                    "a guest thread is still executing");
            }
        }

        // FEXCore's per-range invalidation is driven through the syscall
        // handler's callback into the JIT; the entry point available to an
        // embedder is ClearCodeCache, which drops the thread's translated code
        // wholesale. That is coarser than the requested range but never stale:
        // over-invalidating costs recompilation, under-invalidating would let a
        // thread execute code that no longer exists.
        for (auto& [id, entry] : threads_) {
            if (entry.native != nullptr) {
                context_->ClearCodeCache(entry.native);
            }
        }
        return Result<void>{};
    }

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
