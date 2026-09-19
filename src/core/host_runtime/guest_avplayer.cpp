// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <optional>
#include <thread>
#include "common/logging/log.h"
#include "common/profiler.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_avplayer.h"
#include "core/libraries/avplayer/avplayer_error.h"
#include "core/libraries/avplayer/avplayer_impl.h"

namespace Core::HostRuntime {
using namespace GuestCpu;
using namespace Libraries::AvPlayer;
namespace {
struct Failure {
    s32 code;
    const char* reason = "invalid parameter";
};
void Need(bool ok, s32 code = ORBIS_AVPLAYER_ERROR_INVALID_PARAMS,
          const char* reason = "invalid parameter") {
    if (!ok)
        throw Failure{code, reason};
}
constexpr size_t MaxBuffer = 64 << 20, MaxResident = 256 << 20;
thread_local void* callback_player{};
thread_local bool event_callback{};
struct Identity {
    u64 begin, end, generation;
};
static_assert(sizeof(AvPlayerInitData) == 120);
static_assert(sizeof(AvPlayerInitDataEx) == 176);
static_assert(sizeof(AvPlayerFrameInfo) == 40);
static_assert(sizeof(AvPlayerFrameInfoEx) == 104);
static_assert(sizeof(AvPlayerSourceDetails) == 128);
} // namespace
bool IsAvPlayerNid(std::string_view nid) {
    return std::ranges::find(AvPlayerNids, nid) != std::end(AvPlayerNids);
}
struct GuestAvPlayer::Impl {
    GuestAddressSpace& space;

