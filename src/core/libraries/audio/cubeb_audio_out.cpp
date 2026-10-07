// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <cubeb/cubeb.h>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"
#include "core/libraries/audio/audioout_transfer.h"
#include "core/libraries/audio/surround_virtualizer.h"

#ifdef _WIN32
#include <objbase.h>
#endif

namespace Libraries::AudioOut {

namespace {

void CubebLog(const char* fmt, ...) {
    char message[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    LOG_DEBUG(Lib_AudioOut, "cubeb: {}", message);
}

} // namespace

// Every cubeb control call runs on this one host thread: WASAPI needs COM on the
// calling thread, and the guest threads that open ports have guest-sized stacks.
// The data callbacks run on cubeb's own threads.
class CubebHost {
public:
    CubebHost() {
        thread = std::thread([this] { Loop(); });
        ctx = Run([] {
            cubeb_set_log_callback(CUBEB_LOG_NORMAL, &CubebLog);
            cubeb* context{};
            if (const int ret = cubeb_init(&context, "shadPS4", nullptr); ret != CUBEB_OK) {
                LOG_ERROR(Lib_AudioOut, "Failed to create cubeb context: {}", ret);
                return static_cast<cubeb*>(nullptr);
            }
            LOG_INFO(Lib_AudioOut, "cubeb audio backend: {}", cubeb_get_backend_id(context));
            return context;
        });
    }

    ~CubebHost() {
        Run([this] {
            if (ctx) {
                cubeb_destroy(ctx);
            }
            cubeb_set_log_callback(CUBEB_LOG_DISABLED, nullptr);
        });
        {
            std::lock_guard lock{mutex};
            quit = true;
        }
        cv.notify_one();
        thread.join();
    }

    template <typename F>
    auto Run(F&& f) -> decltype(f()) {
        std::packaged_task<decltype(f())()> task{std::forward<F>(f)};
        auto result = task.get_future();
        {
            std::lock_guard lock{mutex};
            tasks.emplace_back([&task] { task(); });
        }
        cv.notify_one();
        return result.get();
    }

