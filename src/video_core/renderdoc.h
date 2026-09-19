// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include "common/types.h"

namespace VideoCore {

/// Loads renderdoc dynamic library module.
void LoadRenderDoc();

/// Begins a capture if a renderdoc instance is attached.
void StartCapture();

/// Ends current renderdoc capture.
void EndCapture();

/// Triggers capturing process.
void TriggerCapture();

/// Sets output directory for captures
void SetOutputDir(const std::filesystem::path& path, const std::string& prefix);

/// Returns true when RenderDoc API was loaded and is usable.
bool IsRenderDocLoaded();

/// The process-wide capture coordinator (spec §3.2), backed by the loaded
/// RenderDoc API. Arm/Query/Cancel drive request/receipt captures; the presenter
/// calls its OnFrameBoundary after a provable present. Valid for the process
/// lifetime.
class CaptureCoordinator;
[[nodiscard]] CaptureCoordinator& GetCaptureCoordinator();

/// Called by the presenter after a successful present, so an armed capture
/// advances at a provable frame boundary. Cheap no-op when no capture is armed.
void BindCaptureTarget(u64 generation, void* instance, void* window, std::string driver);
void StopCaptureTarget(u64 generation);
void UnbindCaptureTarget(u64 generation);
void NotifyPresentBoundary(u64 generation, u64 present_id);
bool NeedsGuestCaptureBoundary(u64 generation);
// The PM4 owner drains accepted host submissions before calling this boundary.
void NotifyGuestFlipBoundary(u64 generation, u64 guest_flip_id);
class CaptureBinding final {
public:
    CaptureBinding() = default;
    CaptureBinding(const CaptureBinding&) = delete;
    CaptureBinding& operator=(const CaptureBinding&) = delete;
    ~CaptureBinding() { Close(); }
    void Bind(u64 generation, void* instance, void* window, std::string driver) {
        BindCaptureTarget(generation, instance, window, std::move(driver));
        generation_ = generation;
    }
    void Stop() { if (generation_) StopCaptureTarget(generation_); }
    void Close() { if (generation_) { UnbindCaptureTarget(generation_); generation_ = 0; } }
    u64 Generation() const { return generation_; }
private:
    u64 generation_{};
};

enum class ScreenshotRequest : u32 {
    None = 0,
    GameOnly = 1,
    WithOverlays = 2,
};

struct ScreenshotRequests {
    u32 game_only_count{};
    u32 with_overlays_count{};
};

/// Queues an in-emulator screenshot request to be consumed by the presenter.
void RequestScreenshot(ScreenshotRequest request);

/// Atomically consumes and returns pending "game only" screenshot request counter.
u32 ConsumeGameOnlyScreenshotRequests();

/// Atomically consumes and returns pending "with overlays" screenshot request counter.
u32 ConsumeWithOverlaysScreenshotRequests();

/// Atomically consumes and returns pending screenshot request counters.
ScreenshotRequests ConsumeScreenshotRequests();

} // namespace VideoCore
