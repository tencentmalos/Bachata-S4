// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/profiler.h"
#include <charconv>
#include <mutex>
#include <chrono>
#include <thread>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#ifdef SHADPS4_PROFILER_RING
#include "common/path_util.h"
#include <spatial/core/utils/LiteTrace.h>
#endif

namespace Common::Profiler {
Scope::Scope(const char* name) noexcept {
#ifdef SHADPS4_PROFILER_RING
    if (spatial::ProfilerRing::Enabled()) {
        generation = spatial::ProfilerRing::Generation();
        spatial::LiteTrace::begin(name);
        active = true;
    }
#endif
}
Scope::~Scope() {
#ifdef SHADPS4_PROFILER_RING
    if (active && generation == spatial::ProfilerRing::Generation()) spatial::LiteTrace::end();
#endif
}
Phase::Phase(const char* name) noexcept {
#ifdef SHADPS4_PROFILER_RING
    if (Enabled()) {
        generation = spatial::ProfilerRing::Generation();
        cookie = spatial::LiteTrace::regionCookieCreate();
        spatial::LiteTrace::trackDef(0x5348414453544147ULL, "shadPS4.RuntimeStages");
        spatial::LiteTrace::regionBegin(name, cookie, 0x5348414453544147ULL);
    }
#endif
}
Phase::~Phase() { End(); }
void Phase::End() noexcept {
#ifdef SHADPS4_PROFILER_RING
    if (cookie && generation == spatial::ProfilerRing::Generation())
        spatial::LiteTrace::regionEnd(cookie);
#endif
    cookie = 0;
}
void Counter(const char* name, int64_t value) noexcept {
#ifdef SHADPS4_PROFILER_RING
    spatial::LiteTrace::counter(name, value);
#endif
}
void Bookmark(const char* name) noexcept {
#ifdef SHADPS4_PROFILER_RING
    spatial::LiteTrace::bookmark(name);
#endif
}
void Frame() noexcept {
#ifdef SHADPS4_PROFILER_RING
    spatial::LiteTrace::frameStart();
#endif
}
bool Enabled() noexcept {
#ifdef SHADPS4_PROFILER_RING
    return spatial::ProfilerRing::Enabled();
#else
    return false;
#endif
}
void Initialize() {
#ifdef SHADPS4_PROFILER_RING
    static std::once_flag once;
    std::call_once(once, [] {
        spatial::ProfilerRing::Initialize("shadPS4",
            (FS::GetUserPath(FS::PathType::LogDir) / "profiler").string(), true);
#if defined(__ANDROID__)
        // Explicit one-process debug opt-in: arm before launch so Prepare and
        // guest bootstrap are captured even before the service accepts commands.
        char value[PROP_VALUE_MAX]{};
        const int length = __system_property_get("debug.shadps4.profile_startup_seconds", value);
        uint32_t seconds{};
        const auto [end, ec] = std::from_chars(value, value + length, seconds);
        if (length && ec == std::errc{} && end == value + length && seconds && seconds <= 600) {
            spatial::ProfilerRing::StartFileCapture(512, seconds);
            for (int attempt = 0; attempt < 100; ++attempt) {
                const auto status = spatial::ProfilerRing::CaptureStatus();
                if (status.find("capture_state=recording") != std::string::npos ||
                    status.find("capture_state=failed") != std::string::npos) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
#endif
    });
#endif
}
std::string Control(const std::vector<std::string>& args) {
#ifdef SHADPS4_PROFILER_RING
    const auto action = args.empty() ? "status" : args[0];
    if (action == "status" && args.size() <= 1) return spatial::ProfilerRing::Status();
    if ((action == "start" || action == "stop") && args.size() == 1)
        return spatial::ProfilerRing::SetEnabled(action == "start");
    if (action == "dump" && args.size() <= 2) {
        uint32_t frames = 120;
        if (args.size() == 2) {
            const auto& s = args[1];
            auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), frames);
            if (ec != std::errc{} || end != s.data() + s.size() || frames > 10000)
                return "error=invalid_frame_window\n";
        }
        return spatial::ProfilerRing::RequestDump(frames);
    }
    return "error=invalid_arguments\n";
#else
    return "error=profiler_ring_not_built\n";
#endif
}
std::string CaptureControl(const std::vector<std::string>& args) {
    const auto action = args.empty() ? "status" : args[0];
    uint32_t max_mib = 256, seconds = 180;
    if (action == "file" || action == "socket") {
        if (args.size() > 3) return "error=invalid_arguments\n";
        auto parse = [](const std::string& s, uint32_t& value, uint32_t maximum) {
            auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
            return ec == std::errc{} && end == s.data() + s.size() && value && value <= maximum;
        };
        if ((args.size() > 1 && !parse(args[1], max_mib, 1024)) ||
            (args.size() > 2 && !parse(args[2], seconds, 3600)))
            return "error=invalid_arguments\n";
    } else if ((action != "status" && action != "stop") || args.size() > 1) {
        return "error=invalid_arguments\n";
    }
#ifdef SHADPS4_PROFILER_RING
    if (action == "status") return spatial::ProfilerRing::CaptureStatus();
    if (action == "stop") return spatial::ProfilerRing::StopCapture();
    if (action == "file") return spatial::ProfilerRing::StartFileCapture(max_mib, seconds);
    return spatial::ProfilerRing::StartSocketCapture(max_mib, seconds);
#else
    return "error=profiler_ring_not_built\n";
#endif
}
}

