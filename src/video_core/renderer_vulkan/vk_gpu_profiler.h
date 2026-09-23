// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include "common/gpu_timing.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/submission_worker.h"

namespace Vulkan {
class Instance;
// Scheduler-owner only. Bounded query leases are reused only after successful
// submission, timeline completion AND available results. No query WAIT bit.
class GpuProfiler {
public:
    using Stage = Common::Profiler::GpuStage;
    // ZonesPerBatch bounds detail zones per command buffer (render passes, dispatches,
    // transfers and buffer uploads); overflow is counted in dropped_zones, never blocks.
    static constexpr uint32_t Invalid = UINT32_MAX, BatchCount = 32, ZonesPerBatch = 1024;
    GpuProfiler(const Instance& instance, Stage stage);
    void BeginBatch(vk::CommandBuffer cmd, uint64_t completed);
    uint32_t Begin(vk::CommandBuffer cmd, Stage stage);
    void End(vk::CommandBuffer cmd, uint32_t zone);
    void Prepare(vk::CommandBuffer cmd);
    void FrameEnd();
    void PresentKind(bool redraw);
    void EndBatch(vk::CommandBuffer cmd);
    void Submitting();
    void Submitted(uint64_t tick);
    void Queued(uint64_t tick, std::shared_ptr<SubmissionReceipt> receipt);
    void Collect(uint64_t completed);
    // Increments at every BeginBatch; a zone may only be ended inside the batch that began it.
    uint64_t BatchSerial() const { return batch_serial; }
private:
    struct Zone { Stage stage{}; bool ended{}; };
    struct Batch {
        uint64_t tick{}, generation{}, submit_before_ns{};
        uint32_t count{}, partial_retries{};
        bool used{}, submitted{}, frame_end{};
        std::shared_ptr<SubmissionReceipt> receipt;
        std::array<Zone, ZonesPerBatch> zones;
    };
    bool Initialize();
    void Calibrate();
    const Instance& instance;
    std::shared_ptr<Common::Profiler::GpuTimingState> stats;
    vk::UniqueQueryPool pool;
    std::array<Batch, BatchCount> batches{};
    Stage stage;
    uint32_t current{Invalid}, guest_zone{Invalid}, prepare_zone{Invalid}, next_slot{};
    uint8_t bits{}, context{};
    double period{}, guest_frame_ns{};
    uint64_t frame_generation{}, last_retired_tick{};
    uint64_t context_generation{}, batch_serial{};
    uint64_t best_bound{UINT64_MAX}, anchor_cpu{}, anchor_gpu{};
    uint32_t calibration_samples{};
    bool attempted{}, calibration_attempted{}, disabled{}, frame_incomplete{true};
};
}
