// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_pad.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::Pad;
static unsigned checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__); } } while (0)
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made) return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    auto read = [&]<class T>(u64 address) {
        T value{};
        CHECK(space->Read(GuestAddress{address}, std::as_writable_bytes(std::span{&value, 1})));
        return value;
    };
    auto write = [&](u64 address, const auto& value) {
        CHECK(space->Write(GuestAddress{address}, std::as_bytes(std::span{&value, 1})));
    };
    std::array<GuestUser, 4> users{};
    users[0] = {1000, "first"}; users[1] = {1001, "second"};
    GuestPlatform platform(users, 0x9000000, 1, false);
    OrbisPadAdapter adapter;
    GuestPad pad(adapter, platform);
    auto call = [&](std::string_view nid, std::array<u64, 6> args = {}) {
        CHECK(IsPadNid(nid));
        return pad.Dispatch(*space, nid, args);
    };
    CHECK(call("hv1luiJrqQM") == u32(ORBIS_PAD_ERROR_NOT_INITIALIZED));
    auto token = adapter.BeginSession();
    CHECK(call("xk0AcarP3V4", {1000}) == u32(ORBIS_PAD_ERROR_NOT_INITIALIZED));
    CHECK(call("hv1luiJrqQM") == 0);
    CHECK(call("xk0AcarP3V4", {1000}) == u32(ORBIS_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN));
    CHECK(platform.Initialize() == 0);
    CHECK(call("xk0AcarP3V4", {UINT64_MAX}) == u32(ORBIS_DEVICE_SERVICE_ERROR_INVALID_USER));
    CHECK(call("xk0AcarP3V4", {2000}) == u32(ORBIS_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN));
    CHECK(call("xk0AcarP3V4", {1000, 0, 0, 1}) == u32(ORBIS_PAD_ERROR_INVALID_ARG));
    CHECK(call("xk0AcarP3V4", {1000, 0, 1}) == u32(ORBIS_PAD_ERROR_INVALID_ARG));
    CHECK(adapter.SetConnected(token, 0, true) == PadResult::Ok);
    CHECK(adapter.Submit(token, 0, {.buttons = 0x4000, .left_x = -1}) == PadResult::Ok);
    const u64 handle = call("xk0AcarP3V4", {1000});
    CHECK(handle > 0 && s32(handle) > 0);
    CHECK(call("u1GRHp+oWoY", {1000}) == handle);
    CHECK(call("xk0AcarP3V4", {1000}) == u32(ORBIS_PAD_ERROR_ALREADY_OPENED));
    CHECK(adapter.Submit(token, 0, {.buttons = 0x8000, .left_x = 1}) == PadResult::Ok);
    CHECK(call("q1cHNfGycLI", {handle, 1, 1}) == u32(ORBIS_PAD_ERROR_INVALID_ARG));
    CHECK(call("q1cHNfGycLI", {handle, base, 0}) == u32(ORBIS_PAD_ERROR_INVALID_ARG));
    CHECK(call("q1cHNfGycLI", {handle, base, UINT64_MAX}) == u32(ORBIS_PAD_ERROR_INVALID_ARG));
    CHECK(call("q1cHNfGycLI", {handle, base, ORBIS_PAD_MAX_DATA_NUM + 1}) == u32(ORBIS_PAD_ERROR_INVALID_ARG));
    CHECK(call("q1cHNfGycLI", {handle, base, 1}) == 1); // invalid outputs didn't consume history
    auto data = read.operator()<OrbisPadData>(base);
    CHECK(data.connected && u32(data.buttons) == 0x4000 && data.leftStick.x == 0);
    CHECK(call("YndgXqQVV7c", {handle, base}) == 0);
    CHECK(call("gjP9-KQzoUk", {handle, base}) == 0);
    CHECK(read.operator()<OrbisPadControllerInformation>(base).connected);
    CHECK(call("hGbf2QTBmqc", {handle, base}) == 0);
    CHECK(read.operator()<OrbisPadExtendedControllerInformation>(base).padType1 == 0);
    write(base, OrbisPadVibrationParam{255, 128});
    CHECK(call("yFVnOdGxvZY", {handle, base}) == u32(ORBIS_PAD_ERROR_NOT_PERMITTED)); // no phone rumble
    spatial::input::DeviceCapabilities caps; caps.has_rumble = true;
    const auto epoch = adapter.RegisterDevice(token, 0, 42, caps);
    CHECK(epoch != 0);
    CHECK(call("yFVnOdGxvZY", {handle, base}) == 0);
    auto commands = adapter.DrainHaptics(token);
    CHECK(commands.size() == 1 && commands[0].device.connection_epoch == epoch && commands[0].large_motor == 1);
    CHECK(call("yFVnOdGxvZY", {handle, 1}) == u32(ORBIS_PAD_ERROR_INVALID_ARG));
    CHECK(adapter.DrainHaptics(token).empty());
    write(base, OrbisPadLightBarParam{1, 1, 1});
    CHECK(call("RR4novUEENY", {handle, base}) == u32(ORBIS_PAD_ERROR_INVALID_LIGHTBAR_SETTING));
    write(base, OrbisPadLightBarParam{255, 1, 1});
    CHECK(call("RR4novUEENY", {handle, base}) == u32(ORBIS_PAD_ERROR_NOT_PERMITTED));
    CHECK(call("rIZnR6eSpvk", {handle}) == 0);
    CHECK(call("clVvL4ZDntw", {handle, 1}) == 0);
    CHECK(call("WFIiSfXGUq8", {1001}) != handle);
    CHECK(call("6ncge5+l5Qs", {handle}) == 0);
    CHECK(call("YndgXqQVV7c", {handle, base}) == u32(ORBIS_PAD_ERROR_INVALID_HANDLE));
    CHECK(call("6ncge5+l5Qs", {handle}) == u32(ORBIS_PAD_ERROR_INVALID_HANDLE));
    commands = adapter.DrainHaptics(token);
    CHECK(commands.size() == 1 && commands[0].cancel);
    adapter.EndSession(token);
    const auto next_token = adapter.BeginSession();
    CHECK(next_token != token);
    CHECK(call("hv1luiJrqQM") == u32(ORBIS_PAD_ERROR_NOT_INITIALIZED)); // stale domain cannot adopt replacement input
    CHECK(call("YndgXqQVV7c", {handle, base}) == u32(ORBIS_PAD_ERROR_NOT_INITIALIZED));
    GuestPad next(adapter, platform);
    CHECK(next.Dispatch(*space, "hv1luiJrqQM", {}) == 0);
    const auto next_handle = next.Dispatch(*space, "xk0AcarP3V4", {1000});
    CHECK(s32(next_handle) > 0 && next_handle != handle);
    adapter.EndSession(next_token);
    std::printf("GUEST_PAD checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
