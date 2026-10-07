// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// libSceFiber for the Android host: fibers are switched by changing the registers an HLE call
// returns with.
//
// The guest never runs on a host stack: an HLE call arrives as the guest's register file, and the
// registers the call returns with are the ones the guest continues with. The veneer it called
// through ends in `ret`. Switching fibers therefore needs no assembly: keep the callee-saved
// registers, the stack pointer and MXCSR of the context being left, load those of the context
// being entered, and return; the `ret` pops the return address of whichever context was switched
// in. A fiber that never ran is entered by putting its entry address, and the address of a trap
// for an entry that returns, on its stack, so that the `ret` lands on the entry as a call would.
//
// Approach: AstroQuest v0.18 (9ff3e43), shadps4-arm64-main/src/core/libraries/fiber/fiber_fex.cpp
// (GPL-2.0-or-later). Semantics and error codes follow the desktop implementation
// (src/core/libraries/fiber/fiber.cpp). Differences from both:
//   - guest memory is reached through GuestFiberMemory (checked, pinned access in production), not
//     dereferenced; the fiber's state word is changed with a compare-exchange on the guest word;
//   - what the desktop keeps in the guest TCB (the thread's context while fibers run on it) and in
//     the fiber (a pointer to its saved context) is kept here, per session, keyed by guest thread
//     and by fiber address;
//   - the x87 control word is not part of the HLE register file, so a fiber started with
//     SetFpuRegs gets MXCSR 0x9fc0 but keeps the thread's FCW;
//   - a fiber whose entry returns, or whose stack signature was overwritten, ends the session with
//     a structured guest fault (the desktop asserts).
//
// The fiber object is opaque to the title (SceFiber is 256 bytes of storage); the layout below is
// the desktop one, so traces and the GPU replay read the same fields on both hosts.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "common/types.h"
#include "core/guest_cpu/api/registers.h"
#include "core/libraries/fiber/fiber_error.h"

namespace Core::HostRuntime {

// Guest memory as the fiber library needs it. Production backs it with GuestAddressSpace
// (checked, pinned), tests with a plain buffer.
class GuestFiberMemory {
public:
    virtual ~GuestFiberMemory() = default;
    virtual bool Read(u64 address, std::span<std::byte> into) = 0;
    virtual bool Write(u64 address, std::span<const std::byte> from) = 0;
    // Replaces the 4-byte-aligned u32 at `address` with `desired` if it holds `expected`, as one
    // atomic operation. nullopt when the word cannot be accessed.
    virtual std::optional<bool> CompareExchange(u64 address, u32 expected, u32 desired) = 0;
};

class GuestFiber {
public:
    static constexpr std::string_view Suffix = "#libSceFiber#1#libSceFiber#Function";
    static constexpr std::array<std::string_view, 15> Nids{
        "hVYD7Ou2pCQ", // sceFiberInitialize
        "7+OJIpko9RY", // _sceFiberInitializeWithInternalOptionImpl
        "asjUJJ+aa8s", // sceFiberOptParamInitialize
        "JeNX5F-NzQU", // sceFiberFinalize
        "a0LLrZWac0M", // sceFiberRun
        "PFT2S-tJ7Uk", // sceFiberSwitch
        "p+zLIOg27zU", // sceFiberGetSelf
        "B0ZX2hx9DMw", // sceFiberReturnToThread
        "avfGJ94g36Q", // _sceFiberAttachContextAndRun
        "ZqhZFuzKT6U", // _sceFiberAttachContextAndSwitch
        "uq2Y5BFz0PE", // sceFiberGetInfo
        "Lcqty+QNWFc", // sceFiberStartContextSizeCheck
        "Kj4nXMpnM8Y", // sceFiberStopContextSizeCheck
        "JzyT91ucGDc", // sceFiberRename
        "0dy4JtMUcMQ", // sceFiberGetThreadFramePointerAddress
    };
    static bool IsNid(std::string_view nid) {
        for (const auto n : Nids)
            if (n == nid)
                return true;
        return false;
    }

