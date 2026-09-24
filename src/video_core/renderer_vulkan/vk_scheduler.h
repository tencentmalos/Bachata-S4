// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <array>
#include <atomic>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <queue>
#include <span>
#include <string>
#include <vector>

#include "common/unique_function.h"
#include "video_core/renderer_vulkan/render_break.h"
#include "video_core/renderer_vulkan/render_pass_stats.h"
#include "video_core/renderer_vulkan/vk_command_recorder.h"
#include "video_core/amdgpu/pm4_trace.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

namespace tracy {
class VkCtxScope;
}

namespace Vulkan {

class Instance;

struct RenderAttachment {
    vk::ImageView image_view;
    vk::ImageLayout image_layout;
    std::array<u32, 4> clear_value;
    union {
        u32 is_clear;
        struct {
            bool has_depth;
            bool depth_clear;
            bool has_stencil;
            bool stencil_clear;
        };
    };
};
static_assert(std::has_unique_object_representations_v<RenderAttachment>);

struct RenderState {
    std::array<RenderAttachment, 8> color_attachments;
    RenderAttachment depth_stencil_attachment;
    u16 width;
    u16 height;
    u16 num_layers;
    u16 num_color_attachments;

    bool operator==(const RenderState& other) const noexcept {
        return std::memcmp(this, &other, sizeof(RenderState)) == 0;
    }
};
static_assert(std::has_unique_object_representations_v<RenderState>);

struct SubmitInfo {
    std::array<vk::Semaphore, 3> wait_semas;
    std::array<u64, 3> wait_ticks;
    std::array<vk::PipelineStageFlags, 3> wait_stages;
    std::array<vk::Semaphore, 3> signal_semas;
    std::array<u64, 3> signal_ticks;
    vk::Fence fence;
    u32 num_wait_semas;
    u32 num_signal_semas;

    void AddWait(vk::Semaphore semaphore, u64 tick = 1,
                 vk::PipelineStageFlags stages = vk::PipelineStageFlagBits::eAllCommands) {
        wait_semas[num_wait_semas] = semaphore;
        wait_stages[num_wait_semas] = stages;
        wait_ticks[num_wait_semas++] = tick;
    }

    void AddSignal(vk::Semaphore semaphore, u64 tick = 1) {
        signal_semas[num_signal_semas] = semaphore;
        signal_ticks[num_signal_semas++] = tick;
    }

    void AddSignal(vk::Fence fence) {
        this->fence = fence;
    }
};

using Viewports = boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS>;
using Scissors = boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS>;
using ColorWriteMasks = std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS>;
struct StencilOps {
    vk::StencilOp fail_op{};
    vk::StencilOp pass_op{};
    vk::StencilOp depth_fail_op{};
    vk::CompareOp compare_op{};

    bool operator==(const StencilOps& other) const {
        return fail_op == other.fail_op && pass_op == other.pass_op &&
               depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
    }
};
struct DynamicState {
    struct {
        bool viewports : 1;
        bool scissors : 1;

        bool depth_test_enabled : 1;
        bool depth_write_enabled : 1;
        bool depth_compare_op : 1;

        bool depth_bounds_test_enabled : 1;
        bool depth_bounds : 1;

        bool depth_bias_enabled : 1;
        bool depth_bias : 1;

        bool stencil_test_enabled : 1;
        bool stencil_front_ops : 1;
        bool stencil_front_reference : 1;
        bool stencil_front_write_mask : 1;
        bool stencil_front_compare_mask : 1;
        bool stencil_back_ops : 1;
        bool stencil_back_reference : 1;
        bool stencil_back_write_mask : 1;
        bool stencil_back_compare_mask : 1;

        bool primitive_restart_enable : 1;
        bool rasterizer_discard_enable : 1;
        bool cull_mode : 1;
        bool front_face : 1;

        bool blend_constants : 1;
        bool color_write_masks : 1;
        bool line_width : 1;
        bool feedback_loop_enabled : 1;
    } dirty_state{};

