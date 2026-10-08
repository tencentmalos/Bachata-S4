// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
#include "common/types.h"
#include "core/libraries/audio/audioout_backend.h"
namespace Core::GuestCpu {
class GuestAddressSpace;
}
namespace Core::HostRuntime {
class GuestClock;
inline constexpr std::string_view AudioNids[]{"JfEPXVxhFqA", "ekNvsT22rsY", "s1--uE9mBFw",
                                              "QOQtbeDqsT4", "w3PdaSTSwGE", "b+uAV89IlxE",
                                              "Ptlts326pds", "GrQ9s4IrNaQ", "R5hemoKKID8",
                                              "wVwPU50pS1c"};
bool IsAudioNid(std::string_view nid);
// DebugBus `audio_capture start <seconds> | status | save`: raw guest PCM per port and the time
// each block was accepted, written under the log directory (audio-capture/).
std::string AudioCaptureCommand(const std::vector<std::string>& args);
// DebugBus `audio_pacing on | off | status`: admit guest output blocks at the block period, as
// the PS4 does (default on), or as soon as the device queue has room.
std::string AudioPacingCommand(const std::vector<std::string>& args);
class GuestAudio {
public:
    using Factory = std::function<std::unique_ptr<Libraries::AudioOut::PortBackend>(
        Libraries::AudioOut::PortOut&)>;
    GuestAudio(GuestCpu::GuestAddressSpace& space, GuestClock& clock, Factory factory = {});
    ~GuestAudio();
    u64 Dispatch(std::string_view nid, const std::array<u64, 6>& args, std::stop_token stop = {});
    // Owned host PCM follows the same device queue and cancellation as Guest PCM.
    u64 OutputHost(u32 handle, std::span<const u8> pcm, std::stop_token stop = {},
                   bool nonblocking = false);
    void RequestStop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Core::HostRuntime
