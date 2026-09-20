// SPDX-License-Identifier: GPL-2.0-or-later
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "common/profiler.h"
#include "core/host_runtime/guest_sync_metrics.h"
using namespace Core::HostRuntime::SyncMetrics;
namespace {
unsigned checks{};
void Check(bool ok, const char* reason) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", reason);
        std::exit(1);
    }
}
std::mutex counters_mutex;
std::unordered_map<std::string, int64_t> counters;
std::atomic<bool> trace_enabled{};
thread_local unsigned scopes{};
} // namespace
// Test transport: exercises names/values/lifetimes without an SDK or file writer.
namespace Common::Profiler {
Scope::Scope(const char*) noexcept {
    ++scopes;
}
Scope::~Scope() {
    --scopes;
}
bool Enabled() noexcept {
    return trace_enabled.load();
}
void Counter(const char* name, int64_t value) noexcept {
    std::lock_guard lock(counters_mutex);
    counters[name] = value;
}
} // namespace Common::Profiler
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--overhead") {
        Session s(1);
        for (unsigned round = 0; round < 3; ++round)
            for (unsigned mode : {0, 1, 2, 1, 0}) {
                s.Enable(mode != 0, mode == 2);
                auto start = NowNs();
                for (unsigned i = 0; i < 1000000; ++i) {
                    if (s.Enabled()) {
                        Call c(&s, Operation::MutexLock);
                        { Phase p(Stage::Guard); }
                        c.Finish(true, 0);
                    }
                }
                std::printf("{\"case\":\"metrics_only_no_sdk\",\"round\":%u,\"mode\":%u,\"ns_per_"
                            "call\":%.2f}\n",
                            round, mode, double(NowNs() - start) / 1000000);
            }
        return 0;
    }
    Check(Classify("HLE.scePthreadMutexLock") == Operation::MutexLock, "sce alias");
    Check(Classify("pthread_mutex_lock") == Operation::MutexLock, "posix alias");
    Check(Classify("sceKernelWaitSema") == Operation::SemaWait, "kernel sem distinct");
    Check(Classify("sem_reltimedwait_np") == Operation::SemTimed, "relative POSIX sem");
    Check(Classify("scePthreadSemTimedwait") == Operation::SemTimed, "sce sem timeout alias");
    Check(Classify("sem_wait") == Operation::SemWait, "posix sem distinct");
    Check(Classify("HLE.scePthreadCondTimedwait") == Operation::CondTimed, "timed cond");
    Check(Classify("scePthreadRwlockTimedwrlock") == Operation::RwTimedWrite, "timed writer");
    Check(Classify("HLE.sceKernelUsleep") == Operation::Usleep, "sleep");
    Check(Classify("sceKernelGetTscFrequency") == Operation::None, "clock excluded");
    Check(Classify("sceKernelGetProcessTime") == Operation::None, "time excluded");
    Check(Classify("pthread_mutexattr_settype") == Operation::MutexConfig, "config separate");
    Check(Classify("sceKernelWaitEventFlag") == Operation::EventWait, "event");
    Check(Command({"status"}).find("no_session") != std::string::npos, "no stale global session");
    auto s = std::make_shared<Session>(77);
    SetControl(s);
    {
        Call c(s.get(), Operation::MutexLock);
        c.Finish(true, 0);
    }
    Check(s->Read().shards == 0, "OFF does not allocate");
    Check(Command({"start", "78"}).find("error") != std::string::npos, "reject stale context");
    Check(!s->Enabled(), "stale enable unchanged");
    s->Enable(true, true);
    {
        Call c(s.get(), Operation::MutexLock);
        {
            Phase p(Stage::Lookup);
            p.End();
            p.End();
        }
        c.Finish(true, 0);
    }
    {
        Call c(s.get(), Operation::MutexLock);
        c.Finish(true, 0x80020010);
    }
    {
        Call c(s.get(), Operation::MutexLock);
        c.Finish(true, 60);
    }
    {
        Call c(s.get(), Operation::MutexLock);
        c.Finish(true, 0x80020004);
    }
    {
        Call c(s.get(), Operation::MutexLock);
        c.Finish(false, 99);
    }
    { Call c(s.get(), Operation::MutexLock); /* Exception unwinding/abandoned invocation. */ }
    auto row = s->Read().rows[unsigned(Operation::MutexLock)];
    Check(row.calls == 6 && row.completed == 6, "completed and exception calls");
    Check(row.nonzero == 3 && row.busy == 1 && row.timeouts == 1 && row.interrupted == 1,
          "result domains");
    Check(row.host_errors == 1 && row.exceptions == 1, "host errors are not guest errno");
    Check(row.stage_calls[0] == 1, "phase End idempotent");
    Check(scopes == 0, "phase trace paired");
    {
        Call outer(s.get(), Operation::CondWait);
        {
            Call inner(s.get(), Operation::MutexLock);
            { Phase p(Stage::Guard); }
            inner.Finish(true, 0);
        }
        { Phase p(Stage::Reacquire); }
        outer.Finish(true, 0);
    }
    Check(s->Read().rows[unsigned(Operation::CondWait)].stage_calls[4] == 1,
          "nested call restores parent");
    Check(s->Read().rows[unsigned(Operation::MutexLock)].stage_calls[1] == 1,
          "inner phases distinct");
    {
        Call c(s.get(), Operation::SemWait);
        s->Enable(false);
        Check(s->Read().rows[unsigned(Operation::SemWait)].calls == 1,
              "pending started call visible");
        Check(s->Read().rows[unsigned(Operation::SemWait)].completed == 0, "pending not completed");
        {
            Call ignored(s.get(), Operation::SemWait);
            ignored.Finish(true, 0);
        }
        c.Finish(true, uint64_t(-1));
    }
    row = s->Read().rows[unsigned(Operation::SemWait)];
    Check(row.calls == 1 && row.completed == 1 && row.nonzero == 1,
          "stop retains in-flight completion");
    Check(row.timeouts == 0 && row.busy == 0, "POSIX -1 does not invent errno");
    s->Enable(true);
    {
        Call c(s.get(), Operation::Epoll);
        c.Finish(true, 60);
    }
    Check(s->Read().rows[unsigned(Operation::Epoll)].timeouts == 0, "epoll event count not errno");
    {
        Call c(s.get(), Operation::Sleep);
        c.Finish(true, 60);
    }
    Check(s->Read().rows[unsigned(Operation::Sleep)].timeouts == 0, "remaining sleep not errno");
    {
        Call c(s.get(), Operation::MutexConfig);
        c.Finish(true, 4);
    }
    Check(s->Read().rows[unsigned(Operation::MutexConfig)].interrupted == 0,
          "getter value not errno");
    Check(PhaseMask(Operation::MutexLock) == 15 && PhaseMask(Operation::Once) == 0,
          "unmeasured phase explicit");
    std::barrier start(9);
    std::vector<std::jthread> workers;
    for (unsigned i = 0; i < 8; ++i)
        workers.emplace_back([&] {
            start.arrive_and_wait();
            for (unsigned n = 0; n < 10000; ++n) {
                Call c(s.get(), Operation::RwRead);
                c.Finish(true, 0);
            }
        });
    start.arrive_and_wait();
    for (unsigned i = 0; i < 50; ++i)
        (void)s->Read(); // Concurrent snapshots / TSan.
    workers.clear();
    row = s->Read().rows[unsigned(Operation::RwRead)];
    uint64_t histogram{};
    for (auto n : row.histogram)
        histogram += n;
    Check(row.calls == 80000 && row.completed == 80000 && histogram == 80000,
          "thread-sharded exact final total");
    Check(row.elapsed_ns >= row.max_ns && row.max_ns > 0, "elapsed aggregation");
    trace_enabled = true;
    s->Enable(true, true);
    for (unsigned i = 0; i < 12; ++i) {
        Call c(s.get(), Operation::SemaWait, 123, 456, 789, 2);
        {
            Phase p(Stage::Park);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        c.Finish(true, 0);
    }
    s->Enable(false);
    s->Emit();
    row = s->Read().rows[unsigned(Operation::SemaWait)];
    Check(row.histogram[4] + row.histogram[5] == 12, "long duration histogram");
    Check(row.locator_dropped == 4, "bounded locator emissions retain dropped count");
    Check(row.stage_calls[2] == 12, "park entries");
    {
        std::lock_guard lock(counters_mutex);
        Check(counters["HLE.Sync.Sema.Wait.Calls"] == 12, "counter names and totals");
        Check(counters["HLE.Sync.Sema.Wait.Ge1ms"] == 12, "long call counter");
        Check(counters["HLE.Sync.Long.Thread"] == 789 && counters["HLE.Sync.Long.Arg0"] == 456,
              "thread/argument locator");
    }
    Check(s->Status().find("not CPU") != std::string::npos, "elapsed semantics explicit");
    Check(Command({"reset", "77"}).find("error") != std::string::npos, "no racing resets");
    const auto previous_id = s->Read().session;
    s.reset();
    Check(Command({}).find("no_session") != std::string::npos, "expired session control");
    auto second = std::make_shared<Session>(77);
    second->Enable(true);
    SetControl(second);
    {
        Call c(second.get(), Operation::MutexLock);
        c.Finish(true, 0);
    }
    Check(second->Read().session != previous_id, "session identity never aliases TLS cache");
    Check(second->Read().rows[unsigned(Operation::MutexLock)].calls == 1,
          "new session no stale totals");
    std::printf("guest_sync_metrics: %u checks PASS\n", checks);
}