    // The guest thread a call comes from: stable for the life of that thread.
    struct ThreadKey {
        u64 id{};
        u64 generation{};
        friend bool operator<(const ThreadKey& a, const ThreadKey& b) {
            return a.id != b.id ? a.id < b.id : a.generation < b.generation;
        }
    };

    // `entry_return` is the guest address a fiber's entry function returns to: a trap.
    // `note_stack` is told about every fiber stack before it is first entered.
    GuestFiber(GuestFiberMemory& memory, u64 entry_return,
               void (*note_stack)(u64 base, u64 size) = nullptr)
        : memory(memory), entry_return(entry_return), note_stack(note_stack) {}

    // Runs one libSceFiber function on the registers of the call. The result is left in RAX;
    // calls that switch context also change the stack pointer and the callee-saved registers.
    // Returns a message when the guest has to be stopped instead (a corrupted fiber stack, a
    // pointer the guest gave that cannot be written).
    std::optional<std::string> Call(std::string_view nid, GuestCpu::RegisterFile& regs,
                                    ThreadKey thread) {
        Frame f{regs};
        if (nid == "hVYD7Ou2pCQ" || nid == "7+OJIpko9RY") {
            const bool internal = nid == "7+OJIpko9RY";
            std::array<u64, 3> stack{};
            if (!ReadStack(regs, std::span{stack.data(), internal ? 3u : 2u}))
                return "sceFiberInitialize: stack arguments unreadable";
            const u32 flags = internal ? static_cast<u32>(stack[1]) : 0;
            const u32 build = static_cast<u32>(internal ? stack[2] : stack[1]);
            f.Result(Initialize(f.Arg(0), f.Arg(1), f.Arg(2), f.Arg(3), f.Arg(4), f.Arg(5),
                                stack[0], flags, build));
        } else if (nid == "asjUJJ+aa8s") {
            f.Result(OptParamInitialize(f.Arg(0)));
        } else if (nid == "JeNX5F-NzQU") {
            f.Result(Finalize(f.Arg(0)));
        } else if (nid == "a0LLrZWac0M") {
            return Run(f, thread, f.Arg(0), 0, 0, f.Arg(1), f.Arg(2));
        } else if (nid == "avfGJ94g36Q") {
            return Run(f, thread, f.Arg(0), f.Arg(1), f.Arg(2), f.Arg(3), f.Arg(4));
        } else if (nid == "PFT2S-tJ7Uk") {
            return Switch(f, thread, f.Arg(0), 0, 0, f.Arg(1), f.Arg(2));
        } else if (nid == "ZqhZFuzKT6U") {
            return Switch(f, thread, f.Arg(0), f.Arg(1), f.Arg(2), f.Arg(3), f.Arg(4));
        } else if (nid == "B0ZX2hx9DMw") {
            return ReturnToThread(f, thread, f.Arg(0), f.Arg(1));
        } else if (nid == "p+zLIOg27zU") {
            f.Result(GetSelf(thread, f.Arg(0)));
        } else if (nid == "uq2Y5BFz0PE") {
            f.Result(GetInfo(f.Arg(0), f.Arg(1)));
        } else if (nid == "Lcqty+QNWFc") {
            u32 expected = 0;
            f.Result(static_cast<u32>(f.Arg(0)) != 0 ? ORBIS_FIBER_ERROR_INVALID
                     : size_check.compare_exchange_strong(expected, 1) ? ORBIS_OK
                                                                     : ORBIS_FIBER_ERROR_STATE);
        } else if (nid == "Kj4nXMpnM8Y") {
            u32 expected = 1;
            f.Result(size_check.compare_exchange_strong(expected, 0) ? ORBIS_OK
                                                                     : ORBIS_FIBER_ERROR_STATE);
        } else if (nid == "JzyT91ucGDc") {
            f.Result(Rename(f.Arg(0), f.Arg(1)));
        } else if (nid == "0dy4JtMUcMQ") {
            f.Result(GetThreadFramePointerAddress(thread, f.Arg(0)));
        } else {
            return "libSceFiber: unknown function " + std::string(nid);
        }
        return std::nullopt;
    }

