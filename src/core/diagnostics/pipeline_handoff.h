// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from Citron common/pipeline_handoff: bounded, opt-in identity events.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "common/profiler.h"

namespace Core::Diagnostics {
struct DiagnosticsSnapshot;
namespace Handoff {
using Id = std::uint64_t;
extern std::atomic<Id> enabled_generation;
inline bool Enabled(Id generation) {
    return generation && enabled_generation.load(std::memory_order_relaxed) == generation;
}
Id NextId() noexcept;
// Capture IDs, object IDs, submission IDs and timeline ticks are separate identities.
void Event(Id generation, const char* kind, Id object, Id token, Id value = 0);
void EndGeneration(Id generation);
std::string Control(const std::vector<std::string>& args, const DiagnosticsSnapshot& snapshot);

class Scope {
public:
    Scope(const char* name, Id generation, Id object = 0, Id related = 0, bool wait = false);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Common::Profiler::Scope profiler_scope;
    const char* name;
    Id generation{}, capture{}, token{}, object{}, related{};
    bool wait{};
};
} // namespace Handoff
} // namespace Core::Diagnostics

// The disabled producer path evaluates no clock, token or event arguments.
#define SHAD_HANDOFF(generation, kind, ...)                                                        \
    do {                                                                                           \
        if (::Core::Diagnostics::Handoff::Enabled(generation)) [[unlikely]]                        \
            ::Core::Diagnostics::Handoff::Event(generation, kind, __VA_ARGS__);                    \
    } while (false)