    Viewports viewports{};
    Scissors scissors{};

    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};

    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};

    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};

    bool stencil_test_enabled{};
    StencilOps stencil_front_ops{};
    u32 stencil_front_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_front_compare_mask{};
    StencilOps stencil_back_ops{};
    u32 stencil_back_reference{};
    u32 stencil_back_write_mask{};
    u32 stencil_back_compare_mask{};

    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};

    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    bool feedback_loop_enabled{};

    /// Commits the dynamic state to the provided command buffer.
    void Commit(const Instance& instance, const RecordingCommandBuffer& cmdbuf);

    /// Invalidates all dynamic state to be flushed into the next command buffer.
    void Invalidate() {
        std::memset(&dirty_state, 0xFF, sizeof(dirty_state));
    }

    void SetViewports(const Viewports& viewports_) {
        if (!std::ranges::equal(viewports, viewports_)) {
            viewports = viewports_;
            dirty_state.viewports = true;
        }
    }

    void SetScissors(const Scissors& scissors_) {
        if (!std::ranges::equal(scissors, scissors_)) {
            scissors = scissors_;
            dirty_state.scissors = true;
        }
    }

    void SetDepthTestEnabled(const bool enabled) {
        if (depth_test_enabled != enabled) {
            depth_test_enabled = enabled;
            dirty_state.depth_test_enabled = true;
        }
    }

    void SetDepthWriteEnabled(const bool enabled) {
        if (depth_write_enabled != enabled) {
            depth_write_enabled = enabled;
            dirty_state.depth_write_enabled = true;
        }
    }

    void SetDepthCompareOp(const vk::CompareOp compare_op) {
        if (depth_compare_op != compare_op) {
            depth_compare_op = compare_op;
            dirty_state.depth_compare_op = true;
        }
    }

    void SetDepthBoundsTestEnabled(const bool enabled) {
        if (depth_bounds_test_enabled != enabled) {
            depth_bounds_test_enabled = enabled;
            dirty_state.depth_bounds_test_enabled = true;
        }
    }

    void SetDepthBounds(const float min, const float max) {
        if (depth_bounds_min != min || depth_bounds_max != max) {
            depth_bounds_min = min;
            depth_bounds_max = max;
            dirty_state.depth_bounds = true;
        }
    }

    void SetDepthBiasEnabled(const bool enabled) {
        if (depth_bias_enabled != enabled) {
            depth_bias_enabled = enabled;
            dirty_state.depth_bias_enabled = true;
        }
    }

    void SetDepthBias(const float constant, const float clamp, const float slope) {
        if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
            depth_bias_slope != slope) {
            depth_bias_constant = constant;
            depth_bias_clamp = clamp;
            depth_bias_slope = slope;
            dirty_state.depth_bias = true;
        }
    }

    void SetStencilTestEnabled(const bool enabled) {
        if (stencil_test_enabled != enabled) {
            stencil_test_enabled = enabled;
            dirty_state.stencil_test_enabled = true;
        }
    }

    void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
        if (stencil_front_ops != front_ops) {
            stencil_front_ops = front_ops;
            dirty_state.stencil_front_ops = true;
        }
        if (stencil_back_ops != back_ops) {
            stencil_back_ops = back_ops;
            dirty_state.stencil_back_ops = true;
        }
    }

    void SetStencilReferences(const u32 front_reference, const u32 back_reference) {
        if (stencil_front_reference != front_reference) {
            stencil_front_reference = front_reference;
            dirty_state.stencil_front_reference = true;
        }
        if (stencil_back_reference != back_reference) {
            stencil_back_reference = back_reference;
            dirty_state.stencil_back_reference = true;
        }
    }

    void SetStencilWriteMasks(const u32 front_write_mask, const u32 back_write_mask) {
        if (stencil_front_write_mask != front_write_mask) {
            stencil_front_write_mask = front_write_mask;
            dirty_state.stencil_front_write_mask = true;
        }
        if (stencil_back_write_mask != back_write_mask) {
            stencil_back_write_mask = back_write_mask;
            dirty_state.stencil_back_write_mask = true;
        }
    }

    void SetStencilCompareMasks(const u32 front_compare_mask, const u32 back_compare_mask) {
        if (stencil_front_compare_mask != front_compare_mask) {
            stencil_front_compare_mask = front_compare_mask;
            dirty_state.stencil_front_compare_mask = true;
        }
        if (stencil_back_compare_mask != back_compare_mask) {
            stencil_back_compare_mask = back_compare_mask;
            dirty_state.stencil_back_compare_mask = true;
        }
    }

    void SetPrimitiveRestartEnabled(const bool enabled) {
        if (primitive_restart_enable != enabled) {
            primitive_restart_enable = enabled;
            dirty_state.primitive_restart_enable = true;
        }
    }

    void SetCullMode(const vk::CullModeFlags cull_mode_) {
        if (cull_mode != cull_mode_) {
            cull_mode = cull_mode_;
            dirty_state.cull_mode = true;
        }
    }

    void SetFrontFace(const vk::FrontFace front_face_) {
        if (front_face != front_face_) {
            front_face = front_face_;
            dirty_state.front_face = true;
        }
    }

    void SetBlendConstants(const std::array<float, 4> blend_constants_) {
        if (blend_constants != blend_constants_) {
            blend_constants = blend_constants_;
            dirty_state.blend_constants = true;
        }
    }

    void SetRasterizerDiscardEnabled(const bool enabled) {
        if (rasterizer_discard_enable != enabled) {
            rasterizer_discard_enable = enabled;
            dirty_state.rasterizer_discard_enable = true;
        }
    }

    void SetColorWriteMasks(const ColorWriteMasks& color_write_masks_) {
        if (!std::ranges::equal(color_write_masks, color_write_masks_)) {
            color_write_masks = color_write_masks_;
            dirty_state.color_write_masks = true;
        }
    }

    void SetLineWidth(const float width) {
        if (line_width != width) {
            line_width = width;
            dirty_state.line_width = true;
        }
    }

    void SetAttachmentFeedbackLoopEnabled(const bool enabled) {
        if (feedback_loop_enabled != enabled) {
            feedback_loop_enabled = enabled;
            dirty_state.feedback_loop_enabled = true;
        }
    }
};