    // The guest object, as both hosts lay it out.
    struct Record {
        u32 magic_start;
        u32 state;
        u64 entry;
        u64 arg_on_initialize;
        u64 addr_context;
        u64 size_context;
        char name[32];
        u64 context;
        u32 flags;
        u32 pad0;
        u64 context_start;
        u64 context_end;
        u32 magic_end;
        u32 pad1;
    };
    struct Info {
        u64 size;
        u64 entry;
        u64 arg_on_initialize;
        u64 addr_context;
        u64 size_context;
        char name[32];
        u64 size_context_margin;
        u8 pad[48];
    };

    static constexpr u32 kSignature0 = 0xdef1649c;
    static constexpr u32 kSignature1 = 0xb37592a0;
    static constexpr u32 kOptSignature = 0xbb40e64d;
    static constexpr u64 kStackSignature = 0x7149f2ca7149f2ca;
    static constexpr u64 kStackSizeCheck = 0xdeadbeefdeadbeef;
    static constexpr u64 kContextMinimum = 512;
    static constexpr u32 kFw350 = 0x3500000;
    static constexpr u32 kRun = 1, kIdle = 2, kTerminated = 3;
    static constexpr u32 kFlagContextSizeCheck = 0x10, kFlagSetFpuRegs = 0x100;
    static constexpr u32 kFiberMxcsr = 0x9fc0;

private:
    // The registers of the context being left or entered that a call has to keep.
    struct Saved {
        u64 rbx{}, rsp{}, rbp{}, r12{}, r13{}, r14{}, r15{};
        u32 mxcsr{};
    };
    // A fiber that gave the thread up inside sceFiberSwitch or sceFiberReturnToThread.
    struct Suspended {
        Saved registers;
        u64 arg_on_run{}; // where that call reports the argument it is resumed with
    };
    // A thread inside sceFiberRun: what the desktop keeps behind tcb_fiber.
    struct ThreadState {
        Saved registers; // the sceFiberRun call, continued by sceFiberReturnToThread
        u64 arg_on_return{};
        u64 current{};
    };

    struct Frame {
        GuestCpu::RegisterFile& regs;
        u64 Arg(size_t i) const {
            static constexpr std::array<GuestCpu::Gpr, 6> order{
                GuestCpu::Gpr::Rdi, GuestCpu::Gpr::Rsi, GuestCpu::Gpr::Rdx,
                GuestCpu::Gpr::Rcx, GuestCpu::Gpr::R8,  GuestCpu::Gpr::R9};
            return regs.Get(order[i]);
        }
        void Result(s32 value) {
            regs.Set(GuestCpu::Gpr::Rax, static_cast<u32>(value));
        }
    };

    static Saved Save(const GuestCpu::RegisterFile& r) {
        using GuestCpu::Gpr;
        return {r.Get(Gpr::Rbx), r.Get(Gpr::Rsp), r.Get(Gpr::Rbp), r.Get(Gpr::R12),
                r.Get(Gpr::R13), r.Get(Gpr::R14), r.Get(Gpr::R15), r.mxcsr};
    }
    static void Load(GuestCpu::RegisterFile& r, const Saved& s) {
        using GuestCpu::Gpr;
        r.Set(Gpr::Rbx, s.rbx);
        r.Set(Gpr::Rsp, s.rsp);
        r.Set(Gpr::Rbp, s.rbp);
        r.Set(Gpr::R12, s.r12);
        r.Set(Gpr::R13, s.r13);
        r.Set(Gpr::R14, s.r14);
        r.Set(Gpr::R15, s.r15);
        r.mxcsr = s.mxcsr;
    }

    template <class T>
    bool Get(u64 address, T& value) {
        return memory.Read(address, std::as_writable_bytes(std::span{&value, 1}));
    }
    template <class T>
    bool Put(u64 address, const T& value) {
        return memory.Write(address, std::as_bytes(std::span{&value, 1}));
    }
    template <class T>
    bool PutField(u64 fiber, size_t offset, const T& value) {
        return Put(fiber + offset, value);
    }
    bool ReadStack(const GuestCpu::RegisterFile& regs, std::span<u64> out) {
        // Stack arguments begin above the return address the call pushed.
        const u64 rsp = regs.Get(GuestCpu::Gpr::Rsp);
        return memory.Read(rsp + 8, std::as_writable_bytes(out));
    }

