// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "common/types.h"

namespace Vulkan {

class Instance;
class Pipeline;

/// Bounded pool that creates the driver objects of deferred pipelines.
///
/// A worker only calls the driver with the pipeline's prepared create state: it never touches
/// the scheduler, guest memory or cache locks, so a thread waiting for a build can never be
/// part of a cycle. The first user of a pipeline that is still queued builds it itself instead
/// of waiting behind other jobs (see Pipeline::WaitHandle); workers skip such jobs.
///
/// Two queues: demand (pipelines a draw or dispatch asked for; bounded) and backlog (preloaded
/// pipelines, in the order the last sessions needed them; unbounded). Workers always take
/// demand first, and at most `backlog_concurrency` of them build backlog entries at a time, so
/// warming the cache does not crowd out the game after startup.
class PipelineCompiler {
public:
    static constexpr std::size_t MaxQueued = 64;

    explicit PipelineCompiler(u32 threads);
    ~PipelineCompiler();

    PipelineCompiler(const PipelineCompiler&) = delete;
    PipelineCompiler& operator=(const PipelineCompiler&) = delete;

    u32 Threads() const noexcept {
        return static_cast<u32>(workers.size());
    }

    /// Queues a pending pipeline a draw or dispatch needs. False when full or stopped; the
    /// caller then builds it.
    bool Submit(const Pipeline* pipeline);

    /// Queues pending preloaded pipelines, highest priority first. False when stopped.
    bool SubmitBacklog(std::span<const Pipeline* const> pipelines);

    /// Moves a backlog entry to the front of the demand queue (it is needed now).
    void Promote(const Pipeline* pipeline);

    /// How many workers may build backlog entries at the same time.
    void SetBacklogConcurrency(u32 count);

    /// Before a pipeline is destroyed: removes it from both queues and waits until no worker is
    /// still using it. It is then built (Ready) or still pending, never mid-build.
    void Forget(const Pipeline* pipeline);

    /// Joins the workers after their current build. Queued pipelines stay pending and are
    /// built by their first user, if any.
    void Stop();

    std::size_t BacklogSize();

private:
    void Worker(std::stop_token token);

    std::mutex mutex{};
    std::condition_variable_any cv{};
    std::deque<const Pipeline*> queue{};
    std::deque<const Pipeline*> backlog{};
    u32 backlog_concurrency{1};
    u32 backlog_active{};
    std::vector<const Pipeline*> in_hand{}; ///< Taken by a worker and not finished yet.
    std::condition_variable_any done_cv{};
    std::vector<std::jthread> workers{};
    bool stopped{};
};

/// Worker count for this device: `pipeline_compile_workers` when set, otherwise 2 on Android,
/// up to 4 on desktop, and 1 on Qualcomm's proprietary driver.
u32 PipelineCompileThreads(const Instance& instance);

} // namespace Vulkan
