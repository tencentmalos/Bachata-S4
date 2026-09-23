// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_audio.h"
#include "core/host_runtime/guest_audio3d.h"
#include "core/host_runtime/guest_clock.h"
#include "core/libraries/audio3d/audio3d.h"
#include "core/libraries/audio3d/audio3d_error.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
using namespace Libraries::Audio3d;
using namespace Libraries::AudioOut;
static unsigned checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)
struct State {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::vector<std::vector<s16>> blocks;
    bool block{};
};
class Sink : public PortBackend {
    std::shared_ptr<State> state;
    u32 samples;

public:
    Sink(std::shared_ptr<State> state, PortOut& port)
        : state(state), samples(port.buffer_frames * 2) {}
    void Output(void* ptr) override {
        (void)OutputChecked(ptr, {});
    }
    int OutputChecked(void* ptr, std::stop_token stop) override {
        std::unique_lock lock(state->mutex);
        state->blocks.emplace_back(static_cast<s16*>(ptr), static_cast<s16*>(ptr) + samples);
        state->cv.notify_all();
        state->cv.wait(lock, stop, [&] { return !state->block; });
        return stop.stop_requested() ? -1 : 0;
    }
    void SetVolume(const std::array<int, 8>&) override {}
};
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 1 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    u64 b = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{b}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    CHECK(space->Map({GuestAddress{b + 0x4000}, 0x4000},
                     GuestPermission::Read | GuestPermission::Write));
    auto write = [&](u64 ptr, const auto& value) {
        CHECK(space->WriteData(GuestAddress{ptr}, std::as_bytes(std::span{&value, 1})));
    };
    auto read = [&](u64 ptr) {
        u32 out{};
        CHECK(space->ReadData(GuestAddress{ptr}, std::as_writable_bytes(std::span{&out, 1})));
        return out;
    };
    auto state = std::make_shared<State>();
    GuestClock clock;
    GuestAudio audio(*space, clock,
                     [state](PortOut& port) { return std::make_unique<Sink>(state, port); });
    GuestAudio3d engine(*space, audio);
    auto call = [&](std::string_view nid, std::array<u64, 6> a = {}) {
        return u32(engine.Dispatch(nid, a));
    };
    CHECK(call("lw0qrdSjZt8", {1}) == u32(ORBIS_AUDIO3D_ERROR_NOT_READY));
    CHECK(call("UmCvjSmuZIw", {1}) == u32(ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER));
    CHECK(call("UmCvjSmuZIw") == 0);
    CHECK(call("UmCvjSmuZIw") == u32(ORBIS_AUDIO3D_ERROR_NOT_READY));
    CHECK(call("Im+jOoa5WAI", {b}) == 0);
    CHECK(call("XeDDK0xJWQA", {255, b, b + 0x100}) == 0);
    u32 port = read(b + 0x100);
    CHECK(call("XeDDK0xJWQA", {255, b, b + 0x9000}) == u32(ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER));
    CHECK(call("YaaDbDwKpFM", {port, b + 0x110, b + 0x114}) == 0);
    CHECK(read(b + 0x110) == 0 && read(b + 0x114) == 2);
    write(b + 0x110, u32(77));
    CHECK(call("YaaDbDwKpFM", {port, b + 0x110, b + 0x9000}) ==
          u32(ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER));
    CHECK(read(b + 0x110) == 77);
    std::array<s16, 512> stereo{};
    for (size_t i = 0; i < stereo.size(); i += 2) {
        stereo[i] = 16384;
        stereo[i + 1] = -8192;
    }
    CHECK(space->WriteData(GuestAddress{b + 0x3e00}, std::as_bytes(std::span(stereo)).first(512)));
    CHECK(space->WriteData(
        GuestAddress{b + 0x4000},
        std::as_bytes(std::span(stereo)).subspan(512))); // one PCM input across two mappings
    CHECK(call("9tEwE0GV0qo", {port, 2, 0, b + 0x3e00, 256}) == 0);
    CHECK(call("lw0qrdSjZt8", {port}) == 0);
    CHECK(call("VEVhZ9qd4ZY", {port, 1}) == 0); // depth not reached: keep frame
    CHECK(call("YaaDbDwKpFM", {port, b + 0x110, 0}) == 0 && read(b + 0x110) == 1);
    CHECK(call("ZOGrxWLgQzE", {port}) == 0);
    {
        std::lock_guard lock(state->mutex);
        CHECK(state->blocks.size() == 1);
        for (size_t i = 0; i < 512; i += 2) {
            CHECK(state->blocks[0][i] == 16383);
            CHECK(state->blocks[0][i + 1] == -8191);
        }
    }
    CHECK(call("jO2tec4dJ2M", {port, b + 0x120}) == 0);
    auto object = read(b + 0x120);
    std::array<float, 256> mono;
    mono.fill(0.25f);
    write(b + 0x2000, mono);
    struct Pcm {
        u32 format, pad;
        u64 buffer;
        u32 count, pad2;
    } pcm{1, 0, b + 0x2000, 256, 0};
    struct Attr {
        u32 id, pad;
        u64 value, size;
    };
    write(b + 0x200, pcm);
    write(b + 0x240, 2.0f);
    std::array<Attr, 2> attrs{{{1, 0, b + 0x200, 24}, {3, 0, b + 0x240, 4}}};
    write(b + 0x300, attrs);
    CHECK(call("4uyHN9q4ZeU", {port, object, 2, b + 0x300}) == 0);
    // Bad second input must not enqueue the otherwise valid first PCM.
    attrs[1].value = b + 0x9000;
    write(b + 0x300, attrs);
    CHECK(call("4uyHN9q4ZeU", {port, object, 2, b + 0x300}) ==
          u32(ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER));
    write(b + 0x244, u32(2));
    CHECK(call("V1FBFpNIAzk", {port, object, 6, b + 0x244, 4}) == 0);
    CHECK(call("1HXxo-+1qCw", {port, object}) == 0);
    CHECK(call("ZOGrxWLgQzE", {port}) == 0);
    {
        std::lock_guard lock(state->mutex);
        CHECK(state->blocks.size() == 2);
        CHECK(state->blocks.back()[0] == 0 && state->blocks.back()[1] == 16383);
    }
    CHECK(call("ZOGrxWLgQzE", {port}) == 0);
    {
        std::lock_guard lock(state->mutex);
        CHECK(state->blocks.size() == 2);
    }
    CHECK(call("1HXxo-+1qCw", {port, object}) == u32(ORBIS_AUDIO3D_ERROR_INVALID_OBJECT));
    CHECK(call("Yq9bfUQ0uJg", {port, 1, b + 0x240, 4}) == u32(ORBIS_AUDIO3D_ERROR_NOT_SUPPORTED));
    write(b + 0x248, 0.0f);
    CHECK(call("Yq9bfUQ0uJg", {port, 0x10001, b + 0x248, 4}) == 0);
    CHECK(call("Yq9bfUQ0uJg", {port, 0x10001, b + 0x240, 4}) ==
          u32(ORBIS_AUDIO3D_ERROR_NOT_SUPPORTED));
    CHECK(call("VEVhZ9qd4ZY", {port, 2}) == u32(ORBIS_AUDIO3D_ERROR_INVALID_PARAMETER));
    CHECK(call("lw0qrdSjZt8", {port}) == 0);
    CHECK(call("lw0qrdSjZt8", {port}) == 0);
    CHECK(call("lw0qrdSjZt8", {port}) == u32(ORBIS_AUDIO3D_ERROR_NOT_READY));
    // Flush blocks on the real sink; close must cancel it without taking submit mutex.
    {
        std::lock_guard lock(state->mutex);
        state->block = true;
    }
    auto flush =
        std::async(std::launch::async, [&] { return engine.Dispatch("ZOGrxWLgQzE", {port}); });
    {
        std::unique_lock lock(state->mutex);
        CHECK(state->cv.wait_for(lock, std::chrono::seconds(3),
                                 [&] { return state->blocks.size() > 2; }));
    }
    CHECK(call("VEVhZ9qd4ZY", {port, 0}) == u32(ORBIS_AUDIO3D_ERROR_NOT_READY));
    CHECK(call("OyVqOeVNtSk", {port}) == 0);
    CHECK(flush.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    CHECK(s32(flush.get()) < 0);
    CHECK(call("lw0qrdSjZt8", {port}) == u32(ORBIS_AUDIO3D_ERROR_INVALID_PORT));
    CHECK(call("WW1TS2iz5yc") == 0);
    CHECK(call("UmCvjSmuZIw") == 0);
    engine.RequestStop();
    CHECK(call("UmCvjSmuZIw") == u32(ORBIS_AUDIO3D_ERROR_NOT_READY));
    std::printf("checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
