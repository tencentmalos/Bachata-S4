// Review-only counterexamples against 30e3b21f. Not an acceptance test.
// Backend is injected; no actual Vulkan/RenderDoc/device work is performed.
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include "video_core/renderdoc_capture.h"
#include "core/diagnostics/diagnostics_commands.h"
#include "core/diagnostics/diagnostics_hub_registry.h"
#include "spatial/debugbus/DebugCommandRegistry.h"

using namespace VideoCore;
using namespace Core::Diagnostics;
using namespace std::chrono_literals;

struct Backend : IRenderDocBackend {
    bool capturing{};
    unsigned starts{}, ends{}, files{}, work{}, captured_work{};
    std::promise<void>* end_entered{};
    std::shared_future<void> release_end;
    bool IsLoaded() const override { return true; }
    u32 GetNumCaptures() override { return files; }
    void StartFrameCapture() override { ++starts; capturing = true; }
    bool EndFrameCapture() override {
        if (end_entered) {
            end_entered->set_value();
            release_end.wait();
        }
        if (!capturing) return false;
        capturing = false;
        ++ends;
        ++files;
        return true;
    }
    bool GetCapture(u32, std::string& path, u64& timestamp) override {
        path = "/nonexistent/review-only.rdc";
        timestamp = 1;
        return true;
    }
    void RecordWork() { ++work; if (capturing) ++captured_work; }
};

namespace VideoCore {
Backend command_backend;
CaptureCoordinator& GetCaptureCoordinator() {
    static CaptureCoordinator coordinator{command_backend};
    return coordinator;
}
bool IsRenderDocLoaded() { return true; }
}

unsigned reproduced{}, missing{};
void Observe(const char* id, bool bad_behavior) {
    std::printf("%s %s\n", id, bad_behavior ? "REPRODUCED" : "NOT_REPRODUCED");
    bad_behavior ? ++reproduced : ++missing;
}
bool Has(const std::string& s, const char* text) { return s.find(text) != std::string::npos; }

int main() {
    {
        Backend b;
        CaptureCoordinator c{b};
        c.Arm(1, "run-a", "cap-a", 0);
        b.RecordWork(); // Presenter work occurs before the successful-present hook.
        c.OnFrameBoundary(1);
        auto r = c.Query(2);
        Observe("R1_same_boundary_zero_work_ready", b.starts == 1 && b.ends == 1 &&
            b.captured_work == 0 && r.state == CaptureRequestState::Ready);
        Observe("R2_ready_without_file_or_sidecar", r.state == CaptureRequestState::Ready &&
            r.file_path == "/nonexistent/review-only.rdc");
    }
    {
        Backend b;
        CaptureCoordinator c{b};
        c.SetTimeoutNs(10);
        c.Arm(1, "run-a", "cap-a", 0);
        c.OnFrameBoundary(100); // No intervening status poll.
        Observe("R3_late_boundary_bypasses_timeout", c.Query(100).state == CaptureRequestState::Ready);
    }
    {
        Backend b;
        CaptureCoordinator c{b};
        c.SetTimeoutNs(10);
        c.Arm(3, "run-a", "cap-a", 0);
        c.OnFrameBoundary(1);
        const auto r = c.Query(100);
        c.Cancel();
        Observe("R4_timeout_then_cancel_leaves_backend_active",
            r.state == CaptureRequestState::Failed && b.capturing && b.ends == 0);
        const auto second = c.Arm(3, "run-a", "cap-b", 101);
        c.OnFrameBoundary(102);
        Observe("R5_rearm_starts_over_active_capture", second.request_id == 2 && b.starts == 2 && b.capturing);
    }
    {
        Backend b;
        CaptureCoordinator c{b};
        c.Arm(3, "run-a", "cap-a", 0);
        c.OnFrameBoundary(1);
        c.Cancel();
        Observe("R6_cancel_calls_writing_end_not_discard",
            c.Query(2).state == CaptureRequestState::Cancelled && b.files == 1);
    }
    {
        Backend b;
        std::promise<void> entered, release;
        b.end_entered = &entered;
        b.release_end = release.get_future().share();
        CaptureCoordinator c{b};
        c.Arm(1, "run-a", "cap-a", 0);
        auto rendering = std::async(std::launch::async, [&] { c.OnFrameBoundary(1); });
        entered.get_future().wait(); // Deterministically hold the coordinator inside End.
        std::promise<void> query_started;
        auto query = std::async(std::launch::async, [&] {
            query_started.set_value();
            return c.Query(2);
        });
        query_started.get_future().wait();
        const bool blocked = query.wait_for(100ms) == std::future_status::timeout;
        release.set_value();
        rendering.get();
        (void)query.get();
        Observe("R7_status_waits_for_backend_end", blocked);
    }
    {
        DiagnosticsHub hub;
        spatial::debugbus::DebugCommandRegistry registry;
        u64 now = 1;
        RegisterDiagnosticsCommands(registry, hub, [&] { return now; });
        auto a = hub.Register(1, 42);
        a->SetRunUuid("run-a");
        (void)registry.Handle("renderdoc_capture 1");
        auto& c = GetCaptureCoordinator();
        c.OnFrameBoundary(now);
        const auto first = c.Query(now);
        (void)registry.Handle("renderdoc_capture 3");
        const auto second = c.Query(now);
        Observe("R8_capture_uuid_reused", first.request_id != second.request_id &&
            first.capture_uuid == second.capture_uuid);
        const auto stale = registry.Handle("renderdoc_capture_cancel " + std::to_string(first.request_id));
        Observe("R9_stale_cancel_cancels_new_request", Has(stale, "state: cancelled") &&
            c.Query(now).request_id == second.request_id);

        (void)registry.Handle("renderdoc_capture 1");
        hub.Revoke(1);
        auto b = hub.Register(2, 42);
        b->SetRunUuid("run-b");
        c.OnFrameBoundary(++now); // No generation parameter; production Destroy only revokes hub.
        const auto crossed = c.Query(now);
        Observe("R10_new_generation_finishes_old_capture", crossed.state == CaptureRequestState::Ready &&
            crossed.run_uuid == "run-a");
        const auto invalid = registry.Handle("renderdoc_capture -1");
        Observe("R11_negative_frames_accepted_as_u32max", Has(invalid, "state: armed") &&
            Has(invalid, "requested_frames: 4294967295"));
        c.Cancel();
        hub.Revoke(2);
        Observe("R12_capture_accepted_without_session",
            Has(registry.Handle("renderdoc_capture 1"), "state: armed"));
        c.Cancel();
    }
    std::printf("Review counterexamples: %u reproduced, %u not reproduced. This is NOT implementation acceptance.\n",
                reproduced, missing);
    return missing ? 1 : 0;
}
