// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <string>
#include <vector>

#include "video_core/renderdoc_capture.h"

using namespace VideoCore;

static unsigned checks{}, failures{};
#define CHECK(x)                                                                                    \
    do {                                                                                            \
        ++checks;                                                                                   \
        if (!(x)) {                                                                                 \
            ++failures;                                                                             \
            std::printf("FAIL line %d: %s\n", __LINE__, #x);                                        \
        }                                                                                           \
    } while (0)

// A controllable fake RenderDoc backend.
class FakeBackend : public IRenderDocBackend {
public:
    bool loaded = true;
    u32 num_captures = 0;
    bool capturing = false;
    int starts = 0;
    int ends = 0;
    // When a capture ends, whether a new capture file "appears".
    bool produce_capture_on_end = true;
    std::string path_to_return = "/data/captures/frame.rdc";
    bool get_capture_ok = true;

    bool IsLoaded() const override { return loaded; }
    u32 GetNumCaptures() override { return num_captures; }
    void StartFrameCapture() override { ++starts; capturing = true; }
    bool EndFrameCapture() override {
        if (!capturing) return false;
        ++ends;
        capturing = false;
        if (produce_capture_on_end) ++num_captures;
        return true;
    }
    bool GetCapture(u32 idx, std::string& path, u64& timestamp) override {
        if (!get_capture_ok || idx >= num_captures) return false;
        path = path_to_return;
        timestamp = 1700000000ull + idx;
        return true;
    }
};

int main() {
    // --- happy path: arm 1 frame, one boundary starts, next completes -> Ready ---
    {
        FakeBackend be;
        CaptureCoordinator c(be);
        auto r = c.Arm(1, "run-1", "cap-1", 0);
        CHECK(r.state == CaptureRequestState::Armed);
        CHECK(r.request_id == 1);
        CHECK(r.num_captures_before == 0);
        CHECK(r.run_uuid == "run-1" && r.capture_uuid == "cap-1");
        // First boundary starts and (frames==1) completes in the same boundary.
        c.OnFrameBoundary(1);
        CHECK(be.starts == 1 && be.ends == 1);
        auto q = c.Query(2);
        CHECK(q.state == CaptureRequestState::Ready);
        CHECK(q.num_captures_after == 1);
        CHECK(q.file_path == "/data/captures/frame.rdc");
        CHECK(q.capture_timestamp == 1700000000ull);
    }

    // --- multi-frame: 3 frames need 3 boundaries ---
    {
        FakeBackend be;
        CaptureCoordinator c(be);
        c.Arm(3, "run", "cap", 0);
        c.OnFrameBoundary(1);  // start + frame 1
        CHECK(c.Query(1).state == CaptureRequestState::Capturing);
        c.OnFrameBoundary(2);  // frame 2
        CHECK(c.Query(2).state == CaptureRequestState::Capturing);
        c.OnFrameBoundary(3);  // frame 3 -> end
        CHECK(c.Query(3).state == CaptureRequestState::Ready);
        CHECK(be.starts == 1 && be.ends == 1);
    }

    // --- RenderDoc absent -> Failed immediately, never fake success ---
    {
        FakeBackend be; be.loaded = false;
        CaptureCoordinator c(be);
        auto r = c.Arm(1, "run", "cap", 0);
        CHECK(r.state == CaptureRequestState::Failed);
        CHECK(r.failure_reason.find("not loaded") != std::string::npos);
    }

    // --- busy: a second Arm while armed returns the existing request ---
    {
        FakeBackend be;
        CaptureCoordinator c(be);
        auto r1 = c.Arm(2, "run", "cap-a", 0);
        auto r2 = c.Arm(2, "run", "cap-b", 0);
        CHECK(r1.request_id == r2.request_id);       // same request (busy)
        CHECK(r2.capture_uuid == "cap-a");           // not clobbered by cap-b
        CHECK(r2.state == CaptureRequestState::Armed);
    }

    // --- no new capture appeared -> Failed, never Ready ---
    {
        FakeBackend be; be.produce_capture_on_end = false;
        CaptureCoordinator c(be);
        c.Arm(1, "run", "cap", 0);
        c.OnFrameBoundary(1);
        auto q = c.Query(2);
        CHECK(q.state == CaptureRequestState::Failed);
        CHECK(q.failure_reason.find("no new capture") != std::string::npos);
    }

    // --- capture appeared but path not retrievable -> Failed ---
    {
        FakeBackend be; be.get_capture_ok = false;
        CaptureCoordinator c(be);
        c.Arm(1, "run", "cap", 0);
        c.OnFrameBoundary(1);
        auto q = c.Query(2);
        CHECK(q.state == CaptureRequestState::Failed);
        CHECK(q.failure_reason.find("file path") != std::string::npos);
    }

    // --- cancel an in-progress capture ends and discards it ---
    {
        FakeBackend be;
        CaptureCoordinator c(be);
        c.Arm(3, "run", "cap", 0);
        c.OnFrameBoundary(1);  // Capturing
        CHECK(c.Query(1).state == CaptureRequestState::Capturing);
        c.Cancel();
        CHECK(be.ends == 1);  // ended to not leave RenderDoc mid-frame
        CHECK(c.Query(2).state == CaptureRequestState::Cancelled);
    }

    // --- timeout: armed with no boundary within budget -> Failed on Query ---
    {
        FakeBackend be;
        CaptureCoordinator c(be);
        c.SetTimeoutNs(1000);
        c.Arm(1, "run", "cap", 0);
        CHECK(c.Query(500).state == CaptureRequestState::Armed);   // within budget
        CHECK(c.Query(2000).state == CaptureRequestState::Failed); // past budget
        CHECK(c.Query(2000).failure_reason.find("timeout") != std::string::npos);
    }

    // --- after Ready, a new Arm starts a fresh request ---
    {
        FakeBackend be;
        CaptureCoordinator c(be);
        c.Arm(1, "run", "cap-1", 0);
        c.OnFrameBoundary(1);
        CHECK(c.Query(1).state == CaptureRequestState::Ready);
        auto r2 = c.Arm(1, "run", "cap-2", 2);
        CHECK(r2.request_id == 2);
        CHECK(r2.state == CaptureRequestState::Armed);
        CHECK(r2.num_captures_before == 1);  // one capture already exists
    }

    // --- Query(unknown request_id) reports idle/unknown ---
    {
        FakeBackend be;
        CaptureCoordinator c(be);
        c.Arm(1, "run", "cap", 0);
        auto q = c.Query(999u, 0);
        CHECK(q.state == CaptureRequestState::Idle);
        CHECK(q.failure_reason.find("unknown") != std::string::npos);
    }

    std::printf("renderdoc_capture: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
