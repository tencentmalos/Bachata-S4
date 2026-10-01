// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_capture_services.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
static unsigned checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const auto base = space->ReservationBase().value;
    CHECK(space->Map({{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    constexpr u32 absent = u32(ORBIS_KERNEL_ERROR_ENOSYS), invalid = 0x80be0001;
    auto call = [&](std::string_view nid, std::array<u64, 6> args = {}, u32 sdk = 0x09000000) {
        const auto* entry = FindCaptureService(nid);
        CHECK(entry);
        return DispatchCaptureService(*space, *entry, args, sdk);
    };
    for (const auto& entry : CaptureServiceEntries) {
        CHECK(AdmitsCaptureService(entry.nid, entry.suffix));
        CHECK(!AdmitsCaptureService(entry.nid, "#libkernel#1#libkernel#Function"));
        if (entry.call == CaptureServiceCall::ScreenControl)
            CHECK(call(entry.nid) == absent);
    }
    CHECK(!FindCaptureService("unknown"));
    CHECK(call("j7DlalBzHh8", {0, 0x20000}) == 0x81170003);
    CHECK(call("j7DlalBzHh8", {1, 0x1ffff}) == 0x81170003);
    CHECK(call("j7DlalBzHh8", {1, 0x20000, UINT64_MAX, UINT64_MAX}) == absent);
    CHECK(call("DUWhxkyVPj4") == 0x81170102);
    CHECK(call("8hZ2EEl2Tto", {UINT64_MAX}) == 0x81170102);
    for (const auto origin : {0u, 10u, UINT32_MAX})
        CHECK(call("73WQ4Jj0nJI", {base, 0, 0, origin}) == invalid);
    for (u32 origin = 1; origin <= 9; ++origin)
        CHECK(call("73WQ4Jj0nJI", {base, 0, 0, origin}) == absent);
    CHECK(call("ahHhOf+QNkQ", {base, 1919, 1079}, 0x03400000) == absent);
    CHECK(call("ahHhOf+QNkQ", {base, 1920, 0}, 0x03400000) == invalid);
    CHECK(call("ahHhOf+QNkQ", {base, 0, 1080}, 0x03400000) == invalid);
    CHECK(call("ahHhOf+QNkQ", {base, 3839, 2159}) == absent);
    CHECK(call("ahHhOf+QNkQ", {base, 3840, 0}) == invalid);
    CHECK(call("ahHhOf+QNkQ", {base, UINT32_MAX, 0}) == invalid);
    for (const auto address : {u64(0), base + 0x4000, UINT64_MAX})
        CHECK(call("ahHhOf+QNkQ", {address, 0, 0}) == invalid);
    std::array<char, 1024> text;
    text.fill('x');
    CHECK(space->WriteData({base}, std::as_bytes(std::span{text})));
    CHECK(call("ahHhOf+QNkQ", {base}) == invalid);
    text.back() = 0;
    CHECK(space->WriteData({base}, std::as_bytes(std::span{text})));
    CHECK(call("ahHhOf+QNkQ", {base}) == absent);
    CHECK(space->Protect({{base}, 0x4000}, GuestPermission::Read));
    CHECK(call("ahHhOf+QNkQ", {base}) == absent);
    std::array<char, 1024> after{};
    CHECK(space->ReadData({base}, std::as_writable_bytes(std::span{after})));
    CHECK(text == after);
    std::printf("CAPTURE_SERVICES checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