    // Reads a fiber the guest passed. 0 when it is valid, otherwise the error to return.
    s32 LoadFiber(u64 fiber, u64 addr_context, Record& record) {
        if (!fiber)
            return ORBIS_FIBER_ERROR_NULL;
        if ((fiber & 7) || (addr_context & 15))
            return ORBIS_FIBER_ERROR_ALIGNMENT;
        if (!Get(fiber, record) || record.magic_start != kSignature0 ||
            record.magic_end != kSignature1)
            return ORBIS_FIBER_ERROR_INVALID;
        return ORBIS_OK;
    }

    bool CompareExchangeState(u64 fiber, u32 expected, u32 desired) {
        return memory.CompareExchange(fiber + offsetof(Record, state), expected, desired)
            .value_or(false);
    }
    // Hands a fiber the thread has left back: Run -> Idle as one atomic operation, so a thread
    // that sees it Idle and takes it also sees the registers saved before.
    void Release(u64 fiber) {
        CompareExchangeState(fiber, kRun, kIdle);
    }

    // Writes the stack signature, and the fill a context size check measures against.
    bool PrepareStack(u64 base, u64 size, bool size_check) {
        if (!Put(base, kStackSignature))
            return false;
        if (!size_check)
            return true;
        std::array<u64, 512> fill;
        fill.fill(kStackSizeCheck);
        for (u64 at = base + 8; at < base + size;) {
            const u64 bytes = std::min<u64>(sizeof(fill), base + size - at);
            if (!memory.Write(at, std::as_bytes(std::span{fill}).first(bytes)))
                return false;
            at += bytes;
        }
        return true;
    }

    s32 AttachContext(u64 fiber, Record& record, u64 addr_context, u64 size_context) {
        if (size_context && size_context < kContextMinimum)
            return ORBIS_FIBER_ERROR_RANGE;
        if (size_context & 15)
            return ORBIS_FIBER_ERROR_INVALID;
        if (!addr_context || !size_context || record.addr_context)
            return ORBIS_FIBER_ERROR_INVALID;
        if (!PrepareStack(addr_context, size_context, record.flags & kFlagContextSizeCheck))
            return ORBIS_FIBER_ERROR_INVALID;
        record.addr_context = record.context_start = addr_context;
        record.size_context = size_context;
        record.context_end = addr_context + size_context;
        PutField(fiber, offsetof(Record, addr_context), record.addr_context);
        PutField(fiber, offsetof(Record, size_context), record.size_context);
        PutField(fiber, offsetof(Record, context_start), record.context_start);
        PutField(fiber, offsetof(Record, context_end), record.context_end);
        return ORBIS_OK;
    }

    std::optional<std::string> CheckStack(u64 fiber) {
        Record record{};
        if (!Get(fiber, record))
            return "libSceFiber: running fiber is no longer readable";
        u64 signature{};
        if (record.addr_context &&
            (!Get(record.addr_context, signature) || signature != kStackSignature))
            return "libSceFiber: stack overflow in fiber '" +
                   std::string(record.name, strnlen(record.name, sizeof(record.name))) + "'";
        return std::nullopt;
    }

    void Suspend(const GuestCpu::RegisterFile& regs, u64 fiber, u64 arg_on_run) {
        std::scoped_lock lock{mutex};
        suspended[fiber] = Suspended{Save(regs), arg_on_run};
    }
    std::optional<Suspended> TakeSuspended(u64 fiber) {
        std::scoped_lock lock{mutex};
        const auto it = suspended.find(fiber);
        if (it == suspended.end())
            return std::nullopt;
        const Suspended resumed = it->second;
        suspended.erase(it);
        return resumed;
    }
    void Forget(u64 fiber) {
        std::scoped_lock lock{mutex};
        suspended.erase(fiber);
    }
    // The state of a thread inside sceFiberRun. Only that thread adds, changes or removes its
    // entry; the lock covers the map, not the entry, and no guest memory is touched under it
    // (an access may wait for a code publication, which waits for other threads' HLE calls).
    ThreadState* FindThread(ThreadKey key) {
        std::scoped_lock lock{mutex};
        const auto it = threads.find(key);
        return it == threads.end() ? nullptr : &it->second;
    }

