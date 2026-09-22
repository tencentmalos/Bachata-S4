// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <fmt/format.h>
#include <unordered_map>
#include "common/logging/log.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/diagnostics/pipeline_handoff.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderdoc.h"
#include "video_core/renderdoc_capture.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Scheduler::Scheduler(const Instance& instance, GpuProfiler::Stage stage)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore}, gpu_profiler{instance, stage} {
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    if (instance.GpuReshapeAdapter().IsActive()) {
        static constexpr vk::DescriptorPoolSize sizes[] = {
            {vk::DescriptorType::eStorageBuffer, 4096},
            {vk::DescriptorType::eUniformBuffer, 4096},
            {vk::DescriptorType::eSampledImage, 4096},
            {vk::DescriptorType::eStorageImage, 4096},
            {vk::DescriptorType::eCombinedImageSampler, 4096},
            {vk::DescriptorType::eSampler, 4096},
        };
        diagnostic_descriptors = std::make_unique<DescriptorHeap>(instance, &work_semaphore, sizes);
    }
    // Only the guest command stream is recorded on a separate thread.
    if (stage == GpuProfiler::Stage::DrawBatch)
        recorder = std::make_unique<CommandRecorder>();
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread([this](std::stop_token stop) {
            try { PriorityPendingOpsThread(stop); }
            catch (...) {
                // Propagate on the renderer owner at its next pending-op poll.
                // An exception escaping std::thread would terminate the whole APK.
                std::lock_guard lock{priority_pending_ops_mutex};
                priority_error = std::current_exception();
            }
        });
}

