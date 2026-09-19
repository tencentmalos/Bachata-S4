// SPDX-License-Identifier: GPL-2.0-or-later
// Foundation/Oboe owns the device; GuestAudio owns the guest ABI and PCM blocks.
#include <foundation/audio/oboe_output.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include "common/logging/log.h"
#include "common/profiler.h"
#include "core/emulator_settings.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"
#include "core/libraries/audio/audioout_transfer.h"

namespace Libraries::AudioOut {
namespace FA = Foundation::Audio;
namespace {
void Observe(const FA::DeviceSnapshot& s) {
    Common::Profiler::Counter("Audio.Callbacks", s.mixer.callbacks);
    Common::Profiler::Counter("Audio.DeviceFrames", s.mixer.output_frames);
    Common::Profiler::Counter("Audio.ConsumedFrames", s.mixer.consumed_frames);
    Common::Profiler::Counter("Audio.SourceStarvedFrames", s.mixer.starved_frames);
    Common::Profiler::Counter("Audio.EditFrames", s.mixer.edit_frames);
    if (s.xruns >= 0) Common::Profiler::Counter("Audio.Xruns", s.xruns);
    Common::Profiler::Counter("Audio.DeviceQueuedFrames", s.mixer.queued_frames);
    Common::Profiler::Counter("Audio.DeviceBufferFrames", s.buffer_size);
    Common::Profiler::Counter("Audio.Reconnects", s.reconnects);
    LOG_INFO(Lib_AudioOut,
        "Oboe gen={} api={} perf={} share={} rate={} ch={} burst={} capacity={} size={} "
        "sources={} callbacks={} frames={} consumed={} starved={} xruns={} "
        "timestamp_result={} presented={} reconnects={} error={}",
        s.generation, s.api, s.performance, s.sharing, s.sample_rate, s.channels, s.burst,
        s.capacity, s.buffer_size, s.mixer.sources, s.mixer.callbacks, s.mixer.output_frames,
        s.mixer.consumed_frames, s.mixer.starved_frames, s.xruns, s.timestamp_error,
        s.presented_frames, s.reconnects, s.mixer.error);
}
class OboePortBackend final : public PortBackend {
public:
    OboePortBackend(const PortOut& port, std::shared_ptr<FA::AndroidOutput> output)
        : frames(port.buffer_frames), format(port.format_info), output(std::move(output)),
          capacity(std::clamp<size_t>((std::max(1, this->output->Snapshot().burst) * 2 + frames - 1) / frames,
                                      3, FA::AudioSource::MaxCapacity)),
          source(this->output->CreateSource(capacity)) {
        if (!source) throw std::runtime_error("Oboe source admission failed");
        volume.fill(32768);
    }
    ~OboePortBackend() override { source->RequestStop(); }
    bool UsesCallbackQueue() const override { return true; }
    size_t QueueCapacity() const override { return capacity; }
    void Prepare(const void* pcm, const std::array<int, 8>& gain,
                 std::span<float> stereo) const noexcept override {
        PrepareAudioStereo(format, frames, pcm, gain,
                           EmulatorSettings.GetVolumeSlider() * 0.01f, stereo);
    }
    bool CanQueue() const override { return source->HasSpace(); }
    const void* QueueDomain() const override { return output.get(); }
    uint64_t BeginQueueBatch() override { return source->BeginBatch(); }
    void CommitQueueBatch(uint64_t epoch) override { source->CommitBatch(epoch); }
    bool QueuePrepared(std::span<const float> pcm, uint64_t epoch) override {
        return source->TrySubmit(pcm, epoch) != 0;
    }
    uint64_t CompletedBuffers() const override { return source->ConsumedBlocks(); }
    int DeviceError() const override { return source->Error(); }
    void StopCallbacks() override { source->RequestStop(); }
    void SetVolume(const std::array<int, 8>& gain) override { volume = gain; }
    void Output(void* pcm) override { (void)OutputChecked(pcm, {}); }
    int OutputChecked(void* pcm, std::stop_token stop) override {
        // Legacy native consumer only. Production GuestAudio never uses this
        // scratch: it prepares directly from a short pin into its owned blocks.
        if (!pcm) return -1;
        if (scratch.empty()) scratch.resize(size_t(frames) * 2);
        Prepare(pcm, volume, scratch);
        return source->SubmitAndWait(scratch, stop) == FA::SubmitResult::Consumed ? 0 : -1;
    }
private:
    const u32 frames;
    const AudioFormatInfo format;
    std::shared_ptr<FA::AndroidOutput> output; // source destroyed before endpoint
    const size_t capacity;
    std::unique_ptr<FA::AudioSource> source;
    std::array<int, 8> volume{};
    std::vector<float> scratch;
};
}
struct OboeAudioOut::Impl {
    std::mutex mutex;
    std::weak_ptr<FA::AndroidOutput> device;
};
OboeAudioOut::OboeAudioOut() : impl(std::make_unique<Impl>()) {}
OboeAudioOut::~OboeAudioOut() = default;
std::unique_ptr<PortBackend> OboeAudioOut::Open(PortOut& port) {
    std::lock_guard lock(impl->mutex);
    auto device = impl->device.lock();
    if (!device) {
        FA::AndroidOutputConfig config;
        config.observe = Observe;
        device = std::make_shared<FA::AndroidOutput>(std::move(config));
        impl->device = device;
    }
    return std::make_unique<OboePortBackend>(port, std::move(device));
}
} // namespace Libraries::AudioOut
