// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_pipeline_stats.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <utility>

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;
class RecordingCommandBuffer;
class Pipeline;

/// Told when a pipeline whose driver object was deferred has been built (on whichever thread
/// built it).
class PipelineBuildObserver {
public:
    virtual void OnPipelineBuilt(const Pipeline& pipeline, u64 hash) = 0;

protected:
    ~PipelineBuildObserver() = default;
};

/// A pipeline's interface (layouts, stage infos) always exists once it is constructed. Its
/// driver object can be created right away or deferred: then the create state is prepared up
/// front, a compile worker or the first user builds it, and binds wait for it at the point the
/// command executes. Nothing is ever skipped.
class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    /// Only for a pipeline known to be built (Ready()).
    vk::Pipeline Handle() const noexcept {
        return *pipeline;
    }

    bool Ready() const noexcept {
        return build_state.load(std::memory_order_acquire) == BuildState::Ready;
    }

    /// Steady-clock time a deferred build finished (0 for pipelines built in the constructor).
    /// Valid once Ready().
    u64 ReadyNs() const noexcept {
        return ready_ns;
    }

    /// Records the bind. A pipeline that is not built yet is waited for when the command runs:
    /// on the recording thread, or right here when commands are recorded immediately.
    void Bind(const RecordingCommandBuffer& cmdbuf, vk::PipelineBindPoint point) const;

    /// Returns the driver object, building it on this thread if nobody has started yet and
    /// otherwise waiting for the thread that did. `for_bind` counts it as a first-use wait.
    vk::Pipeline WaitHandle(bool for_bind = true) const;

    /// Compile worker side: takes a pending build; false if another thread already has it.
    bool TryClaim() const noexcept;
    /// Builds a claimed pipeline, publishes it and tells the observer.
    void BuildClaimed() const;

    void SetBuildObserver(PipelineBuildObserver* observer, u64 hash,
                          bool preloaded = false) noexcept {
        build_observer = observer;
        build_hash = hash;
        from_preload = preloaded;
    }
    bool Preloaded() const noexcept {
        return from_preload;
    }

    /// Order and session time of this pipeline's first bind (0 when not bound this session).
    u32 FirstUseRank() const noexcept {
        return first_use_rank;
    }
    u32 FirstUseMs() const noexcept {
        return first_use_ms;
    }

    /// GPU command thread: true the first time it is called for a pipeline.
    bool TakePromotion() const noexcept {
        return !std::exchange(promoted, true);
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    /// Driver time of the create call and whether the VkPipelineCache already held it.
    const PipelineCreation& Creation() const noexcept {
        return creation;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    void BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;

    /// Creates the driver object from the prepared create state; any thread, exactly once.
    virtual void CreateNative() const = 0;
    /// Called at the end of a derived constructor that prepared its state but did not build.
    void MarkPending() noexcept {
        build_state.store(BuildState::Pending, std::memory_order_relaxed);
    }

    enum class BuildState : u8 { Pending, Building, Ready };

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    mutable vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;
    mutable PipelineCreation creation{};

private:
    mutable std::atomic<BuildState> build_state{BuildState::Ready};
    mutable u64 ready_ns{};
    mutable std::mutex build_mutex;
    mutable std::condition_variable build_cv;
    PipelineBuildObserver* build_observer{};
    u64 build_hash{};
    bool from_preload{};
    mutable bool promoted{};
    mutable u32 first_use_rank{};
    mutable u32 first_use_ms{};
};

/// Serializes driver pipeline creation where concurrent creates are not trusted (Qualcomm's
/// proprietary driver, as in citron). Empty lock elsewhere.
std::unique_lock<std::mutex> SerializePipelineCreate(const Instance& instance);

} // namespace Vulkan
