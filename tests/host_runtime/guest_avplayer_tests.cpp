// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <thread>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_avplayer.h"
#include "core/libraries/avplayer/avplayer.h"
#include "core/libraries/avplayer/avplayer_error.h"
#include "core/libraries/videodec/video_utils.h"
extern "C" {
#include <libavutil/frame.h>
}
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
using namespace Libraries::AvPlayer;
static std::atomic_uint checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // FFmpeg may pad each source row even when width equals destination pitch.
    std::array<u8, 128 * 32> y{};
    std::array<u8, 128 * 16> uv{};
    std::array<u8, 64 * 48> nv12{};
    y.fill(0xff);
    uv.fill(0xee);
    for (unsigned row = 0; row < 32; ++row)
        std::fill_n(y.data() + row * 128, 64, u8(row + 1));
    for (unsigned row = 0; row < 16; ++row)
        std::fill_n(uv.data() + row * 128, 64, u8(row + 101));
    AVFrame frame{};
    frame.width = 64;
    frame.height = 32;
    frame.data[0] = y.data();
    frame.data[1] = uv.data();
    frame.linesize[0] = frame.linesize[1] = 128;
    Libraries::Videodec::CopyNV12Data(nv12.data(), nv12.size(), frame);
    for (unsigned row = 0; row < 32; ++row)
        CHECK(nv12[row * 64] == row + 1 && nv12[row * 64 + 63] == row + 1);
    for (unsigned row = 0; row < 16; ++row)
        CHECK(nv12[2048 + row * 64] == row + 101 && nv12[2048 + row * 64 + 63] == row + 101);
    if (argc != 2)
        return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<u8> file{std::istreambuf_iterator<char>(input), {}};
    if (file.empty())
        return 2;
    AddressSpaceConfig config;
    config.reservation_size = 32 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    const auto rw = GuestPermission::Read | GuestPermission::Write;
    CHECK(
        space->Map({GuestAddress{base}, 0x1000}, GuestPermission::Read | GuestPermission::Execute));
    CHECK(space->Map({GuestAddress{base + 0x1000}, 0xff000}, rw));
    std::recursive_mutex fixture_memory;
    std::mutex allocations_mutex;
    std::map<u64, u64> allocations;
    std::atomic_uint begins{}, ends{}, calls{}, events{}, allocs{}, frees{}, reads{};
    std::atomic_bool hang_event{}, event_entered{}, cancelled{};
    std::atomic_bool hold_publication{}, publication_entered{};
    std::atomic_bool hold_pause{}, pause_entered{};
    std::atomic_uint play_events{}, ready_events{};
    u64 next = base + 0x100000;
    auto pc = [&](u64 n) { return base + n * 16; };
    auto read = [&]<class T>(u64 at) {
        T v{};
        CHECK(space->Read(GuestAddress{at}, std::as_writable_bytes(std::span{&v, 1})));
        return v;
    };
    auto put = [&](u64 at, const auto& v) {
        std::lock_guard lock(fixture_memory);
        CHECK(space->Write(GuestAddress{at}, std::as_bytes(std::span{&v, 1})));
    };
    auto callbacks_factory = [&] {
        return GuestAvPlayer::Callbacks{
            .begin =
                [&](u64 entry) {
                    CHECK(entry == pc(1));
                    ++begins;
                    cancelled = false;
                    return base + 0x40000;
                },
            .call = [&](u64 entry, std::span<const u64> args) -> u64 {
                ++calls;
                if (entry == pc(1) || entry == pc(3)) {
                    CHECK(args.size() == 3 && args[0] == 0x1234);
                    const u64 bytes = (args[2] + 4095) & ~u64(4095);
                    std::lock_guard lock(fixture_memory);
                    std::lock_guard table(allocations_mutex);
                    const u64 va = next;
                    next += bytes;
                    CHECK(space->Map({GuestAddress{va}, bytes}, rw));
                    allocations.emplace(va, bytes);
                    ++allocs;
                    return va;
                }
                if (entry == pc(2) || entry == pc(4)) {
                    CHECK(args.size() == 2 && args[0] == 0x1234);
                    std::lock_guard lock(fixture_memory);
                    std::lock_guard table(allocations_mutex);
                    auto it = allocations.find(args[1]);
                    CHECK(it != allocations.end());
                    if (it != allocations.end()) {
                        CHECK(space->Unmap({GuestAddress{it->first}, it->second}));
                        allocations.erase(it);
                    }
                    ++frees;
                    return 0;
                }
                if (entry == pc(5)) {
                    CHECK(args.size() == 2 && args[0] == 0x4321);
                    CHECK(read.template operator()<char>(args[1]) == '/');
                    return 1;
                }
                if (entry == pc(6))
                    return 0;
                if (entry == pc(7)) {
                    CHECK(args.size() == 4 && args[0] == 0x4321);
                    ++reads;
                    const size_t n =
                        args[2] < file.size() ? std::min<u64>(args[3], file.size() - args[2]) : 0;
                    if (n) {
                        std::lock_guard lock(fixture_memory);
                        CHECK(space->Write(
                            GuestAddress{args[1]},
                            std::span{reinterpret_cast<const std::byte*>(file.data() + args[2]),
                                      n}));
                    }
                    return n;
                }
                if (entry == pc(8))
                    return file.size();
                if (entry == pc(9)) {
                    CHECK(args.size() == 4 && args[0] == 0x9876);
                    ++events;
                    if (args[1] == u32(AvPlayerEvents::StateReady))
                        ++ready_events;
                    if (args[1] == u32(AvPlayerEvents::StatePlay))
                        ++play_events;
                    if (args[1] == u32(AvPlayerEvents::StatePause)) {
                        pause_entered = true;
                        while (hold_pause && !cancelled)
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    event_entered = true;
                    while (hang_event && !cancelled)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    return 0;
                }
                CHECK(false);
                return 0;
            },
            .cancel = [&] { cancelled = true; },
            .end = [&] { ++ends; },
            .invalidate = [&](u64, size_t) {
                if (hold_publication) {
                    publication_entered = true;
                    while (hold_publication)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            },
        };
    };
    auto owned_player = std::make_unique<GuestAvPlayer>(*space, callbacks_factory);
    auto& player = *owned_player;
    auto call = [&](std::string_view nid, std::array<u64, 6> args = {}) {
        return player.Dispatch(nid, args);
    };
    CHECK(call("eBTreZ84JFY", {UINT64_MAX, UINT64_MAX}) == 0 && calls == 0);
    CHECK(call("aS66RI0gGgo", {0}) == 0);
    CHECK(call("NkJwDzKmIlw", {1234}) == u32(ORBIS_AVPLAYER_ERROR_INVALID_PARAMS));
    AvPlayerInitData data{};
    data.memory_replacement = {reinterpret_cast<void*>(0x1234),
                               reinterpret_cast<AvPlayerAllocate>(pc(1)),
                               reinterpret_cast<AvPlayerDeallocate>(pc(2)),
                               reinterpret_cast<AvPlayerAllocateTexture>(pc(3)),
                               reinterpret_cast<AvPlayerDeallocateTexture>(pc(4))};
    data.file_replacement = {
        reinterpret_cast<void*>(0x4321), reinterpret_cast<AvPlayerOpenFile>(pc(5)),
        reinterpret_cast<AvPlayerCloseFile>(pc(6)), reinterpret_cast<AvPlayerReadOffsetFile>(pc(7)),
        reinterpret_cast<AvPlayerSizeFile>(pc(8))};
    data.event_replacement = {reinterpret_cast<void*>(0x9876),
                              reinterpret_cast<AvPlayerEventCallback>(pc(9))};
    data.num_output_video_framebuffers = 3;
    data.auto_start = true;
    const u64 at = base + 0x1000, path = base + 0x2000, output = base + 0x3000;
    auto bad = data;
    bad.memory_replacement.allocate = nullptr;
    put(at, bad);
    CHECK(call("aS66RI0gGgo", {at}) == 0 && begins == 0);
    AvPlayerInitDataEx ex{};
    ex.this_size = 8;
    put(at, ex);
    CHECK(call("o9eWRkSL+M4", {at, output}) == u32(ORBIS_AVPLAYER_ERROR_INVALID_PARAMS) &&
          begins == 0);
    put(at, data);
    const u64 handle = call("aS66RI0gGgo", {at});
    CHECK(handle && handle < 1000 && begins == 1);
    CHECK(call("ET4Gr-Uu07s", {handle}) == u32(ORBIS_AVPLAYER_ERROR_OPERATION_FAILED));
    CHECK(call("UbQoYawOsfY", {handle}) == 0);
    const char network[] = "https://example.invalid/movie.m3u8";
    put(path, network);
    CHECK(call("KMcEa+rHsIo", {handle, path}) == u32(ORBIS_AVPLAYER_ERROR_NOT_SUPPORTED));
    CHECK(reads == 0);
    const char filename[] = "/app0/probe.mp4";
    put(path, filename);
    CHECK(call("KMcEa+rHsIo", {handle, path}) == 0);
    CHECK(call("k-q+xOxdc3E", {handle, 1}) == 0);
    unsigned video{}, audio{};
    // A concurrent video publication is not EOF. Hold a real decoded frame
    // before returning it, while another guest owner polls IsActive/stream count.
    hold_publication = true;
    auto publish = std::async(std::launch::async, [&] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until) {
            if (call("JdksQu8pNdQ", {handle, output}) == 1)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    });
    const auto entered_by = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!publication_entered && std::chrono::steady_clock::now() < entered_by)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(publication_entered);
    auto active = std::async(std::launch::async, [&] { return call("UbQoYawOsfY", {handle}); });
    auto streams = std::async(std::launch::async, [&] { return call("hdTyRzCXQeQ", {handle}); });
    CHECK(active.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
    CHECK(streams.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
    hold_publication = false;
    CHECK(publish.get());
    ++video;
    CHECK(active.get() == 1);
    CHECK(streams.get() == 2);
    bool nonzero{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline && (video < 12 || audio < 90)) {
        if (call("JdksQu8pNdQ", {handle, output}) == 1) {
            const auto info = read.template operator()<AvPlayerFrameInfoEx>(output);
            const u64 ptr = reinterpret_cast<u64>(info.p_data);
            CHECK(ptr >= base + 0x100000 && ptr < base + config.reservation_size);
            CHECK(info.details.video.width == 64 && info.details.video.height == 48 &&
                  info.details.video.pitch == 64);
            nonzero |= read.template operator()<u64>(ptr) != 0;
            ++video;
        }
        if (call("Wnp1OVcrZgk", {handle, output}) == 1) {
            const auto info = read.template operator()<AvPlayerFrameInfo>(output);
            CHECK(info.details.audio.sample_rate == 48000 && info.details.audio.channel_count == 1);
            CHECK(info.details.audio.size && info.details.audio.size <= 32768);
            ++audio;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("decoded video=%u audio=%u callbacks=%u events=%u reads=%u\n", video, audio,
                calls.load(), events.load(), reads.load());
    CHECK(video == 12 && audio >= 90 && nonzero && events > 0 && reads > 0);
    CHECK(call("Wnp1OVcrZgk", {handle, UINT64_MAX}) == 0);
    CHECK(call("NkJwDzKmIlw", {handle}) == 0);
    CHECK(begins == ends && allocs == frees && allocations.empty());
    CHECK(call("NkJwDzKmIlw", {handle}) == u32(ORBIS_AVPLAYER_ERROR_INVALID_PARAMS));
    // Close must drain events accepted before it started. A queued Play event
    // behind an entered Pause callback cannot disappear just because Close
    // has set its admission flag while waiting for the worker barrier.
    auto manual = data;
    manual.auto_start = false;
    put(at, manual);
    const u64 he = call("aS66RI0gGgo", {at});
    put(at, data);
    const auto ready_before = ready_events.load();
    CHECK(he && call("KMcEa+rHsIo", {he, path}) == 0);
    const auto event_ready_by = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    // Stream discovery precedes the Ready transition/event. A positive
    // stream count alone does not authorize a manual Start.
    while (ready_events == ready_before && std::chrono::steady_clock::now() < event_ready_by)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(ready_events > ready_before);
    CHECK(call("ODJK2sn9w4A", {he, 0}) == 0);
    CHECK(call("ODJK2sn9w4A", {he, 1}) == 0);
    CHECK(call("OVths0xGfho", {he, 1}) == 0);
    CHECK(call("ET4Gr-Uu07s", {he}) == 0);
    bool event_video{};
    while (!event_video && std::chrono::steady_clock::now() < event_ready_by) {
        event_video = call("JdksQu8pNdQ", {he, output}) == 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(event_video);
    hold_pause = true;
    pause_entered = false;
    CHECK(call("9y5v+fGN4Wk", {he}) == 0);
    const auto pause_by = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!pause_entered && std::chrono::steady_clock::now() < pause_by)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(pause_entered);
    const auto played = play_events.load();
    CHECK(call("w5moABNwnRY", {he}) == 0);
    auto close_events = std::async(std::launch::async, [&] { return call("NkJwDzKmIlw", {he}); });
    CHECK(close_events.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
    hold_pause = false;
    CHECK(close_events.get() == 0);
    CHECK(play_events == played + 1);
    // Replacing an interior page cannot authorize a stale decoded buffer or
    // cause its old deallocator to free the replacement mapping.
    const u64 remap_handle = call("aS66RI0gGgo", {at});
    CHECK(remap_handle);
    CHECK(call("k-q+xOxdc3E", {remap_handle, 1}) == 0);
    CHECK(call("KMcEa+rHsIo", {remap_handle, path}) == 0);
    u64 replaced{};
    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!replaced && std::chrono::steady_clock::now() < ready_deadline) {
        if (call("JdksQu8pNdQ", {remap_handle, output}) == 1)
            replaced =
                reinterpret_cast<u64>(read.template operator()<AvPlayerFrameInfoEx>(output).p_data);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(replaced);
    if (replaced) {
        {
            std::lock_guard lock(fixture_memory);
            {
                auto token = space->Quiesce(0);
                CHECK(token);
                if (token)
                    CHECK(space->UpdateVmUnderToken(token.Value(),
                                                    GuestAddressSpace::VmOperation::Map,
                                                    {GuestAddress{replaced + 4096}, 4096}, rw));
            }
            std::array<std::byte, 8192> sentinel;
            sentinel.fill(std::byte{0x5a});
            CHECK(space->Write(GuestAddress{replaced}, std::span{sentinel}.first(4096)));
            CHECK(space->Write(GuestAddress{replaced + 4096}, std::span{sentinel}.subspan(4096)));
        }
        unsigned other_frames{};
        for (unsigned i = 0; i < 150; ++i) {
            const u64 marker = 0xfedcba9876543210ULL;
            put(output, marker);
            if (call("JdksQu8pNdQ", {remap_handle, output}) == 1) {
                CHECK(reinterpret_cast<u64>(
                          read.template operator()<AvPlayerFrameInfoEx>(output).p_data) !=
                      replaced);
                ++other_frames;
            } else
                CHECK(read.template operator()<u64>(output) == marker);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(other_frames > 0);
        CHECK(read.template operator()<u64>(replaced) == 0x5a5a5a5a5a5a5a5aULL);
        CHECK(read.template operator()<u64>(replaced + 4096) == 0x5a5a5a5a5a5a5a5aULL);
    }
    CHECK(call("NkJwDzKmIlw", {remap_handle}) == 0);
    if (replaced) {
        std::lock_guard lock(fixture_memory);
        std::lock_guard table(allocations_mutex);
        CHECK(allocations.size() == 1 && allocations.contains(replaced));
        if (auto it = allocations.find(replaced); it != allocations.end()) {
            auto token = space->Quiesce(0);
            CHECK(token);
            if (token)
                CHECK(space->UpdateVmUnderToken(
                    token.Value(), GuestAddressSpace::VmOperation::Unmap,
                    {GuestAddress{replaced}, it->second}, GuestPermission::None));
            allocations.erase(it);
        }
    }
    // An entered, nonreturning event must be interrupted before native joins.
    hang_event = true;
    event_entered = false;
    const u64 h2 = call("aS66RI0gGgo", {at});
    CHECK(h2 && h2 != handle);
    CHECK(call("KMcEa+rHsIo", {h2, path}) == 0);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!event_entered && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(event_entered);
    auto start = std::chrono::steady_clock::now();
    player.RequestStop();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    CHECK(call("aS66RI0gGgo", {at}) == 0);
    owned_player.reset();
    CHECK(begins == ends);
    // Stop must release an API waiter even while another owner still has the
    // publication gate. It must not wait for that owner's VM copy to finish.
    hang_event = false;
    hold_publication = true;
    publication_entered = false;
    GuestAvPlayer cancel_player(*space, callbacks_factory);
    const u64 hc = cancel_player.Dispatch("aS66RI0gGgo", {at});
    CHECK(hc);
    CHECK(cancel_player.Dispatch("k-q+xOxdc3E", {hc, 1}) == 0);
    CHECK(cancel_player.Dispatch("KMcEa+rHsIo", {hc, path}) == 0);
    auto blocked_publish = std::async(std::launch::async, [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel_player.Dispatch("JdksQu8pNdQ", {hc, output}) == 1)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    });
    const auto publication_by = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!publication_entered && std::chrono::steady_clock::now() < publication_by)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(publication_entered);
    auto cancelled_query = std::async(std::launch::async, [&] {
        return cancel_player.Dispatch("UbQoYawOsfY", {hc});
    });
    CHECK(cancelled_query.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout);
    cancel_player.RequestStop();
    CHECK(cancelled_query.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
    hold_publication = false;
    CHECK(blocked_publish.get());
    CHECK(cancelled_query.get() == 0);
    std::printf("guest_avplayer_tests: %u checks, %u failures\n", checks.load(), failures.load());
    return failures ? 1 : 0;
}
