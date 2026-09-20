// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Core::HostRuntime::SyncMetrics {
// Canonical operations fold sce/POSIX aliases; the existing HLE scope retains the exact name.
#define SHAD_SYNC_OPERATIONS(X)                                                                    \
    X(MutexInit, "Mutex.Init")                                                                     \
    X(MutexDestroy, "Mutex.Destroy") X(MutexLock, "Mutex.Lock") X(MutexTry, "Mutex.TryLock") X(    \
        MutexTimed, "Mutex.TimedLock") X(MutexUnlock, "Mutex.Unlock")                              \
        X(MutexConfig, "Mutex.Config") X(CondInit, "Cond.Init") X(CondDestroy, "Cond.Destroy") X(  \
            CondWait, "Cond.Wait") X(CondTimed, "Cond.TimedWait") X(CondSignal, "Cond.Signal")     \
            X(CondBroadcast, "Cond.Broadcast") X(CondConfig, "Cond.Config") X(                     \
                RwInit, "Rwlock.Init") X(RwDestroy, "Rwlock.Destroy") X(RwRead, "Rwlock.Read")     \
                X(RwWrite, "Rwlock.Write") X(RwTryRead, "Rwlock.TryRead") X(                       \
                    RwTryWrite, "Rwlock.TryWrite") X(RwTimedRead, "Rwlock.TimedRead")              \
                    X(RwTimedWrite, "Rwlock.TimedWrite") X(RwUnlock, "Rwlock.Unlock") X(           \
                        RwConfig, "Rwlock.Config") X(SemInit, "PosixSem.Init")                     \
                        X(SemDestroy, "PosixSem.Destroy") X(SemWait, "PosixSem.Wait") X(           \
                            SemTry, "PosixSem.TryWait") X(SemTimed, "PosixSem.TimedWait")          \
                            X(SemPost, "PosixSem.Post") X(SemValue, "PosixSem.GetValue") X(        \
                                SemaCreate, "Sema.Create") X(SemaDelete, "Sema.Delete")            \
                                X(SemaWait, "Sema.Wait") X(SemaPoll, "Sema.Poll") X(               \
                                    SemaSignal, "Sema.Signal") X(SemaCancel, "Sema.Cancel")        \
                                    X(SemaOpen, "Sema.Open") X(SemaClose, "Sema.Close") X(         \
                                        EventCreate, "EventFlag.Create") X(EventDelete,            \
                                                                           "EventFlag.Delete")     \
                                        X(EventWait, "EventFlag.Wait") X(                          \
                                            EventPoll, "EventFlag.Poll") X(EventSet,               \
                                                                           "EventFlag.Set")        \
                                            X(EventClear, "EventFlag.Clear") X(EventCancel,        \
                                                                               "EventFlag.Cancel") \
                                                X(EventOpen, "EventFlag.Open") X(                  \
                                                    EventClose, "EventFlag.Close")                 \
                                                    X(Join, "Thread.Join") X(Once, "Thread.Once")  \
                                                        X(Yield, "Thread.Yield") X(Sleep,          \
                                                                                   "Thread.Sleep") \
                                                            X(Usleep, "Thread.Usleep")             \
                                                                X(Nanosleep, "Thread.Nanosleep")   \
                                                                    X(Epoll, "Net.EpollWait")
enum class Operation : unsigned {
#define X(id, name) id,
    SHAD_SYNC_OPERATIONS(X)
#undef X
        Count,
    None = Count
};
constexpr unsigned OperationCount = unsigned(Operation::Count);
enum class Stage : unsigned { Lookup, Guard, Park, Publish, Reacquire, Count };
constexpr unsigned StageCount = unsigned(Stage::Count);
Operation Classify(std::string_view name);
const char* Name(Operation op) noexcept;
unsigned PhaseMask(Operation op) noexcept;
uint64_t NowNs() noexcept;

struct Totals {
    uint64_t calls{}, completed{}, elapsed_ns{}, max_ns{}, nonzero{}, timeouts{}, busy{},
        interrupted{};
    uint64_t host_errors{}, exceptions{}, locator_dropped{};
    std::array<uint64_t, 6> histogram{}; // <1us, <10us, <100us, <1ms, <16ms, >=16ms
    std::array<uint64_t, StageCount> stage_calls{}, stage_ns{};
};
struct Snapshot {
    uint64_t session{}, context{}, timestamp_ns{};
    bool enabled{}, detail{};
    unsigned shards{};
    uint64_t untracked_calls{}, emission_failures{};
    std::array<Totals, OperationCount> rows{};
};
class Call;
class Session final {
public:
    explicit Session(uint64_t context);
    ~Session();
    Session(const Session&) = delete;
    void Enable(bool enabled, bool detail = false);
    Snapshot Read() const;
    std::string Status() const;
    void Emit() const;
    uint64_t Context() const noexcept;
    // Disabled fast path never allocates, reads a clock, locks, or writes counters.
    bool Enabled() const noexcept {
        return enabled.load(std::memory_order_relaxed);
    }

private:
    friend class Call;
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::atomic<bool> enabled{}, detailed{};
};
class Call final {
public:
    Call(Session* session, Operation op, uint64_t operation_id = 0, uint64_t object = 0,
         uint64_t thread = 0, uint64_t thread_generation = 0) noexcept;
    ~Call();
    Call(const Call&) = delete;
    void Finish(bool host_ok, uint64_t result) noexcept;

private:
    friend class Phase;
    void End(bool host_ok, uint64_t result, bool exception) noexcept;
    Session* session{};
    void* shard{};
    Operation op{Operation::None};
    uint64_t begin{}, operation_id{}, object{}, thread{}, thread_generation{};
    bool detail{};
    Call* previous{};
    std::array<uint64_t, StageCount> stage_calls{}, stage_ns{};
};
// Optional nested phases. Park includes host CV/futex sleep AND reacquisition of its guard;
// it is NOT on-CPU time or proof of kernel sleeping. No changes to the synchronization protocol.
class Phase final {
public:
    explicit Phase(Stage s) noexcept : stage(s) {
        if (hooks.current)
            hooks.begin(*this, hooks.current);
    }
    ~Phase() {
        End();
    }
    Phase(const Phase&) = delete;
    void End() noexcept {
        if (call)
            hooks.end(*this);
    }

private:
    friend class Call;
    static void Begin(Phase& phase, Call* call) noexcept;
    static void Finish(Phase& phase) noexcept;
    struct Hooks {
        Call* current;
        void (*begin)(Phase&, Call*) noexcept;
        void (*end)(Phase&) noexcept;
    };
    inline static thread_local Hooks hooks{};
    Call* call{};
    Stage stage{};
    uint64_t begin{};
    // Scope storage lives in cpp without a heap allocation.
    alignas(8) std::array<std::byte, 16> trace{};
};
void SetControl(const std::shared_ptr<Session>& session);
std::string Command(const std::vector<std::string>& args);
} // namespace Core::HostRuntime::SyncMetrics