    // Gives the thread to `fiber`: it continues where it suspended itself, or starts its entry
    // function. `previous` is the fiber the thread ran until now; its registers are saved and
    // nothing here touches its stack any more, so another thread may pick it up from now on.
    std::optional<std::string> Enter(Frame& f, ThreadState& thread, u64 fiber,
                                     const Record& record, u64 arg_on_run_to, u64 previous) {
        thread.current = fiber;
        if (previous)
            Release(previous);
        if (const auto resumed = TakeSuspended(fiber)) {
            if (resumed->arg_on_run && !Put(resumed->arg_on_run, arg_on_run_to))
                return "libSceFiber: resumed fiber's argument pointer is not writable";
            Load(f.regs, resumed->registers);
            f.Result(ORBIS_OK);
            return std::nullopt;
        }
        // A fiber without a context of its own borrows the stack of the thread that runs it.
        const u64 top = (record.addr_context ? record.addr_context + record.size_context
                                             : thread.registers.rsp) &
                        ~u64{15};
        // The veneer's `ret` pops the entry point, which then finds the stack as a call left it.
        const std::array<u64, 2> frame{record.entry, entry_return};
        if (!memory.Write(top - 16, std::as_bytes(std::span{frame})))
            return "libSceFiber: fiber stack is not writable";
        using GuestCpu::Gpr;
        f.regs.Set(Gpr::Rsp, top - 16);
        f.regs.Set(Gpr::Rbp, 0);
        f.regs.Set(Gpr::Rdi, record.arg_on_initialize);
        f.regs.Set(Gpr::Rsi, arg_on_run_to);
        if (record.flags & kFlagSetFpuRegs)
            f.regs.mxcsr = kFiberMxcsr;
        f.Result(ORBIS_OK);
        return std::nullopt;
    }

    s32 Initialize(u64 fiber, u64 name, u64 entry, u64 arg_on_initialize, u64 addr_context,
                   u64 size_context, u64 opt_param, u32 flags, u32 build) {
        if (!fiber || !name || !entry)
            return ORBIS_FIBER_ERROR_NULL;
        if ((fiber & 7) || (addr_context & 15) || (opt_param & 7))
            return ORBIS_FIBER_ERROR_ALIGNMENT;
        if (size_context && size_context < kContextMinimum)
            return ORBIS_FIBER_ERROR_RANGE;
        if ((size_context & 15) || (!addr_context != !size_context))
            return ORBIS_FIBER_ERROR_INVALID;
        if (opt_param) {
            u32 magic{};
            if (!Get(opt_param, magic) || magic != kOptSignature)
                return ORBIS_FIBER_ERROR_INVALID;
        }
        Record record{};
        if (!ReadName(name, record.name))
            return ORBIS_FIBER_ERROR_INVALID;
        record.flags = flags;
        if (build >= kFw350)
            record.flags |= kFlagSetFpuRegs;
        if (size_check.load())
            record.flags |= kFlagContextSizeCheck;
        record.entry = entry;
        record.arg_on_initialize = arg_on_initialize;
        if (addr_context) {
            record.addr_context = record.context_start = addr_context;
            record.size_context = size_context;
            record.context_end = addr_context + size_context;
            if (!PrepareStack(addr_context, size_context,
                              record.flags & kFlagContextSizeCheck))
                return ORBIS_FIBER_ERROR_INVALID;
        }
        record.magic_start = kSignature0;
        record.magic_end = kSignature1;
        record.state = kIdle;
        Forget(fiber); // a fiber object reused for a new fiber
        return Put(fiber, record) ? ORBIS_OK : ORBIS_FIBER_ERROR_INVALID;
    }

    bool ReadName(u64 name, char (&out)[32]) {
        std::memset(out, 0, sizeof(out));
        for (size_t i = 0; i < 31; ++i) {
            char c{};
            if (!Get(name + i, c))
                return false;
            if (!c)
                break;
            out[i] = c;
        }
        return true;
    }