class Scheduler {
public:
    explicit Scheduler(const Instance& instance, GpuProfiler::Stage stage = GpuProfiler::Stage::DrawBatch);
    GpuProfiler& GpuProfile() { return gpu_profiler; }
    /// Writes a profiler timestamp in recording order (through the deferred recorder
    /// when it is active) without draining it to the raw command buffer.
    auto TimestampWriter() {
        return [this](vk::QueryPool pool, uint32_t query) {
            CommandBuffer().Custom(0, [pool, query](vk::CommandBuffer cmd) {
                cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
            });
        };
    }
    ~Scheduler();

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush(SubmitInfo& info);

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush();

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish();

    // The present binary semaphore must have an actual submitted signal before
    // vkQueuePresentKHR. This waits for host submission only, not GPU completion.
    void WaitSubmitted();

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick);

    /// Attempts to execute operations whose tick the GPU has caught up with.
    void PopPendingOperations();

    /// Starts a new rendering scope with provided state.
    /// Returns true only when a new Vulkan rendering scope was started.
    // See render_break.h: only real breaks (a pass was open) are counted.
    using RenderBreak = Vulkan::RenderBreak;
    static constexpr const auto& RenderBreakNames = Vulkan::RenderBreakNames;
    // A hoisted operation is logically outside (before) the open pass.
    bool IsRendering() const { return is_rendering && !hoisting; }
    const std::array<uint64_t, size_t(RenderBreak::Count)>& RenderBreaks() const { return render_breaks; }
    uint64_t RenderBegins() const { return render_begins; }
    /// Pass instances whose attachments/area differ from the previous instance: the
    /// boundaries the guest itself expressed by changing render targets.
    uint64_t RenderNatural() const { return render_natural; }
    /// Instances that re-opened the previous state after a break, keyed by what broke
    /// it: these are the tile load/store round trips the guest never asked for.
    const std::array<uint64_t, size_t(RenderBreak::Count)>& RenderResumes() const { return render_resumes; }
    bool LastBeginResumed() const { return last_begin_resumed; }
    /// Bounded diagnostics for pass-breaking transitions/resumes; gpu_memory request re-arms.
    bool TakePassBreakLog() {
        auto n = pass_break_log_budget.load(std::memory_order_relaxed);
        while (n && !pass_break_log_budget.compare_exchange_weak(n, n - 1, std::memory_order_relaxed)) {}
        return n != 0;
    }
    void ArmPassBreakLog(uint32_t lines) { pass_break_log_budget.store(lines, std::memory_order_relaxed); }
    bool BeginRendering(const RenderState& new_state);

    /// Counts a draw recorded inside the open render pass (per-pass draw histogram).
    void NoteDraw() {
        ++pass_draws;
        if (pass_trackable) {
            if (pass_reads.size() + staged_reads.size() > MaxPassRanges ||
                pass_writes.size() + staged_writes.size() > MaxPassRanges) {
                pass_trackable = false;
            } else {
                pass_reads.insert(pass_reads.end(), staged_reads.begin(), staged_reads.end());
                pass_writes.insert(pass_writes.end(), staged_writes.begin(), staged_writes.end());
            }
        }
        ClearStagedAccess();
    }

    /// Buffer barrier hoisting. A draw that continues the open pass and needs a barrier on
    /// a buffer no draw of this pass has written (or, for a write, accessed) depends only on
    /// work recorded before the pass: the barrier goes in the pre-pass slot instead of
    /// ending the pass. ClassifyBarrier is called per barrier with the buffer's whole guest
    /// range (its access state covers the whole buffer); PrepareBarrierHoist once the
    /// draw's render state is known; TakeBarrierHoist when the barriers are recorded.
    void ClassifyBarrier(VAddr address, u64 size, bool write_access) {
        ++barriers_classified;
        if (!is_rendering || !pass_trackable) {
            barrier_conflict = true;
            return;
        }
        const AccessRange r{address, address + size};
        const auto overlaps = [&r](const std::vector<AccessRange>& set) {
            for (const auto& [begin, end] : set)
                if (begin < r.second && r.first < end)
                    return true;
            return false;
        };
        if (overlaps(pass_writes) || (write_access && overlaps(pass_reads)))
            barrier_conflict = true;
    }
    void PrepareBarrierHoist(const RenderState& state) noexcept {
        barrier_pass_continues = is_rendering && render_state == state;
    }
    bool TakeBarrierHoist(const vk::DependencyInfo& dependencies);
    void ResetBarrierHoist() noexcept {
        barrier_conflict = false;
        barrier_pass_continues = false;
        barriers_classified = 0;
    }

    /// Guest memory accessed by the draw being prepared; joins the pass the draw lands in.
    void StageAccess(VAddr address, u64 size, bool write) {
        if (hoisting || !size || !(recorder && recorder->Deferring()))
            return;
        (write ? staged_writes : staged_reads).emplace_back(address, address + size);
    }
    void ClearStagedAccess() noexcept {
        staged_reads.clear();
        staged_writes.clear();
    }

    /// Pass description (targets, load/clear, read-only depth) for the next pass begin:
    /// inserted as a debug label when `marker` is set and kept by the pass log.
    bool WantsPassDescription() const noexcept;
    /// Host debug labels (RenderDoc loaded or host markers enabled): emulator-side work
    /// such as uploads, readbacks and HLE copies is labelled so a capture separates it
    /// from guest draws.
    static bool LabelsEnabled() noexcept;
    void Label(const std::string& text) {
        CommandBuffer().insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = text.c_str()});
    }
    void SetPassDescription(std::string description, bool marker) {
        pending_pass_description = std::move(description);
        pending_pass_marker = marker;
    }
    /// pass_log start [passes] | status | dump | stop (default off).
    static std::string PassLogCommand(const std::vector<std::string>& args);
    /// pass_log only: records which buffer barrier a draw/dispatch needs, so a pass the
    /// barrier ends carries its source (buffer, previous writer, new access, shader).
    static bool PassLogActive() noexcept;
    void NoteBarrierSource(const char* kind, u64 address, u64 size,
                           const vk::BufferMemoryBarrier2& barrier, u64 shader_hash = 0);
    void NoteBreak(std::string_view text) {
        if ((PassLogActive() || AmdGpu::Pm4Trace::Active()) && break_detail.size() <= 1024) {
            break_detail += ' ';
            break_detail += text;
        }
    }
    void ClearBreakDetail() noexcept {
        if (!break_detail.empty())
            break_detail.clear();
    }

    using AccessRange = std::pair<VAddr, VAddr>; // [begin, end)
    /// Tries to place the following commands before the open (held) render pass instead
    /// of breaking it: only when nothing recorded in the pass so far writes what the
    /// operation reads, or reads/writes what it writes. `reads_written` also treats the
    /// reads as writes (e.g. the operation may upload those ranges first). On success the
    /// operation must be closed with EndHoist; EndRendering is a no-op in between.
    bool BeginHoist(std::span<const AccessRange> reads, std::span<const AccessRange> writes,
                    bool reads_written);
    void EndHoist();

    /// Ends current rendering scope.
    void EndRendering(RenderBreak cause = RenderBreak::Other);

    /// Returns the current render state.
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        return dynamic_state;
    }

    /// Returns the current command buffer. With deferred recording on, commands are
    /// copied into chunks and replayed in order by the recording thread.
    RecordingCommandBuffer CommandBuffer() const {
        return {recorder.get(), current_cmdbuf};
    }

    /// The real command buffer for code that records through its own entry points
    /// (profiler timestamps, third-party integrations). Drains deferred recording first.
    vk::CommandBuffer RawCommandBuffer() {
        if (hoisting)
            InterruptHoist();
        if (recorder)
            recorder->CheckOutRaw();
        return current_cmdbuf;
    }

    // Logical scopes can cross Flush; each physical command buffer still gets
    // balanced labels. Only diagnostic callers allocate names.
    void BeginMarker(std::string name);
    void EndMarker();

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore.CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (master_semaphore.IsFree(tick)) {
            return true;
        }
        master_semaphore.Refresh();
        return master_semaphore.IsFree(tick);
    }

    /// Returns the master timeline semaphore.
    // Host passes normally push descriptors. Diagnostic mode uses ordinary sets
    // with the existing timeline-owned heap so SDK instrumentation sees real bindings.
    void BindHostDescriptors(vk::PipelineBindPoint point, vk::PipelineLayout layout,
        vk::DescriptorSetLayout set_layout, vk::ArrayProxy<const vk::WriteDescriptorSet> writes);

    [[nodiscard]] MasterSemaphore* GetMasterSemaphore() noexcept {
        return &master_semaphore;
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        std::unique_lock lk(pending_ops_mutex);
        pending_ops.emplace(std::move(func), CurrentTick());
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Runs as soon as possible in another thread.
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), CurrentTick());
        }
        priority_pending_ops_cv.notify_one();
    }

