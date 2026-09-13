// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace Common {

template <class T>
class Singleton {
public:
    static T* Instance() {
        if (auto* instance = borrowed.load(std::memory_order_acquire))
            return instance;
        std::lock_guard lock{instance_mutex};
        if (auto* instance = borrowed.load(std::memory_order_acquire))
            return instance;
        if (!m_instance) {
            m_instance = std::make_unique<T>();
        }
        return m_instance.get();
    }

    // A session can supply an owned instance to existing desktop call sites.
    // The owner drains all users before destroying this binding. Identity CAS
    // prevents a stale generation from detaching another generation's service.
    class Binding {
    public:
        explicit Binding(T& value) : instance(&value) {
            std::lock_guard lock{instance_mutex};
            T* expected = nullptr;
            if (m_instance || !borrowed.compare_exchange_strong(expected, instance))
                throw std::logic_error("singleton is already owned");
        }
        ~Binding() {
            T* expected = instance;
            borrowed.compare_exchange_strong(expected, nullptr);
        }
        Binding(const Binding&) = delete;
        Binding& operator=(const Binding&) = delete;

    private:
        T* instance;
    };

protected:
    Singleton();
    ~Singleton();

private:
    static inline std::unique_ptr<T> m_instance{};
    static inline std::atomic<T*> borrowed{};
    static inline std::mutex instance_mutex;
};

} // namespace Common
