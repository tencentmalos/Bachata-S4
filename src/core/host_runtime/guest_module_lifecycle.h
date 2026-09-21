// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <functional>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <vector>
#include "common/types.h"
#include "core/host_runtime/guest_module_policy.h"
#include "core/libraries/kernel/orbis_error.h"

namespace Core::HostRuntime {
inline bool IsGuestPluginFile(const std::filesystem::path& path, bool is_file) {
    const auto name = path.filename().string();
    // macOS transfers may include AppleDouble ._name.prx metadata alongside
    // the actual image. These hidden sidecars are not module roots.
    const auto key = GuestModuleNameKey(path);
    return is_file && !name.empty() && name.front() != '.' &&
           (key.ends_with(".prx") || key.ends_with(".sprx"));
}
// All images, relocations and TLS layouts are frozen before guest owners exist.
// Initialization is separate: a prepared plug-in runs DT_INIT only on demand.
// No metadata lock or guest-memory pin survives a callback into the guest.
class GuestModuleLifecycle {
public:
    struct Result { s32 error{}, value{}; bool initialized{}; };
    using Initialize = std::function<s32(u32, u64, u64)>;
    void Add(u32 id, std::vector<u32> dependencies, bool started = false) {
        std::lock_guard lock(mutex);
        if (!modules.emplace(id, Entry{std::move(dependencies),
                started ? State::Started : State::Prepared}).second)
            throw std::logic_error("duplicate prepared module");
    }
    bool Visible(u32 id) const {
        std::lock_guard lock(mutex);
        const auto it = modules.find(id);
        return it != modules.end() && (it->second.state == State::Starting ||
                                      it->second.state == State::Started);
    }
    Result Start(u32 id, u64 owner, u64 bytes, u64 argp, std::stop_token stop,
                 const Initialize& initialize) {
        std::unique_lock lock(mutex);
        auto it = modules.find(id);
        if (it == modules.end()) return {ORBIS_KERNEL_ERROR_ENOENT};
        auto& entry = it->second;
        while (entry.state == State::Starting && entry.owner != owner) {
            // Detect dependency/callback cycles between different owners. Same
            // owner recursion (including ELF dependency cycles) sees its handle.
            std::set<u64> seen{owner};
            u64 next = entry.owner;
            while (next) {
                if (!seen.insert(next).second) return {ORBIS_KERNEL_ERROR_EDEADLK};
                const auto waiting = waits.find(next);
                next = waiting == waits.end() ? 0 : modules.at(waiting->second).owner;
            }
            waits[owner] = id;
            const bool ready = changed.wait(lock, stop, [&] { return entry.state != State::Starting; });
            waits.erase(owner);
            if (!ready) return {ORBIS_KERNEL_ERROR_EINTR};
        }
        if (stop.stop_requested()) return {ORBIS_KERNEL_ERROR_EINTR};
        if (entry.state == State::Failed) return {ORBIS_KERNEL_ERROR_ESTART};
        if (entry.state != State::Prepared) return {};
        entry.state = State::Starting;
        entry.owner = owner;
        lock.unlock();
        auto finish = [&](State state) {
            std::lock_guard guard(mutex);
            entry.state = state;
            entry.owner = 0;
            changed.notify_all();
        };
        try {
            for (const auto dependency : entry.dependencies) {
                const auto result = Start(dependency, owner, 0, 0, stop, initialize);
                if (result.error || (result.initialized && result.value)) {
                    finish(State::Failed);
                    return {result.error ? result.error : ORBIS_KERNEL_ERROR_ESTART};
                }
            }
            if (stop.stop_requested()) {
                finish(State::Failed);
                return {ORBIS_KERNEL_ERROR_EINTR};
            }
            const auto result = initialize(id, bytes, argp);
            finish(State::Started);
            // Match LoadAndStartModule: a returned DT_INIT result goes in pRes;
            // the module handle is still returned, and repeats leave pRes alone.
            return {0, result, true};
        } catch (...) {
            finish(State::Failed);
            throw;
        }
    }
private:
    enum class State { Prepared, Starting, Started, Failed };
    struct Entry { std::vector<u32> dependencies; State state; u64 owner{}; };
    mutable std::mutex mutex;
    std::condition_variable_any changed;
    std::map<u32, Entry> modules;
    std::map<u64, u32> waits;
};
} // namespace Core::HostRuntime
