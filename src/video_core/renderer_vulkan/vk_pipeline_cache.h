// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <unordered_map>
#include <variant>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_driver_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_compiler.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
struct Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module{};
        // Pipeline objects and specialization keys retain pointers into this object.
        // Own one stable copy per permutation, including after vector growth/preload.
        std::unique_ptr<Shader::Info> info;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;
    ModuleList modules{};

    void AddPermut(vk::ShaderModule module, std::unique_ptr<Shader::Info> info,
                   Shader::StageSpecialization&& spec) {
        spec.info = info.get();
        modules.emplace_back(module, std::move(info), std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, std::unique_ptr<Shader::Info> info,
                      Shader::StageSpecialization&& spec, size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1));
        ASSERT(!modules[perm_idx].info); // Existing pipelines may retain that pointer.
        spec.info = info.get();
        modules[perm_idx] = {module, std::move(info), std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

class PipelineCache final : public PipelineBuildObserver {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    /// A stored pipeline read back from disk; its driver object is built later, possibly on
    /// another thread (construction only reads the loaded shader infos and modules).
    struct PreloadJob {
        bool compute{};
        GraphicsPipelineKey graphics_key{};
        ComputePipelineKey compute_key{};
        GraphicsPipeline::SerializationSupport graphics_data{};
        ComputePipeline::SerializationSupport compute_data{};
        std::array<const Shader::Info*, MaxShaderStages> infos{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
        std::unique_ptr<GraphicsPipeline> graphics{};
        std::unique_ptr<ComputePipeline> compute_pipeline{};
    };

    bool LoadComputePipeline(Serialization::Archive& ar, PreloadJob& job);
    bool LoadGraphicsPipeline(Serialization::Archive& ar, PreloadJob& job);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    /// Why the last GetGraphicsPipeline returned null (for capture labels of dropped draws).
    const char* RejectReason() const noexcept {
        return reject_reason ? reject_reason : "pipeline unavailable";
    }

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {});

    const ComputePipeline* GetComputePipeline();

    /// Translates the bound compute shader and prepares its pipeline key without creating the
    /// pipeline, so a dispatch an HLE path replaces never builds one. Null if unsupported.
    const Shader::Info* PrepareComputeProgram();
    /// The pipeline for the key of the last PrepareComputeProgram.
    const ComputePipeline* GetPreparedComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(Shader::HwStage stage, Shader::SwStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

    /// The persistent VkPipelineCache shared with host pipelines (tiling).
    DriverPipelineCache& GetDriverCache() {
        return *driver_cache;
    }

private:
    bool RefreshGraphicsKey();
    bool RefreshGraphicsStages();
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(Shader::HwStage stage, Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

    /// Logs and counts one created pipeline.
    void NotePipeline(const Pipeline& pipeline, u64 hash, bool preload);
    /// A deferred pipeline was built (compile worker or first user).
    void OnPipelineBuilt(const Pipeline& pipeline, u64 hash) override;
    /// Accurate asynchronous compilation is selected and workers exist.
    bool DeferBuilds() const;
    /// Hands a pipeline constructed with defer_build to the compile workers.
    void SubmitBuild(Pipeline& pipeline, u64 hash);
    /// A draw or dispatch needs a pipeline that is still waiting in the preload backlog.
    void PromoteIfPending(const Pipeline& pipeline);

    /// Pipelines recent sessions used: first-use order and time, sessions since, compile cost.
    struct UsageRecord {
        u32 rank{~0U};
        u32 first_ms{~0U};
        u32 idle_sessions{};
        u32 compile_us{};
    };
    static u64 UsageKey(bool compute, u64 hash) {
        return compute ? ~hash : hash;
    }
    void LoadUsage();
    /// Merges this session's first uses into the loaded records and stores them.
    void SaveUsage();
    void MaybeCheckpointUsage();
    /// Creates the driver pipelines of preload jobs on worker threads.
    /// Returns the number of threads used.
    u32 BuildPreloaded(std::vector<PreloadJob>& jobs);

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    std::unique_ptr<DriverPipelineCache> driver_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    DrawIndirectParams draw_indirect_params{};
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    // Declared after the pipelines: destroyed (workers joined) before them.
    std::unique_ptr<PipelineCompiler> compiler;
    std::unordered_map<u64, UsageRecord> usage;
    u32 usage_checkpoint_calls{};
    u32 usage_checkpoint_rank{};
    std::chrono::steady_clock::time_point usage_checkpoint_time{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    const char* reject_reason{}; // why the last graphics key could not be built
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

} // namespace Vulkan
