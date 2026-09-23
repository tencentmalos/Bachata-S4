// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Common::Profiler {
// Detail stages (RenderPass and later) exist only while gpu_timing detail is on: guest
// render passes, guest compute dispatches, host image/copy transfers and per-draw
// buffer uploads. Batch time minus their union is barrier/wait/unattributed residue.
enum class GpuStage : uint8_t { DrawBatch, Guest, Prepare, Fsr, PostProcess, Present, Redraw, Flip, GuestFrame, RenderPass, Dispatch, Transfer, BufferUpload, Readback, Count };
inline constexpr std::array<const char*, size_t(GpuStage::Count)> GpuNames{
    "GPU.DrawBatch", "GPU.GuestCommands", "GPU.HostPrepare", "GPU.FSR", "GPU.PostProcess",
    "GPU.Present", "GPU.OverlayRedraw", "GPU.BlankFlip", "GPU.GuestFrame", "GPU.GuestRenderPass",
    "GPU.GuestDispatch", "GPU.HostTransfer", "GPU.BufferUpload", "GPU.HostReadback"};
// Elapsed device-clock interval, not GPU utilization. Reject ambiguous half-wraps.
inline std::optional<double> GpuElapsedNs(uint64_t begin, uint64_t end, uint8_t bits, double period) {
    if (!bits || bits > 64 || !std::isfinite(period) || period <= 0) return {};
    const auto mask = bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
    const auto delta = (end - begin) & mask;
    if (delta > (mask >> 1)) return {};
    const double ns = double(delta) * period;
    // Counter wire values are signed nanoseconds. double(INT64_MAX) rounds up,
    // so equality is also outside the safely representable conversion range.
    if (!std::isfinite(ns) || ns >= double(INT64_MAX)) return {};
    return ns;
}
inline bool GpuTimestampWritten(uint64_t tick) {
    // Adreno can report VK_SUCCESS for a partially unwritten pair. Match the
    // profiler SDK bridge's sentinel checks in addition to Vulkan availability.
    return tick != 0 && tick != UINT64_MAX;
}
struct GpuTimingSnapshot {
    struct Sample { uint64_t count{}, observed_ns{}; double last_ms{}, total_ms{}, max_ms{}; };
    uint64_t generation{}, retired_batches{}, dropped_batches{}, dropped_zones{}, errors{}, discarded{}, calibration_deviation_ns{};
    uint32_t pending{};
    uint8_t valid_bits{};
    double period{};
    bool supported{}, calibrated{}, estimated_alignment{};
    std::array<Sample, size_t(GpuStage::Count)> stages{};
    std::array<float, 120> guest_history{};
    uint64_t guest_history_count{};
};
struct GpuTimingState {
    std::mutex mutex;
    GpuTimingSnapshot snapshot;
    GpuTimingSnapshot Read() { std::lock_guard lock{mutex}; return snapshot; }
};
bool GpuTimingEnabled() noexcept;
bool GpuTimingDetailed() noexcept;
uint64_t Generation() noexcept;
void RegisterGpuTimingState(const std::shared_ptr<GpuTimingState>& state);
std::string GpuTimingControl(const std::vector<std::string>& args);
uint8_t GpuContext(int64_t cpu_ns, uint64_t gpu_tick, double period, uint8_t bits, const char* name) noexcept;
void GpuZone(uint64_t generation, uint8_t ctx, uint32_t slot, const char* name,
             uint64_t begin, uint64_t end) noexcept;
}
