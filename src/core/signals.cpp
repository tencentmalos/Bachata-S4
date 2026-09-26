// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/signal_context.h"
#include "common/singleton.h"
#include "core/cpu_patches.h" // Windows static guest red-zone protection
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/signals.h"
#include "emulator.h"

#ifdef _WIN32
#include <algorithm>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>
static constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
#else
#include <csignal>
#include <pthread.h>
#endif
#ifdef ARCH_X86_64
#include <Zydis/Formatter.h>
#endif

namespace Core {

// Intel-syntax text of one instruction, decoded from a copy of its bytes so a crash report never
// touches the faulting address itself. `runtime_address` resolves RIP-relative operands.
[[maybe_unused]] static std::string DisassembleInstruction(const void* bytes, u64 size,
                                                           u64 runtime_address) {
    char buffer[256] = "<unable to decode>";
#ifdef ARCH_X86_64
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status = Common::Decoder::Instance()->decodeInstruction(
        instruction, operands, const_cast<void*>(bytes), size);
    if (ZYAN_SUCCESS(status)) {
        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        runtime_address, ZYAN_NULL);
    }
#endif
    return buffer;
}

#if defined(_WIN32)

// Names the C++ type carried by an MSVC-ABI exception (code 0xE06D7363) and, for
// std::exception subclasses, its what(). Records of other exceptions give an empty string.
static std::string DescribeCxxException(const EXCEPTION_RECORD* record) {
    constexpr DWORD CxxExceptionCode = 0xE06D7363;
    constexpr ULONG_PTR CxxMagic = 0x19930520;
    if (record == nullptr || record->ExceptionCode != CxxExceptionCode ||
        record->NumberParameters < 4 || record->ExceptionInformation[0] != CxxMagic ||
        record->ExceptionInformation[2] == 0 || record->ExceptionInformation[3] == 0) {
        return {};
    }
    // x64 ThrowInfo/CatchableType fields are offsets from the throwing module's base.
    const auto base = static_cast<uintptr_t>(record->ExceptionInformation[3]);
    const auto* object = reinterpret_cast<const u8*>(record->ExceptionInformation[1]);
    const auto* throw_info = reinterpret_cast<const s32*>(record->ExceptionInformation[2]);
    const auto* types = reinterpret_cast<const s32*>(base + throw_info[3]);
    std::string names;
    const std::exception* std_exception = nullptr;
    for (s32 i = 0; i < types[0]; ++i) {
        // CatchableType: properties, type descriptor, {mdisp, pdisp, vdisp}, size, copy fn.
        const auto* catchable = reinterpret_cast<const s32*>(base + types[1 + i]);
        // TypeDescriptor: vftable pointer, spare pointer, decorated name.
        const char* name = reinterpret_cast<const char*>(base + catchable[1] + 16);
        names += names.empty() ? name : std::string(" ") + name;
        if (object != nullptr && catchable[3] == -1 &&
            std::strcmp(name, ".?AVexception@std@@") == 0) {
            std_exception = reinterpret_cast<const std::exception*>(object + catchable[2]);
        }
    }
    if (std_exception != nullptr) {
        names += fmt::format(": {}", std_exception->what());
    }
    return names;
}

// Copies memory that may be unmapped or protected; returns the number of bytes copied. A crash
// report must not fault again inside the vectored handler.
static SIZE_T ReadForReport(u64 address, void* out, SIZE_T size) {
    SIZE_T copied = 0;
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &copied);
    return copied;
}

// "module+offset" for guest code, empty otherwise. Best effort: the module list is read without
// the loader's cooperation, which is acceptable for a report written on the way down.
static std::string DescribeGuestAddress(u64 address) {
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    if (const auto* module = linker->FindByAddress(address)) {
        return fmt::format("{}+{:#x}", module->name, address - module->GetBaseAddress());
    }
    return {};
}