Scheduler::~Scheduler() {
    try { WaitSubmitted(); }
    catch (const std::exception& e) {
        // The worker has discarded unexecuted jobs before publishing failure.
        // Never throw from teardown; the owner already observes the same error.
        LOG_ERROR(Render_Vulkan, "Submission worker teardown: {}", e.what());
    }
    priority_pending_ops_thread.request_stop();
    priority_pending_ops_cv.notify_all();
    if (priority_pending_ops_thread.joinable()) priority_pending_ops_thread.join();
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BindHostDescriptors(vk::PipelineBindPoint point, vk::PipelineLayout layout,
    vk::DescriptorSetLayout set_layout, vk::ArrayProxy<const vk::WriteDescriptorSet> writes) {
    if (!diagnostic_descriptors) {
        CommandBuffer().pushDescriptorSetKHR(point, layout, 0, writes);
        return;
    }
    const auto set = diagnostic_descriptors->Commit(set_layout);
    std::vector<vk::WriteDescriptorSet> assigned(writes.begin(), writes.end());
    for (auto& write : assigned) write.dstSet = set;
    instance.GetDevice().updateDescriptorSets(assigned, {});
    CommandBuffer().bindDescriptorSets(point, layout, 0, set, {});
}

bool Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return false;
    }
    // Same state as the instance that was broken: the guest did not change targets,
    // the break (and this re-open) is emulator-imposed. Attribute it to that break.
    const bool resumed = !is_rendering && render_begins != 0 && render_state == new_state;
    EndRendering(RenderBreak::StateChange);
    ++render_begins;
    last_begin_resumed = resumed;
    if (resumed) {
        ++render_resumes[size_t(last_break)];
        if (TakePassBreakLog())
            LOG_INFO(Render_Vulkan, "Internal scale: pass resume after {} {}x{} colors={} depth={}",
                     RenderBreakNames[size_t(last_break)], new_state.width, new_state.height,
                     new_state.num_color_attachments, new_state.depth_stencil_attachment.has_depth);
    } else {
        ++render_natural;
    }
    is_rendering = true;
    render_state = new_state;
    pass_draws = 0;
    {
        // Tile traffic this pass asks for: every attachment is stored; LOAD reads it in.
        constexpr auto o = std::memory_order_relaxed;
        auto& stats = render_pass_stats;
        const u64 pixels = u64(new_state.width) * new_state.height * std::max<u16>(new_state.num_layers, 1);
        u32 loads = 0, clears = 0, stores = 0;
        for (u32 i = 0; i < new_state.num_color_attachments; ++i) {
            const auto& cb = new_state.color_attachments[i];
            if (!cb.image_view)
                continue;
            ++stores;
            cb.is_clear ? ++clears : ++loads;
        }
        const auto& ds = new_state.depth_stencil_attachment;
        if (ds.image_view && (ds.has_depth || ds.has_stencil)) {
            ++stores;
            const bool loaded = (ds.has_depth && !ds.depth_clear) || (ds.has_stencil && !ds.stencil_clear);
            loaded ? ++loads : ++clears;
        }
        pass_loads = loads;
        pass_clears = clears;
        pass_load_pixels = loads * pixels;
        pass_hoists = 0;
        stats.passes.fetch_add(1, o);
        stats.loads.fetch_add(loads, o);
        stats.clears.fetch_add(clears, o);
        stats.stores.fetch_add(stores, o);
        stats.load_pixels.fetch_add(loads * pixels, o);
        stats.store_pixels.fetch_add(stores * pixels, o);
    }

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    pass_reads.clear();
    pass_writes.clear();
    if (recorder) {
        recorder->BeginPass(); // hold the pass so independent work can go before it
        pass_trackable = recorder->Holding() &&
                         !render_pass_stats.hoist_off.load(std::memory_order_relaxed);
    }
    // Inside the held pass: work hoisted before it is not counted as this pass.
    if (Common::Profiler::GpuTimingDetailed()) {
        gpu_render_serial = gpu_profiler.BatchSerial();
        gpu_render_zone = gpu_profiler.BeginWith(GpuProfiler::Stage::RenderPass,
                                                 TimestampWriter(), render_begins);
    }
    CommandBuffer().beginRendering(rendering_info);
    pass_begin_cause = resumed ? last_break : RenderBreak::Other;
    pass_description = std::move(pending_pass_description);
    pending_pass_description.clear();
    if (AmdGpu::Pm4Trace::Active())
        AmdGpu::Pm4Trace::NoteHost(AmdGpu::Pm4Trace::HostEvent::PassBegin, render_begins,
                                   resumed ? 1 : 0, static_cast<u64>(pass_begin_cause),
                                   (u64(render_state.width) << 32) | render_state.height,
                                   pass_description);
    if (pending_pass_marker && !pass_description.empty()) {
        // RenderDoc: what this pass instance is and why it was opened.
        const auto label = fmt::format("shadps4.pass #{} {} {}", render_begins,
                                       resumed ? fmt::format("resumed-after:{}", RenderBreakNames[size_t(last_break)])
                                               : std::string{"guest"},
                                       pass_description);
        CommandBuffer().insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = label.c_str()});
    }
    return true;
}

namespace {
struct PassLogEntry {
    u64 index;
    bool resumed;
    RenderBreak begin_cause, end_cause;
    u32 draws, loads, clears, hoists;
    u64 load_pixels;
    std::string description;
    std::string end_detail;
};
struct PassLog {
    std::mutex mutex;
    std::atomic<u32> remaining{};
    std::vector<PassLogEntry> entries;
    // GPU time per pass index (gpu_timing detail), joined at dump.
    bool collecting{};
    std::unordered_map<u64, u64> gpu_ns;
} pass_log;
} // namespace

bool Scheduler::PassLogActive() noexcept {
    return pass_log.remaining.load(std::memory_order_relaxed) != 0;
}

void Scheduler::NoteBarrierSource(const char* kind, u64 address, u64 size,
                                  const vk::BufferMemoryBarrier2& barrier, u64 shader_hash) {
    if (AmdGpu::Pm4Trace::Active())
        AmdGpu::Pm4Trace::NoteHost(
            AmdGpu::Pm4Trace::HostEvent::Barrier, address, size,
            static_cast<u64>(static_cast<VkAccessFlags2>(barrier.srcAccessMask)),
            static_cast<u64>(static_cast<VkAccessFlags2>(barrier.dstAccessMask)),
            shader_hash ? fmt::format("{} sh={:#x}", kind, shader_hash) : std::string{kind});
    if (!(PassLogActive() || AmdGpu::Pm4Trace::Active()) || break_detail.size() > 1024)
        return;
    break_detail += fmt::format(" [{} {:#x}+{:#x} src={}/{} dst={}{}]", kind, address, size,
                                vk::to_string(barrier.srcAccessMask),
                                vk::to_string(barrier.srcStageMask),
                                vk::to_string(barrier.dstAccessMask),
                                shader_hash ? fmt::format(" sh={:#x}", shader_hash) : std::string{});
}