private:
    void AllocateWorkerCommandBuffers();

    void SubmitExecution(SubmitInfo& info);

    void PriorityPendingOpsThread(std::stop_token stoken);

private:
    const Instance& instance;
    MasterSemaphore master_semaphore;
    CommandPool command_pool;
    GpuProfiler gpu_profiler;
    std::unique_ptr<DescriptorHeap> diagnostic_descriptors;
    DynamicState dynamic_state;
    vk::CommandBuffer current_cmdbuf;
    std::unique_ptr<CommandRecorder> recorder;
    std::vector<std::string> marker_stack;
    uint64_t last_submission{};
    std::condition_variable_any event_cv;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    std::queue<PendingOp> priority_pending_ops;
    std::exception_ptr priority_error;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    std::jthread priority_pending_ops_thread;
    RenderState render_state;
    bool is_rendering = false;
    uint32_t gpu_render_zone = GpuProfiler::Invalid;
    uint64_t gpu_render_serial{};
    std::array<uint64_t, size_t(RenderBreak::Count)> render_breaks{};
    std::array<uint64_t, size_t(RenderBreak::Count)> render_resumes{};
    uint64_t render_begins{}, render_natural{};
    RenderBreak last_break{RenderBreak::Other};
    bool last_begin_resumed{};
    u32 pass_draws{};
    // Pass dependency tracking for hoisting (guest address ranges of the open pass).
    static constexpr size_t MaxPassRanges = 16384;
    bool pass_trackable{};
    bool hoisting{};
    std::vector<AccessRange> pass_reads, pass_writes, staged_reads, staged_writes;
    void InterruptHoist();
    // Pass description / log of the open pass.
    std::string pending_pass_description, pass_description, break_detail;
    bool barrier_conflict{}, barrier_pass_continues{};
    u32 barriers_classified{}; // barriers without a classification are never hoisted
    bool pending_pass_marker{};
    u32 pass_loads{}, pass_clears{}, pass_hoists{};
    u64 pass_load_pixels{};
    RenderBreak pass_begin_cause{RenderBreak::Other};
    std::atomic<uint32_t> pass_break_log_budget{400};
    tracy::VkCtxScope* profiler_scope{};
};

// Detail-mode GPU zone around one host-issued GPU operation (dispatch, transfer,
// buffer upload). Records nothing unless gpu_timing detail is on. If the scheduler
// flushed in between, EndBatch already closed the zone in the old command buffer and
// the destructor skips the end timestamp instead of writing into the new batch.
class GpuZoneScope {
public:
    GpuZoneScope(Scheduler& scheduler_, GpuProfiler::Stage stage) : scheduler{scheduler_} {
        if (!Common::Profiler::GpuTimingDetailed()) return;
        serial = scheduler.GpuProfile().BatchSerial();
        zone = scheduler.GpuProfile().BeginWith(stage, scheduler.TimestampWriter());
    }
    ~GpuZoneScope() {
        if (zone != GpuProfiler::Invalid && scheduler.GpuProfile().BatchSerial() == serial)
            scheduler.GpuProfile().EndWith(zone, scheduler.TimestampWriter());
    }
    GpuZoneScope(const GpuZoneScope&) = delete;
    GpuZoneScope& operator=(const GpuZoneScope&) = delete;
private:
    Scheduler& scheduler;
    uint64_t serial{};
    uint32_t zone{GpuProfiler::Invalid};
};

} // namespace Vulkan