// Registers, the faulting instruction, the innermost stack frame, memory around every register or
// stack value that points at readable data (with its mapping and aliases), the frame-pointer
// chain and the recent mapping changes over those addresses, with guest module offsets. Written
// once, only for an exception nothing else handled, so a guest crash can be taken apart offline
// (tools/ps4-guest-code) instead of being reduced to one address.
static void ReportUnhandledException(const EXCEPTION_POINTERS* pExp) {
    if (pExp == nullptr || pExp->ContextRecord == nullptr || pExp->ExceptionRecord == nullptr) {
        return;
    }
    const CONTEXT& c = *pExp->ContextRecord;
    const EXCEPTION_RECORD& record = *pExp->ExceptionRecord;
    auto* memory = Core::Memory::Instance();
    const auto where = [](u64 address) {
        const std::string guest = DescribeGuestAddress(address);
        return guest.empty() ? fmt::format("{:#x}", address)
                             : fmt::format("{} ({:#x})", guest, address);
    };

    u64 fault_address = 0;
    std::string access;
    if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record.NumberParameters >= 2) {
        const char* kind = record.ExceptionInformation[0] == 1   ? "writing"
                           : record.ExceptionInformation[0] == 8 ? "executing"
                                                                 : "reading";
        fault_address = record.ExceptionInformation[1];
        access = fmt::format(" {} {:#x}", kind, fault_address);
    }
    u8 code[15]{};
    const SIZE_T code_size = ReadForReport(c.Rip, code, sizeof(code));
    LOG_CRITICAL(Debug, "Crash: exception {:#x}{} at {}: {}", record.ExceptionCode, access,
                 where(c.Rip),
                 code_size ? DisassembleInstruction(code, code_size, c.Rip) : "<code unreadable>");
    if (fault_address) {
        LOG_CRITICAL(Debug, "  fault address: {}", memory->DescribeForCrash(fault_address));
    }

    const std::pair<const char*, u64> registers[] = {
        {"rax", c.Rax}, {"rbx", c.Rbx}, {"rcx", c.Rcx}, {"rdx", c.Rdx}, {"rsi", c.Rsi},
        {"rdi", c.Rdi}, {"rbp", c.Rbp}, {"rsp", c.Rsp}, {"r8", c.R8},   {"r9", c.R9},
        {"r10", c.R10}, {"r11", c.R11}, {"r12", c.R12}, {"r13", c.R13}, {"r14", c.R14},
        {"r15", c.R15},
    };
    for (std::size_t i = 0; i < std::size(registers); i += 4) {
        LOG_CRITICAL(Debug, "  {:>3}={:016x} {:>3}={:016x} {:>3}={:016x} {:>3}={:016x}",
                     registers[i].first, registers[i].second, registers[i + 1].first,
                     registers[i + 1].second, registers[i + 2].first, registers[i + 2].second,
                     registers[i + 3].first, registers[i + 3].second);
    }
    LOG_CRITICAL(Debug, "  rip={:016x} eflags={:08x}", c.Rip, c.EFlags);

    // The innermost frame holds the callee-saved registers of its caller, which is where the
    // interesting values of a crash inside a helper (a panic, an assert) end up.
    constexpr u64 MaxFrameBytes = 0x400;
    const u64 frame_bytes =
        c.Rbp > c.Rsp && c.Rbp - c.Rsp < MaxFrameBytes ? c.Rbp - c.Rsp + 0x10 : 0x80;
    std::vector<u64> stack(frame_bytes / 8);
    stack.resize(ReadForReport(c.Rsp, stack.data(), stack.size() * 8) / 8);
    for (std::size_t i = 0; i < stack.size(); i += 4) {
        std::string line;
        for (std::size_t j = i; j < std::min(i + 4, stack.size()); ++j) {
            line += fmt::format(" {:016x}", stack[j]);
        }
        LOG_CRITICAL(Debug, "  [rsp+{:#05x}]{}", i * 8, line);
    }

    // Memory around each distinct value that points at readable data outside the stack and the
    // loaded modules: 0x10 bytes before and 0x20 after, enough to see a heap block header.
    constexpr u64 Before = 0x10;
    constexpr u64 After = 0x20;
    constexpr std::size_t MaxDumps = 24;
    std::vector<std::pair<std::string, u64>> candidates;
    for (const auto& [name, value] : registers) {
        candidates.emplace_back(name, value);
    }
    for (std::size_t i = 0; i < stack.size(); ++i) {
        candidates.emplace_back(fmt::format("rsp+{:#x}", i * 8), stack[i]);
    }
    std::vector<u64> dumped;
    for (const auto& [name, value] : candidates) {
        if (dumped.size() >= MaxDumps) {
            break;
        }
        const bool on_stack = value >= c.Rsp - 0x10000 && value < c.Rsp + 0x100000;
        if (value < 0x10000 || on_stack || std::ranges::find(dumped, value) != dumped.end() ||
            !DescribeGuestAddress(value).empty()) {
            continue;
        }
        u8 bytes[Before + After];
        if (ReadForReport(value - Before, bytes, sizeof(bytes)) != sizeof(bytes)) {
            continue;
        }
        dumped.push_back(value);
        std::string hex;
        for (std::size_t i = 0; i < sizeof(bytes); ++i) {
            hex += fmt::format("{}{:02x}", i == Before ? '|' : ' ', bytes[i]);
        }
        LOG_CRITICAL(Debug, "  [{}] {:#x}:{}  {}", name, value, hex,
                     memory->DescribeForCrash(value));
    }

    // Guest code keeps frame pointers (push rbp; mov rbp, rsp), so [rbp] is the caller's rbp and
    // [rbp+8] the return address. Stops at anything unreadable or not moving up the stack.
    u64 frame = c.Rbp;
    for (int depth = 0; depth < 32 && frame >= c.Rsp; ++depth) {
        u64 link[2]{};
        if (ReadForReport(frame, link, sizeof(link)) != sizeof(link) || link[1] == 0) {
            break;
        }
        LOG_CRITICAL(Debug, "  frame #{:<2} {}", depth, where(link[1]));
        if (link[0] <= frame) {
            break;
        }
        frame = link[0];
    }

    if (fault_address) {
        dumped.push_back(fault_address);
    }
    for (const auto& line : memory->DescribeMappingHistoryForCrash(dumped)) {
        LOG_CRITICAL(Debug, "  mapping {}", line);
    }
}

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    using namespace Libraries::Kernel;
    const auto* signals = Signals::Instance();
    // Windows static guest red-zone protection
    const bool use_static_windows_guest_red_zone_protection =
        WindowsGuestRedZoneProtection::IsStaticPatchingEnabled();
    DWORD code = 0;
    PVOID address = nullptr;

    if (pExp != nullptr && pExp->ExceptionRecord != nullptr) {
        code = pExp->ExceptionRecord->ExceptionCode;
        address = pExp->ExceptionRecord->ExceptionAddress;
    }

    Ucontext guest_context{pExp->ContextRecord};
    Siginfo guest_info{
        ._si_signo = 0,
        ._si_errno = 0,
        ._si_code = POSIX_SI_NOINFO,
        ._si_addr = (void*)guest_context.uc_mcontext.mc_rip,
    };

    bool handled = false;
    bool static_protection_exception = false; // Windows static guest red-zone protection
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        guest_info._si_signo = POSIX_SIGSEGV;
        guest_info._si_code = POSIX_SEGV_MAPERR;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_ILLOPC;
        static_protection_exception = true; // Windows static guest red-zone protection
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case EXCEPTION_PRIV_INSTRUCTION: // Windows static guest red-zone protection
        if (use_static_windows_guest_red_zone_protection) {
            static_protection_exception = true;
            handled = signals->DispatchIllegalInstruction(pExp);
        }
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        guest_info._si_signo = POSIX_SIGBUS;
        guest_info._si_code = POSIX_BUS_ADRALN;
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTDIV;
        break;
    case EXCEPTION_INT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_INTOVF;
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTDIV;
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTINV;
        break;
    case EXCEPTION_FLT_OVERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTOVF;
        break;
    case EXCEPTION_FLT_UNDERFLOW:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTUND;
        break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTSUB; // i am not sure about this one
        break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        guest_info._si_signo = POSIX_SIGFPE;
        guest_info._si_code = POSIX_FPE_FLTRES;
        break;
    case EXCEPTION_FLT_STACK_CHECK:
        guest_info._si_signo = POSIX_SIGILL;
        guest_info._si_code = POSIX_ILL_BADSTK; // i am not sure about this one either
        break;
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_SINGLE_STEP:
        guest_info._si_signo = POSIX_SIGTRAP;
        guest_info._si_code = POSIX_TRAP_BRKPT;
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        // Used by OutputDebugString functions.
        return EXCEPTION_CONTINUE_EXECUTION;
    case MS_VC_EXCEPTION:
        LOG_DEBUG(Debug, "Pass MS_VC_EXCEPTION at {} to handler", address);
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        break;
    }

    if (handled) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (guest_info._si_signo != 0) {
        if (g_curthread &&
            g_curthread->DispatchSignal(guest_info._si_signo, &guest_info, &guest_context)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    // A vectored handler sees every exception first. Ones raised in software (C++ throw,
    // RPC and WIL errors inside system DLLs such as the orientation-sensor API) belong to
    // their raiser's own handlers; they are not guest faults, so leave them to SEH.
    if (pExp != nullptr && pExp->ExceptionRecord != nullptr &&
        (pExp->ExceptionRecord->ExceptionFlags & EXCEPTION_SOFTWARE_ORIGINATE)) {
        LOG_DEBUG(Debug, "Passing software exception {:#x} at {} to its handlers {}", code,
                  address, DescribeCxxException(pExp->ExceptionRecord));
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Windows static guest red-zone protection
    const bool report_unhandled = use_static_windows_guest_red_zone_protection
                                      ? static_protection_exception
                                      : code != EXCEPTION_BREAKPOINT;
    if (report_unhandled) { // Windows static guest red-zone protection
        LOG_CRITICAL(Debug, "Unhandled Exception code {:#x} at {} {}", code, address,
                     DescribeCxxException(pExp ? pExp->ExceptionRecord : nullptr));
        ReportUnhandledException(pExp);
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static LPTOP_LEVEL_EXCEPTION_FILTER previous_unhandled_filter = nullptr;

// Reached only when nothing handled the exception. SignalHandler lets software exceptions
// pass untouched, so name them here before the process ends (hardware faults were already
// reported there).
static LONG WINAPI UnhandledExceptionReport(EXCEPTION_POINTERS* pExp) noexcept {
    if (pExp != nullptr && pExp->ExceptionRecord != nullptr &&
        (pExp->ExceptionRecord->ExceptionFlags & EXCEPTION_SOFTWARE_ORIGINATE)) {
        LOG_CRITICAL(Debug, "Unhandled software exception {:#x} at {} {}",
                     pExp->ExceptionRecord->ExceptionCode,
                     pExp->ExceptionRecord->ExceptionAddress,
                     DescribeCxxException(pExp->ExceptionRecord));
    }
    return previous_unhandled_filter ? previous_unhandled_filter(pExp)
                                     : EXCEPTION_CONTINUE_SEARCH;
}

#else

static s32 NativeSiCodeToGuest(s32 sig, s32 code) {
    using namespace Libraries::Kernel;
    switch (sig) {
    case SIGUSR1:
        return POSIX_SI_LWP;
    case SIGSEGV:
        switch (code) {
        case SEGV_MAPERR:
            return POSIX_SEGV_MAPERR;
        case SEGV_ACCERR:
            return POSIX_SEGV_ACCERR;
        }
    case SIGBUS:
        switch (code) {
        case BUS_ADRALN:
            return POSIX_BUS_ADRALN;
        case BUS_ADRERR:
            return POSIX_BUS_ADRERR;
        case BUS_OBJERR:
            return POSIX_BUS_OBJERR;
        }
    case SIGILL:
        switch (code) {
        case ILL_ILLOPC:
            return POSIX_ILL_ILLOPC;
        case ILL_ILLOPN:
            return POSIX_ILL_ILLOPN;
        case ILL_ILLADR:
            return POSIX_ILL_ILLADR;
        case ILL_ILLTRP:
            return POSIX_ILL_ILLTRP;
        case ILL_PRVOPC:
            return POSIX_ILL_PRVOPC;
        case ILL_PRVREG:
            return POSIX_ILL_PRVREG;
        case ILL_COPROC:
            return POSIX_ILL_COPROC;
        case ILL_BADSTK:
            return POSIX_ILL_BADSTK;
        }
    case SIGFPE:
        switch (code) {
        case FPE_INTOVF:
            return POSIX_FPE_INTOVF;
        case FPE_INTDIV:
            return POSIX_FPE_INTDIV;
        case FPE_FLTDIV:
            return POSIX_FPE_FLTDIV;
        case FPE_FLTOVF:
            return POSIX_FPE_FLTOVF;
        case FPE_FLTUND:
            return POSIX_FPE_FLTUND;
        case FPE_FLTRES:
            return POSIX_FPE_FLTRES;
        case FPE_FLTINV:
            return POSIX_FPE_FLTINV;
        case FPE_FLTSUB:
            return POSIX_FPE_FLTSUB;
        }
    case SIGTRAP:
        switch (code) {
        case TRAP_BRKPT:
            return POSIX_TRAP_BRKPT;
        case TRAP_TRACE:
            return POSIX_TRAP_TRACE;
#ifdef __FreeBSD__
        case TRAP_DTRACE:
            return POSIX_TRAP_DTRACE;
#endif
        }

    default:
        return POSIX_SI_NOINFO;
    }
}

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    using namespace Libraries::Kernel;
    auto* thread = g_curthread;
    const auto* signals = Signals::Instance();

    auto* code_address = Common::GetRip(raw_context);

    Ucontext context{info, reinterpret_cast<ucontext_t*>(raw_context)};
    Siginfo guest_info{};
    if (info) {
        guest_info = *reinterpret_cast<Siginfo*>(info);
        guest_info._si_signo = sig == SIGUSR1 ? 0 : NativeToOrbisSignal(info->si_signo);
        guest_info._si_errno = NativeToPosixErrno(info->si_errno);
        guest_info._si_code = NativeSiCodeToGuest(sig, info->si_code);
        guest_info._si_addr = context.HasGuestContext()
                                  ? reinterpret_cast<void*>(context.uc_mcontext.mc_rip)
                                  : nullptr;
    }
    Siginfo* info_p = info ? &guest_info : nullptr;
    Ucontext* context_p = raw_context ? &context : nullptr;

    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        const bool is_write = Common::IsWriteError(raw_context);
        if (!signals->DispatchAccessViolation(raw_context, info->si_addr)) {
            if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
                return;
            }
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address), is_write ? "Write to" : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (signals->DispatchIllegalInstruction(raw_context)) {
            return;
        }
    case SIGFPE:
    case SIGTRAP:
    case SIGSYS: {
        if (thread && thread->DispatchSignal(NativeToOrbisSignal(sig), info_p, context_p)) {
            return;
        }

        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
    case SIGSLEEP: {
        // Sleep thread until signal is received again
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, SIGSLEEP);
        sigwait(&sigset, &sig);
        break;
    }
    case SIGUSR1:
        if (thread) {
            thread->DispatchPendingSignals(info_p, context_p);
        }
        break;
    default:
        UNREACHABLE_MSG("Unhandled signal {} at code address {}", sig, fmt::ptr(code_address));
    }
}

#endif

SignalDispatch::SignalDispatch(Delivery delivery) {
    if (delivery == Delivery::External)
        return;
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
    previous_unhandled_filter = SetUnhandledExceptionFilter(UnhandledExceptionReport);
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    ASSERT_MSG(
        sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
            sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
            sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
            sigaction(SIGUSR1, &action, nullptr) == 0 && sigaction(SIGSLEEP, &action, nullptr) == 0,
        "Failed to register signal handlers.");
#endif
    owns_handlers = true;
}

void SignalDispatch::RemoveHandlers() {
    if (!owns_handlers)
        return;
    // asserting here would get into an infinite loop until too
    // many nested exceptions makes the OS kill the process
#if defined(_WIN32)
    SetUnhandledExceptionFilter(previous_unhandled_filter);
    if (!(RemoveVectoredExceptionHandler(handle))) {
        LOG_CRITICAL(Core, "Failed to remove exception handler.");
        std::quick_exit(1);
    }
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

    if (!(sigaction(SIGSEGV, &action, nullptr) == 0 && sigaction(SIGBUS, &action, nullptr) == 0 &&
          sigaction(SIGILL, &action, nullptr) == 0 && sigaction(SIGFPE, &action, nullptr) == 0 &&
          sigaction(SIGTRAP, &action, nullptr) == 0 && sigaction(SIGSYS, &action, nullptr) == 0 &&
          sigaction(SIGUSR1, &action, nullptr) == 0 &&
          sigaction(SIGSLEEP, &action, nullptr) == 0)) {
        LOG_CRITICAL(Core, "Failed to remove signal handlers.");
        std::quick_exit(1);
    }
#endif
    owns_handlers = false;
}

SignalDispatch::~SignalDispatch() {
    RemoveHandlers();
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