void RecordTaggedGpuTime(uint64_t tag, uint64_t ns) {
    std::scoped_lock lk{pass_log.mutex};
    if (pass_log.collecting && pass_log.gpu_ns.size() < 4 * pass_log.entries.capacity())
        pass_log.gpu_ns[tag] = ns;
}

bool Scheduler::LabelsEnabled() noexcept {
    return VideoCore::IsRenderDocLoaded() || EmulatorSettings.IsVkHostMarkersEnabled();
}

bool Scheduler::WantsPassDescription() const noexcept {
    return pass_log.remaining.load(std::memory_order_relaxed) != 0 ||
           AmdGpu::Pm4Trace::Active() || VideoCore::IsRenderDocLoaded();
}

std::string Scheduler::PassLogCommand(const std::vector<std::string>& args) {
    const std::string cmd = args.empty() ? "status" : args[0];
    std::scoped_lock lk{pass_log.mutex};
    if (cmd == "start") {
        u32 n = 2000;
        if (args.size() > 1)
            n = static_cast<u32>(std::clamp<unsigned long>(std::stoul(args[1]), 1, 20000));
        pass_log.entries.clear();
        pass_log.entries.reserve(n);
        pass_log.gpu_ns.clear();
        pass_log.collecting = true;
        pass_log.remaining.store(n, std::memory_order_relaxed);
        return fmt::format("pass_log armed for {} passes\n", n);
    }
    if (cmd == "stop") {
        pass_log.remaining.store(0, std::memory_order_relaxed);
        pass_log.collecting = false;
        return "pass_log stopped\n";
    }
    if (cmd != "status" && cmd != "dump")
        return "usage: pass_log start [passes] | status | dump | stop\n";
    std::string out = fmt::format("pass_log remaining={} entries={} gpu_timed={}\n",
                                  pass_log.remaining.load(std::memory_order_relaxed),
                                  pass_log.entries.size(), pass_log.gpu_ns.size());
    if (cmd == "dump") {
        for (const auto& e : pass_log.entries) {
            const auto gpu = pass_log.gpu_ns.find(e.index);
            out += fmt::format("pass {} {} begin={} end={} draws={} loads={} clears={} load_px={} "
                               "hoists={} gpu_us={} {}\n",
                               e.index, e.resumed ? "resumed" : "guest",
                               e.resumed ? RenderBreakNames[size_t(e.begin_cause)] : "-",
                               RenderBreakNames[size_t(e.end_cause)], e.draws, e.loads, e.clears,
                               e.load_pixels, e.hoists,
                               gpu == pass_log.gpu_ns.end() ? -1.0 : gpu->second / 1000.0,
                               e.description);
            if (!e.end_detail.empty())
                out += fmt::format("  end_detail:{}\n", e.end_detail);
        }
    }
    return out;
}

