// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include <boost/container/static_vector.hpp>

#include "common/profiler.h"

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

std::unique_lock<std::mutex> SerializePipelineCreate(const Instance& instance) {
    static std::mutex mutex;
    if (instance.GetDriverID() == vk::DriverId::eQualcommProprietary) {
        return std::unique_lock{mutex};
    }
    return {};
}

void Pipeline::Bind(const RecordingCommandBuffer& cmdbuf, vk::PipelineBindPoint point) const {
    if (first_use_rank == 0) {
        first_use_rank = PipelineStats::NextUseRank();
        first_use_ms = PipelineStats::SessionMs();
    }
    auto& bound = scheduler.GetDynamicState()
                      .bound_pipeline[point == vk::PipelineBindPoint::eCompute ? 1 : 0];
    if (bound == this && PipelineStats::SkipRepeatedBinds()) {
        PipelineStats::RecordRepeatedBind();
        return;
    }
    bound = this;
    if (Ready()) {
        cmdbuf.bindPipeline(point, *pipeline);
        return;
    }
    // The handle is read when the command runs, never captured while it is still empty.
    cmdbuf.Custom(0, [this, point](vk::CommandBuffer c) { c.bindPipeline(point, WaitHandle()); });
}

bool Pipeline::TryClaim() const noexcept {
    auto expected = BuildState::Pending;
    return build_state.compare_exchange_strong(expected, BuildState::Building,
                                               std::memory_order_acq_rel);
}

void Pipeline::BuildClaimed() const {
    CreateNative();
    if (build_observer) {
        build_observer->OnPipelineBuilt(*this, build_hash);
    }
    ready_ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
    // Last access to this object: a waiter that sees Ready may free it (ReplaceShader), so
    // notify while it still cannot get past the lock.
    std::scoped_lock lock{build_mutex};
    build_state.store(BuildState::Ready, std::memory_order_release);
    build_cv.notify_all();
}

vk::Pipeline Pipeline::WaitHandle(bool for_bind) const {
    if (Ready()) {
        return *pipeline;
    }
    if (TryClaim()) {
        // Still queued behind other work: the first user builds it rather than waiting.
        if (for_bind) {
            PipelineStats::RecordFirstUse(PipelineStats::FirstUse::BuiltByUser, 0);
        }
        BuildClaimed();
        return *pipeline;
    }
    if (!for_bind) {
        std::unique_lock lock{build_mutex};
        build_cv.wait(lock, [this] { return Ready(); });
        return *pipeline;
    }
    Common::Profiler::Scope scope{"Pipeline.WaitReady"};
    const auto start = std::chrono::steady_clock::now();
    {
        std::unique_lock lock{build_mutex};
        build_cv.wait(lock, [this] { return Ready(); });
    }
    PipelineStats::RecordFirstUse(
        PipelineStats::FirstUse::Waited,
        u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 start)
                .count()));
    return *pipeline;
}

void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;

    scheduler.ClearBreakDetail();

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    cmdbuf.pushConstants(*pipeline_layout, stage_flags, 0u, sizeof(push_data), &push_data);

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(bind_point, *pipeline_layout, 0, set_writes);
        return;
    }

    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    cmdbuf.bindDescriptorSets(bind_point, *pipeline_layout, 0, desc_set, {});
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
