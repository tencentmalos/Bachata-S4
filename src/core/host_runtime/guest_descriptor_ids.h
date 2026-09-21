// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <climits>
#include <mutex>
#include <set>
namespace Core::HostRuntime {
// One namespace for session files and sockets. Recycle the lowest free guest
// descriptor; in-flight operations retain the old object, never look it up again.
class GuestDescriptorIds {
public:
    int Allocate() {
        std::lock_guard lock(mutex);
        if (next == INT_MAX)
            return -1;
        const int id = next;
        used.insert(id);
        do {
            ++next;
        } while (next < INT_MAX && used.contains(next));
        return id;
    }
    void Release(int id) {
        std::lock_guard lock(mutex);
        if (used.erase(id))
            next = std::min(next, id);
    }

private:
    std::mutex mutex;
    std::set<int> used;
    int next{3};
};
} // namespace Core::HostRuntime
