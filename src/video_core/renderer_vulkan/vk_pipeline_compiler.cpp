// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <span>

#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_pipeline_compiler.h"
#include "video_core/renderer_vulkan/vk_pipeline_stats.h"

namespace Vulkan {

PipelineCompiler::PipelineCompiler(u32 threads) {
    for (u32 i = 0; i < threads; ++i) {
        workers.emplace_back([this](std::stop_token token) { Worker(token); });
    }
}

PipelineCompiler::~PipelineCompiler() {
    Stop();
}

bool PipelineCompiler::Submit(const Pipeline* pipeline) {
    std::size_t depth{};
    {
        std::scoped_lock lock{mutex};
        if (stopped || workers.empty() || queue.size() >= MaxQueued) {
            PipelineStats::RecordCompileRejected();
            return false;
        }
        queue.push_back(pipeline);
        depth = queue.size();
    }
    cv.notify_one();
    PipelineStats::RecordCompileQueued(depth);
    return true;
}

bool PipelineCompiler::SubmitBacklog(std::span<const Pipeline* const> pipelines) {
    {
        std::scoped_lock lock{mutex};
        if (stopped || workers.empty()) {
            return false;
        }
        backlog.insert(backlog.end(), pipelines.begin(), pipelines.end());
    }
    cv.notify_all();
    return true;
}

void PipelineCompiler::Promote(const Pipeline* pipeline) {
    {
        std::scoped_lock lock{mutex};
        const auto it = std::ranges::find(backlog, pipeline);
        if (it == backlog.end()) {
            return;
        }
        backlog.erase(it);
        queue.push_front(pipeline);
    }
    PipelineStats::RecordBacklogPromoted();
    cv.notify_one();
}

void PipelineCompiler::SetBacklogConcurrency(u32 count) {
    {
        std::scoped_lock lock{mutex};
        backlog_concurrency = count;
    }
    cv.notify_all();
}

std::size_t PipelineCompiler::BacklogSize() {
    std::scoped_lock lock{mutex};
    return backlog.size();
}

void PipelineCompiler::Forget(const Pipeline* pipeline) {
    std::unique_lock lock{mutex};
    std::erase(queue, pipeline);
    std::erase(backlog, pipeline);
    done_cv.wait(lock, [&] { return std::ranges::find(in_hand, pipeline) == in_hand.end(); });
}

void PipelineCompiler::Stop() {
    {
        std::scoped_lock lock{mutex};
        if (stopped) {
            return;
        }
        stopped = true;
        queue.clear();
        backlog.clear();
    }
    for (auto& worker : workers) {
        worker.request_stop();
    }
    cv.notify_all();
    workers.clear(); // jthread joins
}

void PipelineCompiler::Worker(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:PipelineCompile");
    for (;;) {
        const Pipeline* pipeline{};
        bool from_backlog{};
        {
            std::unique_lock lock{mutex};
            const auto has_work = [this] {
                return !queue.empty() ||
                       (!backlog.empty() && backlog_active < backlog_concurrency);
            };
            Common::CondvarWait(cv, lock, token, has_work);
            if (token.stop_requested() || !has_work()) {
                return;
            }
            if (!queue.empty()) {
                pipeline = queue.front();
                queue.pop_front();
            } else {
                pipeline = backlog.front();
                backlog.pop_front();
                from_backlog = true;
                ++backlog_active;
            }
            in_hand.push_back(pipeline);
        }
        if (pipeline->TryClaim()) {
            PipelineStats::RecordCompiledByWorker(from_backlog);
            pipeline->BuildClaimed();
        }
        {
            std::scoped_lock lock{mutex};
            in_hand.erase(std::ranges::find(in_hand, pipeline));
            if (from_backlog) {
                --backlog_active;
            }
        }
        done_cv.notify_all();
        if (from_backlog) {
            cv.notify_one(); // A backlog slot is free again.
        }
    }
}

u32 PipelineCompileThreads(const Instance& instance) {
    if (instance.GetDriverID() == vk::DriverId::eQualcommProprietary) {
        return 1;
    }
    if (const u32 configured = EmulatorSettings.GetPipelineCompileWorkers(); configured != 0) {
        return std::min(configured, 16u);
    }
#ifdef __ANDROID__
    return 2;
#else
    return std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
#endif
}

} // namespace Vulkan
