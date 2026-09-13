// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/host_runtime/orbis_pad_adapter.h"
#include "core/libraries/pad/pad_errors.h"
#include <cstdio>
#include <limits>
using namespace Core::HostRuntime;
using namespace spatial::input;
using namespace Libraries::Pad;
static int checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)
int main() {
    OrbisPadAdapter pad;
    auto t = pad.BeginSession();
    CHECK(t != 0);
    CHECK(pad.SetConnected(t, 0, true) == PadResult::Ok);
    PadSnapshot overlay;
    overlay.buttons = 0x4000;
    overlay.left_x = -1;
    overlay.left_y = 1;
    CHECK(pad.Submit(t, 0, overlay) == PadResult::Ok);
    OrbisPadData d{};
    CHECK(pad.ReadState(0, &d));
    CHECK(d.leftStick.x == 0 && d.leftStick.y == 255 && d.rightStick.x == 128);
    auto before = d.timestamp;
    auto nan = overlay;
    nan.left_x = std::numeric_limits<float>::quiet_NaN();
    CHECK(pad.Submit(t, 0, nan) == PadResult::Rejected);
    CHECK(pad.ReadState(0, &d) && d.timestamp == before);
    CHECK(pad.Submit(t, -1, overlay) == PadResult::BadPort);
    CHECK(pad.SetConnected(t, 0, false) == PadResult::Ok);
    CHECK(pad.Submit(t, 0, overlay) == PadResult::Rejected);
    DeviceCapabilities caps;
    caps.axes = {{Axis::LeftStickX, -32768, 32767, 0}, {Axis::RightTrigger, 0, 255, 0}};
    caps.has_rumble = true;
    auto a = pad.RegisterDevice(t, 0, 100, caps), b = pad.RegisterDevice(t, 1, 101, caps);
    CHECK(a && b && a != b);
    CHECK(!pad.RegisterDevice(t, 2, 100, caps));
    InputPacket p;
    p.session_token = t;
    p.device = {Source::AndroidGamepad, 100, a, "same model"};
    p.sequence = 1;
    p.events = {
        {.button = Button::East, .pressed = true},
        {.kind = InputEvent::Kind::AxisValue, .axis = Axis::LeftStickX, .raw_value = -32768},
        {.kind = InputEvent::Kind::AxisValue, .axis = Axis::RightTrigger, .raw_value = 255}};
    CHECK(pad.SubmitPacket(t, 0, p) == PadResult::Ok);
    CHECK(pad.ReadState(0, &d) && u32(d.buttons) == 0x2200 && d.leftStick.x == 0 &&
          d.analogButtons.r2 == 255);
    CHECK(pad.ReadButtons(1) == 0);
    CHECK(pad.SubmitPacket(t, 0, p) == PadResult::Rejected); // duplicate sequence
    CHECK(pad.Initialize() == 0);
    auto h = pad.Open(1000, 0, 0, 0);
    CHECK(h > 0);
    CHECK(pad.GetHandle(1000, 0, 0) == h);
    CHECK(pad.Open(1000, 0, 0, 0) == ORBIS_PAD_ERROR_ALREADY_OPENED);
    CHECK(pad.Read(h, &d, 1, true) == 1 && u32(d.buttons) == 0x2200);
    // A short down/up between two guest polls remains present in bounded history.
    p.sequence = 2;
    p.events = {{.button = Button::South, .pressed = true}};
    CHECK(pad.SubmitPacket(t, 0, p) == PadResult::Ok);
    p.sequence = 3;
    p.events[0].pressed = false;
    CHECK(pad.SubmitPacket(t, 0, p) == PadResult::Ok);
    OrbisPadData history[ORBIS_PAD_MAX_DATA_NUM]{};
    const int n = pad.Read(h, history, ORBIS_PAD_MAX_DATA_NUM);
    bool saw_down = false, saw_up = false;
    for (int i = 0; i < n; ++i) {
        if (u32(history[i].buttons) & 0x4000)
            saw_down = true;
        else if (saw_down)
            saw_up = true;
    }
    CHECK(saw_down && saw_up);
    OrbisPadControllerInformation info{};
    CHECK(pad.Information(h, &info) == 0 && info.connected);
    OrbisPadVibrationParam rumble{.largeMotor = 60, .smallMotor = 120};
    CHECK(pad.Vibrate(h, &rumble) == 0);
    CHECK(pad.DrainHaptics(t + 1).empty());
    auto c = pad.DrainHaptics(t);
    CHECK(c.size() == 1 && c[0].device.backend_id == 100 && c[0].device.connection_epoch == a);
    CHECK(c.size() == 1 && c[0].small_motor > .47f && c[0].large_motor < .24f);
    CHECK(pad.SetVibration(t, 0, 0, 0) == PadResult::Ok);
    c = pad.DrainHaptics(t);
    CHECK(c.size() == 1 && c[0].cancel);
    CHECK(pad.SetVibration(t, 0, 100, 100) == PadResult::Ok);
    pad.RemoveDevice(t, 0, a);
    CHECK(pad.DrainHaptics(t).empty());
    CHECK(pad.Read(h, &d, 1, true) == 1 && !d.connected && u32(d.buttons) == 0);
    const auto replacement = pad.RegisterDevice(t, 0, 100, caps);
    CHECK(replacement && replacement != a);
    p.sequence = 4;
    CHECK(pad.SubmitPacket(t, 0, p) == PadResult::Rejected);
    pad.RemoveDevice(t, 0, a);
    CHECK(pad.Connected(0));
    p.device.connection_epoch = replacement;
    p.sequence = 1;
    CHECK(pad.SubmitPacket(t, 0, p) == PadResult::Ok);
    pad.FocusLost(t);
    CHECK(pad.ReadButtons(0) == 0 && pad.ReadButtons(1) == 0);
    auto next = pad.BeginSession();
    pad.EndSession(t);
    CHECK(pad.CurrentToken() == next);
    CHECK(pad.Read(h, &d, 1, true) == ORBIS_PAD_ERROR_INVALID_HANDLE);
    CHECK(pad.Submit(t, 0, overlay) == PadResult::WrongSession);
    CHECK(pad.DrainHaptics(t).empty());
    pad.EndSession(next);
    CHECK(pad.CurrentToken() == 0);
    CHECK(pad.Submit(next, 0, overlay) == PadResult::NoSession);
    std::printf("orbis_pad_adapter_tests: %d checks / %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
