// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Common::Profiler {
// Stable across the host SDK boundary; Foundation owns the only ring implementation.
class Scope {
public:
    explicit Scope(const char* name) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    uint64_t generation{};
    bool active{};
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
std::string CaptureControl(const std::vector<std::string>& args);
}
