// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common/types.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/sync/wait_slot.h"

namespace Libraries::Kernel::Sync {

/// pthread_rwlock_t, shared by the desktop kernel and the Android host runtime.
///
/// Owners are opaque 64-bit thread identities. The lock tracks the writer and every reader with
/// its recursion depth, so it detects self-deadlock (EDEADLK) and unlocks by a non-holder
/// (EPERM). Types follow the Orbis rwlock attribute:
///   0: readers enter whenever no writer holds the lock, even while writers wait;
///   1: waiting writers block new readers, but a thread already reading may read again;
///   2: as 1, and a recursive read is refused (EDEADLK).
/// Each object has its own lock; waiters park on their own slots and an unlock wakes only the
/// waiters the new state admits (one writer, or the readers).
class RwLock {
public:
    explicit RwLock(u32 type_ = 0) : type{type_} {}

    /// 0, or POSIX_EBUSY (try), POSIX_EDEADLK, POSIX_EAGAIN (read depth), POSIX_ETIMEDOUT or
    /// POSIX_EINTR. `platform` supplies the wait (see wait_slot.h); unused for a try.
    template <class Platform>
    int Lock(u64 owner, bool write, bool try_only, Platform& platform) {
        std::unique_lock lock{mutex};
        const bool reader = FindReader(owner) != readers.end();
        if (writer == owner || (reader && (write || type == 2))) {
            return try_only ? POSIX_EBUSY : POSIX_EDEADLK;
        }
        if (!Admits(write, reader)) {
            if (try_only) {
                return POSIX_EBUSY;
            }
            Entry entry{platform.Slot(), write, reader};
            ++waiters;
            if (write) {
                ++waiting_writers;
            }
            int result = 0;
            for (;;) {
                queue.push_back(&entry);
                if (!platform.BeforePark()) {
                    result = POSIX_EINTR;
                    break;
                }
                lock.unlock();
                const ParkResult parked = platform.Park();
                platform.AfterPark();
                lock.lock();
                Dequeue(entry);
                if (parked == ParkResult::Interrupted) {
                    result = POSIX_EINTR;
                    break;
                }
                if (Admits(write, reader)) {
                    break;
                }
                if (parked == ParkResult::TimedOut) {
                    result = POSIX_ETIMEDOUT;
                    break;
                }
            }
            Dequeue(entry);
            --waiters;
            Wakes wakes;
            if (write) {
                --waiting_writers;
                // A departing writer may have been what kept readers out.
                if (result != 0) {
                    SelectLocked(wakes);
                }
            }
            if (result != 0) {
                lock.unlock();
                Release(wakes);
                return result;
            }
        }
        if (write) {
            writer = owner;
            return 0;
        }
        auto it = FindReader(owner);
        if (it == readers.end()) {
            readers.emplace_back(owner, 1);
        } else if (it->second == UINT32_MAX) {
            return POSIX_EAGAIN;
        } else {
            ++it->second;
        }
        return 0;
    }

    /// 0, or POSIX_EPERM when `owner` holds neither the write lock nor a read lock.
    int Unlock(u64 owner) {
        Wakes wakes;
        {
            std::scoped_lock lock{mutex};
            if (writer != 0 && writer == owner) {
                writer = 0;
            } else {
                auto it = FindReader(owner);
                if (it == readers.end()) {
                    return POSIX_EPERM;
                }
                if (--it->second == 0) {
                    readers.erase(it);
                }
            }
            SelectLocked(wakes);
        }
        Release(wakes);
        return 0;
    }

    /// Held or waited on: destroy refuses it.
    bool Busy() const {
        std::scoped_lock lock{mutex};
        return writer != 0 || !readers.empty() || waiters != 0;
    }

    u32 Waiting() const {
        std::scoped_lock lock{mutex};
        return waiters;
    }

    u32 Type() const {
        return type;
    }

private:
    struct Entry {
        std::shared_ptr<WaitSlot> slot;
        bool write;
        bool reader; ///< The waiter already holds a read lock.
    };
    using Wakes = std::vector<std::shared_ptr<WaitSlot>>;

    std::vector<std::pair<u64, u32>>::iterator FindReader(u64 owner) {
        return std::ranges::find(readers, owner, &std::pair<u64, u32>::first);
    }

    bool Admits(bool write, bool reader) const {
        if (writer != 0) {
            return false;
        }
        if (write) {
            return readers.empty();
        }
        return type == 0 || waiting_writers == 0 || reader;
    }

    void Dequeue(Entry& entry) {
        const auto it = std::ranges::find(queue, &entry);
        if (it != queue.end()) {
            queue.erase(it);
        }
    }

    /// Takes the waiters the current state admits off the queue: the first writer when it may
    /// enter (and readers may not overtake it), otherwise every admitted reader.
    void SelectLocked(Wakes& wakes) {
        if (queue.empty() || writer != 0) {
            return;
        }
        const auto first_writer = std::ranges::find(queue, true, &Entry::write);
        const bool writer_turn = first_writer != queue.end() && readers.empty() &&
                                 (type != 0 || std::ranges::none_of(queue, [](const Entry* e) {
                                      return !e->write;
                                  }));
        if (writer_turn) {
            wakes.push_back((*first_writer)->slot);
            queue.erase(first_writer);
            return;
        }
        for (auto it = queue.begin(); it != queue.end();) {
            if (!(*it)->write && Admits(false, (*it)->reader)) {
                wakes.push_back((*it)->slot);
                it = queue.erase(it);
            } else {
                ++it;
            }
        }
    }

    static void Release(const Wakes& wakes) {
        for (const auto& slot : wakes) {
            slot->Unpark();
        }
    }

    mutable std::mutex mutex;
    const u32 type;
    u64 writer{};
    std::vector<std::pair<u64, u32>> readers;
    u32 waiters{};
    u32 waiting_writers{};
    std::vector<Entry*> queue;
};

} // namespace Libraries::Kernel::Sync
