// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_missing_content.h"
#include "video_core/renderer_vulkan/vk_pipeline_stats.h"

namespace Vulkan {

PipelineCreationProbe::PipelineCreationProbe(u32 stage_count) {
    ASSERT(stage_count <= stage_feedback.size());
    feedback_ci.pPipelineCreationFeedback = &pipeline_feedback;
    // A per-stage array is optional in newer spec revisions but older validation layers require
    // it to match stageCount, so always provide it.
    feedback_ci.pipelineStageCreationFeedbackCount = stage_count;
    feedback_ci.pPipelineStageCreationFeedbacks = stage_feedback.data();
}

const void* PipelineCreationProbe::Chain(const void* next) {
    feedback_ci.pNext = next;
    return &feedback_ci;
}

void PipelineCreationProbe::Start() {
    start = std::chrono::steady_clock::now();
}

PipelineCreation PipelineCreationProbe::Finish() const {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto flags = pipeline_feedback.flags;
    const bool valid = bool(flags & vk::PipelineCreationFeedbackFlagBits::eValid);
    return {
        .ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        .driver_ns = valid ? pipeline_feedback.duration : 0,
        .feedback = valid,
        .cache_hit =
            valid && bool(flags & vk::PipelineCreationFeedbackFlagBits::eApplicationPipelineCacheHit),
    };
}

namespace PipelineStats {
namespace {

struct Timing {
    std::atomic<u64> count{};
    std::atomic<u64> ns{};
    std::atomic<u64> max_ns{};

    void Add(u64 value) {
        count.fetch_add(1, std::memory_order_relaxed);
        ns.fetch_add(value, std::memory_order_relaxed);
        u64 max = max_ns.load(std::memory_order_relaxed);
        while (value > max && !max_ns.compare_exchange_weak(max, value, std::memory_order_relaxed)) {
        }
    }
    void Reset() {
        count = 0;
        ns = 0;
        max_ns = 0;
    }
    std::string Describe() const {
        const u64 n = count.load(std::memory_order_relaxed);
        const double total_ms = double(ns.load(std::memory_order_relaxed)) / 1e6;
        return fmt::format("{} in {:.1f} ms (mean {:.2f} ms, max {:.2f} ms)", n, total_ms,
                           n ? total_ms / double(n) : 0.0,
                           double(max_ns.load(std::memory_order_relaxed)) / 1e6);
    }
};

/// Creations of one pipeline kind at runtime or during preload.
struct KindCounters {
    Timing wall;
    Timing driver; ///< Only creations with valid feedback.
    std::atomic<u64> hits{};

