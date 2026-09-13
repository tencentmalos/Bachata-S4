// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/libraries/pad/pad.h"
#include "spatial/input/input_hub.h"
#include <array>
#include <deque>
#include <mutex>
#include <optional>

namespace Core::HostRuntime {
inline constexpr int kMaxPadPorts = 4;
struct PadSnapshot {
    std::uint64_t buttons{};
    float left_x{}, left_y{}, right_x{}, right_y{}, left_trigger{}, right_trigger{};
    bool touch_down{};
    float touch_x{}, touch_y{};
};
enum class PadResult { Ok, WrongSession, BadPort, Rejected, NoSession };

// Application-owned PS4 conversion/ports/handles. Common registration, normalized
// state and feedback live in Foundation. This object lives ONLY in the host DSO;
// both app JNI and production scePad* call this same instance.
class OrbisPadAdapter {
public:
    std::uint64_t BeginSession();
    void EndSession(std::uint64_t token);
    std::uint64_t CurrentToken() const;
    PadResult Submit(std::uint64_t token, int port, const PadSnapshot &snapshot);
    PadResult SetConnected(std::uint64_t token, int port, bool connected);
    // Physical device registration returns the Foundation epoch, not a slot id.
    std::uint64_t RegisterDevice(std::uint64_t token, int port, std::int64_t id,
                                 const spatial::input::DeviceCapabilities &caps);
    PadResult SubmitPacket(std::uint64_t token, int port,
                           const spatial::input::InputPacket &packet);
    void RemoveDevice(std::uint64_t token, int port, std::uint64_t epoch);
    void FocusLost(std::uint64_t token);
    bool ReadState(int port, Libraries::Pad::OrbisPadData *out) const;
    std::uint64_t ReadButtons(int port) const;
    bool Connected(int port) const;
    PadResult SetVibration(std::uint64_t token, int port, std::uint8_t small, std::uint8_t large);
    std::vector<spatial::input::HapticCommand> DrainHaptics(std::uint64_t token);
    // Production pad HLE calls these while preserving guest user -> player mapping.
    int Initialize();
    int Open(int user, int type, int index, int port);
    int GetHandle(int user, int type, int index) const;
    int Close(int handle);
    int Read(int handle, Libraries::Pad::OrbisPadData *out, int count, bool latest = false);
    int Information(int handle, Libraries::Pad::OrbisPadControllerInformation *out) const;
    int Vibrate(int handle, const Libraries::Pad::OrbisPadVibrationParam *value);

private:
    struct Port {
        std::optional<spatial::input::DeviceIdentity> physical, overlay;
        PadSnapshot touch{};
        Libraries::Pad::OrbisPadData data{};
        std::deque<Libraries::Pad::OrbisPadData> history;
        std::uint64_t overlay_sequence{};
        int handle{}, user{}, type{}, index{};
    };
    PadResult Validate(std::uint64_t token, int port) const;
    void Publish(int port);
    PadResult VibrateLocked(int port, std::uint8_t small, std::uint8_t large);
    Port *FindHandle(int handle);
    const Port *FindHandle(int handle) const;
    static Libraries::Pad::OrbisPadData Neutral();
    mutable std::mutex mutex_;
    spatial::input::InputHub hub_;
    std::array<Port, kMaxPadPorts> ports_{};
    std::uint64_t token_{};
    std::uint64_t haptic_sequence_{};
    int next_handle_{1};
    bool initialized_{};
};
OrbisPadAdapter &GlobalPadAdapter();
} // namespace Core::HostRuntime
