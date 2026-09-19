// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <span>

namespace Libraries::AudioOut {

struct PortOut;

class PortBackend {
public:
    virtual ~PortBackend() = default;

    /// Guaranteed to be called in intervals of at least port buffer time,
    /// with size equal to port buffer size.
    virtual void Output(void* ptr) = 0;
    // Zero means a complete buffer was accepted; negative means a native error.
    // The Android production domain consumes this result and passes cancellation.
    virtual int OutputChecked(void* ptr, std::stop_token stop) {
        if (stop.stop_requested()) return -1;
        Output(ptr);
        return 0;
    }

    virtual void SetVolume(const std::array<int, 8>& ch_volumes) = 0;

    // Optional device-callback path. Prepare copies/converts a SHORT-LIVED
    // pinned input directly into caller-owned, preallocated stereo PCM. It must
    // not allocate, lock, wait, or touch the device. QueuePrepared borrows that
    // block until CompletedBuffers advances or StopCallbacks returns. The host
    // adapter serializes producers and prechecks all ports for batch admission.
    virtual bool UsesCallbackQueue() const { return false; }
    virtual size_t QueueCapacity() const { return 0; }
    virtual void Prepare(const void*, const std::array<int, 8>&, std::span<float>) const noexcept {}
    virtual bool CanQueue() const { return false; }
    virtual const void* QueueDomain() const { return nullptr; }
    virtual uint64_t BeginQueueBatch() { return 0; }
    virtual void CommitQueueBatch(uint64_t) {}
    virtual bool QueuePrepared(std::span<const float>, uint64_t) { return false; }
    virtual uint64_t CompletedBuffers() const { return 0; }
    virtual int DeviceError() const { return 0; }
    virtual void StopCallbacks() {}
};

class AudioOutBackend {
public:
    AudioOutBackend() = default;
    virtual ~AudioOutBackend() = default;

    virtual std::unique_ptr<PortBackend> Open(PortOut& port) = 0;
};

class SDLAudioOut final : public AudioOutBackend {
public:
    std::unique_ptr<PortBackend> Open(PortOut& port) override;
};

class OpenALAudioOut final : public AudioOutBackend {
public:
    std::unique_ptr<PortBackend> Open(PortOut& port) override;
};

#if defined(__ANDROID__)
class OboeAudioOut final : public AudioOutBackend {
public:
    OboeAudioOut();
    ~OboeAudioOut() override;
    std::unique_ptr<PortBackend> Open(PortOut& port) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif

} // namespace Libraries::AudioOut
