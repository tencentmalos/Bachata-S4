// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/// One vkCreate*Pipelines call: API wall time, and what the driver reported through
/// VkPipelineCreationFeedback (only meaningful when `feedback` is set).
struct PipelineCreation {
    u64 ns{};         ///< Wall time of the create call.
    u64 driver_ns{};  ///< Feedback duration.
    bool feedback{};  ///< The driver set VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT.
    bool cache_hit{}; ///< APPLICATION_PIPELINE_CACHE_HIT: no shader was compiled.
};

/// Chains VkPipelineCreationFeedbackCreateInfo (core in Vulkan 1.3) in front of a create info's
/// pNext chain and times the create call.
class PipelineCreationProbe {
public:
    explicit PipelineCreationProbe(u32 stage_count);

    /// Returns the pNext to put in the create info; `next` is its previous pNext.
    const void* Chain(const void* next);
    void Start();
    PipelineCreation Finish() const;

private:
    vk::PipelineCreationFeedback pipeline_feedback{};
    std::array<vk::PipelineCreationFeedback, 6> stage_feedback{};
    vk::PipelineCreationFeedbackCreateInfo feedback_ci{};
    std::chrono::steady_clock::time_point start{};
};

enum class PipelineKind : u32 { Graphics, Compute, Tiling };