bool Scheduler::BeginHoist(std::span<const AccessRange> reads, std::span<const AccessRange> writes,
                           bool reads_written) {
    if (!is_rendering || hoisting)
        return false;
    constexpr auto o = std::memory_order_relaxed;
    if (!pass_trackable || !recorder || !recorder->Holding()) {
        render_pass_stats.hoist_unavailable.fetch_add(1, o);
        if (PassLogActive() || AmdGpu::Pm4Trace::Active())
            NoteBreak(fmt::format("[hoist unavailable: trackable={} recorder={} holding={}]",
                                  pass_trackable, bool(recorder),
                                  recorder && recorder->Holding()));
        return false;
    }
    const auto overlaps = [](const std::vector<AccessRange>& set, const AccessRange& r) {
        for (const auto& [begin, end] : set)
            if (begin < r.second && r.first < end)
                return true;
        return false;
    };
    for (const auto& w : writes) {
        if (overlaps(pass_reads, w) || overlaps(pass_writes, w)) {
            render_pass_stats.hoist_conflicts.fetch_add(1, o);
            if (PassLogActive() || AmdGpu::Pm4Trace::Active())
                NoteBreak(fmt::format("[hoist conflict: copy writes {:#x}-{:#x}, pass {}]", w.first,
                                      w.second, overlaps(pass_writes, w) ? "wrote it" : "read it"));
            return false;
        }
    }
    for (const auto& r : reads) {
        if (overlaps(pass_writes, r) || (reads_written && overlaps(pass_reads, r))) {
            render_pass_stats.hoist_conflicts.fetch_add(1, o);
            if (PassLogActive() || AmdGpu::Pm4Trace::Active())
                NoteBreak(fmt::format("[hoist conflict: copy reads {:#x}-{:#x}, pass {}]", r.first,
                                      r.second,
                                      overlaps(pass_writes, r) ? "wrote it"
                                                               : "read it and source re-uploads"));
            return false;
        }
    }
    recorder->BeginPrePass();
    hoisting = true;
    ++pass_hoists;
    render_pass_stats.hoisted.fetch_add(1, o);
    return true;
}

void Scheduler::EndHoist() {
    if (!hoisting)
        return;
    recorder->EndPrePass();
    hoisting = false;
}

void Scheduler::InterruptHoist() {
    // Something needs the pass closed (submission, raw command buffer) while a hoisted
    // operation is being recorded: end the held pass now. The operation's commands so far
    // stay before the pass, the rest follows its end; both orders are valid for an
    // operation independent of the pass, and its own order is unchanged.
    recorder->EndPrePass();
    hoisting = false;
    render_pass_stats.hoist_interrupted.fetch_add(1, std::memory_order_relaxed);
    EndRendering(RenderBreak::Other);
}

void Scheduler::EndRendering(RenderBreak cause) {
    if (!is_rendering || hoisting) {
        return;
    }
    ++render_breaks[size_t(cause)];
    last_break = cause;
    is_rendering = false;
    {
        constexpr auto o = std::memory_order_relaxed;
        auto& stats = render_pass_stats;
        (pass_draws == 0 ? stats.empty : pass_draws == 1 ? stats.single
                                        : pass_draws < 8 ? stats.few : stats.many)
            .fetch_add(1, o);
    }
    if (AmdGpu::Pm4Trace::Active())
        AmdGpu::Pm4Trace::NoteHost(AmdGpu::Pm4Trace::HostEvent::PassEnd, render_begins,
                                   static_cast<u64>(cause), pass_draws, pass_load_pixels,
                                   break_detail);
    if (pass_log.remaining.load(std::memory_order_relaxed)) {
        std::scoped_lock lk{pass_log.mutex};
        if (u32 left = pass_log.remaining.load(std::memory_order_relaxed)) {
            pass_log.entries.push_back({render_begins, last_begin_resumed, pass_begin_cause, cause,
                                        pass_draws, pass_loads, pass_clears, pass_hoists,
                                        pass_load_pixels, std::move(pass_description),
                                        std::move(break_detail)});
            pass_log.remaining.store(left - 1, std::memory_order_relaxed);
        }
    }
    pass_description.clear();
    CommandBuffer().endRendering();
    if (gpu_render_zone != GpuProfiler::Invalid && gpu_profiler.BatchSerial() == gpu_render_serial)
        gpu_profiler.EndWith(gpu_render_zone, TimestampWriter());
    gpu_render_zone = GpuProfiler::Invalid;
    pass_trackable = false;
    if (recorder)
        recorder->EndPass(); // queue pre-pass work, then the pass
}

void Scheduler::Flush(SubmitInfo& info) {
    // Android transfers an ended command buffer to a bounded device FIFO. Only
    // explicit GPU/resource waits and queue capacity cause producer backpressure.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
    WaitSubmitted();
    gpu_profiler.Collect(work_semaphore.KnownGpuTick());
}

void Scheduler::WaitSubmitted() {
    if (auto* worker = instance.Submissions()) {
        Common::Profiler::Scope scope{"Vulkan.WaitSubmitted"};
        worker->Wait(last_submission);
    }
}

