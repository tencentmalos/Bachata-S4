// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "common/profiler.h"
#include "common/logging/log.h"
#include <chrono>
#include <limits>
#include <ctime>

namespace Vulkan {
using namespace Common::Profiler;
namespace {
uint64_t Now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
const char* Lane(GpuStage stage) {
    return stage == GpuStage::DrawBatch ? "Vulkan queue 0 / guest+prepare" :
           stage == GpuStage::Present ? "Vulkan queue 0 / present+redraw" : "Vulkan queue 0 / blank";
}
}
GpuProfiler::GpuProfiler(const Instance& instance, Stage stage)
    : instance{instance}, stats{instance.GpuTiming()}, stage{stage} {
    std::lock_guard lock{stats->mutex};
    stats->snapshot.generation = instance.DiagnosticGeneration();
    RegisterGpuTimingState(stats);
}
bool GpuProfiler::Initialize() {
    if (attempted) return bool(pool) && !disabled;
    attempted = true;
    const auto physical = instance.GetPhysicalDevice();
    const auto families = physical.getQueueFamilyProperties();
    bits = families[instance.GetGraphicsQueueFamilyIndex()].timestampValidBits;
    period = physical.getProperties().limits.timestampPeriod;
    if (bits && bits <= 64 && std::isfinite(period) && period > 0) {
        auto result = instance.GetDevice().createQueryPoolUnique({
            .queryType = vk::QueryType::eTimestamp, .queryCount = BatchCount * ZonesPerBatch * 2});
        if (result.result == vk::Result::eSuccess) pool = std::move(result.value);
    }
    { std::lock_guard lock{stats->mutex};
      stats->snapshot.supported = bool(pool); stats->snapshot.period = period; stats->snapshot.valid_bits = bits;
      if (!pool) ++stats->snapshot.errors; }
    LOG_INFO(Render_Vulkan, "Litep GPU timestamp pool: supported={} period={} bits={}", bool(pool), period, bits);
    return bool(pool);
}
void GpuProfiler::Calibrate() {
    if (calibration_attempted) return;
    calibration_attempted = true;
    if (!instance.HasCalibratedTimestamps()) {
        LOG_INFO(Render_Vulkan, "Litep GPU calibration: extension unavailable"); return;
    }
#if defined(__linux__)
    const auto domains = instance.GetPhysicalDevice().getCalibrateableTimeDomainsEXT();
    if (domains.result != vk::Result::eSuccess) return;
    for (auto domain : domains.value) LOG_INFO(Render_Vulkan, "Litep GPU time domain: {}", vk::to_string(domain));
    auto has = [&](auto value) { return std::find(domains.value.begin(), domains.value.end(), value) != domains.value.end(); };
    if (!has(vk::TimeDomainEXT::eDevice)) return;
    const bool monotonic = has(vk::TimeDomainEXT::eClockMonotonic);
    if (!monotonic && !has(vk::TimeDomainEXT::eClockMonotonicRaw)) return;
    const std::array<vk::CalibratedTimestampInfoEXT, 2> info{{
        {.timeDomain = vk::TimeDomainEXT::eDevice},
        {.timeDomain = monotonic ? vk::TimeDomainEXT::eClockMonotonic : vk::TimeDomainEXT::eClockMonotonicRaw}}};
    std::array<uint64_t, 2> values{};
    uint64_t deviation{};
    const auto result = instance.GetDevice().getCalibratedTimestampsEXT(2, info.data(), values.data(), &deviation);
    if (result != vk::Result::eSuccess || values[1] > uint64_t(INT64_MAX) ||
        !GpuTimestampWritten(values[0])) return;
    if (!monotonic) {
        // libstdc++/libc++ steady_clock uses CLOCK_MONOTONIC on Linux/Android.
        // Bridge RAW without assuming it shares that epoch. Bound the sampling
        // uncertainty; no queue submission or GPU wait is involved.
        const auto before = Now();
        timespec raw{};
        if (clock_gettime(CLOCK_MONOTONIC_RAW, &raw)) return;
        const auto after = Now();
        const int64_t raw_ns = int64_t(raw.tv_sec) * 1000000000ll + raw.tv_nsec;
        const int64_t offset = int64_t(before + (after - before) / 2) - raw_ns;
        const auto converted = static_cast<__int128>(values[1]) + offset;
        if (converted < 0 || converted > INT64_MAX) return;
        values[1] = uint64_t(converted);
        deviation += after - before;
    }
    anchor_gpu = values[0];
    context = GpuContext(int64_t(values[1]), anchor_gpu, period, bits, Lane(stage));
    std::lock_guard lock{stats->mutex};
    stats->snapshot.calibrated = context != 0;
    stats->snapshot.calibration_deviation_ns = std::max(stats->snapshot.calibration_deviation_ns, deviation);
#endif
}
void GpuProfiler::BeginBatch(vk::CommandBuffer cmd, uint64_t completed) {
    Collect(completed); // Drain previous enabled recordings even after recording is disabled.
    current = guest_zone = prepare_zone = Invalid;
    if (!GpuTimingEnabled() || disabled || !Initialize()) return;
    if (context_generation != Generation()) {
        // Like spruntime's resetSdkContext, a new recording has a fresh clock
        // epoch. Existing Vulkan leases remain owned until completion; they are
        // discarded by generation, never reset/reused while still in flight.
        context_generation = Generation(); context = 0;
        calibration_attempted = false; best_bound = UINT64_MAX;
        calibration_samples = 0; anchor_cpu = anchor_gpu = 0;
    }
    if (!context) Calibrate();
    for (uint32_t i = 0; i < BatchCount; ++i) if (!batches[i].used) { current = i; break; }
    if (current == Invalid) {
        std::lock_guard lock{stats->mutex}; ++stats->snapshot.dropped_batches; return;
    }
    batches[current] = {.generation = Generation(), .used = true};
    ++batch_serial;
    // Reset outside any rendering scope, only after the previous lease retired.
    cmd.resetQueryPool(*pool, current * ZonesPerBatch * 2, ZonesPerBatch * 2);
    Begin(cmd, stage); // Outer command-buffer interval; nested phases are non-additive.
    if (stage == Stage::DrawBatch) guest_zone = Begin(cmd, Stage::Guest);
}
uint32_t GpuProfiler::Begin(vk::CommandBuffer cmd, Stage kind) {
    if (current == Invalid) return Invalid;
    auto& b = batches[current];
    // Detail zones leave four slots for the outer batch/guest/prepare intervals.
    const bool detail = kind >= Stage::RenderPass;
    if (b.count == ZonesPerBatch || (detail && b.count >= ZonesPerBatch - 4)) {
        std::lock_guard lock{stats->mutex}; ++stats->snapshot.dropped_zones; return Invalid;
    }
    const auto index = b.count++;
    b.zones[index] = {.stage = kind};
    cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool, (current * ZonesPerBatch + index) * 2);
    return index;
}
void GpuProfiler::End(vk::CommandBuffer cmd, uint32_t zone) {
    if (current == Invalid || zone == Invalid) return;
    auto& b = batches[current];
    if (zone >= b.count || b.zones[zone].ended) return;
    cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *pool, (current * ZonesPerBatch + zone) * 2 + 1);
    b.zones[zone].ended = true;
}
void GpuProfiler::Prepare(vk::CommandBuffer cmd) {
    End(cmd, guest_zone); prepare_zone = Begin(cmd, Stage::Prepare);
}
void GpuProfiler::FrameEnd() { if (current != Invalid) batches[current].frame_end = true; }
void GpuProfiler::PresentKind(bool redraw) {
    if (current != Invalid) batches[current].zones[0].stage = redraw ? Stage::Redraw : Stage::Present;
}
void GpuProfiler::EndBatch(vk::CommandBuffer cmd) {
    if (current == Invalid) return;
    // Always close a lease recorded before a toggle, even when no longer emitting.
    for (uint32_t i = batches[current].count; i > 0; --i) End(cmd, i - 1);
}
void GpuProfiler::Submitting() { if (current != Invalid) batches[current].submit_before_ns = Now(); }
void GpuProfiler::Submitted(uint64_t tick) {
    if (current == Invalid) return;
    auto& b = batches[current]; b.tick = tick; b.submitted = true;
    std::lock_guard lock{stats->mutex}; ++stats->snapshot.pending;
}
void GpuProfiler::Queued(uint64_t tick, std::shared_ptr<SubmissionReceipt> receipt) {
    if (current == Invalid) return;
    auto& b = batches[current];
    b.tick = tick;
    b.receipt = std::move(receipt);
    std::lock_guard lock{stats->mutex}; ++stats->snapshot.pending;
}
void GpuProfiler::Collect(uint64_t completed) {
    if (!pool || disabled) return;
    Scope scope{"Vulkan.GpuQueryCollect"};
    // Process in submission order so complete guest-frame aggregation cannot reorder.
    for (uint32_t budget = 0; budget < BatchCount; ++budget) {
        uint32_t index = Invalid;
        for (uint32_t i = 0; i < BatchCount; ++i) {
            auto& b = batches[i];
            if (b.receipt && !b.submitted &&
                b.receipt->submitted.load(std::memory_order_acquire)) {
                // Worker owns only the receipt. Query leases/context stay on
                // the recording owner, including across capture generations.
                b.submit_before_ns = b.receipt->started_ns;
                b.submitted = true;
            }
            if (b.used && b.submitted && b.tick <= completed &&
                (index == Invalid || b.tick < batches[index].tick)) index = i;
        }
        if (index == Invalid) break;
        auto& b = batches[index];
        struct Result { uint64_t tick, available; };
        std::array<Result, ZonesPerBatch * 2> results{};
        const auto result = instance.GetDevice().getQueryPoolResults(*pool,
            index * ZonesPerBatch * 2, b.count * 2, sizeof(Result) * b.count * 2,
            results.data(), sizeof(Result), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if (result == vk::Result::eNotReady) break;
        if (result != vk::Result::eSuccess) {
            disabled = true;
            std::lock_guard lock{stats->mutex}; ++stats->snapshot.errors; break;
        }
        bool ready = true;
        for (uint32_t q = 0; q < b.count * 2; ++q) ready &= results[q].available != 0;
        if (!ready) break;
        bool written = true;
        for (uint32_t q = 0; q < b.count * 2; ++q) written &= GpuTimestampWritten(results[q].tick);
        if (!written) {
            if (++b.partial_retries < 30) break;
            // Bounded Adreno partial-readback recovery. Drop the batch instead
            // of inventing a zero-duration pair, and invalidate its frame sum.
            std::lock_guard lock{stats->mutex};
            --stats->snapshot.pending; ++stats->snapshot.discarded;
            ++stats->snapshot.errors; frame_incomplete = true; b = {};
            continue;
        }
        const auto now = Now();
        const bool emit = GpuTimingEnabled() && b.generation == Generation();
        if (!context && emit && b.submit_before_ns && now >= b.submit_before_ns) {
            // No calibrated timestamp extension on the selected KGSL Turnip.
            // Its end timestamp is bracketed by the CPU submit call and observed
            // completion. Pick the tightest asynchronous bracket; never wait to
            // calibrate. These lanes are explicitly approximate, with a bound.
            const auto bound = now - b.submit_before_ns;
            if (bound < best_bound) {
                best_bound = bound; anchor_cpu = b.submit_before_ns + bound / 2;
                // Anchor the BEGIN, not END. The SDK's unsigned valid-bits
                // conversion would wrap earlier begins into the distant future
                // if calibrated at the end of this same batch.
                anchor_gpu = results[0].tick;
            }
            if (best_bound <= 2000000 || ++calibration_samples >= 120) {
                const char* name = stage == Stage::DrawBatch ? "Queue 0 / guest+prepare (estimated clock)" :
                    stage == Stage::Present ? "Queue 0 / present+redraw (estimated clock)" : "Queue 0 / blank (estimated clock)";
                context = GpuContext(int64_t(anchor_cpu), anchor_gpu, period, bits, name);
                std::lock_guard lock{stats->mutex};
                stats->snapshot.estimated_alignment = context != 0;
                stats->snapshot.calibration_deviation_ns = std::max(stats->snapshot.calibration_deviation_ns, (best_bound + 1) / 2);
            }
        }
        if (last_retired_tick && b.tick != last_retired_tick + 1) frame_incomplete = true;
        last_retired_tick = b.tick;
        const bool new_frame_generation = frame_generation != b.generation;
        if (new_frame_generation) { guest_frame_ns = 0; frame_incomplete = true; frame_generation = b.generation; }
        std::lock_guard lock{stats->mutex};
        auto& s = stats->snapshot;
        --s.pending; ++s.retired_batches;
        if (!emit) ++s.discarded;
        auto sample = [&](Stage kind, double ns) {
            auto& p = s.stages[size_t(kind)]; ++p.count; p.observed_ns = now;
            p.last_ms = ns / 1e6; p.total_ms += p.last_ms; p.max_ms = std::max(p.max_ms, p.last_ms);
            Counter(GpuNames[size_t(kind)], int64_t(ns));
        };
        for (uint32_t z = 0; z < b.count; ++z) {
            const auto start = results[z * 2].tick, end = results[z * 2 + 1].tick;
            const auto ns = GpuElapsedNs(start, end, bits, period);
            if (!ns) { ++s.errors; frame_incomplete = true; continue; }
            if (b.zones[z].stage == Stage::Guest) guest_frame_ns += *ns;
            if (emit) {
                sample(b.zones[z].stage, *ns);
                if (context) Counter("GPU.ClockAlignmentUncertaintyNs", int64_t(s.calibration_deviation_ns));
                // Wire slot ids are independent of recycled Vulkan query indices.
                if (context && GpuElapsedNs(anchor_gpu, start, bits, period) && next_slot != UINT32_MAX)
                    GpuZone(b.generation, context, next_slot++, GpuNames[size_t(b.zones[z].stage)], start, end);
            }
        }
        if (b.frame_end) {
            if (emit && !frame_incomplete) {
                sample(Stage::GuestFrame, guest_frame_ns);
                s.guest_history[s.guest_history_count++ % s.guest_history.size()] = float(guest_frame_ns / 1e6);
            }
            guest_frame_ns = 0; frame_incomplete = false;
        }
        b = {}; // Availability was consumed before this slot becomes reusable.
    }
}
}
