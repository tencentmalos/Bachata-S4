// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <cstdio>
#include <future>
#include "video_core/renderdoc_capture.h"
using namespace VideoCore;
using namespace std::chrono_literals;
unsigned checks{}, failures{};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
struct Backend : IRenderDocBackend {
    bool loaded{true}, active{}, discard_ok{true}, finalize_ok{true};
    unsigned starts{}, ends{}, discards{}, files{}, work{};
    std::promise<void>* end_entered{};
    std::shared_future<void> release_end;
    bool IsLoaded() const override { return loaded; }
    bool IsCapturing() override { return active; }
    u32 GetNumCaptures() override { return files; }
    bool StartFrameCapture(const CaptureTarget&, const CaptureReceipt&) override {
        ++starts; active = true; return true;
    }
    bool EndFrameCapture(const CaptureTarget&) override {
        if (end_entered) { end_entered->set_value(); release_end.wait(); }
        ++ends; active = false; ++files; return true;
    }
    bool DiscardFrameCapture(const CaptureTarget&) override {
        ++discards; if (discard_ok) active = false; return discard_ok;
    }
    bool GetCapture(u32, std::string& path, u64& time) override {
        path = "test.rdc"; time = 123; return true;
    }
    bool Finalize(const CaptureTarget&, CaptureReceipt& r) override {
        r.file_size = 123; r.file_sha256 = "verified-test-hash"; r.sidecar_path = "test.rdc.json";
        return finalize_ok;
    }
};
CaptureTarget Target(u64 gen = 10) { return {gen, reinterpret_cast<void*>(1), nullptr, "test", "test-driver"}; }
int main() {
    for (bool expire_write : {false, true}) {
        Backend b; CaptureCoordinator c{b}; c.Bind(Target());
        c.SetTimeoutNs(10); c.SetWriteTimeoutNs(100);
        c.Arm(1, 10, "slow-file", 0, CaptureBoundary::GuestFlip);
        c.OnFrameBoundary(10, 1, 1, CaptureBoundary::GuestFlip);
        std::promise<void> entered, release;
        b.end_entered = &entered; b.release_end = release.get_future().share();
        auto render = std::async(std::launch::async, [&] {
            c.OnFrameBoundary(10, 2, 2, CaptureBoundary::GuestFlip);
        });
        entered.get_future().wait();
        c.Poll(20);
        CHECK(c.Query().state == CaptureRequestState::Writing);
        if (expire_write) c.Poll(103);
        release.set_value(); render.get();
        CHECK(c.Query().state == (expire_write ? CaptureRequestState::Failed : CaptureRequestState::Ready));
        CHECK(!c.Query().cleanup_pending && !b.active);
    }
    {
        Backend b; CaptureCoordinator c{b};
        CHECK(c.Bind(Target()));
        auto r = c.Arm(2, 10, "guest-run", 0, CaptureBoundary::GuestFlip);
        CHECK(r.coverage == "guest_flip_interval" && c.NeedsGuestBoundary(10));
        CHECK(!c.NeedsGuestBoundary(11));
        c.OnFrameBoundary(10, 100, 1); // Present must not consume a guest request.
        CHECK(b.starts == 0);
        c.OnFrameBoundary(10, 9, 2, CaptureBoundary::GuestFlip);
        CHECK(b.starts == 1 && b.ends == 0);
        c.OnFrameBoundary(10, 1000, 3);
        c.OnFrameBoundary(10, 9, 4, CaptureBoundary::GuestFlip);
        CHECK(c.Query().completed_frames == 0);
        c.OnFrameBoundary(10, 10, 5, CaptureBoundary::GuestFlip);
        CHECK(c.Query().completed_frames == 1 && b.ends == 0);
        c.OnFrameBoundary(10, 11, 6, CaptureBoundary::GuestFlip);
        r = c.Query();
        CHECK(r.state == CaptureRequestState::Ready && b.ends == 1);
        CHECK(r.first_guest_flip == 9 && r.last_guest_flip == 11);
        CHECK(r.first_present == 0 && r.last_present == 0 && !c.NeedsGuestBoundary(10));
        c.Arm(1, 10, "guest-run", 7, CaptureBoundary::GuestFlip);
        c.OnFrameBoundary(10, 12, 8, CaptureBoundary::GuestFlip);
        c.OnFrameBoundary(10, 14, 9, CaptureBoundary::GuestFlip);
        CHECK(c.Query().state == CaptureRequestState::Failed);
        CHECK(c.Query().failure_reason == "guest_flip_boundary_gap");
        CHECK(!c.NeedsGuestBoundary(10));
    }
    {
        Backend b; CaptureCoordinator c{b};
        CHECK(c.Arm(1, 10, "run", 0).command_status == "no_matching_renderer");
        CHECK(c.Bind(Target()));
        CHECK(!c.Bind(Target(11)));
        CHECK(c.Arm(0, 10, "run", 0).command_status == "invalid_frames");
        CHECK(c.Arm(9, 10, "run", 0).command_status == "invalid_frames");
        CHECK(c.Arm(1, 11, "run", 0).command_status == "no_matching_renderer");
        auto r = c.Arm(1, 10, "run", 0);
        CHECK(r.state == CaptureRequestState::Armed);
        CHECK(c.Arm(1, 10, "run", 0).command_status == "busy");
        c.OnFrameBoundary(11, 1, 1); CHECK(b.starts == 0);
        c.OnFrameBoundary(10, 5, 1);
        CHECK(b.starts == 1 && b.ends == 0 && b.active);
        CHECK(c.Query().state == CaptureRequestState::Capturing);
        c.OnFrameBoundary(10, 5, 2); CHECK(b.ends == 0);
        if (b.active) ++b.work;
        c.OnFrameBoundary(10, 6, 3);
        r = c.Query();
        CHECK(b.work == 1 && b.ends == 1);
        CHECK(r.state == CaptureRequestState::Ready && r.completed_frames == 1);
        CHECK(r.first_present == 5 && r.last_present == 6);
        CHECK(!r.file_sha256.empty() && !r.sidecar_path.empty());
        auto second = c.Arm(1, 10, "run", 4);
        CHECK(second.capture_uuid != r.capture_uuid);
        CHECK(c.Query(r.request_id, 4).state == CaptureRequestState::Ready);
        CHECK(c.Cancel(r.request_id, 10).command_status == "stale_request");
        CHECK(c.Cancel(second.request_id, 11).command_status == "stale_request");
        CHECK(c.Query().state == CaptureRequestState::Armed);
        c.RequestStop(10); c.Poll(5);
        CHECK(c.Query().state == CaptureRequestState::Cancelled);
        CHECK(b.discards == 0); // Never started; no external capture touched.
        c.Unbind(10); CHECK(c.Bind(Target(11)));
        CHECK(c.Arm(1, 10, "run", 6).command_status == "no_matching_renderer");
    }
    {
        Backend b; CaptureCoordinator c{b}; c.Bind(Target());
        c.Arm(3, 10, "run", 0);
        for (u64 i = 1; i <= 3; ++i) { c.OnFrameBoundary(10, i, i); CHECK(b.ends == 0); }
        c.OnFrameBoundary(10, 4, 4);
        CHECK(c.Query().completed_frames == 3 && b.ends == 1);
    }
    {
        Backend b; CaptureCoordinator c{b}; c.Bind(Target()); c.SetTimeoutNs(10);
        c.Arm(1, 10, "run", 0); c.OnFrameBoundary(10, 1, 100);
        CHECK(b.starts == 0 && c.Query().state == CaptureRequestState::Failed);
        c.Arm(3, 10, "run", 101); c.OnFrameBoundary(10, 1, 102);
        c.Poll(112);
        CHECK(c.Query().state == CaptureRequestState::Failed && !b.active && b.discards == 1);
        CHECK(b.ends == 0 && b.files == 0);
    }
    {
        Backend b; CaptureCoordinator c{b}; c.Bind(Target());
        auto r = c.Arm(3, 10, "run", 0); c.OnFrameBoundary(10, 1, 1);
        b.discard_ok = false;
        CHECK(c.Cancel(r.request_id, 10).state == CaptureRequestState::Cancelling);
        c.Poll(2);
        CHECK(c.Query().cleanup_pending && b.active);
        CHECK(c.Arm(1, 10, "run", 3).command_status == "busy");
        b.discard_ok = true; c.Poll(4);
        CHECK(c.Query().state == CaptureRequestState::Cancelled && !b.active);
        CHECK(b.files == 0);
    }
    {
        Backend b; CaptureCoordinator c{b}; c.Bind(Target());
        c.Arm(1, 10, "run", 0); c.OnFrameBoundary(10, 1, 1);
        b.finalize_ok = false; c.OnFrameBoundary(10, 2, 2);
        CHECK(c.Query().state == CaptureRequestState::Failed);
    }
    {
        Backend b; b.loaded = false; CaptureCoordinator c{b}; c.Bind(Target());
        CHECK(c.Arm(1, 10, "run", 0).state == CaptureRequestState::Failed);
        CHECK(b.starts == 0);
    }
    {
        Backend b; CaptureCoordinator c{b}; c.Bind(Target());
        auto r = c.Arm(1, 10, "run", 0); c.OnFrameBoundary(10, 1, 1);
        std::promise<void> entered, release;
        b.end_entered = &entered; b.release_end = release.get_future().share();
        auto render = std::async(std::launch::async, [&] { c.OnFrameBoundary(10, 2, 2); });
        entered.get_future().wait();
        auto status = std::async(std::launch::async, [&] { return c.Query(); });
        CHECK(status.wait_for(100ms) == std::future_status::ready);
        CHECK(status.get().state == CaptureRequestState::Writing);
        auto cancel = std::async(std::launch::async, [&] { return c.Cancel(r.request_id, 10); });
        CHECK(cancel.wait_for(100ms) == std::future_status::ready);
        CHECK(cancel.get().cleanup_pending);
        release.set_value(); render.get();
        CHECK(c.Query().state == CaptureRequestState::Cancelled);
    }
    {
        Backend b;
        std::atomic<u64> now{0};
        CaptureCoordinator c{b, [&] { return now.load(); }}; c.Bind(Target()); c.SetTimeoutNs(10);
        c.Arm(1, 10, "run", 0); now = 20;
        const auto limit = std::chrono::steady_clock::now() + 1s;
        while (c.Query().state != CaptureRequestState::Failed && std::chrono::steady_clock::now() < limit)
            std::this_thread::yield();
        CHECK(c.Query().state == CaptureRequestState::Failed && b.starts == 0);
    }
    std::printf("renderdoc_capture: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
