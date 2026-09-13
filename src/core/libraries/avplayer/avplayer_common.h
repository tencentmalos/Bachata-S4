// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <queue>
#ifndef __ANDROID__
#include "core/libraries/kernel/threads.h"
#endif

#include "core/libraries/avplayer/avplayer.h"

#define AVPLAYER_IS_ERROR(x) ((x) < 0)

namespace Libraries::AvPlayer {

// Android decoder workers are native bionic threads. Guest callbacks are
// explicitly marshaled by GuestAvPlayer and never enter desktop pthread state.
#ifdef __ANDROID__
class AvPlayerThread {
public:
    ~AvPlayerThread() {
        Stop();
    }
    void Run(std::function<void(std::stop_token)> fn) {
        worker = std::jthread(std::move(fn));
    }
    bool Joinable() const {
        return worker.joinable();
    }
    void RequestStop() {
        worker.request_stop();
    }
    void Stop() {
        worker.request_stop();
        if (worker.joinable())
            worker.join();
    }

private:
    std::jthread worker;
};
#else
using AvPlayerThread = Kernel::Thread;
#endif

enum class AvState {
    Unknown,
    Initial,
    AddingSource,
    Ready,
    Play,
    Stop,
    EndOfFile,
    Pause,
    C0x08,
    Jump,
    TrickMode,
    C0x0B,
    Buffering,
    Starting,
    Error,
};

enum class AvEventType {
    ChangeFlowState = 21,
    WarningId = 22,
    RevertState = 30,
    AddSource = 40,
    Error = 255,
};

union AvPlayerEventData {
    u32 num_frames; // 20
    AvState state;  // AvEventType::ChangeFlowState
    s32 error;      // AvEventType::WarningId
    u32 attempt;    // AvEventType::AddSource
};

struct AvPlayerEvent {
    AvEventType event;
    AvPlayerEventData payload;
};

template <class T>
class AvPlayerQueue {
public:
    size_t Size() {
        std::lock_guard guard(m_mutex);
        return m_queue.size();
    }

    void Push(T&& value) {
        std::lock_guard guard(m_mutex);
        m_queue.emplace(std::forward<T>(value));
    }

    template <class Predicate>
    std::optional<T> PopIf(Predicate predicate) {
        std::lock_guard guard(m_mutex);
        if (m_queue.empty() || !predicate(m_queue.front()))
            return std::nullopt;
        auto result = std::move(m_queue.front());
        m_queue.pop();
        return result;
    }
    std::optional<T> Pop() {
        return PopIf([](const T&) { return true; });
    }

    void Clear() {
        std::lock_guard guard(m_mutex);
        m_queue = {};
    }

private:
    std::mutex m_mutex{};
    std::queue<T> m_queue{};
};

AvPlayerSourceType GetSourceType(std::string_view path);

} // namespace Libraries::AvPlayer
