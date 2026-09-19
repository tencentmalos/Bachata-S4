// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vector>
#include "core/guest_cpu/api/context.h"

namespace Core::GuestCpu::Debug {
enum class Phase { Ready, Guest, Hle, Parked };
enum class WatchAccess : std::uint8_t { Read = 1, Write = 2, Access = 3 };
struct Watchpoint {
    GuestRange range;
    WatchAccess access;
    std::uint64_t mapping_generation{};
};
struct WatchHit {
    std::uint64_t address{}, bytes{}, instruction_rip{}, watched_address{};
    WatchAccess access{};
};
struct Thread {
    ThreadHandle handle;
    std::uint64_t guest_tid{}, host_tid{};
    Phase phase{};
    StopReason reason{};
    std::optional<CpuSnapshot> snapshot;
    std::optional<WatchHit> watch_hit;
};

struct StackFrame {
    std::string kind, provenance, module, symbol;
    std::uint64_t pc{}, sp{}, fp{}, invocation{}, operation{};
};
struct MixedStack {
    std::uint64_t context_id{}, thread_id{}, generation{}, stop_epoch{};
    std::string phase, limitation;
    std::vector<StackFrame> frames;
};

// Backend-neutral debugger contract. Hle snapshots are stable copies, NOT a
// stopped native caller. Writes/Step require an owner parked in the debugger.
// Neither a transport worker nor an HLE observer may drive CpuContext::Step.
class Target {
public:
    virtual ~Target() = default;
    virtual std::vector<Thread> Threads() = 0;
    virtual std::vector<DebugModule> Modules() = 0;
    virtual Result<MixedStack> ReadMixedStack(std::uint64_t thread_id) = 0;
    virtual Status Pause() = 0;
    virtual Status Continue(std::uint64_t thread_id, bool step) = 0;
    virtual Status WriteStoppedRegisters(ThreadHandle, const RegisterPatch&,
                                         std::uint64_t epoch) = 0;
    virtual Status ReadMemory(GuestAddress, std::span<std::byte>) = 0;
    virtual Status WriteMemory(GuestAddress, std::span<const std::byte>) = 0;
    virtual Status Breakpoint(GuestAddress, bool insert) = 0;
    virtual Status SetWatchpoint(GuestRange, WatchAccess, bool insert) = 0;
    virtual Status Detach() = 0;
};
} // namespace Core::GuestCpu::Debug