/// Session counters behind DebugBus `pipeline_cache` and the Pipeline.* profiler counters.
namespace PipelineStats {

struct Settings {
    bool recipe_cache{};  ///< Guest shader/pipeline recipes on disk (pipeline_cache_enabled).
    bool recipe_store{};  ///< The recipe store actually opened for this title.
    bool driver_cache{};  ///< Driver VkPipelineCache file (driver_pipeline_cache).
    u32 compile_threads{};
};

/// How new pipelines get their driver object. The configured mode can be overridden at
/// runtime (DebugBus `pipeline_cache mode`); the change applies to pipelines created after it,
/// and for AsyncGraphicsSkip to draws recorded after it.
///   Sync: created on the GPU command thread before the draw continues.
///   AsyncAccurate: created by compile workers; binds wait where the command executes.
///   AsyncGraphicsSkip: as AsyncAccurate, but eligible direct draws whose pipeline is not built
///     yet are dropped (lossy: objects, shadows or effects can be missing for a few frames).
enum class CompileMode : u32 { Sync, AsyncAccurate, AsyncGraphicsSkip };
void LatchConfiguredCompileMode();   ///< Reads Vulkan.pipeline_compile_mode (session start).
CompileMode ConfiguredCompileMode(); ///< The latched configured mode.
CompileMode EffectiveCompileMode();  ///< Override if set, else configured. Cheap.
const char* CompileModeName(CompileMode mode);

/// Why a draw whose pipeline was not built was drawn (after waiting) instead of skipped.
enum class SkipBlocker : u32 {
    None,
    SideEffects, ///< A stage writes buffers/images, uses atomics, or its translation failed.
    Predicated,  ///< Draw packet under predication.
    StreamOut,   ///< Stream-out enabled.
    MetaOp,      ///< Depth/stencil clear or copy, or a colour meta operation.
    TooLong,     ///< This pipeline has been skipped for too long; wait instead.
    TableFull,   ///< Skipping is off for the session (see SkipOff).
    Paused,      ///< Skipping is paused (see PauseSkipping).
    Count,
};
/// Why skipping was turned off for the session.
enum class SkipOff : u32 {
    None,
    TableFull,  ///< A record of affected targets or missing content is full.
    Readback,   ///< Content missing from skipped draws reached a CPU readback.
    Indirect,   ///< ... was used as indirect draw/dispatch arguments.
    CrossFrame, ///< ... was read stale in a later frame, MaxSkipPauses times.
};
void DisableSkipping(SkipOff reason);
/// Frames a pause lasts, and how many pauses a session gets before skipping is off for good.
constexpr u64 SkipPauseFrames = 60;
constexpr u32 MaxSkipPauses = 16;
/// Stale content missing from skipped draws was read (see MissingContent): draws wait for their
/// pipelines for the next SkipPauseFrames frames before more draws are dropped. False when the
/// pauses are used up; skipping is then off for the session (SkipOff::CrossFrame).
bool PauseSkipping(u64 epoch);
/// True while skipping is off for the session, or paused in frame `epoch`. A pause whose last
/// frame has passed ends here.
bool SkippingOff(u64 epoch);
void RecordSkipBlocked(SkipBlocker reason);
/// A dropped draw; `held` when its pipeline was already built but had been skipped earlier in
/// the same frame; `first_for_pipeline` for the first draw a pipeline loses.
void RecordSkippedDraw(u64 epoch, bool held, bool first_for_pipeline);
/// Records an attachment a dropped draw would have written. False when the table is full.
bool RecordSkippedTarget(u64 address, bool depth, u64 epoch);
/// A pipeline that had been skipped was drawn again.
void RecordSkipEnded(u64 skipped_draws);
bool SkipDisabledForSession();

/// What a bind found when it needed a deferred pipeline.
enum class FirstUse { Waited, BuiltByUser };
void RecordFirstUse(FirstUse kind, u64 wait_ns);
void RecordCompileQueued(std::size_t depth);
void RecordCompileRejected();
void RecordCompiledByWorker(bool from_backlog);
void RecordBacklogPromoted();

/// First binds of pipelines in this session: order and time since the pipeline cache was
/// created (the preload priority of the next session).
u32 NextUseRank();
/// Pipelines bound so far this session.
u32 UsedPipelines();
u32 SessionMs();

struct PreloadTiers {
    u32 usage_entries{};   ///< Pipelines with a usage record from earlier sessions.
    u32 waited{};          ///< Built before the game started (within the wait budget).
    u32 background{};      ///< Left to the compile workers.
    u64 wait_ns{};
    u64 construct_ns{};    ///< Reading recipes, modules and layouts.
};
void RecordPreloadTiers(const PreloadTiers& tiers);
void RecordUsageSaved(u32 entries);
void RecordDeferred(PipelineKind kind);
/// A pipeline counted by RecordDeferred got its driver object, or was destroyed without one.
void RecordDeferredDone();
/// Deferred pipelines still waiting for their driver object (the "Building" status bar label).
u32 PendingBuilds();

void Reset();
void SetSettings(const Settings& settings);
void RecordModule(u64 translate_ns);
void RecordPipeline(PipelineKind kind, bool preload, const PipelineCreation& creation);
void RecordPreload(u32 stored, u32 threads, u64 ns);

struct DriverCacheState {
    std::string path{};
    std::string identity{};    ///< Vendor/device/driver version/UUID the file is valid for.
    std::string load_result{}; ///< What happened to the stored blob at startup.
    u64 loaded_bytes{};
};
void SetDriverCacheState(DriverCacheState state);
void RecordDriverCacheSave(u64 bytes, u64 ns);

/// How a draw finds the shader permutation matching its resources (DebugBus
/// `pipeline_cache spec_match`):
///   Fast: StageSpecialization::Matches compares each candidate as it reads the resources.
///   Full: builds the draw's StageSpecialization and compares it (the previous way).
///   Verify: both, counting draws where they disagree.
enum class SpecMatch : int { Fast, Full, Verify };
SpecMatch SpecMatchMode(); ///< Cheap.
/// Verify mode: one candidate comparison; `agree` false when Fast and Full differed.
void RecordSpecVerify(bool agree, u64 program_hash, u32 permutation, bool fast_result);
/// Verify mode: one permutation lookup. `runtime_checks`: candidates whose runtime info was
/// compared; `candidates`: candidates whose resources were compared; `start_rejects` and
/// `fetch_rejects`: those of them the fast mode rejects on the bindings start or on the vertex
/// fetch shader before reading resources; `created`: none matched.
void RecordSpecLookup(u32 runtime_checks, u32 candidates, u32 start_rejects, u32 fetch_rejects,
                      bool created);

std::string Command(const std::vector<std::string>& args);

} // namespace PipelineStats

} // namespace Vulkan
