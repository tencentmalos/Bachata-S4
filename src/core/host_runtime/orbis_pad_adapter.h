// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/libraries/pad/pad.h"
#include "spatial/input/input_hub.h"
#include <array>
#include <deque>
#include <mutex>
#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <string_view>

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
    OrbisPadAdapter();
    ~OrbisPadAdapter();
    // Lifecycle identity and admission share the input lock with publication.
    void DebugLifecycle(std::uint64_t pid, std::uint64_t generation, std::string_view uuid,
                        std::uint32_t phase);
    std::string DebugCommand(const std::vector<std::string>& args);
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
    int Read(int handle, Libraries::Pad::OrbisPadData *out, int count, bool latest = false,
             bool guest_read = false);
    int Information(int handle, Libraries::Pad::OrbisPadControllerInformation *out) const;
    int Vibrate(int handle, const Libraries::Pad::OrbisPadVibrationParam *value);

public:
    enum class TraceSource : std::uint8_t { Physical, Overlay, Debugbus, Merged };
    enum class TraceKind : std::uint8_t {
        InitialState, CommandRequested, StatePublished, GuestPolled, Released,
        Gap, SessionChanged, TraceStopped,
    };
    struct TraceValue {
        PadSnapshot state{};
        bool connected{};
        bool valid{};
    };
    struct TraceEntry {
        std::uint64_t seq{};
        std::uint64_t monotonic_ns{};
        std::uint64_t host_receive_started_ns{};
        std::uint64_t host_received_ns{};
        std::uint64_t related_seq{};
        std::uint64_t action_id{};
        int port{};
        std::int64_t source_id{};
        TraceSource source{TraceSource::Merged};
        TraceKind kind{TraceKind::StatePublished};
        TraceValue value{};
        TraceValue previous{};
        bool intercepted{};
        std::string reason;
    };
private:
    struct DebugReceipt;
    struct DebugState;
    struct Sample {
        Libraries::Pad::OrbisPadData data;
        std::shared_ptr<DebugReceipt> debug;
    };
    struct Port {
        std::optional<spatial::input::DeviceIdentity> physical, overlay, debug;
        PadSnapshot touch{}, debug_touch{};
        std::shared_ptr<DebugReceipt> debug_receipt;
        Libraries::Pad::OrbisPadData data{};
        std::deque<Sample> history;
        std::uint64_t overlay_sequence{}, debug_sequence{};
        int handle{}, user{}, type{}, index{};
    };
    void ResetDebugLocked();
    void DebugFocusLocked(bool focused);
    void ReleaseDebugLocked(std::string_view reason, bool discard_history);
    void ReleaseDebugPortLocked(int port, std::string_view reason, bool discard_history);
    void ObserveDebugLocked(const Sample& sample, bool intercepted);
    void TracePublishLocked(int port);
    void TraceGuestPollLocked(int port, const Sample& sample, bool intercepted);
    void TraceReleasedLocked(int port, std::string_view reason);
    void TraceCommandLocked(TraceKind kind, int port, std::uint64_t action_id,
                            std::string_view reason = {});
    void TraceAppendLocked(TraceEntry entry);
    void DebugWatchdog();
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
    std::unique_ptr<DebugState> debug_;
};
OrbisPadAdapter &GlobalPadAdapter();
} // namespace Core::HostRuntime
