// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <algorithm>
#include "video_core/vr_frame.h"

#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "core/libraries/videoout/video_out.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>
#include <queue>

namespace AmdGpu { struct Image; }

namespace Vulkan {
struct Frame;
struct PresentSource;
}

namespace Libraries::VideoOut {

struct VideoOutPort {
    SceVideoOutResolutionStatus resolution;
    std::array<VideoOutBuffer, MaxDisplayBuffers> buffer_slots;
    std::array<u64, MaxDisplayBuffers> local_labels{};
    std::span<u64, MaxDisplayBuffers> buffer_labels{local_labels};
    std::atomic<bool> stopping{false};
    static_assert(sizeof(buffer_labels[0]) == 8u);
    std::array<BufferAttributeGroup, MaxDisplayBufferGroups> groups;
    FlipStatus flip_status;
    SceVideoOutVblankStatus vblank_status;
    std::vector<Kernel::OrbisKernelEqueue> flip_events;
    std::vector<Kernel::OrbisKernelEqueue> vblank_events;
    std::mutex vo_mutex;
    std::mutex port_mutex;
    std::condition_variable vo_cv;
    std::condition_variable vblank_cv;
    std::condition_variable flip_cv; // port_mutex: host presentation queue admission
    std::atomic<int> flip_rate{0};
    int prev_index = -1;
    std::atomic<bool> is_open{false};
    bool is_hdr = false;

    s32 FindFreeGroup() const {
        s32 index = 0;
        while (index < groups.size() && groups[index].is_occupied) {
            index++;
        }
        return index;
    }

    bool IsVoLabel(const u64* address) const {
        const u64* start = &buffer_labels[0];
        const u64* end = &buffer_labels[MaxDisplayBuffers - 1];
        return address >= start && address <= end;
    }

    void WaitVoLabel(auto&& pred) {
        std::unique_lock lk{vo_mutex};
        vo_cv.wait(lk, [&] { return stopping || pred(); });
    }

    void SignalVoLabel() {
        std::scoped_lock lk{vo_mutex};
        vo_cv.notify_one();
    }

    [[nodiscard]] int NumRegisteredBuffers() const {
        return std::count_if(buffer_slots.cbegin(), buffer_slots.cend(),
                             [](auto& buffer) { return buffer.group_index != -1; });
    }
};

struct ServiceThreadParams {
    u32 unknown;
    bool set_priority;
    u32 priority;
    bool set_affinity;
    u64 affinity;
};

class VideoOutDriver {
public:
    VideoOutDriver(u32 width, u32 height, std::function<u64()> process_time = {},
                   std::function<u64()> tsc = {}, u64* guest_labels = nullptr,
                   std::function<void(std::exception_ptr)> fault = {});
    void RequestStop();
    std::atomic<u64> guest_presents{};
    void Join();
    ~VideoOutDriver();

    static constexpr s32 MainPortHandle = 1;
    // The TV next to a PSVR headset (SCE_VIDEO_OUT_BUS_TYPE_AUX_SOCIAL_SCREEN). Nothing shows
    // it: flips complete as soon as they are submitted and vblanks come at 60 Hz.
    static constexpr s32 SocialPortHandle = 2;
    static constexpr u32 SocialScreenRefreshRate = 60;

    int Open(const ServiceThreadParams* params, s32 bus_type = SCE_VIDEO_OUT_BUS_TYPE_MAIN);
    void Close(s32 handle);

    VideoOutPort* GetPort(s32 handle);

    int RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses, s32 bufferNum,
                        const BufferAttribute* attribute);
    int UnregisterBuffers(VideoOutPort* port, s32 attributeIndex);
    int ChangeBufferAttribute(VideoOutPort* port, s32 bufferIndex,
                              const BufferAttribute* attribute);

    bool SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop = false);

    void SaveReplayState(VideoCore::Replay::VideoOutState& state);
    /// GPU replay: opens the main port with the captured buffers. From then on the flip labels
    /// in guest memory change only through the replayed memory writes.
    void RestoreReplayState(const VideoCore::Replay::VideoOutState& state);

    void SetVrCadence(std::function<void()> callback);
    void SetVrActive(bool active);
    bool SubmitVrFrame(VideoOutPort* port, s32 index, u64 sequence,
                       const VideoCore::VrFrameSource& source,
                       std::function<void(bool)> complete);

private:
    struct Request {
        u64 diagnostic_id{};
        Vulkan::Frame* frame;
        // Instead of a frame: a guest image the present thread composes (FSR/post-process).
        std::shared_ptr<Vulkan::PresentSource> source;
        VideoOutPort* port;
        s64 flip_arg;
        s32 index;
        bool eop;
        std::function<void(bool)> complete;
        /// GPU fences deferred before the flip was prepared (Liverpool::FenceMark): the flip
        /// completes after them.
        u64 fence_mark{};

        operator bool() const noexcept {
            return frame != nullptr || source != nullptr;
        }
    };

    void Flip(const Request& req);
    // Flip status, flip events and the previous buffer's label, once a flip is done.
    void CompleteFlip(VideoOutPort* port, s32 index, s64 flip_arg, bool eop, u64 diagnostic_id);
    void SignalVblank(VideoOutPort& port);
    void DrawBlankFrame(); // Video port out not open
    void DrawLastFrame();  // Used when there is no flip request
    // A redraw when the window keeps no composed frame (direct presentation): the displayed
    // guest buffer is captured again on the GPU command thread, which owns the texture cache.
    // The guest does not write a displayed buffer before its next flip completes.
    void RequestRedrawCapture();
    // The captured redraw source, if it still shows the displayed buffer.
    std::shared_ptr<Vulkan::PresentSource> TakeRedrawSource();
    void DropRedrawSource();
    void SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop = false);
    void PresentThread(std::stop_token token);

    std::function<u64()> process_time;
    std::function<u64()> read_tsc;
    std::mutex lifecycle_mutex;
    std::mutex mutex;
    VideoOutPort main_port{};
    VideoOutPort social_port{};
    u32 social_vblank_accumulator{};
    std::mutex vr_cadence_mutex;
    std::function<void()> vr_cadence;
    bool vr_active{};
    std::atomic<bool> replay{};
    std::jthread present_thread;
    std::queue<Request> requests;
    std::mutex redraw_mutex;
    std::shared_ptr<Vulkan::PresentSource> redraw_source;
    u64 redraw_serial{};
    std::atomic<bool> redraw_capturing{};
    // The main port's buffer on screen, and a count of its completed flips.
    std::atomic<s32> displayed_index{-1};
    std::atomic<u64> displayed_serial{};
};

// Session pause (the Android app went to the background): the present thread takes no flips and
// signals no vblanks until released, and redraws the last frame only while a surface exists, so
// the paused game shows behind the session menu. Process-wide; one session runs at a time.
void SetSessionPaused(bool paused);

} // namespace Libraries::VideoOut
