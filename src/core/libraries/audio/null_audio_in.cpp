// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// No-microphone AudioIn backend used on Android, where SDL audio capture is not
// linked. It satisfies the PortInBackend contract by returning silence at the
// requested buffer cadence and reporting available — the same observable behavior
// SDLAudioIn produces when the host has no capture device. This is an honest no-mic
// fallback: it does not pretend to be a real microphone, and a title that needs
// actual capture is not served by it (a later stage may add an AAudio input path).

#include <chrono>
#include <cstring>
#include <thread>

#include "audioin_backend.h"
#include "core/libraries/audio/audioin.h"

namespace Libraries::AudioIn {

namespace {

class NullPortIn final : public PortInBackend {
public:
    explicit NullPortIn(PortIn& port) : port(port) {}

    int Read(void* out_buffer) override {
        const int bytes = static_cast<int>(port.samples_num * port.sample_size * port.channels_num);
        if (out_buffer != nullptr && bytes > 0) {
            std::memset(out_buffer, 0, static_cast<std::size_t>(bytes));
        }
        // Pace the read to the buffer's real-time duration so a polling title does
        // not spin: samples_num frames at freq Hz.
        if (port.freq > 0 && port.samples_num > 0) {
            const auto micros = static_cast<long long>(port.samples_num) * 1'000'000LL /
                                static_cast<long long>(port.freq);
            std::this_thread::sleep_for(std::chrono::microseconds(micros));
        }
        return bytes;
    }

    void Clear() override {}

    bool IsAvailable() override {
        return true;
    }

private:
    PortIn& port;
};

} // namespace

std::unique_ptr<PortInBackend> NullAudioIn::Open(PortIn& port) {
    return std::make_unique<NullPortIn>(port);
}

} // namespace Libraries::AudioIn
