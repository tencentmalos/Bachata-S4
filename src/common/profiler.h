// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Common::Profiler {
struct FlowToken { uint64_t id{}, generation{}; };
FlowToken Post(const char* name) noexcept;
// Stable across the host SDK boundary; Foundation owns the only ring implementation.
class Scope {
public:
    explicit Scope(const char* name) noexcept;
    Scope(const char* name, FlowToken flow) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    uint64_t generation{};
    bool active{};
};
// DebugBus `profiler fine on|off`, off by default.
extern std::atomic<bool> fine_enabled;
// A Scope for per-draw detail (several per draw on the GPU thread). Recorded only while
// `profiler fine on`; otherwise one relaxed load, since each recorded pair costs ~0.2 us there.
class FineScope {
public:
    explicit FineScope(const char* name) noexcept {
        if (fine_enabled.load(std::memory_order_relaxed)) [[unlikely]] {
            scope.emplace(name);
        }
    }
    FineScope(const FineScope&) = delete;
    FineScope& operator=(const FineScope&) = delete;
private:
    std::optional<Scope> scope;
};
// Cookie-paired elapsed stage; does not create frames or claim on-CPU time.
class Phase {
public:
    explicit Phase(const char* name) noexcept;
    ~Phase();
    Phase(const Phase&) = delete;
    Phase& operator=(const Phase&) = delete;
    void End() noexcept;
private:
    uint64_t cookie{}, generation{};
};
void Counter(const char* name, int64_t value) noexcept;
void Bookmark(const char* name) noexcept;
void Frame() noexcept;
bool Enabled() noexcept;
// Initialized after app-owned host paths exist. No SDK owner in JNI/FEX.
void Initialize();
std::string Control(const std::vector<std::string>& args);
// A new frame source (game session): slow-frame detection starts its baseline over, and slow-frame
// captures from here on are labelled with `identity` (title, build, frame source).
void BeginSession(const std::string& identity) noexcept;
std::string CaptureControl(const std::vector<std::string>& args);
}
