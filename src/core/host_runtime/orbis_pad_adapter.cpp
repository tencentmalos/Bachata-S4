// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/host_runtime/orbis_pad_adapter.h"
#include "core/libraries/pad/pad_errors.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace Core::HostRuntime {
using namespace spatial::input;
using namespace Libraries::Pad;
namespace {
// Foundation names physical positions; PS4 bit assignments belong here.
constexpr std::array<std::uint32_t, 18> Bits = {0x4000, 0x2000, 0x8000, 0x1000, 0x10,    0x40,
                                                0x80,   0x20,   0x400,  0x800,  0x100,   0x200,
                                                0x2,    0x4,    0x8,    0x1,    0x10000, 0x100000};
u8 Stick(float v) {
    return u8(std::lround(128 + std::clamp(v, -1.f, 1.f) * (v < 0 ? 128 : 127)));
}
u8 Trigger(float v) {
    return u8(std::lround(std::clamp(v, 0.f, 1.f) * 255));
}
u64 Now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
DeviceCapabilities OverlayCapabilities() {
    DeviceCapabilities caps;
    for (int i = 0; i < 6; ++i)
        caps.axes.push_back({Axis(i), i < 4 ? -1.f : 0.f, 1.f, 0.f});
    return caps; // No invented actuator capability for a touch overlay.
}
} // namespace
OrbisPadData OrbisPadAdapter::Neutral() {
    OrbisPadData d{};
    d.leftStick = d.rightStick = {128, 128};
    d.orientation.w = 1;
    return d;
}
u64 OrbisPadAdapter::BeginSession() {
    std::lock_guard lock(mutex_);
    token_ = hub_.BeginSession();
    initialized_ = false;
    for (auto &p : ports_) {
        p = {};
        p.data = Neutral();
    }
    return token_;
}
void OrbisPadAdapter::EndSession(u64 token) {
    std::lock_guard lock(mutex_);
    if (!token || token != token_)
        return;
    hub_.EndSession(token);
    token_ = 0;
    initialized_ = false;
    for (auto &p : ports_) {
        p = {};
        p.data = Neutral();
    }
}
u64 OrbisPadAdapter::CurrentToken() const {
    std::lock_guard lock(mutex_);
    return token_;
}
PadResult OrbisPadAdapter::Validate(u64 token, int port) const {
    if (!token_)
        return PadResult::NoSession;
    if (token != token_)
        return PadResult::WrongSession;
    if (port < 0 || port >= kMaxPadPorts)
        return PadResult::BadPort;
    return PadResult::Ok;
}
void OrbisPadAdapter::Publish(int port) {
    auto &p = ports_[port];
    auto d = Neutral();
    std::array<float, 6> axes{};
    u32 buttons{};
    // Overlay and physical hold independent state; releasing one cannot release
    // buttons held by the other. Stronger axis wins, physical breaks equal ties.
    for (auto *identity : {&p.overlay, &p.physical}) {
        if (!*identity)
            continue;
        const auto state = hub_.Snapshot(**identity);
        if (!state || !state->connected)
            continue;
        d.connected = true;
        for (size_t i = 0; i < Bits.size(); ++i)
            if (state->buttons[i])
                buttons |= Bits[i];
        for (size_t i = 0; i < axes.size(); ++i)
            if (std::abs(state->axes[i]) >= std::abs(axes[i]))
                axes[i] = state->axes[i];
    }
    if (axes[4] >= 0.5f)
        buttons |= 0x100;
    if (axes[5] >= 0.5f)
        buttons |= 0x200;
    d.buttons = OrbisPadButtonDataOffset(buttons);
    d.leftStick = {Stick(axes[0]), Stick(axes[1])};
    d.rightStick = {Stick(axes[2]), Stick(axes[3])};
    d.analogButtons.l2 = Trigger(axes[4]);
    d.analogButtons.r2 = Trigger(axes[5]);
    if (p.overlay && p.touch.touch_down) {
        d.touchData.touchNum = 1;
        d.touchData.touch[0] = {u16(std::lround(std::clamp(p.touch.touch_x, 0.f, 1.f) * 1919)),
                                u16(std::lround(std::clamp(p.touch.touch_y, 0.f, 1.f) * 949)), 1};
    }
    d.connectedCount = p.data.connectedCount + (d.connected && !p.data.connected ? 1 : 0);
    d.timestamp = std::max(Now(), p.data.timestamp + 1);
    p.data = d;
    if (p.history.size() == ORBIS_PAD_MAX_DATA_NUM)
        p.history.pop_front();
    p.history.push_back(d);
}
PadResult OrbisPadAdapter::SetConnected(u64 token, int port, bool connected) {
    std::lock_guard lock(mutex_);
    if (auto r = Validate(token, port); r != PadResult::Ok)
        return r;
    auto &p = ports_[port];
    if (connected && !p.overlay) {
        const auto epoch = hub_.RegisterDevice(token, Source::OnScreenOverlay, port, "overlay",
                                               OverlayCapabilities());
        if (!epoch)
            return PadResult::Rejected;
        p.overlay = DeviceIdentity{Source::OnScreenOverlay, port, epoch, "overlay"};
    } else if (!connected && p.overlay) {
        hub_.RemoveDevice(*p.overlay);
        p.overlay.reset();
        p.touch = {};
    }
    Publish(port);
    return PadResult::Ok;
}
PadResult OrbisPadAdapter::Submit(u64 token, int port, const PadSnapshot &s) {
    const std::array<float, 8> values = {s.left_x,       s.left_y,        s.right_x, s.right_y,
                                         s.left_trigger, s.right_trigger, s.touch_x, s.touch_y};
    if (std::any_of(values.begin(), values.end(), [](float v) { return !std::isfinite(v); }))
        return PadResult::Rejected;
    std::lock_guard lock(mutex_);
    if (auto r = Validate(token, port); r != PadResult::Ok)
        return r;
    auto &p = ports_[port];
    if (!p.overlay)
        return PadResult::Rejected; // no reconnect through stale input
    InputPacket packet;
    packet.session_token = token;
    packet.device = *p.overlay;
    packet.sequence = ++p.overlay_sequence;
    for (size_t i = 0; i < Bits.size(); ++i) {
        InputEvent e;
        e.button = Button(i);
        e.pressed = (s.buttons & Bits[i]) != 0;
        packet.events.push_back(e);
    }
    for (int i = 0; i < 6; ++i) {
        InputEvent e;
        e.kind = InputEvent::Kind::AxisValue;
        e.axis = Axis(i);
        e.raw_value = values[i];
        packet.events.push_back(e);
    }
    if (hub_.Submit(packet) != InputResult::Ok)
        return PadResult::Rejected;
    p.touch = s;
    Publish(port);
    return PadResult::Ok;
}
u64 OrbisPadAdapter::RegisterDevice(u64 token, int port, std::int64_t id,
                                    const DeviceCapabilities &caps) {
    std::lock_guard lock(mutex_);
    if (Validate(token, port) != PadResult::Ok)
        return 0;
    auto &p = ports_[port];
    // Do not steal a live slot, including a duplicate backend id in another slot.
    if (p.physical)
        return 0;
    for (auto &other : ports_)
        if (other.physical && other.physical->backend_id == id)
            return 0;
    const auto epoch = hub_.RegisterDevice(token, Source::AndroidGamepad, id, "", caps);
    if (!epoch)
        return 0;
    p.physical = DeviceIdentity{Source::AndroidGamepad, id, epoch, ""};
    Publish(port);
    return epoch;
}
PadResult OrbisPadAdapter::SubmitPacket(u64 token, int port, const InputPacket &input) {
    std::lock_guard lock(mutex_);
    if (auto r = Validate(token, port); r != PadResult::Ok)
        return r;
    auto &p = ports_[port];
    if (!p.physical || input.device != *p.physical || input.session_token != token)
        return PadResult::Rejected;
    if (hub_.Submit(input) != InputResult::Ok)
        return PadResult::Rejected;
    Publish(port);
    return PadResult::Ok;
}
void OrbisPadAdapter::RemoveDevice(u64 token, int port, u64 epoch) {
    std::lock_guard lock(mutex_);
    if (Validate(token, port) != PadResult::Ok)
        return;
    auto &p = ports_[port];
    if (!p.physical || p.physical->connection_epoch != epoch)
        return;
    hub_.RemoveDevice(*p.physical);
    p.physical.reset();
    Publish(port);
}
void OrbisPadAdapter::FocusLost(u64 token) {
    std::lock_guard lock(mutex_);
    if (!token || token != token_)
        return;
    for (int port = 0; port < kMaxPadPorts; ++port) {
        auto &p = ports_[port];
        p.touch = {};
        for (auto *id : {&p.overlay, &p.physical}) {
            if (!*id)
                continue;
            auto state = hub_.Snapshot(**id);
            if (!state)
                continue;
            InputPacket q;
            q.session_token = token;
            q.device = **id;
            q.sequence = state->last_sequence + 1;
            q.control = ControlEvent::FocusLost;
            (void)hub_.Submit(q);
            if (id == &p.overlay)
                p.overlay_sequence = q.sequence;
        }
        p.history.clear();
        Publish(port);
    }
}
bool OrbisPadAdapter::ReadState(int port, OrbisPadData *out) const {
    std::lock_guard lock(mutex_);
    if (!out || port < 0 || port >= kMaxPadPorts)
        return false;
    *out = token_ ? ports_[port].data : Neutral();
    return true;
}
u64 OrbisPadAdapter::ReadButtons(int port) const {
    OrbisPadData d{};
    return ReadState(port, &d) ? u64(d.buttons) : 0;
}
bool OrbisPadAdapter::Connected(int port) const {
    OrbisPadData d{};
    return ReadState(port, &d) && d.connected;
}
PadResult OrbisPadAdapter::VibrateLocked(int port, u8 small, u8 large) {
    auto &p = ports_[port];
    if (!p.physical)
        return PadResult::Rejected; // no phone fallback for absent controller actuator
    HapticCommand command;
    command.session_token = token_;
    command.device = *p.physical;
    command.sequence = ++haptic_sequence_;
    command.small_motor = small / 255.f;
    command.large_motor = large / 255.f;
    command.duration_ms = 1000;
    command.cancel = !small && !large;
    return hub_.EnqueueHaptic(command) == InputResult::Ok ? PadResult::Ok : PadResult::Rejected;
}
PadResult OrbisPadAdapter::SetVibration(u64 token, int port, u8 small, u8 large) {
    std::lock_guard lock(mutex_);
    if (auto r = Validate(token, port); r != PadResult::Ok)
        return r;
    return VibrateLocked(port, small, large);
}
std::vector<HapticCommand> OrbisPadAdapter::DrainHaptics(u64 token) {
    std::lock_guard lock(mutex_);
    return hub_.DrainHaptics(token, kMaxPadPorts);
}
OrbisPadAdapter::Port *OrbisPadAdapter::FindHandle(int h) {
    if (h <= 0 || !token_)
        return nullptr;
    for (auto &p : ports_)
        if (p.handle == h)
            return &p;
    return nullptr;
}
const OrbisPadAdapter::Port *OrbisPadAdapter::FindHandle(int h) const {
    return const_cast<OrbisPadAdapter *>(this)->FindHandle(h);
}
int OrbisPadAdapter::Initialize() {
    std::lock_guard lock(mutex_);
    initialized_ = token_ != 0;
    return initialized_ ? 0 : ORBIS_PAD_ERROR_NOT_INITIALIZED;
}
int OrbisPadAdapter::Open(int user, int type, int index, int port) {
    std::lock_guard lock(mutex_);
    if (!initialized_ || !token_)
        return ORBIS_PAD_ERROR_NOT_INITIALIZED;
    if (port < 0 || port >= kMaxPadPorts || type != ORBIS_PAD_PORT_TYPE_STANDARD || index != 0)
        return ORBIS_PAD_ERROR_INVALID_ARG;
    auto &p = ports_[port];
    if (p.handle)
        return ORBIS_PAD_ERROR_ALREADY_OPENED;
    if (next_handle_ == std::numeric_limits<int>::max())
        return ORBIS_PAD_ERROR_FATAL;
    p.user = user;
    p.type = type;
    p.index = index;
    p.handle = next_handle_++;
    p.history.clear();
    p.history.push_back(p.data);
    return p.handle;
}
int OrbisPadAdapter::GetHandle(int user, int type, int index) const {
    std::lock_guard lock(mutex_);
    if (!initialized_ || !token_)
        return ORBIS_PAD_ERROR_NOT_INITIALIZED;
    for (auto &p : ports_)
        if (p.handle && p.user == user && p.type == type && p.index == index)
            return p.handle;
    return ORBIS_PAD_ERROR_DEVICE_NO_HANDLE;
}
int OrbisPadAdapter::Close(int handle) {
    std::lock_guard lock(mutex_);
    auto *p = FindHandle(handle);
    if (!p)
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    (void)VibrateLocked(int(p - ports_.data()), 0, 0);
    p->handle = 0;
    p->history.clear();
    return 0;
}
int OrbisPadAdapter::Read(int handle, OrbisPadData *out, int count, bool latest) {
    std::lock_guard lock(mutex_);
    if (!out || count < 1 || count > ORBIS_PAD_MAX_DATA_NUM)
        return ORBIS_PAD_ERROR_INVALID_ARG;
    auto *p = FindHandle(handle);
    if (!p)
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    if (latest || p->history.empty()) {
        *out = p->data;
        return 1;
    }
    int n = 0;
    while (n < count && !p->history.empty()) {
        out[n++] = p->history.front();
        p->history.pop_front();
    }
    return n;
}
int OrbisPadAdapter::Information(int handle, OrbisPadControllerInformation *out) const {
    std::lock_guard lock(mutex_);
    if (!out)
        return ORBIS_PAD_ERROR_INVALID_ARG;
    auto *p = FindHandle(handle);
    if (!p)
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    *out = {};
    out->touchPadInfo.pixelDensity = 1;
    out->touchPadInfo.resolution = {1920, 950};
    out->stickInfo.deadZoneLeft = out->stickInfo.deadZoneRight = 1;
    out->connectionType = ORBIS_PAD_CONNECTION_TYPE_LOCAL;
    out->deviceClass = OrbisPadDeviceClass::Standard;
    out->connected = p->data.connected;
    out->connectedCount = p->data.connectedCount;
    return 0;
}
int OrbisPadAdapter::Vibrate(int handle, const OrbisPadVibrationParam *v) {
    std::lock_guard lock(mutex_);
    if (!v)
        return ORBIS_PAD_ERROR_INVALID_ARG;
    auto *p = FindHandle(handle);
    if (!p)
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    if (!p->data.connected)
        return ORBIS_PAD_ERROR_DEVICE_NOT_CONNECTED;
    return VibrateLocked(int(p - ports_.data()), v->smallMotor, v->largeMotor) == PadResult::Ok
               ? 0
               : ORBIS_PAD_ERROR_NOT_PERMITTED;
}
OrbisPadAdapter &GlobalPadAdapter() {
    static OrbisPadAdapter adapter;
    return adapter;
}
} // namespace Core::HostRuntime
