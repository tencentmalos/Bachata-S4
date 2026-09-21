// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include "common/types.h"

namespace Core::HostRuntime {
inline std::string GuestModuleFold(std::string_view name) {
    std::string result{name};
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
// Only the known packaging suffix is interchangeable; no substring/fuzzy match.
inline std::string GuestModuleNameKey(const std::filesystem::path& path) {
    auto name = GuestModuleFold(path.filename().string());
    if (name.ends_with(".debug_prx")) name.replace(name.size() - 10, 10, ".prx");
    return name;
}
inline bool GuestModuleAliasIdentity(const std::filesystem::path& requested,
                                     std::string_view exported_module) {
    const auto name = GuestModuleNameKey(requested);
    if (!name.ends_with(".prx") && !name.ends_with(".sprx")) return false;
    return std::filesystem::path(name).stem().string() == GuestModuleFold(exported_module);
}
inline bool GuestModuleNeedsLoadArguments(const std::filesystem::path& relative) {
    // Unity's native plug-in module_start consumes its versioned descriptor.
    // A DT_NEEDED relocation edge does not supply that descriptor.
    return relative.parent_path() == std::filesystem::path("Media/Plugins");
}
struct GuestModuleInitializationPlan {
    std::map<u32, std::vector<u32>> dependencies;
    std::vector<u32> order;
    std::set<u32> startup;
};
inline GuestModuleInitializationPlan PlanGuestModuleInitialization(
    const std::map<u32, std::vector<u32>>& graph, const std::vector<u32>& roots,
    const std::set<u32>& parameterized) {
    GuestModuleInitializationPlan plan;
    for (const auto& [id, deps] : graph)
        for (auto dep : deps)
            if (!parameterized.contains(dep)) plan.dependencies[id].push_back(dep);
    std::function<void(u32)> visit = [&](u32 id) {
        if (parameterized.contains(id) || !plan.startup.insert(id).second) return;
        for (auto dep : plan.dependencies[id]) visit(dep);
        if (id) plan.order.push_back(id);
    };
    for (auto id : roots) visit(id);
    return plan;
}
} // namespace Core::HostRuntime
