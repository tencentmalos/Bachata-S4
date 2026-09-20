// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <sstream>
#include <thread>
#include "common/profiler.h"
#include "core/host_runtime/guest_sync_metrics.h"

namespace Core::HostRuntime::SyncMetrics {
namespace {
constexpr std::array Names{
#define X(id, name) name,
    SHAD_SYNC_OPERATIONS(X)
#undef X
};
constexpr const char* StageTags[] = {"HLE.Sync.Lookup", "HLE.Sync.Guard", "HLE.Sync.Park",
                                     "HLE.Sync.Publish", "HLE.Sync.Reacquire"};
thread_local Call* current{};
std::atomic<uint64_t> next_session{1};
std::mutex control_mutex;
std::weak_ptr<Session> control;
constexpr auto Relaxed = std::memory_order_relaxed;
struct AtomicTotals {
    std::atomic<uint64_t> calls{}, completed{}, elapsed_ns{}, max_ns{}, nonzero{}, timeouts{},
        busy{}, interrupted{};
    std::atomic<uint64_t> host_errors{}, exceptions{}, locator_dropped{};
    std::array<std::atomic<uint64_t>, 6> histogram{};
    std::array<std::atomic<uint64_t>, StageCount> stage_calls{}, stage_ns{};
};
// Exactly one physical-thread writer per shard. Readers never hold the writer's lock.
struct alignas(64) Shard {
    std::array<AtomicTotals, OperationCount> rows{};
    uint64_t locator_window{};
    unsigned locator_count{};
};
void Add(std::atomic<uint64_t>& field, uint64_t value = 1) noexcept {
    field.store(field.load(Relaxed) + value, Relaxed);
}
struct Binding {
    const char* name;
    Operation op;
};
} // namespace
Operation Classify(std::string_view raw) {
    if (raw.starts_with("HLE."))
        raw.remove_prefix(4);
    if (raw.starts_with("scePthread"))
        raw.remove_prefix(10);
    else if (raw.starts_with("pthread_"))
        raw.remove_prefix(8);
    else if (raw.starts_with("sceKernel"))
        raw.remove_prefix(9);
    std::string key;
    for (unsigned char c : raw)
        if (c != '_')
            key += c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : char(c);
    const Binding bindings[] = {
        // Guest fast-path primitives (guest/runtime/sync): only contended paths reach these.
        {"shadsyncwait", Operation::AddrWait},
        {"shadsyncwake", Operation::AddrWake},
        {"mutexinit", Operation::MutexInit},
        {"mutexinitformono", Operation::MutexInit},
        {"mutexdestroy", Operation::MutexDestroy},
        {"mutexlock", Operation::MutexLock},
        {"mutextrylock", Operation::MutexTry},
        {"mutextimedlock", Operation::MutexTimed},
        {"mutexreltimedlocknp", Operation::MutexTimed},
        {"mutexunlock", Operation::MutexUnlock},
        {"condinit", Operation::CondInit},
        {"conddestroy", Operation::CondDestroy},
        {"condwait", Operation::CondWait},
        {"condtimedwait", Operation::CondTimed},
        {"condreltimedwaitnp", Operation::CondTimed},
        {"condsignal", Operation::CondSignal},
        {"condsignalto", Operation::CondSignal},
        {"condsignaltonp", Operation::CondSignal},
        {"condbroadcast", Operation::CondBroadcast},
        {"rwlockinit", Operation::RwInit},
        {"rwlockdestroy", Operation::RwDestroy},
        {"rwlockrdlock", Operation::RwRead},
        {"rwlockwrlock", Operation::RwWrite},
        {"rwlocktryrdlock", Operation::RwTryRead},
        {"rwlocktrywrlock", Operation::RwTryWrite},
        {"rwlocktimedrdlock", Operation::RwTimedRead},
        {"rwlocktimedwrlock", Operation::RwTimedWrite},
        {"rwlockreltimedrdlocknp", Operation::RwTimedRead},
        {"rwlockreltimedwrlocknp", Operation::RwTimedWrite},
        {"rwlockunlock", Operation::RwUnlock},
        {"seminit", Operation::SemInit},
        {"semdestroy", Operation::SemDestroy},
        {"semwait", Operation::SemWait},
        {"semtrywait", Operation::SemTry},
        {"semtimedwait", Operation::SemTimed},
        {"semreltimedwaitnp", Operation::SemTimed},
        {"sempost", Operation::SemPost},
        {"semgetvalue", Operation::SemValue},
        {"createsema", Operation::SemaCreate},
        {"deletesema", Operation::SemaDelete},
        {"waitsema", Operation::SemaWait},
        {"pollsema", Operation::SemaPoll},
        {"signalsema", Operation::SemaSignal},
        {"cancelsema", Operation::SemaCancel},
        {"opensema", Operation::SemaOpen},
        {"closesema", Operation::SemaClose},
        {"createeventflag", Operation::EventCreate},
        {"deleteeventflag", Operation::EventDelete},
        {"waiteventflag", Operation::EventWait},
        {"polleventflag", Operation::EventPoll},
        {"seteventflag", Operation::EventSet},
        {"cleareventflag", Operation::EventClear},
        {"canceleventflag", Operation::EventCancel},
        {"openeventflag", Operation::EventOpen},
        {"closeeventflag", Operation::EventClose},
        {"join", Operation::Join},
        {"once", Operation::Once},
        {"yield", Operation::Yield},
        {"schedyield", Operation::Yield},
        {"sleep", Operation::Sleep},
        {"usleep", Operation::Usleep},
        {"nanosleep", Operation::Nanosleep},
        {"scenetepollwait", Operation::Epoll},
    };
    for (const auto& b : bindings)
        if (key == b.name)
            return b.op;
    if (key.starts_with("mutex"))
        return Operation::MutexConfig;
    if (key.starts_with("cond"))
        return Operation::CondConfig;
    if (key.starts_with("rwlock"))
        return Operation::RwConfig;
    return Operation::None;
}
const char* Name(Operation op) noexcept {
    return op < Operation::Count ? Names[unsigned(op)] : "Unknown";
}
unsigned PhaseMask(Operation op) noexcept {
    // Zero means uninstrumented, never "zero cost". Bits follow Stage.
    switch (op) {
    case Operation::MutexLock:
    case Operation::MutexTry:
        return 0b01111;
    case Operation::MutexUnlock:
        return 0b01011;
    case Operation::CondWait:
    case Operation::CondTimed:
        return 0b10100;
    case Operation::RwRead:
    case Operation::RwWrite:
    case Operation::RwTryRead:
    case Operation::RwTryWrite:
    case Operation::RwTimedRead:
    case Operation::RwTimedWrite:
    case Operation::SemWait:
    case Operation::SemTry:
    case Operation::SemTimed:
    case Operation::SemaWait:
    case Operation::SemaPoll:
    case Operation::EventWait:
    case Operation::EventPoll:
        return 0b00110;
    case Operation::AddrWait:
        return 0b00110;
    case Operation::RwUnlock:
    case Operation::SemPost:
    case Operation::SemaCreate:
    case Operation::SemaDelete:
    case Operation::SemaSignal:
    case Operation::AddrWake:
    case Operation::SemaCancel:
    case Operation::SemaOpen:
    case Operation::SemaClose:
        return 0b00010;
    default:
        return 0;
    }
}
uint64_t NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct Session::Impl {
    uint64_t id{next_session.fetch_add(1, Relaxed)}, context;
    mutable std::mutex shards_mutex;
    std::vector<std::unique_ptr<Shard>> shards;
    std::mutex worker_mutex;
    std::condition_variable_any changed;
    std::jthread worker;
    std::atomic<uint64_t> untracked_calls{}, emission_failures{};
    explicit Impl(uint64_t context) : context(context) {}
    Shard* Writer() {
        struct Cache {
            uint64_t id{};
            Shard* shard{};
        };
        static thread_local Cache cache;
        if (cache.id == id)
            return cache.shard;
        auto shard = std::make_unique<Shard>();
        auto* ptr = shard.get();
        {
            std::lock_guard lock(shards_mutex);
            shards.push_back(std::move(shard));
        }
        cache = {id, ptr};
        return ptr;
    }
};
Session::Session(uint64_t context) : impl(std::make_unique<Impl>(context)) {}
Session::~Session() {
    if (impl->worker.joinable()) {
        impl->worker.request_stop();
        impl->changed.notify_all();
        impl->worker.join();
    }
}
uint64_t Session::Context() const noexcept {
    return impl->context;
}
void Session::Enable(bool on, bool detail) {
    std::lock_guard lock(impl->worker_mutex);
    detailed.store(detail, Relaxed);
    enabled.store(on, Relaxed);
    if (on && !impl->worker.joinable())
        impl->worker = std::jthread([this](std::stop_token stop) {
            std::unique_lock lock(impl->worker_mutex);
            while (!stop.stop_requested()) {
                impl->changed.wait_for(lock, stop, std::chrono::milliseconds(500),
                                       [] { return false; });
                if (stop.stop_requested())
                    break;
                lock.unlock();
                if (Enabled() && Common::Profiler::Enabled()) {
                    try {
                        Emit();
                    } catch (...) {
                        impl->emission_failures.fetch_add(1, Relaxed);
                    }
                }
                lock.lock();
            }
        });
}
Snapshot Session::Read() const {
    Snapshot out{.session = impl->id,
                 .context = impl->context,
                 .timestamp_ns = NowNs(),
                 .enabled = Enabled(),
                 .detail = detailed.load(Relaxed)};
    out.untracked_calls = impl->untracked_calls.load(Relaxed);
    out.emission_failures = impl->emission_failures.load(Relaxed);
    std::lock_guard lock(impl->shards_mutex);
    out.shards = impl->shards.size();
    for (const auto& shard : impl->shards)
        for (unsigned i = 0; i < OperationCount; ++i) {
            auto& a = shard->rows[i];
            auto& b = out.rows[i];
#define SUM(f) b.f += a.f.load(Relaxed)
            SUM(completed);
            SUM(calls);
            SUM(elapsed_ns);
            SUM(nonzero);
            SUM(timeouts);
            SUM(busy);
            SUM(interrupted);
            SUM(host_errors);
            SUM(exceptions);
            SUM(locator_dropped);
#undef SUM
            b.max_ns = std::max(b.max_ns, a.max_ns.load(Relaxed));
            for (unsigned j = 0; j < 6; ++j)
                b.histogram[j] += a.histogram[j].load(Relaxed);
            for (unsigned j = 0; j < StageCount; ++j) {
                b.stage_calls[j] += a.stage_calls[j].load(Relaxed);
                b.stage_ns[j] += a.stage_ns[j].load(Relaxed);
            }
        }
    return out;
}
std::string Session::Status() const {
    const auto s = Read();
    std::ostringstream out;
    out << "{\"schema\":1,\"session\":" << s.session << ",\"context\":" << s.context
        << ",\"timestamp_ns\":" << s.timestamp_ns
        << ",\"enabled\":" << (s.enabled ? "true" : "false")
        << ",\"detail\":" << (s.detail ? "true" : "false") << ",\"shards\":" << s.shards
        << ",\"untracked_calls\":" << s.untracked_calls
        << ",\"emission_failures\":" << s.emission_failures
        << ",\"fast_path\":\"" << FastPathStatus() << '"'
        << ",\"semantics\":\"session cumulative; rolling snapshot; completed inclusive elapsed "
           "includes waits, not CPU or critical path; start/stop does not reset; in-flight calls "
           "finish in their original sample\""
        << ",\"histogram_upper_ns\":[1000,10000,100000,1000000,16000000,null],\"operations\":[";
    bool comma{};
    for (unsigned i = 0; i < OperationCount; ++i) {
        const auto& r = s.rows[i];
        if (!r.calls)
            continue;
        if (comma)
            out << ',';
        comma = true;
        out << "{\"kind\":" << i << ",\"operation\":\"" << Names[i] << '"';
#define FIELD(f) out << ",\"" #f "\":" << r.f
        FIELD(calls);
        FIELD(completed);
        FIELD(elapsed_ns);
        FIELD(max_ns);
        FIELD(nonzero);
        FIELD(timeouts);
        FIELD(busy);
        FIELD(interrupted);
        FIELD(host_errors);
        FIELD(exceptions);
        FIELD(locator_dropped);
#undef FIELD
        out << ",\"phase_mask\":" << PhaseMask(Operation(i));
        out << ",\"inflight\":" << (r.calls >= r.completed ? r.calls - r.completed : 0)
            << ",\"histogram\":[";
        for (unsigned j = 0; j < 6; ++j) {
            if (j)
                out << ',';
            out << r.histogram[j];
        }
        out << "],\"phases\":[";
        for (unsigned j = 0; j < StageCount; ++j) {
            if (j)
                out << ',';
            out << "{\"name\":\"" << StageTags[j] << "\",\"calls\":" << r.stage_calls[j]
                << ",\"elapsed_ns\":" << r.stage_ns[j] << '}';
        }
        out << "]}";
    }
    out << "]}\n";
    return out.str();
}
void Session::Emit() const {
    const auto s = Read();
    Common::Profiler::Scope scope{"HLE.Sync.Snapshot"};
    Common::Profiler::Counter("HLE.Sync.Session", s.session);
    Common::Profiler::Counter("HLE.Sync.Context", s.context);
    Common::Profiler::Counter("HLE.Sync.SnapshotNs", s.timestamp_ns);
    Common::Profiler::Counter("HLE.Sync.Enabled", s.enabled);
    Common::Profiler::Counter("HLE.Sync.Detail", s.detail);
    Common::Profiler::Counter("HLE.Sync.Shards", s.shards);
    Common::Profiler::Counter("HLE.Sync.UntrackedCalls", s.untracked_calls);
    Common::Profiler::Counter("HLE.Sync.EmissionFailures", s.emission_failures);
    // Every name has process lifetime, including SDKs that retain pointers to names.
#define X(id, name)                                                                                \
    {                                                                                              \
        const auto& r = s.rows[unsigned(Operation::id)];                                           \
        if (r.calls) {                                                                             \
            Common::Profiler::Counter("HLE.Sync." name ".PhaseMask", PhaseMask(Operation::id));    \
            Common::Profiler::Counter("HLE.Sync." name ".Calls", r.calls);                         \
            Common::Profiler::Counter("HLE.Sync." name ".Completed", r.completed);                 \
            Common::Profiler::Counter("HLE.Sync." name ".ElapsedNs", r.elapsed_ns);                \
            Common::Profiler::Counter("HLE.Sync." name ".MaxNs", r.max_ns);                        \
            Common::Profiler::Counter("HLE.Sync." name ".Ge1ms", r.histogram[4] + r.histogram[5]); \
            Common::Profiler::Counter("HLE.Sync." name ".Ge16ms", r.histogram[5]);                 \
            Common::Profiler::Counter("HLE.Sync." name ".Nonzero", r.nonzero);                     \
            Common::Profiler::Counter("HLE.Sync." name ".Timeouts", r.timeouts);                   \
            Common::Profiler::Counter("HLE.Sync." name ".Busy", r.busy);                           \
            Common::Profiler::Counter("HLE.Sync." name ".Interrupted", r.interrupted);             \
            Common::Profiler::Counter("HLE.Sync." name ".HostErrors", r.host_errors);              \
            Common::Profiler::Counter("HLE.Sync." name ".Exceptions", r.exceptions);               \
            Common::Profiler::Counter("HLE.Sync." name ".LocatorDropped", r.locator_dropped);      \
            Common::Profiler::Counter("HLE.Sync." name ".LookupNs", r.stage_ns[0]);                \
            Common::Profiler::Counter("HLE.Sync." name ".GuardNs", r.stage_ns[1]);                 \
            Common::Profiler::Counter("HLE.Sync." name ".ParkCalls", r.stage_calls[2]);            \
            Common::Profiler::Counter("HLE.Sync." name ".ParkNs", r.stage_ns[2]);                  \
            Common::Profiler::Counter("HLE.Sync." name ".PublishNs", r.stage_ns[3]);               \
            Common::Profiler::Counter("HLE.Sync." name ".ReacquireNs", r.stage_ns[4]);             \
        }                                                                                          \
    }
    SHAD_SYNC_OPERATIONS(X)
#undef X
}
Call::Call(Session* s, Operation o, uint64_t operation, uint64_t obj, uint64_t tid,
           uint64_t gen) noexcept {
    if (!s || o == Operation::None || !s->Enabled())
        return;
    // Instrumentation allocation failure must not turn a valid guest operation into a fault.
    try {
        shard = s->impl->Writer();
    } catch (...) {
        s->impl->untracked_calls.fetch_add(1, Relaxed);
        return;
    }
    session = s;
    op = o;
    operation_id = operation;
    object = obj;
    thread = tid;
    thread_generation = gen;
    detail = s->detailed.load(Relaxed);
    Add(static_cast<Shard*>(shard)->rows[unsigned(op)].calls);
    previous = current;
    current = this;
    Phase::hooks = {detail ? this : nullptr, &Phase::Begin, &Phase::Finish};
    begin = NowNs();
}
Call::~Call() {
    if (session)
        End(false, 0, true);
}
void Call::Finish(bool ok, uint64_t result) noexcept {
    if (session)
        End(ok, result, false);
}
void Call::End(bool ok, uint64_t result, bool exception) noexcept {
    const auto end = NowNs(), elapsed = end - begin;
    current = previous;
    Phase::hooks.current = previous && previous->detail ? previous : nullptr;
    auto& writer = *static_cast<Shard*>(shard);
    auto& r = writer.rows[unsigned(op)];
    Add(r.elapsed_ns, elapsed);
    r.max_ns.store(std::max(r.max_ns.load(Relaxed), elapsed), Relaxed);
    unsigned bucket{};
    for (uint64_t limit : {1000ULL, 10000ULL, 100000ULL, 1000000ULL, 16000000ULL}) {
        if (elapsed < limit)
            break;
        ++bucket;
    }
    Add(r.histogram[bucket]);
    if (exception)
        Add(r.exceptions);
    else if (!ok)
        Add(r.host_errors);
    else {
        const auto code = uint32_t(result);
        // Epoll success is an event count; other admitted sync operations return status/remaining
        // sleep.
        if (code && !(op == Operation::Epoll && int32_t(code) >= 0))
            Add(r.nonzero);
        const bool status_return = op != Operation::Epoll && op != Operation::Sleep &&
                                   op != Operation::MutexConfig && op != Operation::CondConfig &&
                                   op != Operation::RwConfig;
        if (status_return && (code == 60 || code == 0x8002003c))
            Add(r.timeouts);
        if (status_return && (code == 16 || code == 35 || code == 0x80020010 || code == 0x80020023))
            Add(r.busy);
        if (status_return && (code == 4 || code == 85 || code == 0x80020004 || code == 0x80020055))
            Add(r.interrupted);
    }
    if (detail)
        for (unsigned i = 0; i < StageCount; ++i) {
            Add(r.stage_calls[i], stage_calls[i]);
            Add(r.stage_ns[i], stage_ns[i]);
        }
    if (detail && elapsed >= 1000000 && Common::Profiler::Enabled()) {
        if (end - writer.locator_window >= 1000000000) {
            writer.locator_window = end;
            writer.locator_count = 0;
        }
        if (writer.locator_count++ < 8) {
            Common::Profiler::Scope locator{"HLE.Sync.LongCall"};
            Common::Profiler::Counter("HLE.Sync.Long.Session", session->impl->id);
            Common::Profiler::Counter("HLE.Sync.Long.Operation", operation_id);
            Common::Profiler::Counter("HLE.Sync.Long.Kind", unsigned(op));
            Common::Profiler::Counter("HLE.Sync.Long.Arg0", object);
            Common::Profiler::Counter("HLE.Sync.Long.Thread", thread);
            Common::Profiler::Counter("HLE.Sync.Long.ThreadGeneration", thread_generation);
            Common::Profiler::Counter("HLE.Sync.Long.BeginNs", begin);
            Common::Profiler::Counter("HLE.Sync.Long.EndNs", end);
            Common::Profiler::Counter("HLE.Sync.Long.Result", result);
        } else
            Add(r.locator_dropped);
    }
    Add(r.completed);
    session = nullptr;
}
void Phase::Begin(Phase& phase, Call* call) noexcept {
    phase.call = call;
    static_assert(sizeof(Common::Profiler::Scope) <= sizeof(phase.trace));
    static_assert(alignof(Common::Profiler::Scope) <= alignof(Phase));
    new (phase.trace.data()) Common::Profiler::Scope(StageTags[unsigned(phase.stage)]);
    phase.begin = NowNs();
    if (phase.stage == Stage::Park || phase.stage == Stage::Reacquire) {
        // Emitted at entry so a never-returning wait still has an object/owner.
        // Detailed mode only: these locators can be expensive at high wait rates.
        Common::Profiler::Counter("HLE.Sync.Wait.Operation", call->operation_id);
        Common::Profiler::Counter("HLE.Sync.Wait.Arg0", call->object);
        Common::Profiler::Counter("HLE.Sync.Wait.Thread", call->thread);
        Common::Profiler::Counter("HLE.Sync.Wait.ThreadGeneration", call->thread_generation);
    }
}
void Phase::Finish(Phase& phase) noexcept {
    phase.call->stage_ns[unsigned(phase.stage)] += NowNs() - phase.begin;
    ++phase.call->stage_calls[unsigned(phase.stage)];
    std::launder(reinterpret_cast<Common::Profiler::Scope*>(phase.trace.data()))->~Scope();
    phase.call = nullptr;
}
void SetControl(const std::shared_ptr<Session>& session) {
    std::lock_guard lock(control_mutex);
    control = session;
}
namespace {
std::mutex fast_path_mutex;
std::string fast_path_status{"not_installed"};
} // namespace
void SetFastPathStatus(std::string status) {
    std::lock_guard lock(fast_path_mutex);
    fast_path_status = std::move(status);
}
std::string FastPathStatus() {
    std::lock_guard lock(fast_path_mutex);
    return fast_path_status;
}
std::string Command(const std::vector<std::string>& args) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard lock(control_mutex);
        s = control.lock();
    }
    if (!s)
        return "{\"status\":\"no_session\"}\n";
    if (args.empty() || args == std::vector<std::string>{"status"})
        return s->Status();
    if (args == std::vector<std::string>{"dump"}) {
        s->Emit();
        return s->Status();
    }
    if (args.size() != 2 || (args[0] != "start" && args[0] != "detail" && args[0] != "stop") ||
        args[1] != std::to_string(s->Context()))
        return "{\"error\":\"use status | dump | start/detail/stop <current context>; cumulative, "
               "no reset\"}\n";
    s->Enable(args[0] != "stop", args[0] == "detail");
    s->Emit();
    return s->Status();
}
} // namespace Core::HostRuntime::SyncMetrics
