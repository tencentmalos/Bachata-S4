// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include "common/assert.h"
#include "common/recursive_lock.h"

namespace Common::Detail {

namespace {

struct RecursiveLockState {
    void* mutex;
    RecursiveLockType type;
    int count;
};

// A thread holds few recursive locks at a time, so a short array replaces a hash map and taking
// a lock does not allocate (Rasterizer::IsMapped takes one in every guest write fault handler).
constexpr size_t MaxHeldRecursiveLocks = 8;
thread_local std::array<RecursiveLockState, MaxHeldRecursiveLocks> g_recursive_locks{};
thread_local size_t g_recursive_lock_count = 0;

RecursiveLockState* FindRecursiveLock(void* mutex) {
    for (size_t i = 0; i < g_recursive_lock_count; ++i) {
        if (g_recursive_locks[i].mutex == mutex) {
            return &g_recursive_locks[i];
        }
    }
    return nullptr;
}

} // namespace

bool IncrementRecursiveLock(void* mutex, RecursiveLockType type) {
    if (RecursiveLockState* state = FindRecursiveLock(mutex)) {
        ASSERT(state->type == type);
        ++state->count;
        return false;
    }
    ASSERT(g_recursive_lock_count < MaxHeldRecursiveLocks);
    g_recursive_locks[g_recursive_lock_count++] = {mutex, type, 1};
    return true;
}

bool DecrementRecursiveLock(void* mutex, RecursiveLockType type) {
    RecursiveLockState* state = FindRecursiveLock(mutex);
    ASSERT(state != nullptr && state->type == type && state->count > 0);
    if (--state->count != 0) {
        return false;
    }
    *state = g_recursive_locks[--g_recursive_lock_count];
    return true;
}

} // namespace Common::Detail
