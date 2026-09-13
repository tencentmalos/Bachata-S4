// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cstdio>
#include <latch>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_audio.h"
#include "core/host_runtime/guest_clock.h"
#include "core/libraries/audio/audioout_error.h"
#include "core/libraries/audio/audioout_transfer.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::AudioOut;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)
struct SinkState {
    std::mutex mutex;
    std::condition_variable_any cv;
    bool block{}, entered{};
    int error{};
    unsigned calls{};
    std::vector<u8> pcm;
    std::array<int, 8> volume{};
};
class Sink : public PortBackend {
public:
    std::shared_ptr<SinkState> state;
    u32 size;
    Sink(std::shared_ptr<SinkState> state, PortOut& port)
        : state(std::move(state)), size(port.BufferSize()) {}
    void Output(void* ptr) override {
        (void)OutputChecked(ptr, {});
    }
    int OutputChecked(void* ptr, std::stop_token stop) override {
        std::unique_lock lock(state->mutex);
        state->entered = true;
        ++state->calls;
        state->pcm.assign(static_cast<u8*>(ptr), static_cast<u8*>(ptr) + size);
        state->cv.notify_all();
        state->cv.wait(lock, stop, [&] { return !state->block; });
        return stop.stop_requested() ? -1 : state->error;
    }
    void SetVolume(const std::array<int, 8>& volume) override {
        std::lock_guard lock(state->mutex);
        state->volume = volume;
    }
};
int main() {
    std::vector<u32> offsets;
    auto transfer = TransferAudioFrames(
        10, {},
        [&](u32 offset, u32 remaining, int64_t timeout) {
            offsets.push_back(offset);
            return std::min(remaining, 3u);
        },
        [](int) { return false; });
    CHECK(!transfer.error && transfer.accepted == 10 && (offsets == std::vector<u32>{0, 3, 6, 9}));
    int calls{};
    offsets.clear();
    transfer = TransferAudioFrames(
        10, {},
        [&](u32 offset, u32 remaining, int64_t) {
            offsets.push_back(offset);
            if (++calls == 2)
                return -20;
            return int(std::min(remaining, 5u));
        },
        [](int error) { return error == -20; });
    CHECK(!transfer.error && transfer.accepted == 10 && (offsets == std::vector<u32>{0, 5, 5}));
    std::stop_source cancel;
    transfer = TransferAudioFrames(
        10, cancel.get_token(),
        [&](u32, u32, int64_t) {
            cancel.request_stop();
            return 3;
        },
        [](int) { return false; });
    CHECK(transfer.cancelled && transfer.accepted == 3);
    transfer = TransferAudioFrames(
        10, {}, [](u32, u32, int64_t) { return -30; }, [](int) { return false; });
    CHECK(transfer.error == -30);
    transfer = TransferAudioFrames(
        10, {}, [](u32, u32, int64_t) { return 11; }, [](int) { return false; });
    CHECK(transfer.error < 0);
    transfer =
        TransferAudioFrames(10, {}, [](u32, u32, int64_t) { return 0; }, [](int) { return false; });
    CHECK(transfer.timed_out && transfer.accepted == 0);

    std::array<int, 8> volume{16384, 0, 32768, 32768, 32768, 32768, 32768, 32768};
    std::array<s16, 2> input{16384, 16384};
    std::array<float, 8> output{};
    ConvertAudioFrames(GetFormatInfo(OrbisAudioOutParamFormat::S16Stereo), 1, input.data(), volume,
                       0.5f, output);
    CHECK(output[0] == 0.125f && output[1] == 0);
    volume.fill(32768);
    std::array<float, 8> channels{0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
    ConvertAudioFrames(GetFormatInfo(OrbisAudioOutParamFormat::Float_8CH_Std), 1, channels.data(),
                       volume, 1, output);
    CHECK(output[4] == channels[6] && output[6] == channels[4]);
    AddressSpaceConfig config{};
    config.reservation_size = 1 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    auto write = [&](u64 address, const auto& value) {
        CHECK(space->Write(GuestAddress{address}, std::as_bytes(std::span{&value, 1})));
    };
    auto read = [&]<class T>(u64 address) {
        T value{};
        CHECK(space->Read(GuestAddress{address}, std::as_writable_bytes(std::span{&value, 1})));
        return value;
    };
    GuestClock clock;
    std::recursive_mutex vm;
    auto sink = std::make_shared<SinkState>();
    GuestAudio audio(*space, vm, clock,
                     [sink](PortOut& port) { return std::make_unique<Sink>(sink, port); });
    auto call = [&](std::string_view nid, std::array<u64, 6> args = {}) {
        return audio.Dispatch(nid, args);
    };
    CHECK(call("JfEPXVxhFqA") == 0);
    CHECK(call("JfEPXVxhFqA") == u32(ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT));
    CHECK(call("ekNvsT22rsY", {1, 0, 0, 0, 48000, 1}) == u32(ORBIS_AUDIO_OUT_ERROR_INVALID_SIZE));
    CHECK(call("ekNvsT22rsY", {1, 4, 0, 256, 48000, 1}) == u32(ORBIS_AUDIO_OUT_ERROR_NOT_OPENED));
    const u64 handle = call("ekNvsT22rsY", {1, 0, 0, 256, 48000, 1});
    CHECK(s32(handle) > 0);
    CHECK(call("QOQtbeDqsT4", {handle, 1}) == u32(ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER));
    CHECK(call("QOQtbeDqsT4", {handle + 1, base}) == u32(ORBIS_AUDIO_OUT_ERROR_INVALID_PORT));
    write(base, s32(16384));
    CHECK(call("b+uAV89IlxE", {handle, 3, base}) == 0);
    write(base, s32(-1));
    CHECK(call("b+uAV89IlxE", {handle, 3, base}) == u32(ORBIS_AUDIO_OUT_ERROR_INVALID_VOLUME));
    std::array<s16, 512> pcm{};
    pcm.fill(321);
    write(base + 0x1000, pcm);
    CHECK(call("QOQtbeDqsT4", {handle, base + 0x1000}) == 512);
    CHECK(call("QOQtbeDqsT4", {handle, 0}) == 0); // drain accepted buffer
    {
        std::lock_guard lock(sink->mutex);
        CHECK(sink->calls == 1 && sink->pcm.size() == 1024 && sink->volume[0] == 16384);
    }
    CHECK(call("Ptlts326pds", {handle, base}) == 0);
    CHECK(read.operator()<u64>(base) > 0);
    CHECK(call("GrQ9s4IrNaQ", {handle, base}) == 0);
    CHECK(read.operator()<OrbisAudioOutPortState>(base).channel == 2);
    struct Param {
        u32 handle, pad;
        u64 ptr;
    };
    std::array<Param, 2> duplicate{
        {{u32(handle), 0, base + 0x1000}, {u32(handle), 0, base + 0x1000}}};
    write(base, duplicate);
    CHECK(call("w3PdaSTSwGE", {base, 2}) == u32(ORBIS_AUDIO_OUT_ERROR_INVALID_PORT));
    const u64 second = call("ekNvsT22rsY", {1, 0, 0, 256, 48000, 1});
    CHECK(second != handle && s32(second) > 0);
    duplicate[1].handle = second;
    duplicate[1].ptr = 1;
    write(base, duplicate);
    CHECK(call("w3PdaSTSwGE", {base, 2}) == u32(ORBIS_AUDIO_OUT_ERROR_INVALID_POINTER));
    {
        std::lock_guard lock(sink->mutex);
        CHECK(sink->calls == 1);
    }
    duplicate[1].ptr = base + 0x1000;
    write(base, duplicate);
    CHECK(call("w3PdaSTSwGE", {base, 2}) == 256);
    CHECK(call("QOQtbeDqsT4", {handle, 0}) == 0);
    CHECK(call("QOQtbeDqsT4", {second, 0}) == 0);
    auto reverse = duplicate;
    std::swap(reverse[0], reverse[1]);
    write(base + 0x100, reverse);
    std::atomic<bool> concurrent_ok{true};
    std::jthread group1([&] {
        for (int i = 0; i < 3; ++i)
            if (call("w3PdaSTSwGE", {base, 2}) != 256)
                concurrent_ok = false;
    });
    std::jthread group2([&] {
        for (int i = 0; i < 3; ++i)
            if (call("w3PdaSTSwGE", {base + 0x100, 2}) != 256)
                concurrent_ok = false;
    });
    group1.join();
    group2.join();
    CHECK(concurrent_ok);
    CHECK(call("QOQtbeDqsT4", {handle, 0}) == 0);
    CHECK(call("QOQtbeDqsT4", {second, 0}) == 0);
    {
        std::lock_guard lock(sink->mutex);
        sink->block = true;
        sink->entered = false;
    }
    CHECK(call("QOQtbeDqsT4", {handle, base + 0x1000}) == 512);
    {
        std::unique_lock lock(sink->mutex);
        CHECK(sink->cv.wait_for(lock, std::chrono::seconds(1), [&] { return sink->entered; }));
    }
    // Entered native output can be stopped without holding any VM pin/gate.
    const auto start = std::chrono::steady_clock::now();
    CHECK(call("s1--uE9mBFw", {handle}) == 0);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    CHECK(call("QOQtbeDqsT4", {handle, base}) == u32(ORBIS_AUDIO_OUT_ERROR_INVALID_PORT));
    {
        std::lock_guard lock(sink->mutex);
        sink->block = false;
        sink->error = -40;
    }
    CHECK(call("QOQtbeDqsT4", {second, base + 0x1000}) == 512);
    CHECK(call("QOQtbeDqsT4", {second, 0}) == u32(ORBIS_AUDIO_OUT_ERROR_TRANS_EVENT));
    CHECK(call("s1--uE9mBFw", {second}) == 0);
    audio.RequestStop();
    CHECK(call("JfEPXVxhFqA") == u32(ORBIS_AUDIO_OUT_ERROR_NOT_INIT));
    GuestAudio failed(*space, vm, clock,
                      [](PortOut&) -> std::unique_ptr<PortBackend> { return {}; });
    CHECK(failed.Dispatch("JfEPXVxhFqA", {}) == 0);
    CHECK(failed.Dispatch("ekNvsT22rsY", {1, 0, 0, 256, 48000, 1}) ==
          u32(ORBIS_AUDIO_OUT_ERROR_TRANS_EVENT));
    std::printf("GUEST_AUDIO checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