    cubeb* Context() const {
        return ctx;
    }

private:
    void Loop() {
        Common::SetCurrentThreadName("shadPS4:CubebControl");
#ifdef _WIN32
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
        std::unique_lock lock{mutex};
        while (true) {
            cv.wait(lock, [this] { return quit || !tasks.empty(); });
            if (tasks.empty()) {
                break;
            }
            auto task = std::move(tasks.front());
            tasks.pop_front();
            lock.unlock();
            task();
            lock.lock();
        }
#ifdef _WIN32
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
#endif
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
    bool quit{};
    cubeb* ctx{};
    std::thread thread;
};

// The port thread converts each guest buffer straight into a ring of device frames
// and waits for room there, so the device clock paces the port like PS4 hardware.
// The cubeb callback only copies out of the ring. Stereo devices get the same PS4
// downmix as the Android Oboe path; devices with 6+ channels get the 7.1 mix.
class CubebPortBackend final : public PortBackend {
public:
    CubebPortBackend(std::shared_ptr<CubebHost> host_, const PortOut& port)
        : host(std::move(host_)), type(port.type), format(port.format_info),
          guest_frames(port.buffer_frames), guest_frame_bytes(port.format_info.FrameSize()),
          sample_rate(port.sample_rate),
          period(
              std::chrono::nanoseconds(1'000'000'000ULL * port.buffer_frames / port.sample_rate)) {
        for (auto& channel : volume) {
            channel.store(ORBIS_AUDIO_OUT_VOLUME_0DB, std::memory_order_relaxed);
        }
        host->Run([this] { CreateStream(); });
    }

    ~CubebPortBackend() override {
        host->Run([this] { DestroyStream(); });
        LOG_INFO(Lib_AudioOut,
                 "cubeb port closed: {} callbacks, {} frames, {} underruns ({} frames), "
                 "{} dropped buffers",
                 callbacks.load(), frames_played.load(), underruns.load(), underrun_frames.load(),
                 dropped_buffers.load());
    }

    bool PacesOutput() const override {
        return true;
    }

    void Output(void* ptr) override {
        OutputChecked(ptr, {});
    }

    int OutputChecked(void* ptr, std::stop_token stop) override {
        if (!stream && !TryReopen()) {
            return PaceWithoutDevice(stop);
        }
        if (failed.load(std::memory_order_acquire)) {
            ++dropped_buffers;
            if (!TryReopen()) {
                return PaceWithoutDevice(stop);
            }
        }
        const u64 seen_callbacks = callbacks.load(std::memory_order_acquire);
        if (stalled) {
            if (seen_callbacks == stall_callbacks) {
                ++dropped_buffers;
                return PaceWithoutDevice(stop);
            }
            stalled = false;
            LOG_INFO(Lib_AudioOut, "cubeb stream resumed after a stall");
        }

        const u64 high_water =
            std::min<u64>(capacity - guest_frames,
                          std::max<u64>(latency, request_peak.load(std::memory_order_relaxed)) +
                              2ULL * guest_frames);
        bool room;
        {
            std::unique_lock lock{wait_mutex};
            room = wait_cv.wait_until(lock, stop, std::chrono::steady_clock::now() + stall_timeout,
                                      [&] {
                                          return failed.load(std::memory_order_acquire) ||
                                                 Queued() + guest_frames <= high_water;
                                      });
        }
        if (stop.stop_requested()) {
            return -1;
        }
        if (!room || failed.load(std::memory_order_acquire)) {
            if (!room && callbacks.load(std::memory_order_acquire) == seen_callbacks) {
                stalled = true;
                stall_callbacks = seen_callbacks;
                LOG_WARNING(Lib_AudioOut, "cubeb stream stalled, pacing without the device");
            }
            ++dropped_buffers;
            return PaceWithoutDevice(stop);
        }
        Write(ptr);
        next_nominal = std::chrono::steady_clock::now() + period;
        return 0;
    }

    void SetVolume(const std::array<int, 8>& ch_volumes) override {
        for (size_t i = 0; i < volume.size(); ++i) {
            volume[i].store(ch_volumes[i], std::memory_order_relaxed);
        }
    }

    std::string DebugStatus() override {
        std::string peak;
        for (u32 ch = 0; ch < channels; ++ch) {
            peak += fmt::format("{}{:.3f}", ch ? "/" : "",
                                peaks[ch].exchange(0.0f, std::memory_order_relaxed));
        }
        return fmt::format("cubeb {} -> {} ch, latency {} frames, queued {}, callbacks {}, "
                           "frames {}, underruns {} ({} frames), dropped {}, peak {}{}{}",
                           format.num_channels, channels, latency, Queued(), callbacks.load(),
                           frames_played.load(), underruns.load(), underrun_frames.load(),
                           dropped_buffers.load(), peak, stream ? "" : ", no stream",
                           failed.load() ? ", failed" : "");
    }

private:
    u64 Queued() const {
        return write_pos.load(std::memory_order_relaxed) - read_pos.load(std::memory_order_acquire);
    }

    void Write(const void* input) {
        std::array<int, 8> gains;
        for (size_t i = 0; i < gains.size(); ++i) {
            gains[i] = volume[i].load(std::memory_order_relaxed);
        }
        // Volume above 100% amplifies after the guest gains, as the SDL backend does.
        const float slider = static_cast<float>(EmulatorSettings.GetVolumeSlider()) * 0.01f;
        const float boost = std::max(slider, 1.0f);

        const u64 write = write_pos.load(std::memory_order_relaxed);
        const u32 start = static_cast<u32>(write & (capacity - 1));
        const u32 first = std::min(guest_frames, capacity - start);
        const auto* bytes = static_cast<const u8*>(input);
        Convert(bytes, first, start, gains, slider, boost);
        if (first < guest_frames) {
            Convert(bytes + size_t(first) * guest_frame_bytes, guest_frames - first, 0, gains,
                    slider, boost);
        }
        write_pos.store(write + guest_frames, std::memory_order_release);
    }

    void Convert(const u8* input, u32 frames, u32 ring_frame, const std::array<int, 8>& gains,
                 float slider, float boost) {
        const std::span<float> out{ring.data() + size_t(ring_frame) * channels,
                                   size_t(frames) * channels};
        if (channels == 8) {
            PrepareAudioSurround71(format, frames, input, gains, slider, out);
        } else if (format.num_channels == 8 && VirtualSurroundEnabled()) {
            // Write converts a port's buffers in order, as the filter state needs.
            if (!virtualizer) virtualizer.emplace(guest_frames, sample_rate);
            virtualizer->Process(format, frames, input, gains, slider, out);
        } else {
            PrepareAudioStereo(format, frames, input, gains, slider, out);
        }
        if (boost != 1.0f) {
            for (float& sample : out) {
                sample *= boost;
            }
        }
    }

    static long DataCallback(cubeb_stream*, void* user, const void*, void* output, long frames) {
        static_cast<CubebPortBackend*>(user)->Consume(static_cast<float*>(output),
                                                      static_cast<u32>(frames));
        return frames;
    }

    void Consume(float* out, u32 frames) {
        const u64 read = read_pos.load(std::memory_order_relaxed);
        const u64 available = write_pos.load(std::memory_order_acquire) - read;
        const u32 count = static_cast<u32>(std::min<u64>(available, frames));
        const u32 start = static_cast<u32>(read & (capacity - 1));
        const u32 first = std::min(count, capacity - start);
        std::copy_n(ring.data() + size_t(start) * channels, size_t(first) * channels, out);
        std::copy_n(ring.data(), size_t(count - first) * channels, out + size_t(first) * channels);
        if (count > 0) {
            std::copy_n(out + size_t(count - 1) * channels, channels, last_frame.begin());
            std::array<float, 8> block_peak{};
            for (u32 i = 0; i < count; ++i) {
                for (u32 ch = 0; ch < channels; ++ch) {
                    block_peak[ch] =
                        std::max(block_peak[ch], std::abs(out[size_t(i) * channels + ch]));
                }
            }
            for (u32 ch = 0; ch < channels; ++ch) {
                if (block_peak[ch] > peaks[ch].load(std::memory_order_relaxed)) {
                    peaks[ch].store(block_peak[ch], std::memory_order_relaxed);
                }
            }
        }
        if (count < frames) {
            // Fade the last played frame out over the gap instead of stepping to zero.
            const u32 gap = frames - count;
            const u32 fade = std::min(gap, 64u);
            for (u32 i = 0; i < gap; ++i) {
                const float level = i < fade ? 1.0f - float(i + 1) / float(fade) : 0.0f;
                for (u32 ch = 0; ch < channels; ++ch) {
                    out[size_t(count + i) * channels + ch] = last_frame[ch] * level;
                }
            }
            last_frame.fill(0.0f);
            if (read != 0 || count != 0) {
                underruns.fetch_add(1, std::memory_order_relaxed);
                underrun_frames.fetch_add(gap, std::memory_order_relaxed);
            }
        }
        read_pos.store(read + count, std::memory_order_release);
        frames_played.fetch_add(frames, std::memory_order_relaxed);
        // The first requests prefill the device buffer; size the queue by the later ones.
        if (callbacks.fetch_add(1, std::memory_order_release) >= 2 &&
            frames > request_peak.load(std::memory_order_relaxed)) {
            request_peak.store(frames, std::memory_order_relaxed);
        }
        {
            std::lock_guard lock{wait_mutex};
        }
        wait_cv.notify_one();
    }

    static void StateCallback(cubeb_stream*, void* user, cubeb_state state) {
        auto& self = *static_cast<CubebPortBackend*>(user);
        if (state == CUBEB_STATE_ERROR) {
            LOG_ERROR(Lib_AudioOut, "cubeb stream error");
            self.failed.store(true, std::memory_order_release);
            {
                std::lock_guard lock{self.wait_mutex};
            }
            self.wait_cv.notify_one();
        }
    }

    // Host thread only.
    bool CreateStream() {
        cubeb* ctx = host->Context();
        const std::string device_name = DeviceName();
        if (!ctx || device_name == "None") {
            LOG_INFO(Lib_AudioOut, "No cubeb output for port type {}", static_cast<int>(type));
            return false;
        }

        cubeb_devid device{};
        u32 device_channels = 0;
        cubeb_device_collection collection{};
        const bool named = !device_name.empty() && device_name != "Default Device";
        const bool enumerated = named && cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_OUTPUT,
                                                                 &collection) == CUBEB_OK;
        for (size_t i = 0; enumerated && i < collection.count; ++i) {
            const cubeb_device_info& info = collection.device[i];
            if (info.friendly_name && device_name == info.friendly_name &&
                info.state == CUBEB_DEVICE_STATE_ENABLED) {
                device = info.devid;
                device_channels = info.max_channels;
                break;
            }
        }
        if (named && !device) {
            LOG_WARNING(Lib_AudioOut, "Audio device '{}' not found, using the default device",
                        device_name);
        }
        if (!device && cubeb_get_max_channel_count(ctx, &device_channels) != CUBEB_OK) {
            device_channels = 2;
        }

        channels = format.num_channels == 8 && device_channels >= 6 ? 8 : 2;
        cubeb_stream_params params{};
        params.format = CUBEB_SAMPLE_FLOAT32NE;
        params.rate = sample_rate;
        params.channels = channels;
        params.layout = channels == 8 ? CUBEB_LAYOUT_3F4_LFE : CUBEB_LAYOUT_STEREO;
        params.prefs = CUBEB_STREAM_PREF_NONE;

        u32 min_latency = 0;
        if (cubeb_get_min_latency(ctx, &params, &min_latency) != CUBEB_OK) {
            min_latency = 2 * guest_frames;
        }
        latency = std::max(min_latency, guest_frames);
        capacity = std::bit_ceil(std::max(4 * (latency + guest_frames), 8192u));
        ring.assign(size_t(capacity) * channels, 0.0f);
        read_pos.store(0, std::memory_order_relaxed);
        write_pos.store(0, std::memory_order_relaxed);
        request_peak.store(0, std::memory_order_relaxed);
        last_frame.fill(0.0f);
        failed.store(false, std::memory_order_relaxed);
        stall_timeout = std::max<std::chrono::nanoseconds>(
            std::chrono::milliseconds(250),
            std::chrono::nanoseconds(8'000'000'000ULL * latency / sample_rate));

        const std::string name = fmt::format("shadPS4 port {}", static_cast<int>(type));
        cubeb_stream* created{};
        int ret = cubeb_stream_init(ctx, &created, name.c_str(), nullptr, nullptr, device, &params,
                                    latency, &DataCallback, &StateCallback, this);
        if (enumerated) {
            cubeb_device_collection_destroy(ctx, &collection);
        }
        if (ret != CUBEB_OK) {
            LOG_ERROR(Lib_AudioOut, "Failed to create cubeb stream: {}", ret);
            return false;
        }
        if (ret = cubeb_stream_start(created); ret != CUBEB_OK) {
            LOG_ERROR(Lib_AudioOut, "Failed to start cubeb stream: {}", ret);
            cubeb_stream_destroy(created);
            return false;
        }
        stream = created;
        LOG_INFO(Lib_AudioOut,
                 "cubeb output '{}': {} Hz, guest {} ch -> {} ch (device {} ch), latency {} "
                 "frames (min {}), queue {} frames",
                 device_name, sample_rate, format.num_channels, channels, device_channels, latency,
                 min_latency, capacity);
        return true;
    }

    // Host thread only.
    void DestroyStream() {
        if (!stream) {
            return;
        }
        if (const int ret = cubeb_stream_stop(stream); ret != CUBEB_OK) {
            LOG_WARNING(Lib_AudioOut, "Failed to stop cubeb stream: {}", ret);
        }
        cubeb_stream_destroy(stream);
        stream = nullptr;
    }

    // Port thread: reopen a failed or missing stream at most every two seconds.
    bool TryReopen() {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_reopen || !host->Context() || DeviceName() == "None") {
            return false;
        }
        next_reopen = now + std::chrono::seconds(2);
        host->Run([this] {
            DestroyStream();
            CreateStream();
        });
        stalled = false;
        if (stream) {
            LOG_INFO(Lib_AudioOut, "cubeb stream reopened");
        }
        return stream != nullptr;
    }

    // Without a running device, keep the guest at the nominal buffer rate and drop the audio.
    int PaceWithoutDevice(std::stop_token stop) {
        const auto now = std::chrono::steady_clock::now();
        if (next_nominal + std::chrono::milliseconds(100) < now) {
            next_nominal = now;
        }
        next_nominal += period;
        std::unique_lock lock{wait_mutex};
        wait_cv.wait_until(lock, stop, next_nominal, [] { return false; });
        return stop.stop_requested() ? -1 : 0;
    }

    std::string DeviceName() const {
        return type == OrbisAudioOutPort::PadSpk ? EmulatorSettings.GetSDLPadSpkOutputDevice()
                                                 : EmulatorSettings.GetSDLMainOutputDevice();
    }

    const std::shared_ptr<CubebHost> host;
    const OrbisAudioOutPort type;
    const AudioFormatInfo format;
    const u32 guest_frames;
    const u32 guest_frame_bytes;
    const u32 sample_rate;
    const std::chrono::nanoseconds period;

    // Set on the host thread while no callback runs.
    cubeb_stream* stream{};
    u32 channels{2};
    u32 latency{};
    u32 capacity{};
    std::vector<float> ring;
    std::optional<SurroundVirtualizer> virtualizer; // 8-channel port, stereo device
    std::chrono::nanoseconds stall_timeout{};

    alignas(64) std::atomic<u64> write_pos{0};
    alignas(64) std::atomic<u64> read_pos{0};
    std::atomic<u32> request_peak{0};
    std::atomic<u64> callbacks{0};
    std::atomic<u64> frames_played{0};
    std::atomic<u64> underruns{0};
    std::atomic<u64> underrun_frames{0};
    std::atomic<bool> failed{false};
    std::atomic<u64> dropped_buffers{0};
    std::array<std::atomic<float>, 8> peaks{}; // max |sample| per channel since last status
    std::array<std::atomic<int>, 8> volume;
    std::array<float, 8> last_frame{}; // callback thread only

    std::mutex wait_mutex;
    std::condition_variable_any wait_cv;

    // Port thread only.
    bool stalled{};
    u64 stall_callbacks{};
    std::chrono::steady_clock::time_point next_nominal{};
    std::chrono::steady_clock::time_point next_reopen{};
};

CubebAudioOut::CubebAudioOut() : host(std::make_shared<CubebHost>()) {}

CubebAudioOut::~CubebAudioOut() = default;

bool CubebAudioOut::Ready() const {
    return host->Context() != nullptr;
}

std::unique_ptr<PortBackend> CubebAudioOut::Open(PortOut& port) {
    return std::make_unique<CubebPortBackend>(host, port);
}

} // namespace Libraries::AudioOut
