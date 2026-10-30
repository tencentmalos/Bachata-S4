// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/path_util.h"
#include "common/profiler.h"
#include <profiler/sdk/Sdk.h>
#include <profiler/sdk/FileSink.h>
#include <profiler/sdk/SlowFrameCapture.h>
#include <spatial/core/utils/ProfilerRing.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <limits>
#include <thread>

namespace {
std::filesystem::path output;
}
// Only the host path is supplied by the fixture; ring, SDK, capture and control are real.
const std::filesystem::path& Common::FS::GetUserPath(PathType) { return output; }

int main(int argc, char** argv) {
    using namespace std::chrono_literals;
    namespace P = Common::Profiler;
    namespace S = profiler::sdk;
    output = argc > 1 ? argv[1] : "profiler-test-evidence";
    std::filesystem::create_directories(output);
    int checks{}, failures{};
    const auto check = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
    };
    const auto frames = [] {
        const auto status = P::Control({"status"});
        const auto key = status.find("\nframe_count=");
        return std::stoull(status.substr(key + 13));
    };
    const auto dump = [&](const char* name) {
        S::FileSink sink((output / name).string());
        S::dump_litetrace_to(&sink, std::numeric_limits<uint64_t>::max(), 0);
        sink.close();
    };

    P::Initialize();
    P::BeginSession("title=synthetic frame=videoout_present_success");
    check(P::Enabled(), "startup records before the first successful present");
    {
        P::Phase startup{"Startup.CadenceTest"};
        P::Scope work{"Startup.Work"};
        P::Counter("GNM.SubmittedEpochs", 1);
    }
    check(frames() == 0, "startup work does not synthesize a frame");
    dump("startup.prof");

    // Submissions, blank/redraw/failed attempts must not advance the frame clock.
    for (int i = 2; i <= 40; ++i) {
        P::Counter("GNM.SubmittedEpochs", i);
        P::PresentedFrame(false);
    }
    check(frames() == 0, "submission bursts and unsuccessful presents do not become frames");
    const auto flow = P::Post("VideoOut.FlipDispatch");
    check(flow.id != 0, "queued flip gets an explicit flow identity");
    std::promise<void> frames_ready, release_present;
    auto release = release_present.get_future();
    std::thread present([&] {
        for (int i = 0; i < 8; ++i) {
            P::Scope dispatch{"VideoOut.FlipDispatch", i == 0 ? flow : P::FlowToken{}};
            P::PresentedFrame(true);
            P::Counter("VideoOut.PresentedGuestFrames", i + 1);
            std::this_thread::sleep_for(2ms);
        }
        frames_ready.set_value();
        release.wait(); // The production presenter is resident while its ring is exported.
    });
    frames_ready.get_future().wait();
    check(frames() == 8, "one marker per accepted new guest frame");
    dump("present.prof");
    release_present.set_value();
    present.join();

    S::SlowFrameConfig config;
    config.min_samples = 4;
    config.min_duration_ns = 50'000'000;
    config.pre_frames = 2;
    config.post_frames = 0;
    config.cooldown_ns = 0;
    S::start_slow_frame_capture(config);
    const auto warmup = [] {
        for (int i = 0; i < 7; ++i) {
            P::PresentedFrame(true);
            std::this_thread::sleep_for(10ms);
        }
    };
    warmup();
    P::ResetFrameHistory();
    std::this_thread::sleep_for(100ms);
    P::PresentedFrame(true);
    check(S::slow_frame_capture_status().detected == 0, "lifecycle pause discards the old baseline");
    warmup();
    std::this_thread::sleep_for(100ms);
    P::PresentedFrame(true);
    for (int i = 0; i < 100 && S::slow_frame_capture_status().captures.empty(); ++i)
        std::this_thread::sleep_for(10ms);
    S::stop_slow_frame_capture();
    const auto status = S::slow_frame_capture_status();
    check(status.detected == 1, "actual long present interval triggers once");
    check(status.captures.size() == 1 && status.captures.front().error.empty(),
          "automatic capture has a retained closed frame window");
    if (!status.captures.empty()) {
        S::FileSink sink((output / "slow-present.prof").string());
        check(S::dump_slow_frame_to(&sink, status.captures.front().id), "stopped capture remains exportable");
        sink.close();
    }
    P::PresentedFrame(true);
    std::this_thread::sleep_for(30ms);
    check(S::slow_frame_capture_status().captures.size() == status.captures.size(),
          "later presentation does not clear stopped captures");
    P::Control({"stop"});
    const auto stopped_frames = frames();
    P::PresentedFrame(true);
    check(frames() == stopped_frames, "disabled ring emits no frame");
    S::shutdown();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