#include "common/gpu_timing.h"
#include <atomic>
#include <sstream>
#ifdef SHADPS4_PROFILER_RING
#include <profiler/sdk/Sdk.h>
#endif
namespace Common::Profiler {
namespace {
std::atomic<bool> gpu_enabled{false};
std::atomic<bool> gpu_detailed{false};
std::atomic<uint32_t> gpu_epoch{1};
std::mutex gpu_state_mutex;
std::weak_ptr<GpuTimingState> gpu_state;
}
bool GpuTimingEnabled() noexcept { return gpu_enabled.load(std::memory_order_relaxed) && Enabled(); }
bool GpuTimingDetailed() noexcept { return gpu_detailed.load(std::memory_order_relaxed) && GpuTimingEnabled(); }
uint64_t Generation() noexcept {
#ifdef SHADPS4_PROFILER_RING
    return (spatial::ProfilerRing::Generation() << 32) | gpu_epoch.load(std::memory_order_relaxed);
#else
    return gpu_epoch.load(std::memory_order_relaxed);
#endif
}
void RegisterGpuTimingState(const std::shared_ptr<GpuTimingState>& state) {
    std::lock_guard lock{gpu_state_mutex}; gpu_state = state;
}
std::string GpuTimingControl(const std::vector<std::string>& args) {
    const auto action = args.empty() ? "status" : args[0];
    if (args.size() > 1 || (action != "status" && action != "start" && action != "stop" && action != "detail"))
        return "error=invalid_arguments\n";
    if (action != "status") {
        bool changed = gpu_enabled.exchange(action != "stop") != (action != "stop");
        if (action != "stop") changed |= gpu_detailed.exchange(action == "detail") != (action == "detail");
        if (changed) gpu_epoch.fetch_add(1);
    }
    std::shared_ptr<GpuTimingState> state;
    { std::lock_guard lock{gpu_state_mutex}; state = gpu_state.lock(); }
    std::ostringstream out;
    out << "gpu_timing: " << (GpuTimingEnabled() ? "enabled" : "off")
        << "\ndetailed: " << gpu_detailed.load() << "\nrequested: " << gpu_enabled.load() << "\nring_enabled: " << Enabled()
        << "\ncoverage: device elapsed intervals (may include GPU semaphore/memory waits); excludes host API time; nested zones are not additive\n";
    if (!state) { out << "session: unavailable\n"; return out.str(); }
    const auto s = state->Read();
    out << "generation: " << s.generation << "\nsupported: " << s.supported
        << "\ncalibrated: " << s.calibrated << "\nestimated_alignment: " << s.estimated_alignment << "\ncalibration_deviation_ns: " << s.calibration_deviation_ns
        << "\ntimestamp_valid_bits: " << unsigned(s.valid_bits) << "\ntimestamp_period_ns: " << s.period
        << "\nretired_batches: " << s.retired_batches << "\npending: " << s.pending
        << "\ndropped_batches: " << s.dropped_batches << "\ndropped_zones: " << s.dropped_zones
        << "\ndiscarded: " << s.discarded << "\nerrors: " << s.errors << '\n';
    for (size_t i = 0; i < s.stages.size(); ++i) {
        const auto& p = s.stages[i];
        out << GpuNames[i] << ": count=" << p.count << " last_ms=" << p.last_ms
            << " total_ms=" << p.total_ms << " max_ms=" << p.max_ms << " observed_ns=" << p.observed_ns << '\n';
    }
    return out.str();
}
uint8_t GpuContext(int64_t cpu_ns, uint64_t gpu_tick, double period, uint8_t bits, const char* name) noexcept {
#ifdef SHADPS4_PROFILER_RING
    if (Enabled()) try { return ::profiler::sdk::gpu_context_create({cpu_ns, gpu_tick, period, bits}, name); } catch (...) {}
#endif
    return 0;
}
void GpuZone(uint64_t generation, uint8_t ctx, uint32_t slot, const char* name, uint64_t begin, uint64_t end) noexcept {
#ifdef SHADPS4_PROFILER_RING
    if (ctx && Enabled() && generation == Generation()) try {
        ::profiler::sdk::gpu_zone_begin(ctx, slot, 0, name);
        ::profiler::sdk::gpu_time(ctx, slot, begin);
        ::profiler::sdk::gpu_zone_end(ctx, slot);
        ::profiler::sdk::gpu_time(ctx, slot, end);
    } catch (...) {}
#endif
}
}