    std::function<Callbacks()> make_callbacks;
    std::mutex mutex;
    bool stopping{};
    u64 next_id{1};
    struct Player;
    std::map<u64, std::shared_ptr<Player>> players;
    Impl(GuestAddressSpace& space, std::function<Callbacks()> factory)
        : space(space), make_callbacks(std::move(factory)) {}
    bool Valid(u64 at, size_t n, GuestPermission perm) {
        return at && n && bool(space.ValidateRange({GuestAddress{at}, n}, perm));
    }
    template <class T>
    T Read(u64 at) {
        T out{};
        Need(bool(space.ReadData(GuestAddress{at}, std::as_writable_bytes(std::span{&out, 1}))));
        return out;
    }
    void Put(u64 at, const void* p, size_t n) {

        Need(bool(
            space.WriteData(GuestAddress{at}, std::span{static_cast<const std::byte*>(p), n})));
    }
    template <class T>
    void Put(u64 at, const T& value) {
        Put(at, &value, sizeof(value));
    }
    std::string String(u64 at, size_t limit = 4096) {
        Need(at && at <= UINT64_MAX - limit);
        std::string result;
        for (size_t i = 0; i < limit; ++i) {
            const char c = Read<char>(at + i);
            if (!c)
                return result;
            result += c;
        }
        throw Failure{ORBIS_AVPLAYER_ERROR_INVALID_PARAMS};
    }
    std::vector<Identity> Identify(u64 at, size_t n) {
        Need(Valid(at, n, GuestPermission::Read | GuestPermission::Write));
        std::vector<Identity> result;
        for (const u64 end = at + n; at < end;) {
            auto mapping = space.Query(GuestAddress{at});
            Need(bool(mapping));
            const u64 next = std::min(end, mapping.Value().range.End());
            Need(next > at);
            result.push_back({at, next, mapping.Value().mapping_generation});
            at = next;
        }
        return result;
    }
    void Check(const std::vector<Identity>& identity) {
        for (const auto& part : identity) {
            Need(Valid(part.begin, part.end - part.begin,
                       GuestPermission::Read | GuestPermission::Write));
            for (u64 at = part.begin; at < part.end;) {
                auto map = space.Query(GuestAddress{at});
                Need(map && map.Value().mapping_generation == part.generation);
                const u64 next = std::min(part.end, map.Value().range.End());
                Need(next > at);
                at = next;
            }
        }
    }
    struct Player {
        Impl& owner;
        Callbacks callbacks;
        AvPlayerInitData guest{};
        std::array<char, 4> language{};
        std::recursive_timed_mutex api;
        std::mutex queue_mutex, buffers_mutex;
        std::condition_variable changed;
        std::deque<std::packaged_task<u64()>> queue;
        std::atomic_bool stopped{}, closing{};
        bool ready{};
        std::exception_ptr startup_error;
        u64 scratch{};
        u64 handle{}; // immutable after publication; lifecycle diagnostics only
        std::optional<bool> queried_active; // API mutex protects query diagnostics
        u64 video_frames{}, audio_frames{};
        size_t scratch_used{}; // callback worker only; nested event -> file calls preserve payload
        struct Scratch {
            Player& player;
            size_t old;
            u64 address;
            Scratch(Player& player, size_t bytes) : player(player), old(player.scratch_used) {
                Need(bytes && bytes <= ScratchSize && old <= ScratchSize - bytes);
                address = player.scratch + old;
                player.scratch_used += (bytes + 7) & ~size_t(7);
                Need(player.scratch_used <= ScratchSize);
            }
            ~Scratch() {
                player.scratch_used = old;
            }
        };
        std::thread worker;
        std::unique_ptr<AvPlayer> native;
        struct Buffer {
            u64 guest{};
            size_t size{};
            bool texture{};
            std::unique_ptr<u8, decltype(&std::free)> host{nullptr, &std::free};
            std::vector<Identity> identity;
        };
        std::map<void*, Buffer> buffers;
        size_t resident{};
        Player(Impl& owner, const AvPlayerInitData& data)
            : owner(owner), callbacks(owner.make_callbacks()), guest(data) {}
        void Open() {
            if (guest.default_language) {
                language =
                    owner.Read<std::array<char, 4>>(reinterpret_cast<u64>(guest.default_language));
            }
            worker = std::thread([this] {
                callback_player = this;
                try {
                    scratch =
                        callbacks.begin(reinterpret_cast<u64>(guest.memory_replacement.allocate));
                    {
                        std::lock_guard lock(queue_mutex);
                        ready = true;
                    }
                    changed.notify_all();
                    for (;;) {
                        std::packaged_task<u64()> task;
                        {
                            std::unique_lock lock(queue_mutex);
                            changed.wait(lock, [this] { return stopped || !queue.empty(); });
                            if (stopped)
                                break;
                            task = std::move(queue.front());
                            queue.pop_front();
                        }
                        task();
                    }
                } catch (...) {
                    std::lock_guard lock(queue_mutex);
                    startup_error = std::current_exception();
                    ready = true;
                }
                {
                    std::lock_guard lock(queue_mutex);
                    queue.clear();
                }
                changed.notify_all();
                // begin may fail after allocating a guest owner; end is idempotent
                // for an empty owner and must also clean up partial attachment.
                callbacks.end();
                callback_player = nullptr;
            });
            {
                std::unique_lock lock(queue_mutex);
                changed.wait(lock, [this] { return ready; });
                if (startup_error)
                    std::rethrow_exception(startup_error);
            }
            AvPlayerInitData data = guest;
            data.memory_replacement = {this, Allocate, Deallocate, AllocateTexture,
                                       DeallocateTexture};
            const auto& f = guest.file_replacement;
            if (f.open && f.close && f.read_offset && f.size)
                data.file_replacement = {this, OpenFile, CloseFile, ReadFile, SizeFile};
            else
                data.file_replacement = {}; // same partial-replacement policy as desktop
            data.event_replacement = guest.event_replacement.event_callback
                                         ? AvPlayerEventReplacement{this, Event}
                                         : AvPlayerEventReplacement{};
            data.default_language = language.data();
            native = std::make_unique<AvPlayer>(data);
        }
        ~Player() {
            Close();
        }
        bool Close() {
            bool orderly = !stopped;
            closing = true;
            // Close native workers while callback owner still serves deallocation/file close.
            // Session cancellation has already interrupted that owner and drops all callbacks.
            // Drain any entered event first. A nonreturning guest callback is
            // interrupted before joining native workers that may need callbacks.
            if (worker.joinable() && !stopped) {
                std::packaged_task<u64()> barrier([] { return u64{}; });
                auto done = barrier.get_future();
                {
                    std::lock_guard lock(queue_mutex);
                    queue.push_back(std::move(barrier));
                }
                changed.notify_one();
                if (done.wait_for(std::chrono::milliseconds(200)) != std::future_status::ready) {
                    orderly = false;
                    Stop();
                }
            }
            native.reset();
            orderly &= !stopped;
            Stop(false);
            if (worker.joinable())
                worker.join();
            return orderly;
        }
        void Stop(bool interrupt = true) {
            stopped = true;
            if (interrupt)
                callbacks.cancel();
            {
                std::lock_guard lock(queue_mutex);
                queue.clear();
            }
            changed.notify_all();
        }
        u64 OnWorker(std::function<u64()> job) {
            Common::Profiler::Scope profile{"AvPlayer.CallbackQueueWait"};
            Need(!stopped, ORBIS_AVPLAYER_ERROR_OPERATION_FAILED);
            if (callback_player == this)
                return job();
            std::packaged_task<u64()> task(std::move(job));
            auto result = task.get_future();
            {
                std::lock_guard lock(queue_mutex);
                Need(!stopped, ORBIS_AVPLAYER_ERROR_OPERATION_FAILED);
                queue.push_back(std::move(task));
            }
            changed.notify_one();
            return result.get();
        }
        u64 Invoke(u64 pc, std::initializer_list<u64> args, bool is_event = false) {
            Common::Profiler::Scope profile{is_event ? "AvPlayer.GuestEvent" : "AvPlayer.GuestCallback"};
            Need(pc && !stopped, ORBIS_AVPLAYER_ERROR_OPERATION_FAILED);
            const bool previous = event_callback;
            event_callback = is_event;
            try {
                const auto value =
                    callbacks.call(pc, std::span<const u64>(args.begin(), args.size()));
                event_callback = previous;
                return value;
            } catch (...) {
                event_callback = previous;
                throw;
            }
        }
        static void* PS4_SYSV_ABI Allocate(void* p, u32 a, u32 n) {
            return static_cast<Player*>(p)->Alloc(a, n, false);
        }
        static void* PS4_SYSV_ABI AllocateTexture(void* p, u32 a, u32 n) {
            return static_cast<Player*>(p)->Alloc(a, n, true);
        }
        void* Alloc(u32 align, u32 size, bool texture) noexcept {
            try {
                Need(size && size <= MaxBuffer && align && !(align & (align - 1)));
                {
                    std::lock_guard lock(buffers_mutex);
                    Need(resident <= MaxResident - size, ORBIS_AVPLAYER_ERROR_NO_MEMORY);
                    resident += size;
                }
                Buffer b;
                b.size = size;
                b.texture = texture;
                void* host{};
                if (posix_memalign(&host, std::max<size_t>(align, sizeof(void*)), size)) {
                    std::lock_guard lock(buffers_mutex);
                    resident -= size;
                    return nullptr;
                }
                b.host.reset(static_cast<u8*>(host));
                std::memset(host, 0, size);
                try {
                    b.guest = OnWorker([&] {
                        const auto& m = guest.memory_replacement;
                        return Invoke(
                            reinterpret_cast<u64>(texture ? m.allocate_texture : m.allocate),
                            {reinterpret_cast<u64>(m.object_ptr), align, size});
                    });
                    Need(b.guest && !(b.guest & (align - 1)));

                    b.identity = owner.Identify(b.guest, size);
                    std::lock_guard lock(buffers_mutex);
                    buffers.emplace(host, std::move(b));
                } catch (...) {
                    std::lock_guard lock(buffers_mutex);
                    resident -= size;
                    throw;
                }
                return host;
            } catch (...) {
                return nullptr;
            }
        }
        static void PS4_SYSV_ABI Deallocate(void* p, void* memory) {
            static_cast<Player*>(p)->Free(memory);
        }
        static void PS4_SYSV_ABI DeallocateTexture(void* p, void* memory) {
            static_cast<Player*>(p)->Free(memory);
        }
        void Free(void* memory) noexcept {
            Buffer b;
            {
                std::lock_guard lock(buffers_mutex);
                auto it = buffers.find(memory);
                if (it == buffers.end())
                    return;
                b = std::move(it->second);
                resident -= b.size;
                buffers.erase(it);
            }
            if (stopped)
                return;
            try {
                // Never free a replacement mapping that only reuses the old VA.
                {

                    owner.Check(b.identity);
                }
                OnWorker([&] {
                    const auto& m = guest.memory_replacement;
                    return Invoke(
                        reinterpret_cast<u64>(b.texture ? m.deallocate_texture : m.deallocate),
                        {reinterpret_cast<u64>(m.object_ptr), b.guest});
                });
            } catch (...) { /* cancellation/fault propagated by the owned callback context */
            }
        }
        static s32 PS4_SYSV_ABI OpenFile(void* p, const char* path) {
            auto& self = *static_cast<Player*>(p);
            try {
                return s32(self.OnWorker([&] {
                    const size_t n = strnlen(path, 4096);
                    Need(n < 4096);
                    Scratch slot(self, n + 1);
                    self.owner.Put(slot.address, path, n + 1);
                    return self.Invoke(
                        reinterpret_cast<u64>(self.guest.file_replacement.open),
                        {reinterpret_cast<u64>(self.guest.file_replacement.object_ptr),
                         slot.address});
                }));
            } catch (...) {
                return -1;
            }
        }
        static s32 PS4_SYSV_ABI CloseFile(void* p) {
            auto& self = *static_cast<Player*>(p);
            try {
                return s32(self.OnWorker([&] {
                    return self.Invoke(
                        reinterpret_cast<u64>(self.guest.file_replacement.close),
                        {reinterpret_cast<u64>(self.guest.file_replacement.object_ptr)});
                }));
            } catch (...) {
                return -1;
            }
        }
        static u64 PS4_SYSV_ABI SizeFile(void* p) {
            auto& self = *static_cast<Player*>(p);
            try {
                return self.OnWorker([&] {
                    return self.Invoke(
                        reinterpret_cast<u64>(self.guest.file_replacement.size),
                        {reinterpret_cast<u64>(self.guest.file_replacement.object_ptr)});
                });
            } catch (...) {
                return 0;
            }
        }
        static s32 PS4_SYSV_ABI ReadFile(void* p, u8* data, u64 offset, u32 size) {
            auto& self = *static_cast<Player*>(p);
            try {
                return s32(self.OnWorker([&] {
                    Need(size && size <= ScratchSize && offset <= UINT64_MAX - size);
                    Scratch slot(self, size);
                    const s32 got = s32(
                        self.Invoke(reinterpret_cast<u64>(self.guest.file_replacement.read_offset),
                                    {reinterpret_cast<u64>(self.guest.file_replacement.object_ptr),
                                     slot.address, offset, size}));
                    if (got < 0)
                        return u64(u32(got));
                    Need(u32(got) <= size);
                    if (got)
                        Need(bool(self.owner.space.ReadData(
                            GuestAddress{slot.address},
                            std::span{reinterpret_cast<std::byte*>(data), size_t(got)})));
                    return u64(got);
                }));
            } catch (...) {
                return -1;
            }
        }
        static void PS4_SYSV_ABI Event(void* p, AvPlayerEvents kind, s32 source, void* data) {
            auto& self = *static_cast<Player*>(p);
            if (self.closing || self.stopped)
                return;
            const bool payload = data && kind == AvPlayerEvents::WarningId;
            const u32 value = payload ? *static_cast<const u32*>(data) : 0;
            // Do not park the native controller under its event/source locks on a
            // guest callback that may itself select streams or start playback.
            std::packaged_task<u64()> task([&self, kind, source, payload, value] {
                // Close rejects new events at enqueue and drains this queue
                // before destroying native state. Already accepted events must
                // still run: dropping a queued StateStop can strand the guest
                // waiting for completion after a movie has already ended.
                // Global Stop alone cancels pending callbacks.
                if (self.stopped)
                    return u64{};
                event_callback = true;
                LOG_INFO(Lib_AvPlayer, "Guest event begin player={} event={:#x} callback={:#x}",
                         self.handle, u32(kind),
                         reinterpret_cast<u64>(self.guest.event_replacement.event_callback));
                try {
                    Scratch slot(self, sizeof(value));
                    if (payload)
                        self.owner.Put(slot.address, value);
                    const auto result = self.Invoke(
                        reinterpret_cast<u64>(self.guest.event_replacement.event_callback),
                        {reinterpret_cast<u64>(self.guest.event_replacement.object_ptr), u32(kind),
                         u32(source), payload ? slot.address : 0},
                        true);
                    event_callback = false;
                    LOG_INFO(Lib_AvPlayer, "Guest event returned player={} event={:#x}",
                             self.handle, u32(kind));
                    return result;
                } catch (...) {
                    event_callback = false;
                    LOG_WARNING(Lib_AvPlayer, "Guest event failed player={} event={:#x}",
                                self.handle, u32(kind));
                    throw;
                }
            });
            {
                std::lock_guard lock(self.queue_mutex);
                if (self.stopped || self.closing)
                    return;
                if (self.queue.size() >= 256) {
                    self.stopped = true;
                    self.callbacks.cancel();
                } else
                    self.queue.push_back(std::move(task));
            }
            self.changed.notify_one();
        }
        template <class T>
        void Publish(T& frame, u64 output) {

            Need(owner.Valid(output, sizeof(T), GuestPermission::Write));
            std::lock_guard lock(buffers_mutex);
            auto it = buffers.find(frame.p_data);
            Need(it != buffers.end());
            const auto& b = it->second;
            owner.Check(b.identity);
            // Keep native current-frame ownership until the next polling call;
            // decoder threads cannot reuse it while this short copy executes.
            owner.Put(b.guest, b.host.get(), b.size);
            if (callbacks.invalidate)
                callbacks.invalidate(b.guest, b.size);
            frame.p_data = reinterpret_cast<decltype(frame.p_data)>(b.guest);
            owner.Put(output, frame);
        }
    };
    void Stop() {
        std::lock_guard lock(mutex);
        stopping = true;
        for (auto& [id, p] : players)
            p->Stop();
    }
    ~Impl() {
        Stop();
        players.clear();
    }
    u64 Dispatch(std::string_view nid, const std::array<u64, 6>& a) {
        // Exact desktop compatibility: callback is neither stored nor invoked.
        if (nid == "eBTreZ84JFY")
            return 0;
        if (nid == "aS66RI0gGgo" || nid == "o9eWRkSL+M4") {
            AvPlayerInitData data{};
            if (nid == "aS66RI0gGgo")
                data = Read<AvPlayerInitData>(a[0]);
            else {
                const auto ex = Read<AvPlayerInitDataEx>(a[0]);
                Need(ex.this_size >= sizeof(ex) && ex.this_size <= 4096 &&
                     Valid(a[1], 8, GuestPermission::Write));
                data.memory_replacement = ex.memory_replacement;
                data.file_replacement = ex.file_replacement;
                data.event_replacement = ex.event_replacement;
                data.default_language = ex.default_language;
                data.auto_start = ex.auto_start;
                data.num_output_video_framebuffers = ex.num_output_video_framebuffers;
            }
            const auto& m = data.memory_replacement;
            for (u64 pc : {reinterpret_cast<u64>(m.allocate), reinterpret_cast<u64>(m.deallocate),
                           reinterpret_cast<u64>(m.allocate_texture),
                           reinterpret_cast<u64>(m.deallocate_texture)})
                Need(Valid(pc, 1, GuestPermission::Execute));
            const auto& f = data.file_replacement;
            if (f.open && f.close && f.read_offset && f.size)
                for (u64 pc : {reinterpret_cast<u64>(f.open), reinterpret_cast<u64>(f.close),
                               reinterpret_cast<u64>(f.read_offset), reinterpret_cast<u64>(f.size)})
                    Need(Valid(pc, 1, GuestPermission::Execute));
            if (data.event_replacement.event_callback)
                Need(Valid(reinterpret_cast<u64>(data.event_replacement.event_callback), 1,
                           GuestPermission::Execute));
            {
                std::lock_guard lock(mutex);
                Need(!stopping && players.size() < 8);
            }
            auto p = std::make_shared<Player>(*this, data);
            p->Open();
            u64 id{};
            {
                std::lock_guard lock(mutex);
                Need(!stopping && players.size() < 8);
                id = next_id++; // no native pointer and no same-session handle reuse
                p->handle = id;
                players.emplace(id, p);
                LOG_INFO(Lib_AvPlayer, "Guest player opened id={} event_callback={:#x}",
                         id, reinterpret_cast<u64>(data.event_replacement.event_callback));
            }
            if (nid == "aS66RI0gGgo")
                return id;
            try {
                Put(a[1], id);
            } catch (...) {
                std::lock_guard lock(mutex);
                players.erase(id);
                throw;
            }
            return 0;
        }
        std::shared_ptr<Player> p;
        {
            std::lock_guard lock(mutex);
            auto it = players.find(a[0]);
            Need(!stopping && it != players.end(), ORBIS_AVPLAYER_ERROR_INVALID_PARAMS,
                 "unknown or stopped player");
            p = it->second;
        }
        if (callback_player == p.get()) {
            // Allocator/file callbacks run while native decoder locks may be held.
            Need(event_callback && nid != "NkJwDzKmIlw" && nid != "ZC17w3vB5Lo",
                 ORBIS_AVPLAYER_ERROR_OPERATION_FAILED);
        }
        std::unique_lock lock(p->api, std::defer_lock);
        if (callback_player == p.get()) {
            // A native API may be waiting for this callback worker. Blocking
            // here would form an API -> callback -> API cycle.
            lock.try_lock();
        } else {
            // Desktop queries wait for source ownership; contention is not EOF
            // or an operation failure. In particular IsActive must not return
            // false merely because the audio owner is publishing a frame.
            // Never retain a VM gate/pin here, and allow session Stop to cancel
            // a waiter even if the current API is waiting for guest callbacks.
            Common::Profiler::Scope profile{"AvPlayer.ApiWait"};
            while (!p->closing && !p->stopped &&
                   !lock.try_lock_for(std::chrono::milliseconds(1))) {}
        }
        Need(lock.owns_lock() && !p->closing && !p->stopped, ORBIS_AVPLAYER_ERROR_OPERATION_FAILED);
        if (nid == "NkJwDzKmIlw") {
            p->closing = true;
            {
                std::lock_guard table(mutex);
                players.erase(a[0]);
            }
            LOG_INFO(Lib_AvPlayer, "Guest Close begin player={}", p->handle);
            const bool closed = p->Close();
            LOG_INFO(Lib_AvPlayer, "Guest Close completed player={} orderly={}", p->handle, closed);
            return closed ? 0 : u32(ORBIS_AVPLAYER_ERROR_OPERATION_FAILED);
            // Workers joined even if another rejected caller retains a shared handle.
        }
        auto& player = *p->native;
        if (nid == "HD1YKVU26-M")
            return u32(player.PostInit(Read<AvPlayerPostInitData>(a[1])));
        if (nid == "KMcEa+rHsIo" || nid == "x8uvuFOPZhU") {
            std::string path;
            auto type = AvPlayerSourceType::Unknown;
            if (nid == "KMcEa+rHsIo")
                path = String(a[1]);
            else {
                Need(a[1] == u32(AvPlayerUriType::Source));
                const auto d = Read<AvPlayerSourceDetails>(a[2]);
                Need(d.uri.length && d.uri.length < 4096);
                path.resize(d.uri.length);
                Need(bool(
                    space.ReadData(GuestAddress{reinterpret_cast<u64>(d.uri.name)},
                                   std::as_writable_bytes(std::span{path.data(), path.size()}))));
                if (path.back() == '\0')
                    path.pop_back();
                Need(path.find('\0') == std::string::npos);
                type = d.source_type;
            }
            // Local mounted content only. Do not let an FFmpeg protocol URL escape
            // the guest mount policy; HLS is also unsupported on desktop.
            Need(path.starts_with('/') && path.find("://") == std::string::npos &&
                     type != AvPlayerSourceType::Hls,
                 ORBIS_AVPLAYER_ERROR_NOT_SUPPORTED);
            return u32(player.AddSourceEx(path, type));
        }
        if (nid == "hdTyRzCXQeQ")
            return u32(player.GetStreamCount());
        if (nid == "d8FcbzfAdQw") {
            Need(Valid(a[2], sizeof(AvPlayerStreamInfo), GuestPermission::Write),
                 ORBIS_AVPLAYER_ERROR_INVALID_PARAMS, "stream-info output mapping");
            AvPlayerStreamInfo info{};
            const s32 rc = player.GetStreamInfo(u32(a[1]), info);
            if (!rc)
                Put(a[2], info);
            return u32(rc);
        }
        if (nid == "ODJK2sn9w4A")
            return u32(player.EnableStream(u32(a[1])));
        if (nid == "ET4Gr-Uu07s")
            return u32(player.Start());
        if (nid == "ZC17w3vB5Lo")
            return u32(player.Stop());
        if (nid == "9y5v+fGN4Wk")
            return u32(player.Pause());
        if (nid == "w5moABNwnRY")
            return u32(player.Resume());
        if (nid == "UbQoYawOsfY") {
            const bool active = player.IsActive();
            if (!p->queried_active || *p->queried_active != active) {
                LOG_INFO(Lib_AvPlayer,
                         "Guest IsActive player={} active={} video_frames={} audio_frames={} time_ms={}",
                         p->handle, active, p->video_frames, p->audio_frames, player.CurrentTime());
                p->queried_active = active;
            }
            return active;
        }
        if (nid == "wwM99gjFf1Y")
            return player.CurrentTime();
        if (nid == "OVths0xGfho")
            return player.SetLooping(bool(a[1])) ? 0 : u32(ORBIS_AVPLAYER_ERROR_OPERATION_FAILED);
        if (nid == "k-q+xOxdc3E") {
            Need(a[1] <= 1);
            return u32(player.SetAvSyncMode(AvPlayerAvSyncMode(a[1])));
        }
        if (nid == "JdksQu8pNdQ") {
            Need(Valid(a[1], sizeof(AvPlayerFrameInfoEx), GuestPermission::Write));
            AvPlayerFrameInfoEx frame{};
            if (!player.GetVideoData(frame))
                return 0;
            p->Publish(frame, a[1]);
            ++p->video_frames;
            return 1;
        }
        if (nid == "o3+RWnHViSg" || nid == "Wnp1OVcrZgk") {
            Need(Valid(a[1], sizeof(AvPlayerFrameInfo), GuestPermission::Write));
            AvPlayerFrameInfo frame{};
            const bool ok =
                nid == "Wnp1OVcrZgk" ? player.GetAudioData(frame) : player.GetVideoData(frame);
            if (!ok)
                return 0;
            p->Publish(frame, a[1]);
            if (nid == "Wnp1OVcrZgk") ++p->audio_frames;
            else ++p->video_frames;
            return 1;
        }
        return u32(ORBIS_AVPLAYER_ERROR_NOT_SUPPORTED);
    }
};
GuestAvPlayer::GuestAvPlayer(GuestAddressSpace& space, std::function<Callbacks()> factory)
    : impl(std::make_unique<Impl>(space, std::move(factory))) {}
GuestAvPlayer::~GuestAvPlayer() = default;
void GuestAvPlayer::RequestStop() {
    impl->Stop();
}
u64 GuestAvPlayer::Dispatch(std::string_view nid, const std::array<u64, 6>& args) {
    try {
        return impl->Dispatch(nid, args);
    } catch (...) {
        const bool zero_on_failure = nid == "aS66RI0gGgo" || nid == "UbQoYawOsfY" ||
                                    nid == "Wnp1OVcrZgk" || nid == "o3+RWnHViSg" ||
                                    nid == "JdksQu8pNdQ";
        try {
            throw;
        } catch (const Failure& e) {
            // Failed boolean queries must remain observable, without flooding
            // the log when a guest polls the same invalid handle every frame.
            static std::atomic_uint reported{};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 32)
                LOG_WARNING(Lib_AvPlayer,
                            "Guest AvPlayer {} rejected: {:#x} {} args={:#x},{:#x},{:#x}",
                            nid, u32(e.code), e.reason, args[0], args[1], args[2]);
            return zero_on_failure ? 0 : u32(e.code);
        } catch (const std::bad_alloc&) {
            return zero_on_failure ? 0 : u32(ORBIS_AVPLAYER_ERROR_NO_MEMORY);
        } catch (...) {
            return zero_on_failure ? 0 : u32(ORBIS_AVPLAYER_ERROR_OPERATION_FAILED);
        }
    }
}
} // namespace Core::HostRuntime
