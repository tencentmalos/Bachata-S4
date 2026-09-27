// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class RecordingCommandBuffer;

/// Diagnostic for GPU hangs: the GPU writes a serial into host-coherent memory right before
/// and right after every guest dispatch, and a watchdog reports the dispatch that started but
/// did not finish once both values stop moving. Off unless requested at renderer start with
/// SHADPS4_GPU_BREADCRUMBS=1 or debug.shadps4.gpu_breadcrumbs=1. Every marker is surrounded by
/// full barriers, so dispatches are serialized while it is on.
class GpuBreadcrumbs {
public:
    static bool Requested();

    explicit GpuBreadcrumbs(const Instance& instance);
    ~GpuBreadcrumbs();

    GpuBreadcrumbs(const GpuBreadcrumbs&) = delete;
    GpuBreadcrumbs& operator=(const GpuBreadcrumbs&) = delete;

    /// PM4 context recorded with each dispatch (see Liverpool::DispatchDiagnostics).
    struct Context {
        u32 queue;
        bool predicated;
        u32 predication_dw1;
        u32 predication_dw2;
        u32 predications;
    };

    void BeforeDispatch(const RecordingCommandBuffer& cmdbuf, u64 shader_hash, u32 dim_x,
                        u32 dim_y, u32 dim_z, bool indirect, const Context& context);
    void AfterDispatch(const RecordingCommandBuffer& cmdbuf);

private:
    struct Record {
        u32 serial;
        bool indirect;
        u64 shader_hash;
        u32 dim_x, dim_y, dim_z;
        Context context;
    };

    void Watch(std::stop_token stop);
    void Report(u32 begin, u32 end, std::chrono::milliseconds stalled);

    vk::Device device;
    vk::Buffer buffer;
    vk::DeviceMemory memory;
    volatile u32* mapped{};
    u32 next_serial{};
    u32 current_serial{};
    std::atomic<u32> predicated_dispatches{};
    std::chrono::milliseconds threshold{1000};
    std::mutex records_mutex;
    std::array<Record, 1024> records{};
    std::jthread watchdog;
};

} // namespace Vulkan
