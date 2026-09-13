// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <thread>
#include <vector>
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
    std::recursive_mutex& vm;
    GuestClock& clock;
    Factory factory;
    std::mutex mutex;
    std::condition_variable_any changed;
    bool initialized{}, stopping{};
    struct Port {
        PortOut native;
        u32 handle{};
        bool closing{}, ready{}, busy{};
        s32 error{};
        std::vector<u8> pending;
        std::jthread worker;
        // Destruction is only after RequestStop + explicit join, never under mutex.
    };
    std::map<u32, std::shared_ptr<Port>> ports;
    std::array<unsigned, 128> openings{};
    static inline std::atomic<u32> next_handle{1};
    Impl(GuestAddressSpace& space, std::recursive_mutex& vm, GuestClock& clock, Factory factory)
        : space(space), vm(vm), clock(clock), factory(std::move(factory)) {
        if (!this->factory)
            this->factory = [](PortOut& port) -> std::unique_ptr<PortBackend> {
#if defined(__ANDROID__)
                return AAudioOut{}.Open(port);
#else
                return {}; // no silent null-audio success on unsupported hosts
#endif
            };
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
        Require(address && bool(space.Read(GuestAddress{address}, std::as_writable_bytes(bytes))),
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
        std::lock_guard gate(vm);
        auto pin = space.AcquirePinnedSpan({GuestAddress{address}, sizeof(T)}, true);
        Require(bool(pin), ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER);
        std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(T));
    }
    void Worker(Port& port, std::stop_token stop) {
        const auto period = std::chrono::nanoseconds(1000000000ull * port.native.buffer_frames /
                                                     port.native.sample_rate);
        auto next = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            changed.wait(lock, stop, [&] { return port.ready || port.closing; });
            if (port.closing || stop.stop_requested())
                break;
            // A queued buffer cannot make the worker run faster than a guest period.
            changed.wait_until(lock, stop, next, [&] { return port.closing; });
            if (port.closing || stop.stop_requested())
                break;
            next = std::chrono::steady_clock::now() + period;
            auto data = std::move(port.pending);
            const auto volume = port.native.volume;
            port.ready = false;
            port.busy = true;
            lock.unlock();
            int error{};
            try {
                port.native.impl->SetVolume(volume);
                error = port.native.impl->OutputChecked(data.data(), stop);
            } catch (...) {
                error = -1;
            }
            lock.lock();
            port.busy = false;
            if (error < 0) {
                port.error = ORBIS_AUDIO_OUT_ERROR_TRANS_EVENT;
            } else
                port.native.last_output_time = clock.ticks.GetTimeUS(clock.origin);
            changed.notify_all();
        }
        port.busy = false;
        port.ready = false;
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
            changed.notify_all();
        }
        if (port->worker.joinable())
            port->worker.join();
        return 0;
    }
    u64 Output(std::string_view nid, const std::array<u64, 6>& a, std::stop_token stop) {
        struct Request {
            u32 handle, padding;
            u64 address;
        };
        std::vector<Request> requests;
        std::vector<std::shared_ptr<Port>> selected;
        std::vector<std::vector<u8>> pcm;
        const bool multi = nid == "w3PdaSTSwGE";
        if (multi) {
            Require(u32(a[1]) && u32(a[1]) <= 25, ORBIS_AUDIO_OUT_ERROR_PORT_FULL);
            requests.resize(u32(a[1]));
            std::lock_guard gate(vm);
            Read(a[0], std::span<u8>(reinterpret_cast<u8*>(requests.data()),
                                     requests.size() * sizeof(Request)));
        } else
            requests.push_back({u32(a[0]), 0, a[1]});
        {
            std::lock_guard lock(mutex);
            CheckAlive();
            for (auto request : requests) {
                auto port = Find(request.handle);
                Require(std::ranges::find(selected, port) == selected.end(),
                        ORBIS_AUDIO_OUT_ERROR_INVALID_PORT);
                Require(selected.empty() ||
                            selected.front()->native.buffer_frames == port->native.buffer_frames,
                        ORBIS_AUDIO_OUT_ERROR_INVALID_SIZE);
                selected.push_back(port);
            }
        }
        // Copy every input before waiting or enqueueing any port. No guest pointers
        // or mapping leases survive this short VM-protected copy.
        {
            std::lock_guard gate(vm);
            for (size_t i = 0; i < requests.size(); ++i) {
                pcm.emplace_back();
                if (requests[i].address) {
                    pcm.back().resize(selected[i]->native.BufferSize());
                    Read(requests[i].address, pcm.back());
                }
            }
        }
        std::unique_lock lock(mutex);
        changed.wait(lock, stop, [&] {
            return stopping ||
                   std::ranges::any_of(selected,
                                       [](const auto& p) { return p->closing || p->error; }) ||
                   std::ranges::all_of(selected,
                                       [](const auto& p) { return !p->ready && !p->busy; });
        });
        Require(!stop.stop_requested() && !stopping, ORBIS_AUDIO_OUT_ERROR_NOT_OPENED);
        for (const auto& port : selected) {
            Require(!port->closing, ORBIS_AUDIO_OUT_ERROR_NOT_OPENED);
            Require(!port->error, port->error);
        }
        for (size_t i = 0; i < selected.size(); ++i)
            if (!pcm[i].empty()) {
                selected[i]->pending = std::move(pcm[i]);
                selected[i]->ready = true;
            }
        changed.notify_all();
        return multi                 ? selected[0]->native.buffer_frames
               : requests[0].address ? selected[0]->native.buffer_frames *
                                           selected[0]->native.format_info.num_channels
                                     : 0;
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
                    std::lock_guard gate(vm);
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
            // the VM copy to preserve VM -> domain lock ordering everywhere.
            if (nid == "Ptlts326pds") {
                u64 time{};
                {
                    std::lock_guard lock(mutex);
                    CheckAlive();
                    time = Find(u32(a[0]))->native.last_output_time;
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
GuestAudio::GuestAudio(GuestAddressSpace& space, std::recursive_mutex& vm, GuestClock& clock,
                       Factory factory)
    : impl(std::make_unique<Impl>(space, vm, clock, std::move(factory))) {}
GuestAudio::~GuestAudio() = default;
u64 GuestAudio::Dispatch(std::string_view nid, const std::array<u64, 6>& a, std::stop_token stop) {
    return impl->Dispatch(nid, a, stop);
}
void GuestAudio::RequestStop() {
    impl->Stop();
}
} // namespace Core::HostRuntime
