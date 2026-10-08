// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <stop_token>
#include <string_view>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/audio/audioin.h"
#include "core/libraries/audio/audioin_error.h"
namespace Core::HostRuntime {
inline constexpr std::array<std::string_view, 5> AudioInputNids{
    "5NE8Sjc7VC8", "Jh6WbHhnI68", "LozEOU8+anM", "BohEAQ7DlUE", "nya-R5gDYhM"};
inline bool IsAudioInputNid(std::string_view nid) {
    for (auto candidate : AudioInputNids) if (candidate == nid) return true;
    return false;
}
// Android's shared NullAudioIn backend does not publish ports. Keep its real
// validation/error contract; no silence buffer, fake handle, or guest pointer
// reaches a native audio callback. A future microphone provider needs a checked
// data bridge before this admission policy can change.
inline u32 DispatchAudioInput(std::string_view nid, const std::array<u64, 6>& a) {
    using namespace Libraries::AudioIn;
    if (nid == "5NE8Sjc7VC8") return sceAudioInOpen(s32(a[0]), a[1], a[2], a[3], a[4], a[5]);
    if (nid == "nya-R5gDYhM") return sceAudioInHqOpen(s32(a[0]), a[1], a[2], a[3], a[4], a[5]);
    if (nid == "Jh6WbHhnI68") return sceAudioInClose(s32(a[0]));
    if (nid == "BohEAQ7DlUE") return sceAudioInGetSilentState(s32(a[0]));
    return InputWithoutDevice(s32(a[0]), a[1] != 0);
}

// Session-owned counterpart of SDLInPortBackend's disconnected-device mode.
// A logical port can be open while its microphone is absent: reads provide paced
// silence and GetSilentState reports DEVICE_NONE. This never opens Android capture.
class GuestAudioInput {
    struct Port {
        u32 frames{}, rate{}, channels{};
        std::mutex mutex;
        std::condition_variable_any changed;
        std::atomic<bool> closed{};
        std::chrono::steady_clock::time_point next{};
    };
    GuestCpu::GuestAddressSpace& space;
    std::mutex mutex;
    std::map<u32, std::shared_ptr<Port>> ports;
    bool stopped{};
    static inline std::atomic<u32> next_handle{1};

public:
    explicit GuestAudioInput(GuestCpu::GuestAddressSpace& space) : space(space) {}
    void RequestStop() {
        std::lock_guard lock(mutex);
        stopped = true;
        for (const auto& [id, port] : ports) {
            port->closed = true;
            port->changed.notify_all();
        }
    }
    u32 Dispatch(std::string_view nid, const std::array<u64, 6>& a,
                 std::stop_token stop = {}) {
        using namespace Libraries::AudioIn;
        if (nid == "5NE8Sjc7VC8" || nid == "nya-R5gDYhM") {
            if (!u32(a[3]) || u32(a[3]) > 2048) return ORBIS_AUDIO_IN_ERROR_INVALID_SIZE;
            if (u32(a[4]) != 16000 && u32(a[4]) != 48000) return ORBIS_AUDIO_IN_ERROR_INVALID_FREQ;
            if (u32(a[5]) != 0 && u32(a[5]) != 2) return ORBIS_AUDIO_IN_ERROR_INVALID_PARAM;
            if (u32(a[1]) != 0 && u32(a[1]) != 1 && u32(a[1]) != 5) return ORBIS_AUDIO_IN_ERROR_INVALID_TYPE;
            if (u32(a[2])) return ORBIS_AUDIO_IN_ERROR_INVALID_PARAM;
            std::lock_guard lock(mutex);
            if (stopped) return ORBIS_AUDIO_IN_ERROR_NOT_OPENED;
            if (ports.size() >= ORBIS_AUDIO_IN_NUM_PORTS) return ORBIS_AUDIO_IN_ERROR_PORT_FULL;
            auto port = std::make_shared<Port>();
            port->frames = u32(a[3]); port->rate = u32(a[4]); port->channels = a[5] ? 2 : 1;
            port->next = std::chrono::steady_clock::now();
            const auto id = next_handle.fetch_add(1);
            if (!id || id >= 0x10000000) return ORBIS_AUDIO_IN_ERROR_PORT_FULL;
            const auto handle = 0x30000000 | id;
            ports.emplace(handle, std::move(port));
            return handle;
        }
        std::shared_ptr<Port> port;
        {
            std::lock_guard lock(mutex);
            const auto it = ports.find(u32(a[0]));
            if (it == ports.end()) return ORBIS_AUDIO_IN_ERROR_INVALID_HANDLE;
            port = it->second;
            if (nid == "Jh6WbHhnI68") {
                port->closed = true;
                ports.erase(it);
                port->changed.notify_all();
                return 0;
            }
        }
        if (port->closed) return ORBIS_AUDIO_IN_ERROR_NOT_OPENED;
        if (nid == "BohEAQ7DlUE") return ORBIS_AUDIO_IN_SILENT_STATE_DEVICE_NONE;
        if (nid != "LozEOU8+anM") return ORBIS_AUDIO_IN_ERROR_INVALID_PARAM;
        if (!a[1]) return ORBIS_AUDIO_IN_ERROR_INVALID_POINTER;
        std::unique_lock lock(port->mutex);
        port->changed.wait_until(lock, stop, port->next, [&] { return port->closed.load(); });
        if (stop.stop_requested() || port->closed) return ORBIS_AUDIO_IN_ERROR_NOT_OPENED;
        // No guest span or global registry lock is held while pacing input.
        std::array<std::byte, 2048 * 2 * sizeof(s16)> silence{};
        const auto bytes = size_t(port->frames) * port->channels * sizeof(s16);
        if (!space.WriteData(GuestCpu::GuestAddress{a[1]}, std::span{silence}.first(bytes)))
            return ORBIS_AUDIO_IN_ERROR_INVALID_POINTER;
        port->next = std::chrono::steady_clock::now() +
                     std::chrono::nanoseconds(1000000000ull * port->frames / port->rate);
        return port->frames;
    }
};
} // namespace Core::HostRuntime
