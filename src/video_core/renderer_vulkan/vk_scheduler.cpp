// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "core/diagnostics/pipeline_handoff.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Scheduler::Scheduler(const Instance& instance, GpuProfiler::Stage stage)
    : instance{instance}, master_semaphore{instance}, command_pool{instance, &master_semaphore}, gpu_profiler{instance, stage} {
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
        diagnostic_descriptors = std::make_unique<DescriptorHeap>(instance, &master_semaphore, sizes);
    }
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
        current_cmdbuf.pushDescriptorSetKHR(point, layout, 0, writes);
        return;
    }
    const auto set = diagnostic_descriptors->Commit(set_layout);
    std::vector<vk::WriteDescriptorSet> assigned(writes.begin(), writes.end());
    for (auto& write : assigned) write.dstSet = set;
    instance.GetDevice().updateDescriptorSets(assigned, {});
    current_cmdbuf.bindDescriptorSets(point, layout, 0, set, {});
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

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

    if (Common::Profiler::GpuTimingDetailed())
        gpu_render_zone = gpu_profiler.Begin(current_cmdbuf, GpuProfiler::Stage::RenderPass);
    current_cmdbuf.beginRendering(rendering_info);
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    current_cmdbuf.endRendering();
    gpu_profiler.End(current_cmdbuf, gpu_render_zone);
    gpu_render_zone = GpuProfiler::Invalid;
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
    gpu_profiler.Collect(master_semaphore.KnownGpuTick());
}

void Scheduler::WaitSubmitted() {
    if (auto* worker = instance.Submissions()) {
        Common::Profiler::Scope scope{"Vulkan.WaitSubmitted"};
        worker->Wait(last_submission);
    }
}

void Scheduler::Wait(u64 tick) {
    if (tick >= master_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    master_semaphore.Wait(tick);
}

void Scheduler::PopPendingOperations() {
    {
        std::lock_guard lock{priority_pending_ops_mutex};
        if (priority_error) std::rethrow_exception(priority_error);
    }
    master_semaphore.Refresh();
    while (true) {
        Common::UniqueFunction<void> callback;
        {
            std::unique_lock lk(pending_ops_mutex);
            if (pending_ops.empty() || !master_semaphore.IsFree(pending_ops.front().gpu_tick)) break;
            callback = std::move(pending_ops.front().callback);
            pending_ops.pop(); // Retire before invoking a callback which can enqueue more work.
        }
        callback();
    }
}

void Scheduler::BeginMarker(std::string name) {
    if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdBeginDebugUtilsLabelEXT) return;
    marker_stack.push_back(std::move(name));
    current_cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = marker_stack.back().c_str(),
    });
}

void Scheduler::EndMarker() {
    if (marker_stack.empty()) return;
    current_cmdbuf.endDebugUtilsLabelEXT();
    marker_stack.pop_back();
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));
    gpu_profiler.BeginBatch(current_cmdbuf, master_semaphore.KnownGpuTick());
    for (const auto& label : marker_stack) {
        current_cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = label.c_str(),
        });
    }

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

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
    if (instance.Submissions() && CurrentTick() > 8) {
        // Also bound work already accepted by Vulkan, not just the host FIFO.
        // Tick reservation alone must never make a command buffer reusable.
        Common::Profiler::Scope scope{"Vulkan.InflightBudget"};
        master_semaphore.Wait(CurrentTick() - 8);
    }
    const u64 signal_value = master_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    {
        Common::Profiler::Scope scope{"Vulkan.EndCommandBuffer"};
        EndRendering();
        gpu_profiler.EndBatch(current_cmdbuf);
        for (size_t i = 0; i < marker_stack.size(); ++i)
            current_cmdbuf.endDebugUtilsLabelEXT();
        Check(current_cmdbuf.end());
    }

    const vk::Semaphore timeline = master_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    {
        Common::Profiler::Scope scope{"Vulkan.ImGuiUploads"};
        ImGui::Core::TextureManager::Submit();
    }
    // No pointers to the caller's SubmitInfo, current_cmdbuf, or stack arrays
    // escape. The scheduler owns the command pool and drains before destruction.
    auto submit = [instance_ptr = &instance, master = &master_semaphore, generation,
                   signal_value, buffer = current_cmdbuf, packet = info]
                  (SubmissionReceipt& receipt) {
        Common::Profiler::Scope execution{"Vulkan.WorkerSubmit"};
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
        if (result != vk::Result::eSuccess)
            throw std::runtime_error("vkQueueSubmit failed: " + vk::to_string(result));
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
        SHAD_HANDOFF(generation, "submit_accepted", master_semaphore.DiagnosticId(),
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
        master_semaphore.Refresh();
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

        if (!master_semaphore.Wait(op.gpu_tick, stoken) || stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
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
