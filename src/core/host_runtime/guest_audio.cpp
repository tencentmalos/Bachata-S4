// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <optional>
#include <thread>
#include <vector>
#include "common/profiler.h"
#include "common/thread.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_audio.h"
#include "core/host_runtime/guest_clock.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_error.h"
namespace Core::HostRuntime {
using namespace GuestCpu;
using namespace Libraries::AudioOut;
bool IsAudioNid(std::string_view nid) {
    return std::ranges::find(AudioNids, nid) != std::end(AudioNids);
}
namespace {
struct AudioFailure {
    int error;
};
} // namespace
struct GuestAudio::Impl {
    GuestAddressSpace& space;

    GuestClock& clock;
    Factory factory;
    std::mutex mutex;
    std::condition_variable_any changed;
    bool initialized{}, stopping{};
    struct Port {
        PortOut native;
        u32 handle{};
        bool closing{}, busy{};
        s32 error{};
        // Legacy sinks: two pending raw blocks and one in-flight block.
        // Callback sinks: burst-sized preallocated stereo blocks consumed directly;
        // the queue carries only their addresses, never another PCM copy.
        static constexpr size_t Capacity = 2;
        std::array<std::vector<u8>, Capacity> pending;
        std::vector<u8> in_flight;
        std::vector<std::vector<float>> prepared;
        bool callback{};
        u64 accepted{}, completed{};
        size_t head{}, queued{};
        std::jthread worker;
        // Destruction is only after RequestStop + explicit join, never under mutex.
    };
    std::map<u32, std::shared_ptr<Port>> ports;
    std::array<unsigned, 128> openings{};
    static inline std::atomic<u32> next_handle{1};
    Impl(GuestAddressSpace& space, GuestClock& clock, Factory factory)
        : space(space), clock(clock), factory(std::move(factory)) {
        if (!this->factory) {
#if defined(__ANDROID__)
            this->factory = [backend = std::make_shared<OboeAudioOut>()](PortOut& port) {
                return backend->Open(port); // one device shared by this session's ports
            };
#else
            this->factory = [](PortOut&) -> std::unique_ptr<PortBackend> { return {}; };
#endif
        }
    }
    ~Impl() {
        Stop();
        for (auto& [id, port] : ports)
            if (port->worker.joinable())
                port->worker.join();
    }
    void Stop() {
        std::lock_guard lock(mutex);
        stopping = true;
        for (auto& [id, port] : ports) {
            port->closing = true;
            port->worker.request_stop();
            port->native.impl->StopCallbacks();
        }
        changed.notify_all();
    }
    void Require(bool ok, int error) {
        if (!ok)
            throw AudioFailure{error};
    }
    void CheckAlive() {
        Require(initialized && !stopping, ORBIS_AUDIO_OUT_ERROR_NOT_INIT);
    }
    std::shared_ptr<Port> Find(u32 handle) {
        auto it = ports.find(handle);
        Require(it != ports.end() && !it->second->closing, ORBIS_AUDIO_OUT_ERROR_INVALID_PORT);
        return it->second;
    }
    void Read(u64 address, std::span<u8> bytes) {
        Require(address &&
                    bool(space.ReadData(GuestAddress{address}, std::as_writable_bytes(bytes))),
                ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER);
    }
    template <class T>
    T Read(u64 address) {
        T value{};
        Read(address, std::span<u8>(reinterpret_cast<u8*>(&value), sizeof(T)));
        return value;
    }
    template <class T>
    void Put(u64 address, const T& value) {

        auto pin = space.AcquireDataSpan({GuestAddress{address}, sizeof(T)}, true);
        Require(bool(pin), ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER);
        std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(T));
    }
    void Worker(Port& port, std::stop_token stop) {
        Common::SetCurrentThreadName("shad:AudioOut");
        const auto period = std::chrono::nanoseconds(1000000000ull * port.native.buffer_frames /
                                                     port.native.sample_rate);
        auto next = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            {
                Common::Profiler::Scope profile{"Audio.IdleWait"};
                changed.wait(lock, stop, [&] { return (port.queued && !port.error) || port.closing; });
            }
            if (port.closing || stop.stop_requested())
                break;
            // A queued buffer cannot make the worker run faster than a guest period.
            {
                Common::Profiler::Scope profile{"Audio.PaceWait"};
                changed.wait_until(lock, stop, next, [&] { return port.closing; });
            }
            if (port.closing || stop.stop_requested())
                break;
            next = std::chrono::steady_clock::now() + period;
            port.in_flight.swap(port.pending[port.head]);
            port.head = (port.head + 1) % Port::Capacity;
            --port.queued;
            const auto volume = port.native.volume;
            port.busy = true;
            Common::Profiler::Counter("Audio.Port", port.handle);
            Common::Profiler::Counter("Audio.QueuedFrames", port.queued * port.native.buffer_frames);
            changed.notify_all(); // publish the free slot before waiting on a legacy device
            lock.unlock();
            int error{};
            try {
                Common::Profiler::Scope profile{"Audio.DeviceWrite"};
                port.native.impl->SetVolume(volume);
                error = port.native.impl->OutputChecked(port.in_flight.data(), stop);
            } catch (...) {
                error = -1;
            }
            lock.lock();
            port.busy = false;
            if (error < 0) {
                port.error = ORBIS_AUDIO_OUT_ERROR_TRANS_EVENT;
                port.queued = 0;
                for (auto& block : port.pending) block.clear();
            } else {
                ++port.completed;
                port.native.last_output_time = clock.ticks.GetTimeUS(clock.origin);
            }
            changed.notify_all();
        }
        port.busy = false;
        port.queued = 0;
        port.closing = true;
        changed.notify_all();
    }
    u64 Open(const std::array<u64, 6>& a) {
        const u32 type = a[1], frames = a[3], format = u32(a[5]) & 255,
                  attributes = (a[5] >> 16) & 15;
        Require(frames && frames <= 2048 && !(frames & 255), ORBIS_AUDIO_OUT_ERROR_INVALID_SIZE);
        Require(type <= 4 || type == 126 || type == 127, ORBIS_AUDIO_OUT_ERROR_INVALID_PORT_TYPE);
        Require(u32(a[4]) == 48000, ORBIS_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ);
        Require(format <= 7 && attributes <= 2 && (attributes != 2 || type == 3) &&
                    !(type != 0 && (a[5] & 0x70000000)),
                ORBIS_AUDIO_OUT_ERROR_INVALID_FORMAT);
        Require(s32(a[2]) == 0, ORBIS_AUDIO_OUT_ERROR_INVALID_ARG);
        // Pad speaker needs a real controller endpoint; don't redirect it to the phone.
        Require(type != 4, ORBIS_AUDIO_OUT_ERROR_NOT_OPENED);
        const size_t max_ports = type == 0 ? 8 : (type == 2 || type == 3) ? 4 : 1;
        {
            std::lock_guard lock(mutex);
            CheckAlive();
            const size_t count = std::ranges::count_if(
                ports, [&](const auto& item) { return u32(item.second->native.type) == type; });
            Require(count + openings[type] < max_ports, ORBIS_AUDIO_OUT_ERROR_PORT_FULL);
            ++openings[type];
        }
        std::shared_ptr<Port> port;
        try {
            port = std::make_shared<Port>();
            port->native.userId = a[0];
            port->native.type = OrbisAudioOutPort(type);
            port->native.sample_rate = 48000;
            port->native.buffer_frames = frames;
            port->native.format_info = GetFormatInfo(OrbisAudioOutParamFormat(format));
            port->native.volume.fill(32768);
            port->native.impl = factory(port->native);
            Require(bool(port->native.impl), ORBIS_AUDIO_OUT_ERROR_TRANS_EVENT);
            port->callback = port->native.impl->UsesCallbackQueue();
            if (port->callback) {
                const auto capacity = port->native.impl->QueueCapacity();
                Require(capacity && capacity <= 32, ORBIS_AUDIO_OUT_ERROR_TRANS_EVENT);
                port->prepared.resize(capacity);
                for (auto& block : port->prepared) block.resize(size_t(frames) * 2);
            } else {
                for (auto& block : port->pending) block.resize(port->native.BufferSize());
                port->in_flight.resize(port->native.BufferSize());
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            --openings[type];
            throw;
        }
        std::lock_guard lock(mutex);
        --openings[type];
        CheckAlive();
        const u32 id = next_handle.fetch_add(1);
        Require(id && id < 0x10000000, ORBIS_AUDIO_OUT_ERROR_PORT_FULL);
        port->handle =
            0x20000000 | id; // opaque, process-unique: closed/old-session handles stay invalid
        ports.emplace(port->handle, port);
        try {
            if (!port->callback)
                port->worker = std::jthread(
                    [this, raw = port.get()](std::stop_token stop) { Worker(*raw, stop); });
        } catch (...) {
            ports.erase(port->handle);
            throw;
        }
        return port->handle;
    }
    u64 Close(u32 handle) {
        std::shared_ptr<Port> port;
        {
            std::lock_guard lock(mutex);
            CheckAlive();
            port = Find(handle);
            ports.erase(handle);
            port->closing = true;
            port->worker.request_stop();
            port->native.impl->StopCallbacks();
            changed.notify_all();
        }
        if (port->worker.joinable())
            port->worker.join();
        return 0;
    }
    void Refresh(Port& port) {
        if (!port.callback) return;
        if (port.native.impl->DeviceError()) port.error = ORBIS_AUDIO_OUT_ERROR_TRANS_EVENT;
        const auto completed = port.native.impl->CompletedBuffers();
        if (completed > port.completed) {
            port.completed = completed;
            // Compatibility timestamp: observed host consumption, not DAC time.
            port.native.last_output_time = clock.ticks.GetTimeUS(clock.origin);
        }
    }
    u64 Output(std::string_view nid, const std::array<u64, 6>& a, std::stop_token stop,
               std::optional<std::span<const u8>> host = {}, bool nonblocking = false) {
        struct Request { u32 handle, padding; u64 address; };
        std::array<Request, 25> requests{};
        std::array<std::shared_ptr<Port>, 25> selected{};
        std::array<u64, 25> drain_target{};
        const bool multi = nid == "w3PdaSTSwGE";
        const u32 count = multi ? u32(a[1]) : 1;
        Require(count && count <= requests.size(), ORBIS_AUDIO_OUT_ERROR_PORT_FULL);
        if (multi) {

            Read(a[0], std::span<u8>(reinterpret_cast<u8*>(requests.data()), count * sizeof(Request)));
        } else requests[0] = {u32(a[0]), 0, host ? !host->empty() : a[1]};
        bool callback{};
        PortBackend* publication{};
        {
            std::lock_guard lock(mutex);
            CheckAlive();
            for (u32 i = 0; i < count; ++i) {
                auto port = Find(requests[i].handle);
                for (u32 j = 0; j < i; ++j)
                    Require(selected[j] != port, ORBIS_AUDIO_OUT_ERROR_INVALID_PORT);
                Require(i == 0 || selected[0]->native.buffer_frames == port->native.buffer_frames,
                        ORBIS_AUDIO_OUT_ERROR_INVALID_SIZE);
                drain_target[i] = port->accepted;
                callback |= port->callback;
                if (port->callback) {
                    if (!publication) publication = port->native.impl.get();
                    Require(publication->QueueDomain() == port->native.impl->QueueDomain(),
                            ORBIS_AUDIO_OUT_ERROR_INVALID_ARG);
                }
                selected[i] = std::move(port);
            }
        }
        if (host) Require(host->empty() || host->size() == selected[0]->native.BufferSize(),
                          ORBIS_AUDIO_OUT_ERROR_INVALID_SIZE);
        // Fail bad pointers before any queue wait, then release the pins. The
        // input is pinned again at acceptance so no lease/VM gate spans a wait.
        {

            for (u32 i = 0; i < count; ++i) if (!host && requests[i].address) {
                    auto pin = space.AcquireDataSpan(
                        {GuestAddress{requests[i].address}, selected[i]->native.BufferSize()},
                        false);
                    Require(bool(pin), ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER);
            }
        }
        auto ready = [&] {
            if (stopping || stop.stop_requested()) return true;
            bool all = true;
            for (u32 i = 0; i < count; ++i) {
                auto& port = *selected[i];
                Refresh(port);
                if (port.closing || port.error) return true;
                if (!requests[i].address) all &= port.completed >= drain_target[i];
                else all &= port.callback ? port.native.impl->CanQueue() : port.queued < Port::Capacity;
            }
            return all;
        };
        for (;;) {
            {
                std::unique_lock lock(mutex);
                Require(!nonblocking || ready(), ORBIS_AUDIO_OUT_ERROR_BUSY);
                Common::Profiler::Scope profile{"Audio.QueueWait"};
                // Realtime consumer never locks/notifies this condition. Poll
                // its release counters only while a guest producer is waiting.
                if (callback) {
                    while (!ready()) changed.wait_for(lock, stop, std::chrono::milliseconds(1), ready);
                } else changed.wait(lock, stop, ready);
            }
            // Recheck queue credits under the audio domain lock. A lost credit
            // goes back to the cancellable wait with no buffer references.

            std::lock_guard lock(mutex);
            Require(!stop.stop_requested() && !stopping, ORBIS_AUDIO_OUT_ERROR_NOT_OPENED);
            for (u32 i = 0; i < count; ++i) {
                Refresh(*selected[i]);
                Require(!selected[i]->closing, ORBIS_AUDIO_OUT_ERROR_NOT_OPENED);
                Require(!selected[i]->error, selected[i]->error);
            }
            if (!ready()) continue;
            std::array<std::optional<PinnedSpan>, 25> pins;
            std::vector<GuestAddressSpace::DataRequest> inputs;
            for (u32 i = 0; i < count; ++i)
                if (!host && requests[i].address)
                    inputs.push_back(
                        {{GuestAddress{requests[i].address}, selected[i]->native.BufferSize()},
                         GuestPermission::Read});
            auto pinned = space.AcquireDataBatch(inputs, stop);
            Require(bool(pinned), ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER);
            size_t pin_index{};
            for (u32 i = 0; i < count; ++i)
                if (!host && requests[i].address)
                    pins[i].emplace(std::move(pinned.Value()[pin_index++]));
            for (u32 i = 0; i < count; ++i) if (requests[i].address) {
                auto& port = *selected[i];
                if (port.callback) {
                    auto& block = port.prepared[port.accepted % port.prepared.size()];
                    port.native.impl->Prepare((host ? host->data() : reinterpret_cast<const u8*>(pins[i]->Bytes().data())), port.native.volume, block);
                } else {
                    auto& block = port.pending[(port.head + port.queued) % Port::Capacity];
                    std::memcpy(block.data(), (host ? host->data() : reinterpret_cast<const u8*>(pins[i]->Bytes().data())), block.size());
                }
            }
            struct PublishBatch {
                PortBackend* backend;
                u64 epoch;
                ~PublishBatch() { if (backend) backend->CommitQueueBatch(epoch); }
            } batch{publication, publication ? publication->BeginQueueBatch() : 0};
            // From here no fallible guest memory operation remains. Closing is
            // serialized here; callback consumption can only increase credits.
            for (u32 i = 0; i < count; ++i) if (requests[i].address) {
                auto& port = *selected[i];
                if (port.callback) {
                    auto& block = port.prepared[port.accepted % port.prepared.size()];
                    Require(port.native.impl->QueuePrepared(block, batch.epoch), ORBIS_AUDIO_OUT_ERROR_TRANS_EVENT);
                } else ++port.queued;
                ++port.accepted;
                Common::Profiler::Counter("Audio.Port", port.handle);
                Common::Profiler::Counter("Audio.QueuedFrames",
                    (port.accepted - port.completed) * port.native.buffer_frames);
            }
            changed.notify_all();
            return multi ? selected[0]->native.buffer_frames
                         : requests[0].address ? selected[0]->native.buffer_frames *
                                                     selected[0]->native.format_info.num_channels : 0;
        }
    }
    u64 Dispatch(std::string_view nid, const std::array<u64, 6>& a, std::stop_token stop) {
        try {
            if (nid == "JfEPXVxhFqA") {
                std::lock_guard lock(mutex);
                Require(!stopping, ORBIS_AUDIO_OUT_ERROR_NOT_INIT);
                Require(!initialized, ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT);
                initialized = true;
                return 0;
            }
            if (nid == "ekNvsT22rsY")
                return Open(a);
            if (nid == "s1--uE9mBFw")
                return Close(u32(a[0]));
            if (nid == "QOQtbeDqsT4" || nid == "w3PdaSTSwGE")
                return Output(nid, a, stop);
            if (nid == "b+uAV89IlxE") {
                s32 volume{};
                {

                    volume = Read<s32>(a[2]);
                }
                Require(volume >= 0 && volume <= 32768, ORBIS_AUDIO_OUT_ERROR_INVALID_VOLUME);
                Require(!(u32(a[1]) & ~255u), ORBIS_AUDIO_OUT_ERROR_INVALID_FLAG);
                std::lock_guard lock(mutex);
                CheckAlive();
                auto port = Find(u32(a[0]));
                for (u32 i = 0; i < 8; ++i)
                    if (a[1] & (1u << i))
                        port->native.volume[i] = volume;
                return 0;
            }
            // Compute native scalar snapshots under domain lock, release it before
            // copying the snapshot to its retained guest output.
            if (nid == "Ptlts326pds") {
                u64 time{};
                {
                    std::lock_guard lock(mutex);
                    CheckAlive();
                    auto port = Find(u32(a[0]));
                    Refresh(*port);
                    time = port->native.last_output_time;
                }
                Put(a[1], time);
                return 0;
            }
            if (nid == "GrQ9s4IrNaQ") {
                OrbisAudioOutPortState state{};
                {
                    std::lock_guard lock(mutex);
                    CheckAlive();
                    auto port = Find(u32(a[0]));
                    Refresh(*port);
                    Require(!port->error, port->error);
                    state.output = ORBIS_AUDIO_OUT_STATE_OUTPUT_CONNECTED_PRIMARY;
                    state.channel = std::min<u32>(2, port->native.format_info.num_channels);
                    state.volume = -1;
                }
                Put(a[1], state);
                return 0;
            }
            if (nid == "R5hemoKKID8") {
                {
                    std::lock_guard lock(mutex);
                    CheckAlive();
                }
                Put(a[0], OrbisAudioOutSystemState{});
                return 0; // desktop compatibility, no measured loudness
            }
            return u32(ORBIS_AUDIO_OUT_ERROR_INVALID_ARG);
        } catch (AudioFailure e) {
            return u32(e.error);
        } catch (const std::bad_alloc&) {
            return u32(ORBIS_AUDIO_OUT_ERROR_OUT_OF_MEMORY);
        } catch (const std::exception&) {
            return u32(ORBIS_AUDIO_OUT_ERROR_TRANS_EVENT);
        }
    }
};
GuestAudio::GuestAudio(GuestAddressSpace& space, GuestClock& clock, Factory factory)
    : impl(std::make_unique<Impl>(space, clock, std::move(factory))) {}
GuestAudio::~GuestAudio() = default;
u64 GuestAudio::Dispatch(std::string_view nid, const std::array<u64, 6>& a, std::stop_token stop) {
    return impl->Dispatch(nid, a, stop);
}
u64 GuestAudio::OutputHost(u32 handle, std::span<const u8> pcm, std::stop_token stop, bool nonblocking) {
    try { return impl->Output("QOQtbeDqsT4", {handle}, stop, pcm, nonblocking); }
    catch (AudioFailure e) { return u32(e.error); }
    catch (const std::bad_alloc&) { return u32(ORBIS_AUDIO_OUT_ERROR_OUT_OF_MEMORY); }
    catch (const std::exception&) { return u32(ORBIS_AUDIO_OUT_ERROR_TRANS_EVENT); }
}
void GuestAudio::RequestStop() {
    impl->Stop();
}
} // namespace Core::HostRuntime
