// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <thread>
#include "core/host_runtime/guest_page_states.h"
#include "core/host_runtime/guest_video_event_hle.h"
#include "core/host_runtime/guest_video_mode.h"
#include "core/host_runtime/guest_graphics_hle.h"
using namespace Core::GuestCpu;
using Core::HostRuntime::AcquireGraphicsAdmission;
static unsigned checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made) return 2;
    auto space = std::move(made).Value();
    using namespace Core::HostRuntime;
    GuestPageStates states;
    CHECK(states.AllocatedBytes() == 0 && states.Load(0x4000000000ULL) == 0);
    std::thread writer([&] { states.Ensure(0x4000000000ULL, 0x4000); });
    states.Ensure(0x4000000000ULL, 0x4000);
    writer.join();
    states.Ensure(0x1000, 0x1000);
    states.At(0x4000000000ULL / 4096).store(0x9b);
    states.At(1).store(0x83);
    CHECK(states.Load(0x4000000000ULL) == 0x9b && states.Load(0x1000) == 0x83);
    CHECK(states.Load(GuestPageStates::AddressLimit) == 0);
    CHECK(states.AllocatedBytes() == 2 * sizeof(GuestPageStates::Chunk));
    bool refused{};
    try { states.Ensure(GuestPageStates::AddressLimit - 4096, 8192); }
    catch (const std::out_of_range&) { refused = true; }
    CHECK(refused);
    using namespace Libraries;
    const auto address = space->ReservationBase().value;
    CHECK(space->Map({{address}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    CHECK(space->Map({{address + 0x4000}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    {
        const GuestRange command{{address + 0x3ff4}, 28};
        CHECK(!space->AcquireDataSpan(command, true)); // old contract rejected valid stream
        auto output = AcquireGraphicsCommandBuffer(*space, command, true);
        CHECK(output && output.Value().WritableBytes().size() == 28);
        if (output) std::memset(output.Value().WritableBytes().data(), 0x6b, 28);
    }
    {
        auto input = AcquireGraphicsCommandBuffer(*space, {{address + 0x3ff4}, 28}, false);
        CHECK(input && std::ranges::all_of(input.Value().Bytes(), [](auto b) { return b == std::byte{0x6b}; }));
    }
    CHECK(space->Protect({{address + 0x4000}, 0x4000}, GuestPermission::Read));
    CHECK(!AcquireGraphicsCommandBuffer(*space, {{address + 0x3ff4}, 28}, true));
    CHECK(AcquireGraphicsCommandBuffer(*space, {{address + 0x3ff4}, 28}, false));
    CHECK(space->Unmap({{address + 0x4000}, 0x4000}));
    CHECK(!AcquireGraphicsCommandBuffer(*space, {{address + 0x3ff4}, 28}, false));
    CHECK(!AcquireGraphicsCommandBuffer(*space, {{~u64(0) - 3}, 28}, true));
    {
        VideoOut::Mode mode{};
        VideoOut::sceVideoOutModeSetAny_(&mode, sizeof(mode));
        const auto mode_address = address + 0x101; // unaligned input is valid
        const auto options_address = address + 0x201;
        CHECK(space->WriteData({mode_address}, std::as_bytes(std::span{&mode, 1})));
        std::array<u8, 16> options{};
        const std::array<u8, 16> empty_options{};
        options[0] = 4;
        CHECK(space->WriteData({options_address}, std::as_bytes(std::span{options})));
        GuestVideoModeRequest request{};
        std::array<u64, 6> args{1, 0, mode_address, 0, sizeof(mode), 16};
        auto read = [&] { return ReadGuestVideoMode(*space, args, request); };
        // Tetris's exact NULL + sizeof(options) call and the other firmware
        // combinations: size is an ABI version, not a pointer presence flag.
        CHECK(read() == 0);
        CHECK(request.mode.size == 32 && request.mode.colorimetry == 0xff);
        CHECK(request.mode.refresh_rate == ~u64(0) && request.options == empty_options);
        args[5] = 0;
        CHECK(read() == 0);
        args[3] = options_address;
        CHECK(read() == 0 && request.options == options);
        args[5] = 16;
        CHECK(read() == 0 && request.options == options);
        CHECK(space->Protect({{address}, 0x4000}, GuestPermission::Read));
        CHECK(read() == 0 && request.options == options);
        CHECK(space->Protect({{address}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
        args[1] = 1;
        CHECK(read() == u32(ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE));
        args[1] = 0;
        args[4] = 31;
        CHECK(read() == u32(ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE));
        args[4] = 32;
        for (u32 size : {1u, 15u, 17u, ~0u}) {
            args[5] = size;
            CHECK(read() == u32(ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE));
        }
        args[5] = 16;
        args[2] = 0;
        CHECK(read() == u32(ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE));
        for (u64 invalid : {address + 0x3ff0, ~u64(0) - 15}) {
            args[2] = invalid;
            CHECK(read() == u32(ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS));
        }
        args[2] = mode_address;
        args[3] = address + 0x3ff8;
        CHECK(read() == u32(ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS));
        CHECK(request.options == options); // no partially published request
        args[3] = 0;
        CHECK(read() == 0 && request.options == empty_options);
        mode.size = 0;
        CHECK(space->WriteData({mode_address}, std::as_bytes(std::span{&mode, 1})));
        CHECK(read() == u32(ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE));
    }
    Kernel::OrbisKernelEvent event{};
    event.filter = Kernel::OrbisKernelEvent::Filter::VideoOut;
    event.ident = static_cast<u64>(VideoOut::OrbisVideoOutInternalEventId::Flip);
    for (s64 value : {s64(0), s64(123), s64(-1), s64(-123)}) {
        event.data = (u64(value) << 16) | 0x1234;
        CHECK(space->WriteData({address}, std::as_bytes(std::span{&event, 1})));
        CHECK(DispatchVideoEventGetter(*space, "rWUTcKdkUzQ", address, address + 64) == 0);
        s64 output{};
        CHECK(space->ReadData({address + 64}, std::as_writable_bytes(std::span{&output, 1})));
        CHECK(output == value);
        CHECK(DispatchVideoEventGetter(*space, "U2JJtSqNKZI", address, 0) ==
              u32(VideoOut::OrbisVideoOutEventId::Flip));
        CHECK(DispatchVideoEventGetter(*space, "Mt4QHHkxkOc", address, 0) == 1);
    }
    CHECK(DispatchVideoEventGetter(*space, "rWUTcKdkUzQ", 0, address + 64) ==
          u32(ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS));
    CHECK(DispatchVideoEventGetter(*space, "rWUTcKdkUzQ", address, 0) ==
          u32(ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS));
    event.ident = static_cast<u64>(VideoOut::OrbisVideoOutInternalEventId::Vblank);
    event.data = (u64(321) << 16) | 0xffff;
    CHECK(space->WriteData({address}, std::as_bytes(std::span{&event, 1})));
    CHECK(DispatchVideoEventGetter(*space, "Mt4QHHkxkOc", address, 0) == 15);
    CHECK(DispatchVideoEventGetter(*space, "U2JJtSqNKZI", address, 0) ==
          u32(VideoOut::OrbisVideoOutEventId::Vblank));
    event.filter = Kernel::OrbisKernelEvent::Filter::User;
    CHECK(space->WriteData({address}, std::as_bytes(std::span{&event, 1})));
    for (auto nid : VideoEventGetterNids)
        CHECK(DispatchVideoEventGetter(*space, nid, address, address + 64) ==
              u32(ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT));
    const auto later = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto admitted = AcquireGraphicsAdmission(*space, {}, later, {});
    CHECK(admitted && admitted.Value().IsValid());
    CHECK(!space->Quiesce(0));
    admitted.Value() = ExecutionLease{};
    auto token = space->Quiesce(0);
    CHECK(token);
    auto busy = AcquireGraphicsAdmission(*space, {}, later, {});
    CHECK(!busy && busy.Category() == ErrorCategory::Busy);
    unsigned waits{};
    auto expired = AcquireGraphicsAdmission(*space, {}, std::chrono::steady_clock::now(), [&] { ++waits; });
    CHECK(!expired && expired.Category() == ErrorCategory::Busy && waits == 0);
    std::stop_source cancel;
    auto stopped = AcquireGraphicsAdmission(*space, cancel.get_token(), later, [&] {
        ++waits;
        cancel.request_stop();
    });
    CHECK(stopped && !stopped.Value().IsValid() && waits == 1);
    CHECK(!space->AcquireExecutionLease()); // cancellation did not break the VM barrier
    auto already = AcquireGraphicsAdmission(*space, cancel.get_token(), later, [&] { ++waits; });
    CHECK(already && !already.Value().IsValid() && waits == 1);
    token.Value() = QuiescenceToken{};
    auto recovered = AcquireGraphicsAdmission(*space, {}, later, {});
    CHECK(recovered && recovered.Value().IsValid());
    std::printf("GRAPHICS_ADMISSION checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