    s32 OptParamInitialize(u64 opt_param) {
        if (!opt_param)
            return ORBIS_FIBER_ERROR_NULL;
        if (opt_param & 7)
            return ORBIS_FIBER_ERROR_ALIGNMENT;
        return Put(opt_param, kOptSignature) ? ORBIS_OK : ORBIS_FIBER_ERROR_INVALID;
    }

    s32 Finalize(u64 fiber) {
        Record record{};
        if (const s32 error = LoadFiber(fiber, 0, record))
            return error;
        if (!CompareExchangeState(fiber, kIdle, kTerminated))
            return ORBIS_FIBER_ERROR_STATE;
        Forget(fiber);
        return ORBIS_OK;
    }

    std::optional<std::string> Run(Frame& f, ThreadKey key, u64 fiber, u64 addr_context,
                                   u64 size_context, u64 arg_on_run_to, u64 arg_on_return) {
        Record record{};
        if (const s32 error = LoadFiber(fiber, addr_context, record))
            return f.Result(error), std::nullopt;
        if (FindThread(key))
            return f.Result(ORBIS_FIBER_ERROR_PERMISSION), std::nullopt;
        if (addr_context || size_context)
            if (const s32 error = AttachContext(fiber, record, addr_context, size_context))
                return f.Result(error), std::nullopt;
        if (!CompareExchangeState(fiber, kIdle, kRun))
            return f.Result(ORBIS_FIBER_ERROR_STATE), std::nullopt;
        if (note_stack && record.addr_context)
            note_stack(record.addr_context, record.size_context);
        ThreadState* thread{};
        {
            std::scoped_lock lock{mutex};
            thread = &(threads[key] = ThreadState{Save(f.regs), arg_on_return, 0});
        }
        return Enter(f, *thread, fiber, record, arg_on_run_to, 0);
    }

    std::optional<std::string> Switch(Frame& f, ThreadKey key, u64 fiber, u64 addr_context,
                                      u64 size_context, u64 arg_on_run_to, u64 arg_on_run) {
        Record record{};
        if (const s32 error = LoadFiber(fiber, addr_context, record))
            return f.Result(error), std::nullopt;
        ThreadState* const state = FindThread(key);
        if (!state)
            return f.Result(ORBIS_FIBER_ERROR_PERMISSION), std::nullopt;
        if (addr_context || size_context)
            if (const s32 error = AttachContext(fiber, record, addr_context, size_context))
                return f.Result(error), std::nullopt;
        if (!CompareExchangeState(fiber, kIdle, kRun))
            return f.Result(ORBIS_FIBER_ERROR_STATE), std::nullopt;
        if (note_stack && record.addr_context)
            note_stack(record.addr_context, record.size_context);
        ThreadState& thread = *state;
        const u64 current = thread.current;
        // A fiber on a borrowed stack cannot be resumed: there is nothing of it to keep.
        if (Record running{}; Get(current, running) && running.addr_context) {
            if (auto fault = CheckStack(current))
                return fault;
            Suspend(f.regs, current, arg_on_run);
        }
        return Enter(f, thread, fiber, record, arg_on_run_to, current);
    }

    std::optional<std::string> ReturnToThread(Frame& f, ThreadKey key, u64 arg_on_return,
                                              u64 arg_on_run) {
        const ThreadState* const state = FindThread(key);
        if (!state)
            return f.Result(ORBIS_FIBER_ERROR_PERMISSION), std::nullopt;
        const ThreadState thread = *state;
        if (Record running{}; Get(thread.current, running) && running.addr_context) {
            if (auto fault = CheckStack(thread.current))
                return fault;
            Suspend(f.regs, thread.current, arg_on_run);
        }
        {
            std::scoped_lock lock{mutex};
            threads.erase(key);
        }
        Release(thread.current);
        // The thread continues after its sceFiberRun call.
        if (thread.arg_on_return && !Put(thread.arg_on_return, arg_on_return))
            return "libSceFiber: sceFiberRun's return argument pointer is not writable";
        Load(f.regs, thread.registers);
        f.Result(ORBIS_OK);
        return std::nullopt;
    }

    s32 GetSelf(ThreadKey key, u64 out) {
        if (!out)
            return ORBIS_FIBER_ERROR_NULL;
        const ThreadState* const state = FindThread(key);
        if (!state)
            return ORBIS_FIBER_ERROR_PERMISSION;
        return Put(out, state->current) ? ORBIS_OK : ORBIS_FIBER_ERROR_NULL;
    }