    void Reset() {
        wall.Reset();
        driver.Reset();
        hits = 0;
    }
    std::string Describe() const {
        return fmt::format("{}\n    driver feedback {}, cache hits {}", wall.Describe(),
                           driver.Describe(), hits.load());
    }
};

constexpr std::array KindNames = {"graphics", "compute", "tiling"};
std::array<KindCounters, 3> runtime{};
std::array<KindCounters, 3> preloaded{};
Timing modules;
std::atomic<u64> preload_ns{};
std::atomic<u64> preload_stored{};
std::atomic<u32> preload_threads{};
Timing driver_saves;
std::atomic<u64> driver_saved_bytes{};

std::atomic<u64> deferred_graphics{};
std::atomic<u64> deferred_compute{};
// Deferred pipelines (not preloaded) whose driver object is not built yet.
std::atomic<s64> deferred_pending{};
std::atomic<u64> compile_queued{};
std::atomic<u64> compile_peak{};
std::atomic<u64> compile_rejected{};
std::atomic<u64> compiled_by_worker{};
std::atomic<u64> compiled_from_backlog{};
std::atomic<u64> backlog_promoted{};
std::atomic<u32> use_rank{};
std::chrono::steady_clock::time_point session_start = std::chrono::steady_clock::now();
PreloadTiers preload_tiers{};
std::atomic<u32> usage_saved{};
std::atomic<u64> built_by_user{};
Timing first_use_waits;
std::atomic<int> mode_override{-1}; // -1 configured, otherwise CompileMode
std::atomic<int> spec_match{int(SpecMatch::Fast)};
std::atomic<bool> skip_repeated_binds{true};
std::atomic<u64> repeated_binds{};
std::atomic<u64> spec_verified{};
std::atomic<u64> spec_disagreed{};
std::atomic<u64> spec_lookups{};
std::atomic<u64> spec_runtime_checks{};
std::atomic<u64> spec_candidates{};
std::atomic<u64> spec_start_rejects{};
std::atomic<u64> spec_fetch_rejects{};
std::atomic<u64> spec_created{};
std::atomic<u32> configured_mode{u32(CompileMode::Sync)};

// Skip mode. Written by the GPU command thread only; read by DebugBus.
std::array<std::atomic<u64>, u32(SkipBlocker::Count)> skip_blocked{};
std::atomic<u64> skipped_draws{};
std::atomic<u64> held_draws{};
std::atomic<u64> skipped_pipelines{};
std::atomic<u64> ended_pipelines{};
std::atomic<u64> affected_frames{};
std::atomic<u64> longest_run{};
std::atomic<bool> skip_disabled{};
std::atomic<u32> skip_off{u32(SkipOff::None)};
std::atomic<u64> skip_paused_until{}; ///< First frame after the current pause; 0 when none.
std::atomic<u32> skip_pauses{};
std::atomic<u64> skip_last_pause{}; ///< Frame the latest pause began.
u64 last_skip_epoch = ~0ULL;
u64 current_run{};

struct SkippedTarget {
    u64 address;
    bool depth;
    u64 draws;
    u64 first_epoch;
    u64 last_epoch;
};
constexpr std::size_t MaxSkippedTargets = 256;
std::mutex targets_mutex;
std::vector<SkippedTarget> skipped_targets;

std::mutex state_mutex;
Settings settings{};
DriverCacheState driver_state{};

} // namespace

void Reset() {
    for (auto& counters : runtime) {
        counters.Reset();
    }
    for (auto& counters : preloaded) {
        counters.Reset();
    }
    modules.Reset();
    preload_ns = 0;
    preload_stored = 0;
    preload_threads = 0;
    driver_saves.Reset();
    driver_saved_bytes = 0;
    deferred_pending = 0;
    deferred_graphics = 0;
    deferred_compute = 0;
    compile_queued = 0;
    compile_peak = 0;
    compile_rejected = 0;
    compiled_by_worker = 0;
    compiled_from_backlog = 0;
    backlog_promoted = 0;
    use_rank = 0;
    session_start = std::chrono::steady_clock::now();
    usage_saved = 0;
    built_by_user = 0;
    first_use_waits.Reset();
    for (auto& counter : skip_blocked) {
        counter = 0;
    }
    skipped_draws = 0;
    held_draws = 0;
    skipped_pipelines = 0;
    ended_pipelines = 0;
    affected_frames = 0;
    longest_run = 0;
    skip_disabled = false;
    skip_off = u32(SkipOff::None);
    skip_paused_until = 0;
    skip_pauses = 0;
    skip_last_pause = 0;
    last_skip_epoch = ~0ULL;
    current_run = 0;
    {
        std::scoped_lock targets_lock{targets_mutex};
        skipped_targets.clear();
    }
    std::scoped_lock lock{state_mutex};
    settings = {};
    driver_state = {};
    preload_tiers = {};
}

void SetSettings(const Settings& value) {
    std::scoped_lock lock{state_mutex};
    settings = value;
}

void RecordModule(u64 translate_ns) {
    modules.Add(translate_ns);
    Common::Profiler::Counter("Pipeline.GuestTranslations", s64(modules.count.load()));
}

void RecordPipeline(PipelineKind kind, bool preload, const PipelineCreation& creation) {
    auto& counters = (preload ? preloaded : runtime)[u32(kind)];
    counters.wall.Add(creation.ns);
    if (creation.feedback) {
        counters.driver.Add(creation.driver_ns);
        if (creation.cache_hit) {
            counters.hits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!preload) {
        u64 created = 0;
        u64 hits = 0;
        for (const auto& c : runtime) {
            created += c.wall.count.load(std::memory_order_relaxed);
            hits += c.hits.load(std::memory_order_relaxed);
        }
        Common::Profiler::Counter("Pipeline.Created", s64(created));
        Common::Profiler::Counter("Pipeline.DriverCacheHits", s64(hits));
    }
}

void RecordPreload(u32 stored, u32 threads, u64 ns) {
    preload_ns = ns;
    preload_stored = stored;
    preload_threads = threads;
}

void LatchConfiguredCompileMode() {
    const auto name = EmulatorSettings.GetPipelineCompileMode();
    auto mode = CompileMode::Sync;
    if (name == "async_accurate") {
        mode = CompileMode::AsyncAccurate;
    } else if (name == "async_graphics_skip") {
        mode = CompileMode::AsyncGraphicsSkip;
    } else if (name != "sync") {
        LOG_WARNING(Render_Vulkan, "Unknown pipeline_compile_mode '{}', using sync", name);
    }
    configured_mode = u32(mode);
}

CompileMode ConfiguredCompileMode() {
    return CompileMode(configured_mode.load(std::memory_order_relaxed));
}

CompileMode EffectiveCompileMode() {
    const int value = mode_override.load(std::memory_order_relaxed);
    return value < 0 ? ConfiguredCompileMode() : CompileMode(value);
}

const char* CompileModeName(CompileMode mode) {
    switch (mode) {
    case CompileMode::AsyncAccurate:
        return "async_accurate";
    case CompileMode::AsyncGraphicsSkip:
        return "async_graphics_skip";
    default:
        return "sync";
    }
}

void RecordSkipBlocked(SkipBlocker reason) {
    skip_blocked[u32(reason)].fetch_add(1, std::memory_order_relaxed);
    if (reason == SkipBlocker::TableFull) {
        DisableSkipping(SkipOff::TableFull);
    }
}

void DisableSkipping(SkipOff reason) {
    u32 expected = u32(SkipOff::None);
    skip_off.compare_exchange_strong(expected, u32(reason), std::memory_order_relaxed);
    skip_disabled = true;
}

void RecordSkippedDraw(u64 epoch, bool held, bool first_for_pipeline) {
    skipped_draws.fetch_add(1, std::memory_order_relaxed);
    if (held) {
        held_draws.fetch_add(1, std::memory_order_relaxed);
    }
    if (first_for_pipeline) {
        skipped_pipelines.fetch_add(1, std::memory_order_relaxed);
    }
    if (epoch != last_skip_epoch) {
        current_run = epoch == last_skip_epoch + 1 ? current_run + 1 : 1;
        last_skip_epoch = epoch;
        affected_frames.fetch_add(1, std::memory_order_relaxed);
        if (current_run > longest_run.load(std::memory_order_relaxed)) {
            longest_run = current_run;
        }
    }
    Common::Profiler::Counter("Pipeline.SkippedDraws", s64(skipped_draws.load()));
}

bool RecordSkippedTarget(u64 address, bool depth, u64 epoch) {
    std::scoped_lock lock{targets_mutex};
    const auto it = std::ranges::find_if(skipped_targets, [&](const SkippedTarget& target) {
        return target.address == address && target.depth == depth;
    });
    if (it != skipped_targets.end()) {
        ++it->draws;
        it->last_epoch = epoch;
        return true;
    }
    if (skipped_targets.size() >= MaxSkippedTargets) {
        return false;
    }
    skipped_targets.push_back({address, depth, 1, epoch, epoch});
    return true;
}

void RecordSkipEnded(u64 draws) {
    ended_pipelines.fetch_add(1, std::memory_order_relaxed);
    (void)draws;
}

bool SkipDisabledForSession() {
    return skip_disabled.load(std::memory_order_relaxed);
}

bool PauseSkipping(u64 epoch) {
    if (skip_pauses.fetch_add(1, std::memory_order_relaxed) >= MaxSkipPauses) {
        DisableSkipping(SkipOff::CrossFrame);
        return false;
    }
    skip_last_pause.store(epoch, std::memory_order_relaxed);
    skip_paused_until.store(epoch + SkipPauseFrames + 1, std::memory_order_relaxed);
    return true;
}

bool SkippingOff(u64 epoch) {
    if (skip_disabled.load(std::memory_order_relaxed)) {
        return true;
    }
    const u64 until = skip_paused_until.load(std::memory_order_relaxed);
    if (until == 0) {
        return false;
    }
    if (epoch < until) {
        return true;
    }
    if (skip_paused_until.exchange(0, std::memory_order_relaxed) != 0) {
        LOG_INFO(Render_Vulkan, "Skipping unbuilt draws again from frame {} (pause {} of {})",
                 epoch, skip_pauses.load(std::memory_order_relaxed), MaxSkipPauses);
    }
    return false;
}

void RecordFirstUse(FirstUse kind, u64 wait_ns) {
    if (kind == FirstUse::BuiltByUser) {
        built_by_user.fetch_add(1, std::memory_order_relaxed);
    } else {
        first_use_waits.Add(wait_ns);
    }
}

void RecordCompileQueued(std::size_t depth) {
    compile_queued.fetch_add(1, std::memory_order_relaxed);
    u64 peak = compile_peak.load(std::memory_order_relaxed);
    while (depth > peak && !compile_peak.compare_exchange_weak(peak, depth)) {
    }
    Common::Profiler::Counter("Pipeline.CompileQueue", s64(depth));
}

void RecordCompileRejected() {
    compile_rejected.fetch_add(1, std::memory_order_relaxed);
}

void RecordCompiledByWorker(bool from_backlog) {
    compiled_by_worker.fetch_add(1, std::memory_order_relaxed);
    if (from_backlog) {
        compiled_from_backlog.fetch_add(1, std::memory_order_relaxed);
    }
}

void RecordBacklogPromoted() {
    backlog_promoted.fetch_add(1, std::memory_order_relaxed);
}

u32 NextUseRank() {
    return use_rank.fetch_add(1, std::memory_order_relaxed) + 1;
}

u32 UsedPipelines() {
    return use_rank.load(std::memory_order_relaxed);
}

u32 SessionMs() {
    return u32(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - session_start)
                   .count());
}

void RecordPreloadTiers(const PreloadTiers& tiers) {
    std::scoped_lock lock{state_mutex};
    preload_tiers = tiers;
}

void RecordUsageSaved(u32 entries) {
    usage_saved = entries;
}

void RecordDeferred(PipelineKind kind) {
    (kind == PipelineKind::Compute ? deferred_compute : deferred_graphics)
        .fetch_add(1, std::memory_order_relaxed);
    deferred_pending.fetch_add(1, std::memory_order_relaxed);
}

void RecordDeferredDone() {
    deferred_pending.fetch_sub(1, std::memory_order_relaxed);
}

u32 PendingBuilds() {
    return static_cast<u32>(std::max<s64>(0, deferred_pending.load(std::memory_order_relaxed)));
}

void SetDriverCacheState(DriverCacheState state) {
    std::scoped_lock lock{state_mutex};
    driver_state = std::move(state);
}

void RecordDriverCacheSave(u64 bytes, u64 ns) {
    driver_saves.Add(ns);
    driver_saved_bytes = bytes;
}

constexpr std::array SkipOffNames = {"", "a record is full", "missing content reached a CPU readback",
                                     "missing content was used as indirect arguments",
                                     "stale missing content was read too often"};

bool SkipRepeatedBinds() {
    return skip_repeated_binds.load(std::memory_order_relaxed);
}

void RecordRepeatedBind() {
    repeated_binds.fetch_add(1, std::memory_order_relaxed);
}

SpecMatch SpecMatchMode() {
    return SpecMatch(spec_match.load(std::memory_order_relaxed));
}

void RecordSpecVerify(bool agree, u64 program_hash, u32 permutation, bool fast_result) {
    spec_verified.fetch_add(1, std::memory_order_relaxed);
    if (agree) {
        return;
    }
    const u64 count = spec_disagreed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 16) {
        LOG_ERROR(Render_Vulkan,
                  "Permutation match disagrees for shader {:#x} permutation {}: fast {}, full {}",
                  program_hash, permutation, fast_result, !fast_result);
    }
}

void RecordSpecLookup(u32 runtime_checks, u32 candidates, u32 start_rejects, u32 fetch_rejects,
                      bool created) {
    spec_lookups.fetch_add(1, std::memory_order_relaxed);
    spec_runtime_checks.fetch_add(runtime_checks, std::memory_order_relaxed);
    spec_candidates.fetch_add(candidates, std::memory_order_relaxed);
    spec_start_rejects.fetch_add(start_rejects, std::memory_order_relaxed);
    spec_fetch_rejects.fetch_add(fetch_rejects, std::memory_order_relaxed);
    if (created) {
        spec_created.fetch_add(1, std::memory_order_relaxed);
    }
}

std::string Command(const std::vector<std::string>& args) {
    if (args.size() == 2 && args[0] == "bind_skip") {
        if (args[1] != "on" && args[1] != "off") {
            return "usage: pipeline_cache bind_skip on|off\n";
        }
        skip_repeated_binds = args[1] == "on";
        return fmt::format("binding the pipeline already bound is {}\n",
                           args[1] == "on" ? "left out" : "recorded");
    }
    if (args.size() == 2 && args[0] == "spec_match") {
        if (args[1] == "fast") {
            spec_match = int(SpecMatch::Fast);
        } else if (args[1] == "full") {
            spec_match = int(SpecMatch::Full);
        } else if (args[1] == "verify") {
            spec_match = int(SpecMatch::Verify);
        } else {
            return "usage: pipeline_cache spec_match fast|full|verify\n";
        }
        return fmt::format("permutation match {}\n", args[1]);
    }
    if (args.size() == 2 && args[0] == "mode") {
        if (args[1] == "sync") {
            mode_override = int(CompileMode::Sync);
        } else if (args[1] == "async_accurate") {
            mode_override = int(CompileMode::AsyncAccurate);
        } else if (args[1] == "async_graphics_skip") {
            mode_override = int(CompileMode::AsyncGraphicsSkip);
        } else if (args[1] == "config") {
            mode_override = -1;
        } else {
            return "usage: pipeline_cache mode sync|async_accurate|async_graphics_skip|config\n";
        }
        return fmt::format("compile mode {} for pipelines created from now on\n",
                           CompileModeName(EffectiveCompileMode()));
    }
    if (!args.empty() && (args.size() != 1 || args[0] != "status")) {
        return "usage: pipeline_cache [status] | mode sync|async_accurate|async_graphics_skip|"
               "config | spec_match fast|full|verify | bind_skip on|off\n";
    }
    Settings current;
    DriverCacheState state;
    {
        std::scoped_lock lock{state_mutex};
        current = settings;
        state = driver_state;
    }
    std::string out;
    out += fmt::format("settings: recipe cache {} (store {}), driver cache {}\n",
                       current.recipe_cache ? "on" : "off",
                       current.recipe_store ? "open" : "closed",
                       current.driver_cache ? "on" : "off");
    const int override_mode = mode_override.load();
    static constexpr std::array SpecMatchNames = {"fast", "full", "verify"};
    out += fmt::format("permutation match: {}; verified {} candidate comparisons, {} disagreed\n",
                       SpecMatchNames[std::clamp(spec_match.load(), 0, 2)], spec_verified.load(),
                       spec_disagreed.load());
    if (const u64 lookups = spec_lookups.load()) {
        out += fmt::format("permutation lookups (verify mode): {}; per lookup, runtime info "
                           "compared {:.2f}, resources compared {:.2f}, of which {:.2f} have "
                           "another bindings start and {:.2f} another fetch shader; {} new\n",
                           lookups, double(spec_runtime_checks.load()) / lookups,
                           double(spec_candidates.load()) / lookups,
                           double(spec_start_rejects.load()) / lookups,
                           double(spec_fetch_rejects.load()) / lookups, spec_created.load());
    }
    out += fmt::format("repeated pipeline binds left out: {} (bind_skip {})\n",
                       repeated_binds.load(), skip_repeated_binds.load() ? "on" : "off");
    out += fmt::format("compile mode: {} (configured {}{}), {} compile threads\n",
                       CompileModeName(EffectiveCompileMode()),
                       CompileModeName(ConfiguredCompileMode()),
                       override_mode < 0 ? "" : ", overridden", current.compile_threads);
    out += fmt::format("  deferred {} graphics + {} compute; queued {} (peak depth {}), "
                       "queue full {}; built by workers {}, by first user {}\n",
                       deferred_graphics.load(), deferred_compute.load(), compile_queued.load(),
                       compile_peak.load(), compile_rejected.load(), compiled_by_worker.load(),
                       built_by_user.load());
    out += fmt::format("  binds that waited for a build: {}\n", first_use_waits.Describe());
    out += fmt::format("  skipped draws (lossy): {} ({} held after their pipeline was built) of {} "
                       "pipelines, {} resumed; {} frames affected, longest run {} frames{}\n",
                       skipped_draws.load(), held_draws.load(), skipped_pipelines.load(),
                       ended_pipelines.load(), affected_frames.load(), longest_run.load(),
                       skip_disabled.load()
                           ? fmt::format("; skipping off for the session ({})",
                                         SkipOffNames[std::min<u32>(skip_off.load(), 4)])
                           : std::string{});
    if (const u32 pauses = skip_pauses.load()) {
        const u64 until = skip_paused_until.load();
        out += fmt::format("  skipping paused {} times (of {}) for {} frames after stale missing "
                           "content was read; latest from frame {}{}\n",
                           std::min(pauses, MaxSkipPauses), MaxSkipPauses, SkipPauseFrames,
                           skip_last_pause.load(),
                           until ? fmt::format(", paused until frame {}", until) : std::string{});
    }
    static constexpr std::array BlockerNames = {"none",     "side effects", "predicated",
                                                "stream-out", "clear/meta", "skipped too long",
                                                "skipping off", "skipping paused"};
    out += "  unbuilt draws kept instead of skipped:";
    for (u32 i = 1; i < u32(SkipBlocker::Count); ++i) {
        out += fmt::format(" {} {}{}", BlockerNames[i], skip_blocked[i].load(),
                           i + 1 < u32(SkipBlocker::Count) ? "," : "\n");
    }
    {
        std::scoped_lock targets_lock{targets_mutex};
        if (!skipped_targets.empty()) {
            auto sorted = skipped_targets;
            std::ranges::sort(sorted, std::greater{}, &SkippedTarget::draws);
            out += fmt::format("  attachments missing content from skipped draws ({} recorded, "
                               "by draws):\n",
                               sorted.size());
            for (std::size_t i = 0; i < std::min<std::size_t>(sorted.size(), 16); ++i) {
                const auto& t = sorted[i];
                out += fmt::format("    {} {:#x}: {} draws, frames {}..{}\n",
                                   t.depth ? "depth" : "color", t.address, t.draws,
                                   t.first_epoch, t.last_epoch);
            }
        }
    }
    MissingContent::Describe(out);
    out += fmt::format("guest shaders translated: {}\n", modules.Describe());
    for (u32 kind = 0; kind < KindNames.size(); ++kind) {
        out += fmt::format("{} pipelines created: {}\n", KindNames[kind],
                           runtime[kind].Describe());
    }
    out += fmt::format("preload: {} stored, whole preload {:.1f} ms on {} threads\n",
                       preload_stored.load(), double(preload_ns.load()) / 1e6,
                       preload_threads.load());
    PreloadTiers tiers;
    {
        std::scoped_lock lock{state_mutex};
        tiers = preload_tiers;
    }
    out += fmt::format("  {} with usage records; recipes/modules/layouts {:.1f} ms; {} built before "
                       "start in {:.1f} ms, {} left to workers ({} built so far, {} promoted "
                       "by a draw); usage saved {}; pipelines first used this session {}\n",
                       tiers.usage_entries, double(tiers.construct_ns) / 1e6, tiers.waited,
                       double(tiers.wait_ns) / 1e6, tiers.background,
                       compiled_from_backlog.load(), backlog_promoted.load(), usage_saved.load(),
                       use_rank.load());
    for (u32 kind = 0; kind < 2; ++kind) {
        out += fmt::format("  {} preloaded: {}\n", KindNames[kind], preloaded[kind].Describe());
    }
    out += fmt::format("driver cache file: {}\n", state.path.empty() ? "none" : state.path);
    if (!state.path.empty()) {
        out += fmt::format("  identity: {}\n", state.identity);
        out += fmt::format("  at startup: {} ({} bytes)\n", state.load_result, state.loaded_bytes);
        out += fmt::format("  saves: {}, last {} bytes\n", driver_saves.Describe(),
                           driver_saved_bytes.load());
    }
    return out;
}

} // namespace PipelineStats

} // namespace Vulkan
