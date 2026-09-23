// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include "common/logging/log.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_audio.h"
#include "core/host_runtime/guest_audio3d.h"
#include "core/libraries/audio/audioout_error.h"
#include "core/libraries/audio3d/audio3d.h"
#include "core/libraries/audio3d/audio3d_error.h"
namespace Core::HostRuntime {
using namespace GuestCpu;
using namespace Libraries::Audio3d;
bool IsAudio3dNid(std::string_view nid) {
    return std::ranges::find(Audio3dNids, nid) != std::end(Audio3dNids);
}
namespace {
struct Failure {
    s32 error;
};
void Require(bool ok, s32 error = ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER) {
    if (!ok)
        throw Failure{error};
}
void Clear(std::deque<AudioData>& queue) {
    for (auto& data : queue)
        std::free(data.sample_buffer);
    queue.clear();
}
struct PcmOwner {
    AudioData data{};
    ~PcmOwner() {
        std::free(data.sample_buffer);
    }
    PcmOwner() = default;
    PcmOwner(PcmOwner&& rhs) noexcept : data(rhs.data) {
        rhs.data.sample_buffer = nullptr;
    }
    PcmOwner& operator=(PcmOwner&& rhs) noexcept {
        std::free(data.sample_buffer);
        data = rhs.data;
        rhs.data.sample_buffer = nullptr;
        return *this;
    }
    void Append(std::deque<AudioData>& queue) {
        queue.push_back(data);
        data.sample_buffer = nullptr;
    }
};
struct Attribute {
    u32 id, pad;
    u64 value, size;
};
struct Pcm {
    u32 format, pad;
    u64 buffer;
    u32 samples, pad2;
};
static_assert(sizeof(Attribute) == 24 && sizeof(Pcm) == 24);
} // namespace
struct GuestAudio3d::Impl {
    GuestAddressSpace& space;
    GuestAudio& audio;
    std::mutex mutex;
    bool initialized{}, stopping{};
    struct SessionPort {
        Port mix;
        // Serializes output submission only. Stop/Close never wait for this mutex.
        std::mutex submit;
        std::stop_source cancel;
        bool closed{};
        ~SessionPort() {
            Clear(mix.bed_queue);
            Clear(mix.mixed_queue);
            for (auto& [id, obj] : mix.objects)
                Clear(obj.pcm_queue);
        }
    };
    std::map<u32, std::shared_ptr<SessionPort>> ports;
    static inline std::atomic<u32> next_id{1};
    Impl(GuestAddressSpace& space, GuestAudio& audio) : space(space), audio(audio) {}
    void Read(u64 address, std::span<u8> bytes, std::stop_token stop) {
        Require(address &&
                bool(space.ReadData(GuestAddress{address}, std::as_writable_bytes(bytes), stop)));
    }
    template <class T>
    T Read(u64 address, std::stop_token stop) {
        T value{};
        Read(address, {reinterpret_cast<u8*>(&value), sizeof(value)}, stop);
        return value;
    }
    PinnedSpan Output(u64 address, size_t size, std::stop_token stop) {
        Require(address != 0);
        auto pin = space.AcquireDataSpan({GuestAddress{address}, size}, true, stop);
        Require(bool(pin));
        return std::move(pin).Value();
    }
    void Alive() {
        Require(initialized && !stopping, ORBIS_AUDIO3D_ERROR_NOT_READY);
    }
    std::shared_ptr<SessionPort> Find(u32 id) {
        std::lock_guard lock(mutex);
        Alive();
        auto it = ports.find(id);
        Require(it != ports.end(), ORBIS_AUDIO3D_ERROR_INVALID_PORT);
        return it->second;
    }
    void Check(const SessionPort& p) {
        Require(!p.closed, ORBIS_AUDIO3D_ERROR_INVALID_PORT);
    }
    void Close(const std::shared_ptr<SessionPort>& p) {
        s32 handle;
        {
            std::lock_guard lock(p->mix.mutex);
            p->closed = true;
            p->cancel.request_stop();
            handle = std::exchange(p->mix.audio_out_handle, -1);
        }
        if (handle >= 0)
            audio.Dispatch("s1--uE9mBFw", {u32(handle)});
    }
    void Stop(bool final) {
        std::map<u32, std::shared_ptr<SessionPort>> retired;
        {
            std::lock_guard lock(mutex);
            stopping |= final;
            initialized = false;
            retired.swap(ports);
        }
        for (auto& [id, p] : retired)
            Close(p);
    }
    PcmOwner CopyPcm(const Pcm& pcm, u32 channels, u32 granularity, std::stop_token stop) {
        Require(pcm.format <= 1 && pcm.samples && pcm.buffer);
        const size_t sample_size = pcm.format ? 4 : 2;
        Require(!(pcm.buffer & (sample_size - 1)));
        const size_t size = size_t(granularity) * channels * sample_size;
        PcmOwner result;
        result.data = {static_cast<u8*>(std::calloc(1, size)), granularity, channels,
                       OrbisAudio3dFormat(pcm.format)};
        Require(result.data.sample_buffer != nullptr, ORBIS_AUDIO3D_ERROR_OUT_OF_MEMORY);
        // PCM is a byte buffer; all adjacent segments are pinned atomically.
        GuestAddressSpace::DataRequest request{
            {GuestAddress{pcm.buffer},
             size_t(std::min(pcm.samples, granularity)) * channels * sample_size},
            GuestPermission::Read,
            {},
            true};
        auto pins = space.AcquireDataBatch(std::span{&request, 1}, stop);
        Require(bool(pins));
        std::memcpy(result.data.sample_buffer, pins.Value()[0].Bytes().data(), request.range.size);
        return result;
    }
    u64 Open(const std::array<u64, 6>& a, bool create, std::stop_token stop) {
        OrbisAudio3dOpenParameters params{
            0x28, 256, OrbisAudio3dRate(0), 512, 2, OrbisAudio3dBufferMode(0), 0, 2};
        const u64 output = create ? a[3] : a[2];
        if (create) {
            Require(!a[2]);
            params.granularity = a[0];
            params.rate = OrbisAudio3dRate(a[1]);
        } else {
            Require(u32(a[0]) == 255);
            const auto size = Read<u64>(a[1], stop) & ~7ull;
            Require(size == 16 || size == 24 || size == 32 || size == 40);
            Read(a[1], {reinterpret_cast<u8*>(&params), size_t(size)}, stop);
            if (size == 24)
                params.buffer_mode = OrbisAudio3dBufferMode(1);
        }
        Require(u32(params.rate) == 0 && params.granularity >= 256 && !(params.granularity & 255));
        Require(params.max_objects && params.queue_depth && u32(params.buffer_mode) <= 2 &&
                (params.num_beds & ~1u) == 2);
        // Same bounded device burst size as AudioOut. Larger bursts need chunked submission.
        Require(params.granularity <= 2048, ORBIS_AUDIO3D_ERROR_NOT_SUPPORTED);
        const u32 max_depth = params.granularity == 256   ? 64
                              : params.granularity == 512 ? 31
                              : params.granularity == 768 ? 20
                                                          : 15;
        Require(params.queue_depth <= max_depth);
        params.max_objects = std::min(params.max_objects, 512u);
        auto p = std::make_shared<SessionPort>();
        p->mix.parameters = params;
        auto pin = Output(output, 4, stop);
        std::lock_guard lock(mutex);
        Alive();
        Require(ports.size() < MaxPorts, ORBIS_AUDIO3D_ERROR_OUT_OF_RESOURCES);
        const u32 id = next_id.fetch_add(1);
        Require(id && id < 0x7fffffff, ORBIS_AUDIO3D_ERROR_OUT_OF_RESOURCES);
        ports.emplace(id, p);
        std::memcpy(pin.WritableBytes().data(), &id, 4);
        return 0;
    }
    u64 SetAttributes(const std::shared_ptr<SessionPort>& p, u32 object,
                      std::vector<Attribute> attrs, std::stop_token stop) {
        struct Staged {
            u32 id;
            std::vector<u8> blob;
            PcmOwner pcm;
        };
        std::vector<Staged> staged;
        staged.reserve(attrs.size());
        bool reset{};
        for (const auto& attr : attrs) {
            Staged item;
            item.id = attr.id;
            if (attr.id == 9)
                reset = true;
            else if (attr.id == 1) {
                Require(attr.size >= sizeof(Pcm));
                auto pcm = Read<Pcm>(attr.value, stop);
                item.pcm = CopyPcm(pcm, 1, p->mix.parameters.granularity, stop);
            } else if (attr.id == 3 || attr.id == 6) {
                Require(attr.size == 4);
                item.blob.resize(4);
                Read(attr.value, item.blob, stop);
                if (attr.id == 3) {
                    float gain;
                    std::memcpy(&gain, item.blob.data(), 4);
                    Require(std::isfinite(gain));
                } else {
                    u32 ear;
                    std::memcpy(&ear, item.blob.data(), 4);
                    Require(ear <= 2);
                }
            } else {
                Require(false, ORBIS_AUDIO3D_ERROR_NOT_SUPPORTED);
            }
            staged.push_back(std::move(item));
        }
        std::lock_guard lock(p->mix.mutex);
        Check(*p);
        auto it = p->mix.objects.find(object);
        Require(it != p->mix.objects.end() && !it->second.unreserved,
                ORBIS_AUDIO3D_ERROR_INVALID_OBJECT);
        auto& obj = it->second;
        const auto count = std::ranges::count_if(staged, [](const auto& x) { return x.id == 1; });
        Require((reset ? 0 : obj.pcm_queue.size()) + count <= p->mix.parameters.queue_depth,
                ORBIS_AUDIO3D_ERROR_NOT_READY);
        // Prepare replacement containers before publishing, including their allocations.
        auto persistent = reset ? decltype(obj.persistent_attributes){} : obj.persistent_attributes;
        auto queue = reset ? std::deque<AudioData>{} : obj.pcm_queue;
        for (auto& item : staged) {
            if (item.id == 1)
                queue.push_back(item.pcm.data);
            else if (item.id != 9)
                persistent[item.id] = std::move(item.blob);
        }
        if (reset)
            Clear(obj.pcm_queue);
        obj.pcm_queue.swap(queue);
        obj.persistent_attributes.swap(persistent);
        for (auto& item : staged)
            item.pcm.data.sample_buffer = nullptr;
        return 0;
    }
    u64 Push(const std::shared_ptr<SessionPort>& p, bool flush, u32 blocking,
             std::stop_token stop) {
        Require(flush || blocking <= 1);
        std::stop_callback cancelled(stop, [&] { p->cancel.request_stop(); });
        std::unique_lock submit(p->submit, std::defer_lock);
        if (!flush && !blocking)
            Require(submit.try_lock(), ORBIS_AUDIO3D_ERROR_NOT_READY);
        else
            submit.lock();
        s32 handle;
        {
            std::lock_guard lock(p->mix.mutex);
            Check(*p);
            Require(flush || u32(p->mix.parameters.buffer_mode) == 2,
                    ORBIS_AUDIO3D_ERROR_NOT_SUPPORTED);
            handle = p->mix.audio_out_handle;
            if (flush && p->mix.mixed_queue.empty() &&
                (!p->mix.bed_queue.empty() ||
                 std::ranges::any_of(p->mix.objects,
                                     [](const auto& x) { return !x.second.pcm_queue.empty(); }))) {
                const auto result = ProcessMixQueue(p->mix);
                Require(result == 0, result);
            }
            if (p->mix.mixed_queue.empty() ||
                (!flush && p->mix.mixed_queue.size() < p->mix.parameters.queue_depth))
                return 0;
        }
        if (handle < 0) {
            handle =
                s32(audio.Dispatch("ekNvsT22rsY",
                                   {255, u32(Libraries::AudioOut::OrbisAudioOutPort::Audio3d), 0,
                                    p->mix.parameters.granularity, 48000,
                                    u32(Libraries::AudioOut::OrbisAudioOutParamFormat::S16Stereo)},
                                   p->cancel.get_token()));
            Require(handle >= 0, handle);
            bool closed;
            {
                std::lock_guard lock(p->mix.mutex);
                closed = p->closed;
                if (!closed)
                    p->mix.audio_out_handle = handle;
            }
            if (closed) {
                audio.Dispatch("s1--uE9mBFw", {u32(handle)});
                throw Failure{ORBIS_AUDIO3D_ERROR_INVALID_PORT};
            }
        }
        for (;;) {
            AudioData frame;
            {
                std::lock_guard lock(p->mix.mutex);
                Check(*p);
                if (p->mix.mixed_queue.empty() ||
                    (!flush && p->mix.mixed_queue.size() < p->mix.parameters.queue_depth))
                    break;
                frame = p->mix.mixed_queue.front();
            }
            auto result =
                s32(audio.OutputHost(handle, {frame.sample_buffer, size_t(frame.num_samples) * 4},
                                     p->cancel.get_token(), !flush && !blocking));
            Require(result != ORBIS_AUDIO_OUT_ERROR_BUSY, ORBIS_AUDIO3D_ERROR_NOT_READY);
            Require(result >= 0, result);
            {
                std::lock_guard lock(p->mix.mutex);
                std::free(frame.sample_buffer);
                p->mix.mixed_queue.pop_front();
            }
            if (!flush && !blocking)
                break;
        }
        if (flush) {
            const auto result = s32(audio.OutputHost(handle, {}, p->cancel.get_token()));
            Require(result >= 0, result);
        }
        return 0;
    }
    u64 Dispatch(std::string_view nid, const std::array<u64, 6>& a, std::stop_token stop) {
        try {
            Require(!stop.stop_requested(), ORBIS_AUDIO3D_ERROR_NOT_READY);
            if (nid == "UmCvjSmuZIw") {
                Require(!a[0]);
                std::lock_guard lock(mutex);
                Require(!initialized && !stopping, ORBIS_AUDIO3D_ERROR_NOT_READY);
                auto result = s32(audio.Dispatch("JfEPXVxhFqA", {}, stop));
                Require(!result || result == ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT, result);
                initialized = true;
                return 0;
            }
            if (nid == "WW1TS2iz5yc") {
                {
                    std::lock_guard lock(mutex);
                    Alive();
                }
                Stop(false);
                return 0;
            }
            if (nid == "Im+jOoa5WAI") {
                auto pin = Output(a[0], 32, stop);
                OrbisAudio3dOpenParameters params{
                    32, 256, OrbisAudio3dRate(0), 512, 2, OrbisAudio3dBufferMode(2), 0, 2};
                std::memcpy(pin.WritableBytes().data(), &params, 32);
                return 0;
            }
            if (nid == "XeDDK0xJWQA" || nid == "UHFOgVNz0kk")
                return Open(a, nid == "UHFOgVNz0kk", stop);
            auto p = Find(a[0]);
            if (nid == "OyVqOeVNtSk") {
                {
                    std::lock_guard lock(mutex);
                    ports.erase(u32(a[0]));
                }
                Close(p);
                return 0;
            }
            if (nid == "jO2tec4dJ2M") {
                auto pin = Output(a[1], 4, stop);
                std::lock_guard lock(p->mix.mutex);
                Check(*p);
                Require(p->mix.objects.size() < p->mix.parameters.max_objects,
                        ORBIS_AUDIO3D_ERROR_OUT_OF_RESOURCES);
                const u32 id = ++p->mix.next_object_id;
                Require(id && id != 0xffffffff, ORBIS_AUDIO3D_ERROR_OUT_OF_RESOURCES);
                p->mix.objects.emplace(id, ObjectState{});
                std::memcpy(pin.WritableBytes().data(), &id, 4);
                return 0;
            }
            if (nid == "1HXxo-+1qCw") {
                std::lock_guard lock(p->mix.mutex);
                Check(*p);
                auto it = p->mix.objects.find(u32(a[1]));
                Require(it != p->mix.objects.end() && !it->second.unreserved,
                        ORBIS_AUDIO3D_ERROR_INVALID_OBJECT);
                if (it->second.pcm_queue.empty())
                    p->mix.objects.erase(it);
                else
                    it->second.unreserved = true;
                return 0;
            }
            if (nid == "4uyHN9q4ZeU" || nid == "V1FBFpNIAzk") {
                std::vector<Attribute> attrs;
                if (nid == "V1FBFpNIAzk")
                    attrs.push_back({u32(a[2]), 0, a[3], a[4]});
                else {
                    Require(a[2] && a[2] <= 64);
                    attrs.resize(a[2]);
                    Read(a[3],
                         {reinterpret_cast<u8*>(attrs.data()), attrs.size() * sizeof(Attribute)},
                         stop);
                }
                return SetAttributes(p, a[1], std::move(attrs), stop);
            }
            if (nid == "9tEwE0GV0qo") {
                Require(u32(a[1]) == 2 || u32(a[1]) == 6 || u32(a[1]) == 8);
                auto pcm = CopyPcm({u32(a[2]), 0, a[3], u32(a[4]), 0}, a[1],
                                   p->mix.parameters.granularity, stop);
                std::lock_guard lock(p->mix.mutex);
                Check(*p);
                Require(p->mix.bed_queue.size() < p->mix.parameters.queue_depth,
                        ORBIS_AUDIO3D_ERROR_NOT_READY);
                pcm.Append(p->mix.bed_queue);
                return 0;
            }
            if (nid == "lw0qrdSjZt8") {
                std::lock_guard lock(p->mix.mutex);
                Check(*p);
                Require(u32(p->mix.parameters.buffer_mode) != 0, ORBIS_AUDIO3D_ERROR_NOT_SUPPORTED);
                Require(p->mix.mixed_queue.size() < p->mix.parameters.queue_depth,
                        ORBIS_AUDIO3D_ERROR_NOT_READY);
                return ProcessMixQueue(p->mix);
            }
            if (nid == "VEVhZ9qd4ZY" || nid == "ZOGrxWLgQzE")
                return Push(p, nid == "ZOGrxWLgQzE", a[1], stop);
            if (nid == "YaaDbDwKpFM") {
                Require(a[1] || a[2]);
                std::vector<GuestAddressSpace::DataRequest> requests;
                for (auto ptr : {a[1], a[2]})
                    if (ptr)
                        requests.push_back({{GuestAddress{ptr}, 4}, GuestPermission::Write});
                auto pins = space.AcquireDataBatch(requests, stop);
                Require(bool(pins));
                std::lock_guard lock(p->mix.mutex);
                Check(*p);
                u32 level = p->mix.mixed_queue.size(),
                    available = p->mix.parameters.queue_depth - level;
                size_t i{};
                if (a[1])
                    std::memcpy(pins.Value()[i++].WritableBytes().data(), &level, 4);
                if (a[2])
                    std::memcpy(pins.Value()[i].WritableBytes().data(), &available, 4);
                return 0;
            }
            if (nid == "9ZA23Ia46Po") {
                const auto capacity = a[1] ? Read<u32>(a[2], stop) : 4;
                const u32 count = std::min(capacity, 4u);
                const u32 caps[]{1, 3, 6, 9};
                std::vector<GuestAddressSpace::DataRequest> requests{
                    {{GuestAddress{a[2]}, 4}, GuestPermission::Write}};
                if (a[1] && count)
                    requests.push_back({{GuestAddress{a[1]}, count * 4}, GuestPermission::Write});
                auto pins = space.AcquireDataBatch(requests, stop);
                Require(a[2] && bool(pins));
                std::lock_guard lock(p->mix.mutex);
                Check(*p);
                std::memcpy(pins.Value()[0].WritableBytes().data(), &count, 4);
                if (a[1] && count)
                    std::memcpy(pins.Value()[1].WritableBytes().data(), caps, count * 4);
                return 0;
            }
            if (nid == "Yq9bfUQ0uJg" && a[1] == 0x10001) {
                Require(a[3] == 4);
                const auto level = Read<float>(a[2], stop);
                Require(std::isfinite(level));
                // This software mixer has a dry output path. Disabling late reverb
                // is implemented; enabling a nonzero effect still needs a provider.
                Require(level == 0.0f, ORBIS_AUDIO3D_ERROR_NOT_SUPPORTED);
                std::lock_guard lock(p->mix.mutex);
                Check(*p);
                return 0;
            }
            // Desktop's PortSetAttribute is a TODO; no fabricated reverb/HRTF success.
            Require(false, ORBIS_AUDIO3D_ERROR_NOT_SUPPORTED);
        } catch (Failure e) {
            static std::atomic<unsigned> errors{};
            if (errors.fetch_add(1) < 24)
                LOG_WARNING(Lib_Audio3d, "GuestAudio3d {} args={:#x},{:#x},{:#x},{:#x} error={:#x}",
                            nid, a[0], a[1], a[2], a[3], u32(e.error));
            return u32(e.error);
        } catch (const std::bad_alloc&) {
            return u32(ORBIS_AUDIO3D_ERROR_OUT_OF_MEMORY);
        }
        return u32(ORBIS_AUDIO3D_ERROR_NOT_SUPPORTED);
    }
};
GuestAudio3d::GuestAudio3d(GuestAddressSpace& space, GuestAudio& audio)
    : impl(std::make_unique<Impl>(space, audio)) {}
GuestAudio3d::~GuestAudio3d() {
    impl->Stop(true);
}
u64 GuestAudio3d::Dispatch(std::string_view nid, const std::array<u64, 6>& args,
                           std::stop_token stop) {
    return impl->Dispatch(nid, args, stop);
}
void GuestAudio3d::RequestStop() {
    impl->Stop(true);
}
} // namespace Core::HostRuntime
