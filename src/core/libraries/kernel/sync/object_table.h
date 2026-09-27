// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

#include "common/types.h"

namespace Libraries::Kernel::Sync {

/// Guest-visible 32-bit ids for kernel objects (semaphores, event flags). Lookups share the table
/// lock only for the map access and hand out shared ownership, so an object deleted while other
/// threads still wait on it stays alive until they leave it. Objects synchronize themselves.
template <class T>
class ObjectTable {
public:
    /// `capacity` 0 means unlimited.
    explicit ObjectTable(size_t capacity_ = 0) : capacity{capacity_} {}

    /// Publishes an object under a new id; nullopt when the table is full or the ids ran out.
    std::optional<u32> Insert(std::shared_ptr<T> object) {
        std::unique_lock lock{mutex};
        if (capacity != 0 && objects.size() >= capacity) {
            return std::nullopt;
        }
        // Ids stay positive s32 values and are not reused while live.
        for (u32 attempt = 0; attempt < MaxId; ++attempt) {
            const u32 id = next_id;
            next_id = next_id >= MaxId ? 1 : next_id + 1;
            if (objects.try_emplace(id, object).second) {
                return id;
            }
        }
        return std::nullopt;
    }

    std::shared_ptr<T> Find(u32 id) const {
        std::shared_lock lock{mutex};
        const auto it = objects.find(id);
        return it == objects.end() ? nullptr : it->second;
    }

    std::shared_ptr<T> Erase(u32 id) {
        std::unique_lock lock{mutex};
        const auto it = objects.find(id);
        if (it == objects.end()) {
            return nullptr;
        }
        auto object = std::move(it->second);
        objects.erase(it);
        return object;
    }

    size_t Size() const {
        std::shared_lock lock{mutex};
        return objects.size();
    }

private:
    static constexpr u32 MaxId = 0x7FFFFFFE;

    mutable std::shared_mutex mutex;
    std::unordered_map<u32, std::shared_ptr<T>> objects;
    u32 next_id = 1;
    size_t capacity;
};

} // namespace Libraries::Kernel::Sync