    s32 GetThreadFramePointerAddress(ThreadKey key, u64 out) {
        if (!out)
            return ORBIS_FIBER_ERROR_NULL;
        const ThreadState* const state = FindThread(key);
        if (!state)
            return ORBIS_FIBER_ERROR_PERMISSION;
        // The frame pointer of the sceFiberRun call the thread will continue from.
        return Put(out, state->registers.rbp) ? ORBIS_OK : ORBIS_FIBER_ERROR_NULL;
    }

    s32 GetInfo(u64 fiber, u64 info) {
        if (!fiber || !info)
            return ORBIS_FIBER_ERROR_NULL;
        if ((fiber & 7) || (info & 7))
            return ORBIS_FIBER_ERROR_ALIGNMENT;
        Info out{};
        if (!Get(info, out.size) || out.size != sizeof(Info))
            return ORBIS_FIBER_ERROR_INVALID;
        Record record{};
        if (const s32 error = LoadFiber(fiber, 0, record))
            return error;
        out.entry = record.entry;
        out.arg_on_initialize = record.arg_on_initialize;
        out.addr_context = record.addr_context;
        out.size_context = record.size_context;
        std::memcpy(out.name, record.name, sizeof(out.name));
        out.size_context_margin = ~u64{0};
        if ((record.flags & kFlagContextSizeCheck) && record.addr_context)
            out.size_context_margin = Margin(record);
        // Only the reported fields; the reserved tail is the caller's.
        return memory.Write(info, std::as_bytes(std::span{&out, 1}).first(offsetof(Info, pad)))
                   ? ORBIS_OK
                   : ORBIS_FIBER_ERROR_INVALID;
    }

    // Bytes at the far end of the stack the fiber has never written: the fill from just above
    // the signature up to the first word that changed.
    u64 Margin(const Record& record) {
        u64 signature{};
        if (!Get(record.context_start, signature) || signature != kStackSignature)
            return 0;
        std::array<u64, 512> words;
        u64 at = record.context_start + 8;
        while (at < record.context_end) {
            const u64 count = std::min<u64>(words.size(), (record.context_end - at) / 8);
            if (!count || !memory.Read(at, std::as_writable_bytes(std::span{words}).first(count * 8)))
                break;
            for (u64 i = 0; i < count; ++i)
                if (words[i] != kStackSizeCheck)
                    return at + i * 8 - (record.context_start + 8);
            at += count * 8;
        }
        return at - (record.context_start + 8);
    }

    s32 Rename(u64 fiber, u64 name) {
        if (!fiber || !name)
            return ORBIS_FIBER_ERROR_NULL;
        if (fiber & 7)
            return ORBIS_FIBER_ERROR_ALIGNMENT;
        Record record{};
        if (const s32 error = LoadFiber(fiber, 0, record))
            return error;
        char text[32];
        if (!ReadName(name, text))
            return ORBIS_FIBER_ERROR_INVALID;
        return memory.Write(fiber + offsetof(Record, name), std::as_bytes(std::span{text}))
                   ? ORBIS_OK
                   : ORBIS_FIBER_ERROR_INVALID;
    }

    GuestFiberMemory& memory;
    const u64 entry_return;
    void (*const note_stack)(u64, u64);
    std::atomic<u32> size_check{0};
    std::mutex mutex;
    std::map<u64, Suspended> suspended;
    std::map<ThreadKey, ThreadState> threads;
};

static_assert(sizeof(GuestFiber::Info) == 128);
static_assert(offsetof(GuestFiber::Info, name) == 40);
static_assert(offsetof(GuestFiber::Info, size_context_margin) == 72);
static_assert(offsetof(GuestFiber::Record, state) == 4);
static_assert(offsetof(GuestFiber::Record, name) == 40);
static_assert(offsetof(GuestFiber::Record, context) == 72);
static_assert(offsetof(GuestFiber::Record, magic_end) == 104);
static_assert(sizeof(GuestFiber::Record) == 112);

} // namespace Core::HostRuntime