void Scheduler::Wait(u64 tick) {
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    work_semaphore.Wait(tick);
}

void Scheduler::PopPendingOperations() {
    {
        std::lock_guard lock{priority_pending_ops_mutex};
        if (priority_error) std::rethrow_exception(priority_error);
    }
    work_semaphore.Refresh();
    while (true) {
        Common::UniqueFunction<void> callback;
        {
            std::unique_lock lk(pending_ops_mutex);
            if (pending_ops.empty() || !work_semaphore.IsFree(pending_ops.front().gpu_tick)) break;
            callback = std::move(pending_ops.front().callback);
            pending_ops.pop(); // Retire before invoking a callback which can enqueue more work.
        }
        callback();
    }
}

void Scheduler::BeginMarker(std::string name) {
    if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBeginDebugUtilsLabelEXT) return;
    marker_stack.push_back(std::move(name));
    CommandBuffer().beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = marker_stack.back().c_str(),
    });
}

void Scheduler::EndMarker() {
    if (marker_stack.empty()) return;
    CommandBuffer().endDebugUtilsLabelEXT();
    marker_stack.pop_back();
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));
    gpu_profiler.BeginBatch(current_cmdbuf, work_semaphore.KnownGpuTick());
    for (const auto& label : marker_stack) {
        current_cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = label.c_str(),
        });
    }

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

    // The recorder is drained here; later commands replay into the new buffer.
    if (recorder) {
        recorder->SetTarget(current_cmdbuf);
        recorder->ApplyRequestedMode();
    }

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    Common::Profiler::Scope execution_scope{"Vulkan.SubmitExecution"};
    const auto generation = instance.DiagnosticGeneration();
    instance.CheckSubmissionHealth();
    if (hoisting)
        InterruptHoist();
    // Let the recording thread work on queued chunks while this thread waits below.
    EndRendering(RenderBreak::Flush);
    if (on_submit) {
        // Records the final barriers and queues pending sparse binds ahead of this command
        // buffer, which waits on them. Runs before the recorder drain so its commands land.
        Common::Profiler::Scope scope{"Vulkan.SubmitCallback"};
        on_submit(info);
    }
    if (recorder)
        recorder->Dispatch();
    if (instance.Submissions() && CurrentTick() > 8) {
        // Also bound work already accepted by Vulkan, not just the host FIFO.
        // Tick reservation alone must never make a command buffer reusable.
        Common::Profiler::Scope scope{"Vulkan.InflightBudget"};
        work_semaphore.Wait(CurrentTick() - 8);
    }
    if (recorder) {
        Common::Profiler::Scope scope{"Vulkan.RecorderDrain"};
        recorder->Sync(); // the command buffer is ended and submitted by this thread
    }
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    {
        Common::Profiler::Scope scope{"Vulkan.EndCommandBuffer"};
        EndRendering(RenderBreak::Flush);
        gpu_profiler.EndBatch(current_cmdbuf);
        for (size_t i = 0; i < marker_stack.size(); ++i)
            current_cmdbuf.endDebugUtilsLabelEXT();
        Check(current_cmdbuf.end());
    }

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    {
        Common::Profiler::Scope scope{"Vulkan.ImGuiUploads"};
        ImGui::Core::TextureManager::Submit();
    }
    // No pointers to the caller's SubmitInfo, current_cmdbuf, or stack arrays
    // escape. The scheduler owns the command pool and drains before destruction.
    const auto flow = Common::Profiler::Post("Vulkan.PostSubmission");
    auto submit = [instance_ptr = &instance, master = &work_semaphore, generation, flow,
                   signal_value, buffer = current_cmdbuf, packet = info]
                  (SubmissionReceipt& receipt) {
        Common::Profiler::Scope execution{"Vulkan.WorkerSubmit", flow};
        const vk::TimelineSemaphoreSubmitInfo timeline_si{
            .waitSemaphoreValueCount = packet.num_wait_semas,
            .pWaitSemaphoreValues = packet.wait_ticks.data(),
            .signalSemaphoreValueCount = packet.num_signal_semas,
            .pSignalSemaphoreValues = packet.signal_ticks.data(),
        };
        const vk::SubmitInfo submit_info{
            .pNext = &timeline_si,
            .waitSemaphoreCount = packet.num_wait_semas,
            .pWaitSemaphores = packet.wait_semas.data(),
            .pWaitDstStageMask = packet.wait_stages.data(),
            .commandBufferCount = 1,
            .pCommandBuffers = &buffer,
            .signalSemaphoreCount = packet.num_signal_semas,
            .pSignalSemaphores = packet.signal_semas.data(),
        };
        vk::Result result;
        {
            std::unique_lock lock{instance_ptr->QueueMutex(), std::defer_lock};
            {
                Core::Diagnostics::Handoff::Scope scope{"Vulkan.SubmitLock", generation,
                    master->DiagnosticId(), signal_value, true};
                lock.lock();
            }
            Core::Diagnostics::Handoff::Scope scope{"Vulkan.Submit", generation,
                master->DiagnosticId(), signal_value};
            receipt.started_ns = Core::Diagnostics::DiagnosticNowNs();
            result = instance_ptr->GetGraphicsQueue().submit(submit_info, packet.fence);
            SHAD_HANDOFF(generation, "vk_submit", master->DiagnosticId(), signal_value,
                         static_cast<u64>(result));
        }
        // The worker's poison path must see real driver failures as well as
        // injected exceptions; a process-wide assertion bypasses its waiters.
        if (result != vk::Result::eSuccess) {
            // Only cached CPU metadata here: a failed device must not be queried
            // or waited on while preserving the original submission exception.
            try {
                LOG_ERROR(Render_Vulkan,
                          "GPU_SUBMIT_FAILED result={} generation={} scheduler={} tick={} "
                          "cached_retired={} host_serial={} elapsed_ns={} waits={} signals={}",
                          vk::to_string(result), generation, master->DiagnosticId(), signal_value,
                          master->KnownGpuTick(), receipt.serial,
                          Core::Diagnostics::DiagnosticNowNs() - receipt.started_ns,
                          packet.num_wait_semas, packet.num_signal_semas);
            } catch (...) {
                // Diagnostic allocation/logging failure must not mask device loss.
            }
            throw std::runtime_error("vkQueueSubmit failed: " + vk::to_string(result));
        }
        master->Submitted(signal_value);
        if (const auto& diag = instance_ptr->Diagnostics()) {
            diag->MarkAvailable(Core::Diagnostics::AdvanceSignal::QueueSubmit, true);
            diag->Advance(Core::Diagnostics::AdvanceSignal::QueueSubmit,
                          Core::Diagnostics::DiagnosticNowNs());
        }
    };
    if (auto* worker = instance.Submissions()) {
        Common::Profiler::Scope scope{"Vulkan.EnqueueSubmission"};
        auto receipt = worker->Enqueue(std::move(submit));
        last_submission = receipt->serial;
        SHAD_HANDOFF(generation, "submit_accepted", work_semaphore.DiagnosticId(),
                     signal_value, last_submission);
        gpu_profiler.Queued(signal_value, std::move(receipt));
    } else {
        SubmissionReceipt receipt;
        gpu_profiler.Submitting();
        submit(receipt);
        gpu_profiler.Submitted(signal_value);
    }

    {
        Common::Profiler::Scope scope{"Vulkan.ReshapeStatus"};
        instance.GpuReshapeAdapter().PublishStatus();
    }
    {
        Common::Profiler::Scope scope{"Vulkan.RefreshTimeline"};
        work_semaphore.Refresh();
    }
    {
        Common::Profiler::Scope scope{"Vulkan.NextCommandBuffer"};
        AllocateWorkerCommandBuffers();
    }

    // Apply pending operations
    {
        Common::Profiler::Scope scope{"Vulkan.RetireOperations"};
        PopPendingOperations();
    }
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        if (!work_semaphore.Wait(op.gpu_tick, stoken) || stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const RecordingCommandBuffer& cmdbuf) {
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
                                stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                stencil_front_ops.compare_op);
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                    stencil_back_ops.compare_op);
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
}

} // namespace Vulkan
