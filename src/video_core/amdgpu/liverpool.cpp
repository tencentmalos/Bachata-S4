// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <deque>
#include <chrono>
#include <thread>
#include <boost/preprocessor/stringize.hpp>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "common/arch.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/profiler.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/diagnostics/diagnostics_hub_registry.h"
#include "core/diagnostics/pipeline_handoff.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/memory.h"
#include "core/guest_write_watch.h"
#include "core/platform.h"
#include "shader_recompiler/resource.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/memory_diagnostics.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/pm4_trace.h"
#include "video_core/renderdoc.h"
#include "video_core/renderdoc_capture.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/replay/gpu_replay_player.h"
#include "video_core/replay/gpu_replay_recorder.h"

namespace AmdGpu {

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num)                                                                      \
    { BOOST_PP_REPEAT(num, NAME_NUM, name) }

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    FIBER_EXIT;                                                                                    \
    if (stopping)                                                                                  \
        co_return;                                                                                 \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])

// A yield because the queue waits for memory the guest CPU or another queue has yet to write.
// When every queue with work only yields this way, the command processor backs off instead of
// resuming them in a tight loop.
#define YIELD_WAIT(name)                                                                           \
    waiting_yield = true;                                                                          \
    YIELD(name)

#define YIELD_WAIT_GFX() YIELD_WAIT(dcb_task_name)
#define YIELD_WAIT_ASC(id) YIELD_WAIT(acb_task_name[id])

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    if (task.handle.promise().error)                                                               \
        std::rethrow_exception(task.handle.promise().error);                                       \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

static std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset > span.size()) {
        LOG_ERROR(
            Lib_GnmDriver,
            ": packet length exceeds remaining submission size. Packet dword count={}, remaining "
            "submission dwords={}",
            offset, span.size());
        // Return empty subspan so check for next packet bails out
        return {};
    }

    return span.subspan(offset);
}

/// What an EVENT_WRITE_EOP packet writes and raises once the work before it completed.
static Fence EopFence(const PM4CmdEventWriteEop& eop) {
    Fence fence{.address = reinterpret_cast<VAddr>(eop.Address<u32>()),
                .through_backing = true,
                .writer = "eop_label"};
    switch (eop.data_sel.Value()) {
    case DataSelect::None:
        break;
    case DataSelect::Data32Low:
        fence.size = sizeof(u32);
        fence.data = Fence::Data::Value;
        fence.value = eop.DataDWord();
        break;
    case DataSelect::Data64:
        fence.size = sizeof(u64);
        fence.data = Fence::Data::Value;
        fence.value = eop.DataQWord();
        break;
    case DataSelect::GpuClock64:
        fence.size = sizeof(u64);
        fence.data = Fence::Data::GpuClock;
        break;
    case DataSelect::PerfCounter:
        fence.size = sizeof(u64);
        fence.data = Fence::Data::PerfCounter;
        break;
    default:
        UNREACHABLE();
    }
    switch (eop.int_sel.Value()) {
    case InterruptSelect::None:
        break;
    case InterruptSelect::IrqOnly:
        ASSERT(eop.data_sel == DataSelect::None);
        [[fallthrough]];
    case InterruptSelect::IrqWhenWriteConfirm:
        fence.irq = static_cast<s32>(Platform::InterruptId::GfxEop);
        break;
    default:
        UNREACHABLE();
    }
    return fence;
}

/// What a compute RELEASE_MEM packet writes and raises once the work before it completed. A GDS
/// store is not part of it: the caller records it as a GPU copy in queue order.
static Fence ReleaseMemFence(const PM4CmdReleaseMem& release, u32 pipe_id) {
    Fence fence{.address = release.Address<VAddr>(), .writer = "release_mem"};
    switch (release.data_sel.Value()) {
    case DataSelect::Data32Low:
        fence.size = sizeof(u32);
        fence.data = Fence::Data::Value;
        fence.value = release.DataDWord();
        break;
    case DataSelect::Data64:
        fence.size = sizeof(u64);
        fence.data = Fence::Data::Value;
        fence.value = release.DataQWord();
        break;
    case DataSelect::GpuClock64:
        fence.size = sizeof(u64);
        fence.data = Fence::Data::GpuClock;
        break;
    case DataSelect::PerfCounter:
        fence.size = sizeof(u64);
        fence.data = Fence::Data::PerfCounter;
        break;
    case DataSelect::GdsMemStore:
        break;
    default:
        UNREACHABLE();
    }
    switch (release.int_sel.Value()) {
    case InterruptSelect::None:
        break;
    case InterruptSelect::IrqUndocumented:
        [[fallthrough]];
    case InterruptSelect::IrqWhenWriteConfirm:
        fence.irq = static_cast<s32>(pipe_id);
        break;
    default:
        UNREACHABLE();
    }
    return fence;
}

Liverpool::Liverpool() : guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    completion_fences = EmulatorSettings.IsCompletionFences();
    process_thread = std::jthread{[this](std::stop_token token) {
        try { Process(token); }
        catch (...) {
            ReportFault(std::current_exception());
            // Wake pending synchronous native commands with their real failure;
            // keep servicing the queue until Session retires its CPU owners.
            while (!token.stop_requested()) {
                { std::unique_lock lock(submit_mutex);
                  Common::CondvarWait(submit_cv, lock, token, [this] { return num_commands != 0; }); }
                ProcessCommands();
            }
        }
    }};
}

void Liverpool::RequestStop() {
    stopping = true;
    if (vo_port) {
        vo_port->stopping = true;
        vo_port->vo_cv.notify_all();
    }
    submit_cv.notify_all();
}

Liverpool::~Liverpool() {
    RequestStop();
    process_thread.request_stop();
    submit_cv.notify_all();
    process_thread.join();
    for (auto& queue : mapped_queues) {
        while (!queue.submits.empty()) {
            queue.submits.front().destroy();
            queue.submits.pop();
        }
    }
}

void Liverpool::ProcessCommands() {
    // Process incoming commands with high priority
    while (num_commands) {
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            callback = std::move(command_queue.front());
            command_queue.pop();
            --num_commands;
        }
        try {
        callback();
        } catch (...) {
            ReportFault(std::current_exception());
        }
    }
}

void Liverpool::RecordPoll(VideoCore::Replay::WaitKind kind, const void* address,
                           bool satisfied) {
    VideoCore::Replay::Recorder::Instance().OnWaitPoll(
        static_cast<u32>(curr_qid), kind, reinterpret_cast<u64>(address), satisfied);
}

void Liverpool::BeforeReplayPoll() {
    replay_player->Peek();
}

bool Liverpool::AfterReplayPoll(VideoCore::Replay::WaitKind kind, const void* address,
                                bool satisfied) {
    return replay_player->Poll(static_cast<u32>(curr_qid), kind, reinterpret_cast<u64>(address),
                               satisfied);
}

bool Liverpool::ResumeTask(GpuQueue& queue, Task::Handle task) {
    const auto generation = diagnostics ? diagnostics->Generation() : 0;
    SHAD_HANDOFF(generation, "queue_resume", curr_qid, task.promise().diagnostic_id);
    auto& promise = task.promise();
    if (VideoCore::Replay::CaptureHooksActive()) [[unlikely]] {
        if (!promise.replay_resumed && promise.replay_submit.source &&
            promise.replay_submit.source != promise.replay_submit.dcb_addr) {
            // The submission may have been queued before capture began. Its Android host
            // copy must be recorded here, while the coroutine still owns the exact bytes.
            VideoCore::Replay::NoteSubmitContents(promise.diagnostic_id, promise.replay_dcb,
                                                   promise.replay_ccb);
        }
        VideoCore::Replay::Recorder::Instance().OnResume(
            curr_qid, promise.diagnostic_id,
            promise.replay_resumed ? nullptr : &promise.replay_submit);
    }
    promise.replay_resumed = true;
    waiting_yield = false;
    Shader::AdvanceDynamicImageEpoch();
    {
        Core::Diagnostics::Handoff::Scope scope{"PM4.Resume", generation,
            static_cast<u64>(curr_qid), task.promise().diagnostic_id};
        task.resume();
    }
    const bool waited = waiting_yield && !task.done();
    // The task suspended or finished: publish packets it consumed since the last batch.
    FlushPm4Progress();

    if (task.done()) {
        SHAD_HANDOFF(generation, "queue_complete", curr_qid, task.promise().diagnostic_id);
        if (task.promise().error)
            ReportFault(task.promise().error);
        task.destroy();

        std::scoped_lock lock{queue.m_access};
        queue.submits.pop();

        --num_submits;
        std::scoped_lock lock2{submit_mutex};
        submit_cv.notify_all();
    }
    return waited;
}

void Liverpool::SnapshotMemoryDiagnostics() {
    VideoCore::MemoryDiagnostics::Read(true);
    SendCommand<true>([this] {
        if (rasterizer) {
            rasterizer->GetTextureCache().PublishMemoryDiagnostics();
        }
    });
}

void Liverpool::StartReplay(VideoCore::Replay::Player* player) {
    SendCommand([this, player] {
        replay_player = player;
        RunReplay();
    });
}

namespace {
/// CPU time of the calling thread (0 where not measured).
u64 ThreadCpuNs() {
#if defined(__linux__) || defined(__APPLE__)
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0) {
        return static_cast<u64>(time.tv_sec) * 1'000'000'000ULL + static_cast<u64>(time.tv_nsec);
    }
#endif
    return 0;
}
} // namespace

void Liverpool::RunReplay() {
    using namespace VideoCore::Replay;
    auto& player = *replay_player;
    // Injected commands run only between events, and the VideoOut label wait yields: as in
    // the capture.
    replay_capture = true;
    VideoCore::StartCapture();
    // The trace starts at a frame boundary: a capture armed on guest flips starts here.
    if (const auto generation = VideoCore::GetCaptureCoordinator().BoundGeneration();
        VideoCore::NeedsGuestCaptureBoundary(generation)) {
        VideoCore::NotifyGuestFlipBoundary(generation, 0);
    }
    std::deque<u64> in_flight;
    const auto wall_begin = std::chrono::steady_clock::now();
    const u64 cpu_begin = ThreadCpuNs();
    while (const auto* next = player.Peek()) {
        if (stopping.load(std::memory_order_relaxed)) {
            // A session stop (Android replay sessions are cancelled from the UI).
            player.Fail("stopped");
            break;
        }
        const Player::Event event = *next;
        player.Pop();
        player.BeforeEvent(event);
        switch (event.type) {
        case RecordType::Submit: {
            const auto record = event.As<SubmitRecord>();
            const auto* dcb = reinterpret_cast<const u32*>(record.dcb_addr);
            const u32* ccb = reinterpret_cast<const u32*>(record.ccb_addr);
            if (event.payload.size() ==
                sizeof(SubmitRecord) + (record.dcb_dwords + record.ccb_dwords) * sizeof(u32)) {
                // The capture's command buffers were host copies; the trace holds them. They
                // stay alive for the replay: an owned copy re-reads its source on REWIND.
                auto& contents = replay_submit_contents.emplace_back(
                    event.payload.begin() + sizeof(SubmitRecord), event.payload.end());
                dcb = reinterpret_cast<const u32*>(contents.data());
                ccb = dcb + record.dcb_dwords;
            } else if (record.source && record.source != record.dcb_addr) {
                player.Fail(fmt::format("submission {} is missing its host command buffer copy",
                                        record.submission));
                break;
            }
            u32 qid = GfxQueueId;
            if (record.queue == static_cast<u32>(SubmitQueue::Graphics)) {
                SubmitGfx({dcb, record.dcb_dwords}, {ccb, record.ccb_dwords}, record.source);
            } else if (record.gnm_vqid > 0 && record.gnm_vqid < NumTotalQueues &&
                       asc_queues.is_allocated({record.gnm_vqid - 1})) {
                qid = record.gnm_vqid;
                SubmitAsc(record.gnm_vqid, {dcb, record.dcb_dwords});
            } else {
                player.Fail(fmt::format("submission to unmapped compute queue {}",
                                        record.gnm_vqid));
                break;
            }
            // Resume events name the capture's submission ids.
            auto& queue = mapped_queues[qid];
            std::scoped_lock lock{queue.m_access};
            queue.submits.back().promise().replay_submit.submission = record.submission;
            break;
        }
        case RecordType::Resume: {
            const auto record = event.As<ResumeRecord>();
            if (record.queue >= NumTotalQueues) {
                player.Fail(fmt::format("resume of queue {}", record.queue));
                break;
            }
            auto& queue = mapped_queues[record.queue];
            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (!queue.submits.empty()) {
                    task = queue.submits.front();
                }
            }
            if (!task || task.promise().replay_submit.submission != record.submission) {
                player.Fail(fmt::format("queue {} has no submission {} to resume", record.queue,
                                        record.submission));
                break;
            }
            curr_qid = static_cast<s32>(record.queue);
            ResumeTask(queue, task);
            break;
        }
        case RecordType::Command:
            player.RunCommand(event.As<CommandRecord>());
            break;
        case RecordType::BurstEnd:
            if (event.As<BurstEndRecord>().submit_done) {
                VideoCore::EndCapture();
                if (rasterizer) {
                    rasterizer->OnSubmit();
                    // Waits replay their recorded results, so unlike the game (which waits for
                    // its EOP labels) nothing keeps the replay from running ahead of the GPU:
                    // staging and retiring resources would grow without bound (Bloodborne on
                    // the AYN Thor: 1 GiB of staging, lmkd). Keep two submissions in flight.
                    in_flight.push_back(rasterizer->Flush());
                    while (in_flight.size() > 2) {
                        rasterizer->GetScheduler().Wait(in_flight.front());
                        in_flight.pop_front();
                    }
                }
                VideoCore::StartCapture();
            }
            Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
            break;
        case RecordType::WaitPoll:
            player.Fail("a wait outside a resumed queue");
            break;
        default:
            break;
        }
        player.AfterEvent(event);
        // Diagnostics and other host commands queued meanwhile.
        if (num_commands) {
            ProcessCommands();
        }
    }
    VideoCore::EndCapture();
    player.NoteProcessorTime(ThreadCpuNs() - cpu_begin,
                             static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now() - wall_begin)
                                                  .count()));
    replay_capture = false;
    replay_player = nullptr;
    player.Finish();
}

void Liverpool::Process(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandProcessor");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    gpu_id = std::this_thread::get_id();

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(submit_cv, lk, stoken,
                                [this] { return num_commands || num_submits || submit_done; });
        }
        if (stoken.stop_requested()) {
            break;
        }

        processing = true;
        VideoCore::StartCapture();

        curr_qid = -1;
        // Per round over the queues: whether any queue got further, and whether one waited.
        bool round_progress = false;
        bool round_waited = false;
        u32 idle_rounds = 0;

        while (!stoken.stop_requested() && (num_submits || num_commands)) {
            if (num_commands) {
                if (VideoCore::Replay::CaptureHooksActive()) [[unlikely]] {
                    VideoCore::Replay::Recorder::Instance().OnCommands();
                }
                ProcessCommands();
                round_progress = true;
            }

            curr_qid = (curr_qid + 1) % num_mapped_queues.load();
            if (curr_qid == 0) {
                if (round_waited && !round_progress) {
                    // A queue may wait for a value the guest CPU writes only after it saw a
                    // fence of work that is still in the open command buffer.
                    if (++idle_rounds == 1) {
                        FlushFences(fence_labels->flushes_idle);
                    }
                    IdleBackoff(idle_rounds);
                } else {
                    idle_rounds = 0;
                }
                round_progress = false;
                round_waited = false;
            }

            auto& queue = mapped_queues[curr_qid];

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    continue;
                }
                task = queue.submits.front();
            }
            if (ResumeTask(queue, task)) {
                round_waited = true;
            } else {
                round_progress = true;
            }
        }

        const bool frame_end = submit_done;
        if (submit_done) {
            VideoCore::EndCapture();
            if (rasterizer) {
                rasterizer->OnSubmit();
                rasterizer->Flush();
            }
            submit_done = false;
        } else {
            // Out of work: guest threads may wait for these fences now.
            FlushFences(fence_labels->flushes_burst);
        }
        // Before GpuIdle reopens the submission gate.
        VideoCore::Replay::Recorder::Instance().OnBurstEnd(*this, rasterizer, frame_end);

        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
        processing = false;
        submit_cv.notify_all();
    }
}

void Liverpool::IdleBackoff(u32 idle_rounds) {
    // Every queue with work waits on a label, semaphore or rewind that only the guest CPU or a
    // queue further back can satisfy. Stay responsive for a short while, then give the core to
    // the guest threads that have to write it: a new submission or command wakes this wait.
    if (idle_rounds <= 64) {
        for (u32 i = 0; i < 16; ++i) {
#if defined(ARCH_X86_64)
            __asm__ volatile("pause");
#elif defined(ARCH_ARM64)
            __asm__ volatile("yield");
#endif
        }
        return;
    }
    if (idle_rounds <= 128) {
        std::this_thread::yield();
        return;
    }
    Common::Profiler::Scope scope{"PM4.IdleWait"};
    std::unique_lock lk{submit_mutex};
    const u32 submits = num_submits.load();
    submit_cv.wait_for(lk, std::chrono::microseconds{500}, [&] {
        return stopping.load() || num_commands.load() != 0 || num_submits.load() != submits;
    });
}

void Liverpool::SignalFence(Fence fence) {
    if (fence.size == 0 && fence.irq < 0) {
        return;
    }
    if (!rasterizer || !completion_fences.load(std::memory_order_relaxed)) {
        fence_labels->PerformNow(fence);
        return;
    }
    const u64 sequence = fence_labels->Add(fence);
    auto& scheduler = rasterizer->GetScheduler();
    fence_tick = scheduler.CurrentTick();
    // In order with the image write-backs deferred before it.
    scheduler.DeferPriorityOperation(
        [labels = fence_labels, sequence] { labels->PerformThrough(sequence); });
    if (fence.irq >= 0) {
        // A guest thread waits for the interrupt: submit now, not when the burst ends.
        FlushFences(fence_labels->flushes_irq);
    }
}

void Liverpool::SettleWaits() {
    if (wait_debt != 0) {
        fence_labels->PerformThrough(wait_debt, true);
        wait_debt = 0;
    }
}

bool Liverpool::FencesUnsubmitted() const {
    return rasterizer && fence_tick != 0 && fence_tick >= rasterizer->GetScheduler().CurrentTick();
}

void Liverpool::FlushFences(std::atomic<u64>& reason) {
    if (FencesUnsubmitted()) {
        rasterizer->Flush();
        reason.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Liverpool::TestWait(const PM4CmdWaitRegMem& wait) {
    if (wait.mem_space.Value() == PM4CmdWaitRegMem::MemSpace::Memory) {
        u64 sequence{};
        const bool met = wait.TestValue(fence_labels->Read(wait.Address<VAddr>(), sequence));
        if (met && sequence > wait_debt) {
            wait_debt = sequence;
        }
        return met;
    }
    return wait.Test(regs.reg_array);
}

void Liverpool::DrainFences() {
    if (!rasterizer) {
        return;
    }
    FlushFences(fence_labels->flushes_burst);
    const auto begin = std::chrono::steady_clock::now();
    while (fence_labels->Outstanding() != 0 && !stopping) {
        // Rethrows a failure of the completion thread.
        rasterizer->GetScheduler().PopPendingOperations();
        if (std::chrono::steady_clock::now() - begin > std::chrono::seconds{10}) {
            LOG_ERROR(Render, "GPU fences: {} still outstanding after 10 s",
                      fence_labels->Outstanding());
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds{200});
    }
}

void Liverpool::WaitFences(u64 mark) {
    if (!flip_waits_fences.load(std::memory_order_relaxed) ||
        fence_labels->Performed() >= mark) {
        return;
    }
    Common::Profiler::Scope scope{"GPU.FlipFenceWait"};
    // Preparing the flip submitted the command buffers these fences follow: only a GPU fault
    // or a stopped completion thread keeps them from completing.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    if (!fence_labels->WaitPerformed(mark, stopping, deadline) && !stopping) {
        LOG_ERROR(Render, "GPU fences: a flip waited 5 s for {} fences; completing it anyway",
                  mark - fence_labels->Performed());
    }
}

std::string Liverpool::FenceCommand(const std::vector<std::string>& args) {
    const std::string mode = args.empty() ? "status" : args[0];
    if (mode == "flip_wait" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        flip_waits_fences.store(args[1] == "on", std::memory_order_relaxed);
    } else if (args.size() > 1 ||
               (mode != "status" && mode != "completion" && mode != "parse")) {
        return "usage: gpu_fences [status | completion | parse | flip_wait on|off]\n";
    }
    if (mode == "completion" || mode == "parse") {
        const bool enable = mode == "completion";
        SendCommand<true>([this, enable] {
            if (rasterizer) {
                // Fences deferred before the switch take effect before any later one.
                rasterizer->Finish();
                DrainFences();
            }
            completion_fences.store(enable, std::memory_order_relaxed);
        });
    }
    return fmt::format("mode={} flip_wait={} {}\n",
                       completion_fences.load(std::memory_order_relaxed) ? "completion" : "parse",
                       flip_waits_fences.load(std::memory_order_relaxed) ? "on" : "off",
                       fence_labels->Status());
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb, u64 submission) {
    FIBER_ENTER(ccb_task_name);

    // The CE copy is owned by the submission; its guest address is not carried here.
    const u64 trace_ib = Pm4Trace::NextIb();
    const auto trace_base = reinterpret_cast<uintptr_t>(ccb.data());
    if (Pm4Trace::Active())
        Pm4Trace::NoteIbBegin(trace_ib, submission, GfxQueueId, Pm4Trace::IbKind::Constant, 0,
                              static_cast<u32>(ccb.size()));
    while (!stopping && !ccb.empty()) {
        // A GPU replay capture runs injected commands only between queue resumes.
        if (!replay_capture) {
            ProcessCommands();
        }

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (Pm4Trace::Active())
            Pm4Trace::NotePacket(
                trace_ib, submission, GfxQueueId,
                static_cast<u32>((reinterpret_cast<uintptr_t>(header) - trace_base) / 4), 0,
                ccb.first(std::min<size_t>(type == 3 ? header->type3.NumWords() + 1 : 1,
                                           ccb.size())));
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            // const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            while ((cblock.de_count - cblock.ce_count) >= diff) {
                YIELD_CE();
            }
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCeUpdate(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, submission);
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        NotePm4Consumed();
        ccb = NextPacket(ccb, header->type3.NumWords() + 1);
    }
    if (Pm4Trace::Active())
        Pm4Trace::NoteIbEnd(trace_ib);

    FIBER_EXIT;
}

namespace {
// Fatal path only: the packet stream around an undecodable DCB packet, so the
// writer of the corrupt dwords can be identified from the log.
void LogBadPacket(const u32* begin, const u32* at, const u32* end, VAddr source) {
    const auto offset = static_cast<u64>(at - begin);
    LOG_CRITICAL(Render, "Bad PM4 packet at dword {} of DCB guest {:#x} ({} dwords)", offset,
                 source, static_cast<u64>(end - begin));
    const u32* from = at - std::min<u64>(offset, 96);
    const u32* to = at + std::min<u64>(static_cast<u64>(end - at), 32);
    for (const u32* p = from; p < to; p += 8) {
        std::string line;
        for (const u32* q = p; q < std::min(p + 8, to); ++q)
            line += q == at ? fmt::format(" [{:08x}]", *q) : fmt::format(" {:08x}", *q);
        LOG_CRITICAL(Render, "  {:+5}:{}", static_cast<s64>(p - at), line);
    }
}
} // namespace

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb,
                                           u64 submission, VAddr source, const u32* origin) {
    FIBER_ENTER(dcb_task_name);

    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    const u64 trace_ib = Pm4Trace::NextIb();
    if (Pm4Trace::Active())
        Pm4Trace::NoteIbBegin(trace_ib, submission, GfxQueueId, Pm4Trace::IbKind::Graphics,
                              source, static_cast<u32>(dcb.size()));
    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb, submission);
        RESUME_GFX(ce_task);
    }
    // Locals: host markers also follow RenderDoc attachment, and the rasterizer may be absent.
    const bool host_markers_enabled = rasterizer && rasterizer->HostMarkersEnabled();
    const bool guest_markers_enabled = rasterizer && this->guest_markers_enabled;

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    while (!stopping && !dcb.empty()) {
        if (!replay_capture) {
            ProcessCommands();
        }

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        if (host_markers_enabled)
            rasterizer->SetDiagnosticPacket(diagnostic_guest_flip + 1, submission, GfxQueueId,
                source ? source + reinterpret_cast<VAddr>(header) - base_addr : 0);
        const u32 type = header->type;
        if (Pm4Trace::Active())
            Pm4Trace::NotePacket(
                trace_ib, submission, GfxQueueId,
                static_cast<u32>((reinterpret_cast<uintptr_t>(header) - base_addr) / 4),
                source ? source + reinterpret_cast<VAddr>(header) - base_addr : 0,
                dcb.first(std::min<size_t>(type == 3 ? header->type3.NumWords() + 1 : 1,
                                           dcb.size())));

        switch (type) {
        default:
            UNREACHABLE_MSG("Wrong PM4 type {}", type);
            break;
        case 0:
            UNREACHABLE_MSG("Unimplemented PM4 type 0, base reg: {}, size: {}",
                            header->type0.base.Value(), header->type0.NumWords());
            break;
        case 2:
            // Type-2 packet are used for padding purposes
            NotePm4Consumed();
            dcb = NextPacket(dcb, 1);
            continue;
        case 3:
            const u32 count = header->type3.NumWords();
            const PM4ItOpcode opcode = header->type3.opcode;
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
                    ++flip_epoch;
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        rasterizer->ScopeMarkerBegin(label, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        rasterizer->ScopedMarkerInsertColor(label, color, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
                    if (guest_markers_enabled) {
                        rasterizer->ScopeMarkerEnd(true);
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState: {
                regs.SetDefaults();
                break;
            }
            case PM4ItOpcode::SetConfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ConfigRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);
                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
                break;
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);

                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                switch (reg_addr) {
                case ContextRegs::CbColor0Base:
                case ContextRegs::CbColor1Base:
                case ContextRegs::CbColor2Base:
                case ContextRegs::CbColor3Base:
                case ContextRegs::CbColor4Base:
                case ContextRegs::CbColor5Base:
                case ContextRegs::CbColor6Base:
                case ContextRegs::CbColor7Base: {
                    const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                            (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x0e || nop_offset == 0x0d || nop_offset == 0x0b) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    } else {
                        last_cb_extent[col_buf_id].raw = 0;
                    }
                    break;
                }
                case ContextRegs::CbColor0Cmask:
                case ContextRegs::CbColor1Cmask:
                case ContextRegs::CbColor2Cmask:
                case ContextRegs::CbColor3Cmask:
                case ContextRegs::CbColor4Cmask:
                case ContextRegs::CbColor5Cmask:
                case ContextRegs::CbColor6Cmask:
                case ContextRegs::CbColor7Cmask: {
                    const auto col_buf_id =
                        (reg_addr - ContextRegs::CbColor0Cmask) /
                        (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x04) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    }
                    break;
                }
                case ContextRegs::DbZInfo: {
                    if (header->type3.count == 8) {
                        ASSERT_MSG(payload[20] == 0xc0001000,
                                   "NOP hint is missing in DB setup sequence");
                        last_db_extent.raw = payload[21];
                    } else {
                        last_db_extent.raw = 0;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto set_size = (count - 1) * sizeof(u32);

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    ASSERT(set_size <= sizeof(ComputeProgram));
                    auto* addr = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state) +
                                 (set_data->reg_offset - 0x200);
                    std::memcpy(addr, header + 2, set_size);
                } else {
                    std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                                header + 2, set_size);
                }
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                std::memcpy(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset],
                            header + 2, (count - 1) * sizeof(u32));
                break;
            }
            case PM4ItOpcode::SetPredication: {
                LOG_WARNING(Render, "Unimplemented IT_SET_PREDICATION");
                dispatch_diag.predication_dw1 = count >= 1 ? header[1].raw : 0;
                dispatch_diag.predication_dw2 = count >= 2 ? header[2].raw : 0;
                ++dispatch_diag.predications;
                break;
            }
            case PM4ItOpcode::IndexType: {
                const auto* index_type = reinterpret_cast<const PM4CmdDrawIndexType*>(header);
                regs.index_buffer_type.raw = index_type->raw;
                break;
            }
            case PM4ItOpcode::DrawIndex2: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
                regs.max_index_size = draw_index->max_size;
                regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
                regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                draw_predicated = header->type3.predicate.Value() != PM4Predicate::PredDisable;
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DrawIndex2", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(true); });
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                regs.max_index_size = draw_index_off->max_size;
                regs.num_indices = draw_index_off->index_count;
                regs.draw_initiator = draw_index_off->draw_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                draw_predicated = header->type3.predicate.Value() != PM4Predicate::PredDisable;
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexOffset2", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->Draw(true, draw_index_off->index_offset); });
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                regs.num_indices = draw_index->index_count;
                regs.draw_initiator = draw_index->draw_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                draw_predicated = header->type3.predicate.Value() != PM4Predicate::PredDisable;
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DrawIndexAuto", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->Draw(false); });
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0,
                                                 draw_indirect->base_vtx_loc,
                                                 draw_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirect", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0,
                                                 draw_index_indirect->base_vtx_loc,
                                                 draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count, 0, draw_index_indirect->base_vtx_loc,
                            draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (!rasterizer) {
                    break;
                }
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DrawIndexIndirectCountMulti", fmt::make_format_args(cmd_address), [&] {
                        rasterizer->DrawIndirect(
                            true, indirect_args_addr, offset, draw_index_indirect->stride,
                            draw_index_indirect->count,
                            draw_index_indirect->count_indirect_enable.Value()
                                ? draw_index_indirect->count_addr
                                : 0,
                            draw_index_indirect->base_vtx_loc, draw_index_indirect->start_inst_loc);
                    });
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                dispatch_diag.queue = 0;
                dispatch_diag.predicated = header->type3.predicate.Value() != PM4Predicate::PredDisable;
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker("gfx:{}:DispatchDirect", fmt::make_format_args(cmd_address),
                                        [&] { rasterizer->DispatchDirect(); });
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                    break;
                }
                dispatch_diag.queue = 0;
                dispatch_diag.predicated = header->type3.predicate.Value() != PM4Predicate::PredDisable;
                const auto cmd_address = reinterpret_cast<const void*>(header);
                rasterizer->ScopeMarker(
                    "gfx:{}:DispatchIndirect", fmt::make_format_args(cmd_address),
                    [&] { rasterizer->DispatchIndirect(indirect_args_addr, offset, size); });
                break;
            }
            case PM4ItOpcode::NumInstances: {
                const auto* num_instances = reinterpret_cast<const PM4CmdDrawNumInstances*>(header);
                regs.num_instances.num_instances = num_instances->num_instances;
                break;
            }
            case PM4ItOpcode::IndexBase: {
                const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
                regs.index_base_address.base_addr_lo = index_base->addr_lo;
                regs.index_base_address.base_addr_hi = index_base->addr_hi;
                break;
            }
            case PM4ItOpcode::IndexBufferSize: {
                const auto* index_size = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header);
                regs.num_indices = index_size->num_indices;
                break;
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                ASSERT(set_base->base_index == PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable);
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
                LOG_DEBUG(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
                          magic_enum::enum_name(event->event_type.Value()),
                          magic_enum::enum_name(event->event_index.Value()));
                if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
                    // TODO: handle proper synchronization, for now signal that update is done
                    // immediately
                    regs.cp_strmout_cntl.offset_update_done = 1;
                } else if (event->event_index.Value() == EventIndex::ZpassDone) {
                    if (event->event_type.Value() == EventType::PixelPipeStatDump) {
                        static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
                        static constexpr u64 OcclusionCounterStep = 0x2FFFFFFULL;
                        u64* results = event->Address<u64*>();
                        for (s32 i = 0; i < num_counter_pairs; ++i, results += 2) {
                            *results = pixel_counter | OcclusionCounterValidMask;
                        }
                        pixel_counter += OcclusionCounterStep;
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEos: {
                const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                if (rasterizer) {
                    rasterizer->ProcessDownloadImages(!completion_fences);
                }
                event_eos->SignalFence([this](void* address, u64 data, u32 num_bytes) {
                    SignalFence({.address = reinterpret_cast<VAddr>(address),
                                 .size = num_bytes,
                                 .data = Fence::Data::Value,
                                 .through_backing = true,
                                 .value = data,
                                 .writer = "eos_label"});
                });
                if (event_eos->command == PM4CmdEventWriteEos::Command::GdsStore) {
                    ASSERT(event_eos->size == 1);
                    if (rasterizer) {
                        rasterizer->Finish();
                        // The GPU completed every queued fence: they precede the store.
                        fence_labels->PerformThrough(~u64{0}, true);
                        wait_debt = 0;
                        const u32 value = rasterizer->ReadDataFromGds(event_eos->gds_index);
                        *event_eos->Address() = value;
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEop: {
                const auto* event_eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                if (rasterizer) {
                    rasterizer->ProcessDownloadImages(!completion_fences);
                }
                SignalFence(EopFence(*event_eop));
                break;
            }
            case PM4ItOpcode::DmaData: {
                const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                    break;
                }
                ASSERT(dma_data->command.das == 0);
                if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                    rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(),
                                           dma_data->data, true);
                } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                            dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                           dma_data->dst_sel == DmaDataDst::Gds) {
                    rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                           dma_data->NumBytes(), true, false);
                } else if (dma_data->src_sel == DmaDataSrc::Data &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                           dma_data->data, false);
                } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                           dma_data->NumBytes(), false, true);
                } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                            dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                           (dma_data->dst_sel == DmaDataDst::Memory ||
                            dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                    rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(),
                                           dma_data->SrcAddress<VAddr>(), dma_data->NumBytes(),
                                           false, false);
                } else {
                    UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                    u32(dma_data->dst_sel));
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
                const u32 data_size = (header->type3.count.Value() - 2) * 4;
                u64* address = write_data->Address<u64*>();
                SettleWaits();
                if (!write_data->wr_one_addr.Value()) {
                    std::memcpy(address, write_data->data, data_size);
                } else {
                    UNREACHABLE();
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                LOG_WARNING(Render,
                            "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                            "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                            u32(copy_data->src_sel.Value()), u32(copy_data->dst_sel.Value()),
                            copy_data->count_sel.Value(), copy_data->wr_confirm.Value(),
                            u32(copy_data->engine_sel.Value()));
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                if (mem_semaphore->IsSignaling()) {
                    SettleWaits();
                    mem_semaphore->Signal();
                } else {
                    while (!Poll(VideoCore::Replay::WaitKind::MemSemaphore,
                                 mem_semaphore->Address<void*>(),
                                 [&] { return mem_semaphore->Signaled(); })) {
                        YIELD_WAIT_GFX();
                    }
                    mem_semaphore->Decrement();
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
                break;
            }
            case PM4ItOpcode::Rewind: {
                if (!rasterizer) {
                    break;
                }
                // An owned copy waits for the guest to validate its own command buffer, then
                // reads the packets the guest wrote after the REWIND again.
                const auto offset = reinterpret_cast<const u32*>(header) -
                                    reinterpret_cast<const u32*>(base_addr);
                const auto* rewind = reinterpret_cast<const PM4CmdRewind*>(
                    origin ? origin + offset : reinterpret_cast<const u32*>(header));
                while (!Poll(VideoCore::Replay::WaitKind::Rewind, rewind,
                             [&] { return rewind->Valid(); })) {
                    YIELD_WAIT_GFX();
                }
                if (origin) {
                    std::memcpy(const_cast<u32*>(dcb.data()), origin + offset, dcb.size_bytes());
                }
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                const u64* wait_addr = wait_reg_mem->Address<u64*>();
                const bool vo_label = vo_port && vo_port->IsVoLabel(wait_addr);
                if (vo_label && !replay_capture &&
                    num_submits == mapped_queues[GfxQueueId].submits.size()) {
                    Core::Diagnostics::Handoff::Scope wait_scope{
                        "PM4.WaitVideoOutLabel", diagnostics ? diagnostics->Generation() : 0,
                        static_cast<u64>(wait_addr - vo_port->buffer_labels.data()) + 1, 0, true};
                    // The thread blocks until a flip retires: fences of work before the wait
                    // must not stay in the open command buffer meanwhile.
                    if (!TestWait(*wait_reg_mem)) {
                        FlushFences(fence_labels->flushes_idle);
                    }
                    vo_port->WaitVoLabel([&] { return TestWait(*wait_reg_mem); });
                    break;
                }
                while (!Poll(vo_label ? VideoCore::Replay::WaitKind::VoLabel
                                      : VideoCore::Replay::WaitKind::RegMem,
                             wait_addr, [&] { return TestWait(*wait_reg_mem); })) {
                    YIELD_WAIT_GFX();
                }
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {}, submission, reinterpret_cast<VAddr>(indirect_buffer->Address<const u32>()));
                RESUME_GFX(task);

                while (!task.handle.done()) {
                    YIELD_GFX();
                    RESUME_GFX(task);
                }
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    RESUME_GFX(ce_task);
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                LOG_WARNING(Render_Vulkan,
                            "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                            "source_select = {}, buffer_select = {}",
                            strmout->update_memory.Value(),
                            magic_enum::enum_name(strmout->source_select.Value()),
                            strmout->buffer_select.Value());
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                LOG_WARNING(Render_Vulkan, "Unimplemented IT_GET_LOD_STATS");
                break;
            }
            case PM4ItOpcode::CondExec: {
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
                }
                const auto skip = *cond_exec->Address() == false;
                if (skip) {
                    dcb = NextPacket(dcb,
                                     header->type3.NumWords() + 1 + cond_exec->exec_count.Value());
                    continue;
                }
                break;
            }
            default:
                LogBadPacket(reinterpret_cast<const u32*>(base_addr),
                             reinterpret_cast<const u32*>(header), dcb.data() + dcb.size(), source);
                UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                                static_cast<u32>(opcode), count);
            }
            NotePm4Consumed();
            dcb = NextPacket(dcb, header->type3.NumWords() + 1);
            break;
        }
    }

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
    }
    if (Pm4Trace::Active())
        Pm4Trace::NoteIbEnd(trace_ib);

    FIBER_EXIT;
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid, u64 submission,
                                          VAddr source, const u32* origin) {
    FIBER_ENTER(acb_task_name[vqid]);
    auto& queue = asc_queues[{vqid}];
    const bool host_markers_enabled = rasterizer && rasterizer->HostMarkersEnabled();

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    const u64 trace_ib = Pm4Trace::NextIb();
    if (Pm4Trace::Active())
        Pm4Trace::NoteIbBegin(trace_ib, submission, vqid + 1, Pm4Trace::IbKind::Compute, source,
                              static_cast<u32>(acb.size()));
    while (!stopping && !acb.empty()) {
        if (!replay_capture) {
            ProcessCommands();
        }

        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        // A packet completed from the previous ring span has no single guest address.
        const bool trace_split = queue.tmp_dwords > 0;
        const auto trace_offset = static_cast<u32>((reinterpret_cast<VAddr>(header) - base_addr) / 4);
        if (host_markers_enabled)
            rasterizer->SetDiagnosticPacket(0, submission, vqid + 1,
                // A split packet started in the previous ring span. Its original
                // address is not retained; never identify it by the tail span.
                source && queue.tmp_dwords == 0
                    ? source + reinterpret_cast<VAddr>(header) - base_addr : 0);
        u32 next_dw_off = header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            next_dw_off = header->type3.NumWords() + 1 - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                *queue.read_addr += acb.size();
                *queue.read_addr %= queue.ring_size_dw;
            }
            break;
        }

        if (Pm4Trace::Active()) {
            const u32 words = header->type == 3 ? header->type3.NumWords() + 1 : 1;
            Pm4Trace::NotePacket(trace_ib, submission, vqid + 1, trace_offset,
                                 source && !trace_split ? source + VAddr(trace_offset) * 4 : 0,
                                 {reinterpret_cast<const u32*>(header), words});
        }
        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            NotePm4Consumed();
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;

        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid, submission, reinterpret_cast<VAddr>(indirect_buffer->Address<const u32>()));
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC(vqid);
                RESUME_ASC(task, vqid);
            }
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            ASSERT(dma_data->command.das == 0);
            if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(), dma_data->data,
                                       true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                       dma_data->NumBytes(), true, false);
            } else if (dma_data->src_sel == DmaDataSrc::Data &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                       dma_data->data, false);
            } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                       dma_data->NumBytes(), false, true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                // The guest patches a DISPATCH_DIRECT later in its ring: in an owned copy,
                // the same packet of the copy.
                const VAddr ring = origin ? reinterpret_cast<VAddr>(origin) : base_addr;
                const bool in_ring = dst_addr >= ring + sizeof(PM4Header) &&
                                     dst_addr < ring + acb_size;
                const PM4Header* header = reinterpret_cast<const PM4Header*>(
                    base_addr + (dst_addr - ring) - sizeof(PM4Header));
                if (in_ring && num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                } else {
                    rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}", u32(dma_data->src_sel),
                                u32(dma_data->dst_sel));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            if (!rasterizer) {
                break;
            }
            // As on the graphics ring; a packet completed from the previous ring span is not
            // in the copy.
            const auto* packet = reinterpret_cast<const u32*>(header);
            const auto* copy = reinterpret_cast<const u32*>(base_addr);
            const bool in_copy = origin && packet >= copy && packet < copy + acb_size / 4;
            const auto* rewind = reinterpret_cast<const PM4CmdRewind*>(
                in_copy ? origin + (packet - copy) : packet);
            while (!Poll(VideoCore::Replay::WaitKind::Rewind, rewind,
                         [&] { return rewind->Valid(); })) {
                YIELD_ASC(vqid);
            }
            if (in_copy) {
                std::memcpy(const_cast<u32*>(acb.data()), origin + (acb.data() - copy),
                            acb.size_bytes());
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                ASSERT(set_size <= sizeof(ComputeProgram));
                auto* addr = reinterpret_cast<u32*>(&mapped_queues[vqid + 1].cs_state) +
                             (set_data->reg_offset - 0x200);
                std::memcpy(addr, header + 2, set_size);
            } else {
                std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                            header + 2, set_size);
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x}",
                        set_data->vqid.Value(), set_data->reg_offset.Value());
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                rasterizer->DispatchIndirect(it->indirect_addr, 0, size);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            dispatch_diag.queue = vqid + 1;
            dispatch_diag.predicated = header->type3.predicate.Value() != PM4Predicate::PredDisable;
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchDirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { rasterizer->DispatchDirect(); });
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (!rasterizer || (cs_program.dispatch_initiator & 1) == 0) {
                break;
            }
            dispatch_diag.queue = vqid + 1;
            dispatch_diag.predicated = header->type3.predicate.Value() != PM4Predicate::PredDisable;
            const auto cmd_address = reinterpret_cast<const void*>(header);
            rasterizer->ScopeMarker("asc[{}]:{}:DispatchIndirect",
                                    fmt::make_format_args(vqid, cmd_address),
                                    [&] { rasterizer->DispatchIndirect(ib_address, 0, size); });
            break;
        }
        case PM4ItOpcode::WriteData: {
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            SettleWaits();
            if (!write_data->wr_one_addr.Value()) {
                std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            if (mem_semaphore->IsSignaling()) {
                SettleWaits();
                mem_semaphore->Signal();
            } else {
                while (!Poll(VideoCore::Replay::WaitKind::MemSemaphore,
                             mem_semaphore->Address<void*>(),
                             [&] { return mem_semaphore->Signaled(); })) {
                    YIELD_WAIT_ASC(vqid);
                }
                mem_semaphore->Decrement();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            while (!Poll(VideoCore::Replay::WaitKind::RegMem, wait_reg_mem->Address<void*>(),
                         [&] { return TestWait(*wait_reg_mem); })) {
                YIELD_WAIT_ASC(vqid);
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            const auto* release_mem = reinterpret_cast<const PM4CmdReleaseMem*>(header);
            if (rasterizer) {
                rasterizer->ProcessDownloadImages(!completion_fences);
                if (release_mem->data_sel == DataSelect::GdsMemStore) {
                    rasterizer->CopyBuffer(release_mem->Address<VAddr>(), release_mem->gds_index,
                                           release_mem->num_dw * sizeof(u32), false, true);
                }
            }
            SignalFence(ReleaseMemFence(*release_mem, queue.pipe_id));
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }

        NotePm4Consumed();
        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            *queue.read_addr += next_dw_off;
            *queue.read_addr %= queue.ring_size_dw;
        }
    }

    FIBER_EXIT;
}

Liverpool::CmdBuffer Liverpool::CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    ASSERT_MSG(queue.dcb_buffer.capacity() >= queue.dcb_buffer_offset + dcb.size(),
               "dcb copy buffer out of reserved space");
    ASSERT_MSG(queue.ccb_buffer.capacity() >= queue.ccb_buffer_offset + ccb.size(),
               "ccb copy buffer out of reserved space");

    queue.dcb_buffer.resize(
        std::max(queue.dcb_buffer.size(), queue.dcb_buffer_offset + dcb.size()));
    queue.ccb_buffer.resize(
        std::max(queue.ccb_buffer.size(), queue.ccb_buffer_offset + ccb.size()));

    const u32 prev_dcb_buffer_offset = queue.dcb_buffer_offset;
    const u32 prev_ccb_buffer_offset = queue.ccb_buffer_offset;
    if (!dcb.empty()) {
        std::memcpy(queue.dcb_buffer.data() + queue.dcb_buffer_offset, dcb.data(),
                    dcb.size_bytes());
        queue.dcb_buffer_offset += dcb.size();
        dcb = std::span<const u32>{queue.dcb_buffer.begin() + prev_dcb_buffer_offset,
                                   queue.dcb_buffer.begin() + queue.dcb_buffer_offset};
    }

    if (!ccb.empty()) {
        std::memcpy(queue.ccb_buffer.data() + queue.ccb_buffer_offset, ccb.data(),
                    ccb.size_bytes());
        queue.ccb_buffer_offset += ccb.size();
        ccb = std::span<const u32>{queue.ccb_buffer.begin() + prev_ccb_buffer_offset,
                                   queue.ccb_buffer.begin() + queue.ccb_buffer_offset};
    }

    return std::make_pair(dcb, ccb);
}

Liverpool::Task Liverpool::ProcessOwnedGraphics(std::vector<u32> dcb, std::vector<u32> ccb,
                                                u64 submission, VAddr source, const u32* origin) {
    auto task = ProcessGraphics(dcb, ccb, submission, source, origin);
    while (!task.handle.done() && !stopping) {
        task.handle.resume();
        if (!task.handle.done())
            co_yield {};
    }
    if (task.handle.promise().error)
        std::rethrow_exception(task.handle.promise().error);
}

namespace {
// Diagnostic (debug.shadps4.pm4_validate=1): check each submitted DCB copy for
// undecodable packet headers. On a hit, log the packets around it and whether
// guest memory still changes after the copy, which tells a producer that has
// not finished writing (ordering/race) from wrongly written bytes.
bool Pm4ValidateEnabled() {
#if defined(__ANDROID__)
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.shadps4.pm4_validate", value);
        return std::string_view(value) == "1";
    }();
    return enabled;
#else
    return false;
#endif
}
bool KnownType3Opcode(PM4ItOpcode op) {
    switch (op) {
    case PM4ItOpcode::Nop:
    case PM4ItOpcode::SetBase:
    case PM4ItOpcode::ClearState:
    case PM4ItOpcode::IndexBufferSize:
    case PM4ItOpcode::DispatchDirect:
    case PM4ItOpcode::DispatchIndirect:
    case PM4ItOpcode::AtomicGds:
    case PM4ItOpcode::Atomic:
    case PM4ItOpcode::OcclusionQuery:
    case PM4ItOpcode::SetPredication:
    case PM4ItOpcode::RegRmw:
    case PM4ItOpcode::CondExec:
    case PM4ItOpcode::PredExec:
    case PM4ItOpcode::DrawIndirect:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::IndexBase:
    case PM4ItOpcode::DrawIndex2:
    case PM4ItOpcode::ContextControl:
    case PM4ItOpcode::IndexType:
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexAuto:
    case PM4ItOpcode::NumInstances:
    case PM4ItOpcode::DrawIndexMultiAuto:
    case PM4ItOpcode::IndirectBufferConst:
    case PM4ItOpcode::StrmoutBufferUpdate:
    case PM4ItOpcode::DrawIndexOffset2:
    case PM4ItOpcode::WriteData:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::MemSemaphore:
    case PM4ItOpcode::WaitRegMem:
    case PM4ItOpcode::IndirectBuffer:
    case PM4ItOpcode::CopyData:
    case PM4ItOpcode::CommandProcessorDma:
    case PM4ItOpcode::PfpSyncMe:
    case PM4ItOpcode::SurfaceSync:
    case PM4ItOpcode::CondWrite:
    case PM4ItOpcode::EventWrite:
    case PM4ItOpcode::EventWriteEop:
    case PM4ItOpcode::EventWriteEos:
    case PM4ItOpcode::ReleaseMem:
    case PM4ItOpcode::PreambleCntl:
    case PM4ItOpcode::DmaData:
    case PM4ItOpcode::ContextRegRmw:
    case PM4ItOpcode::AcquireMem:
    case PM4ItOpcode::Rewind:
    case PM4ItOpcode::LoadShReg:
    case PM4ItOpcode::LoadConfigReg:
    case PM4ItOpcode::LoadContextReg:
    case PM4ItOpcode::SetConfigReg:
    case PM4ItOpcode::SetContextReg:
    case PM4ItOpcode::SetContextRegIndirect:
    case PM4ItOpcode::SetShReg:
    case PM4ItOpcode::SetShRegOffset:
    case PM4ItOpcode::SetQueueReg:
    case PM4ItOpcode::SetUconfigReg:
    case PM4ItOpcode::LoadConstRam:
    case PM4ItOpcode::WriteConstRam:
    case PM4ItOpcode::DumpConstRam:
    case PM4ItOpcode::IncrementCeCounter:
    case PM4ItOpcode::IncrementDeCounter:
    case PM4ItOpcode::WaitOnCeCounter:
    case PM4ItOpcode::WaitOnDeCounterDiff:
    case PM4ItOpcode::GetLodStats:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
        return true;
    default:
        return false;
    }
}
// Offset (dwords) of the first undecodable packet, or size when all decode.
size_t FirstBadPacket(std::span<const u32> dcb) {
    size_t i = 0;
    while (i < dcb.size()) {
        const auto* header = reinterpret_cast<const PM4Header*>(&dcb[i]);
        if (header->type == 2) {
            ++i;
            continue;
        }
        if (header->type != 3 || !KnownType3Opcode(PM4ItOpcode(header->type3.opcode)))
            return i;
        i += header->type3.NumWords() + 1;
    }
    return dcb.size();
}
void ValidateSubmission(std::span<const u32> copy, std::span<const u32> live, VAddr source) {
    const size_t bad = FirstBadPacket(copy);
    if (bad >= copy.size())
        return;
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 8)
        return;
    LOG_CRITICAL(Render, "pm4_validate: undecodable packet in submitted DCB guest {:#x}", source);
    LogBadPacket(copy.data(), copy.data() + bad, copy.data() + copy.size(), source);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    size_t differing = 0;
    for (size_t i = 0; i < copy.size(); ++i) {
        if (copy[i] == live[i])
            continue;
        if (differing++ < 12)
            LOG_CRITICAL(Render, "pm4_validate: dword {} ({:+}) copy {:08x} now {:08x}", i,
                         static_cast<s64>(i) - static_cast<s64>(bad), copy[i], live[i]);
    }
    LOG_CRITICAL(Render,
                 "pm4_validate: {} of {} dwords changed 2 ms after the copy; live "
                 "memory {} the bad packet",
                 differing, copy.size(),
                 FirstBadPacket(live) == bad ? "still has" : "no longer has");
}
} // namespace

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb, VAddr source) {
    auto& queue = mapped_queues[GfxQueueId];
    const auto submission = Core::Diagnostics::Handoff::NextId();

    if (Pm4Trace::Active())
        Pm4Trace::NoteSubmit(GfxQueueId, submission, source, static_cast<u32>(dcb.size()),
                             reinterpret_cast<u64>(ccb.data()), static_cast<u32>(ccb.size()));
    // The guest's command buffers, before any copy.
    const VideoCore::Replay::SubmitRecord replay_submit{
        static_cast<u32>(VideoCore::Replay::SubmitQueue::Graphics),
        0,
        submission,
        reinterpret_cast<u64>(dcb.data()),
        dcb.size(),
        reinterpret_cast<u64>(ccb.data()),
        ccb.size(),
        source};
    if (!owned_submissions && EmulatorSettings.IsCopyGpuBuffers()) {
        std::tie(dcb, ccb) = CopyCmdBuffers(dcb, ccb);
    }

    std::vector<u32> owned_dcb;
    std::vector<u32> owned_ccb;
    std::span<const u32> replay_dcb = dcb;
    std::span<const u32> replay_ccb = ccb;
    if (owned_submissions) {
        owned_dcb.assign(dcb.begin(), dcb.end());
        owned_ccb.assign(ccb.begin(), ccb.end());
        replay_dcb = owned_dcb;
        replay_ccb = owned_ccb;
        if (Pm4ValidateEnabled())
            ValidateSubmission(owned_dcb, dcb, source);
    }
    auto task = owned_submissions
                    ? ProcessOwnedGraphics(std::move(owned_dcb), std::move(owned_ccb),
                                           submission, source, dcb.data())
                    : ProcessGraphics(dcb, ccb, submission, source);
    const auto generation = diagnostics ? diagnostics->Generation() : 0;
    task.handle.promise().diagnostic_id = submission;
    task.handle.promise().replay_submit = replay_submit;
    task.handle.promise().replay_dcb = replay_dcb;
    task.handle.promise().replay_ccb = replay_ccb;
    {
        std::scoped_lock lock{queue.m_access};
        SHAD_HANDOFF(generation, "queue_enqueue", GfxQueueId, task.handle.promise().diagnostic_id, dcb.size());
        queue.submits.emplace(task.Release());
        // Publish count before releasing the queue to an already-running consumer.
        ++num_submits;
    }

    // WaitGpuIdle callers wait on the same condition: wake everyone so the command processor
    // cannot miss it.
    std::scoped_lock lk{submit_mutex};
    submit_cv.notify_all();
}

Liverpool::Task Liverpool::ProcessOwnedCompute(std::vector<u32> acb, u32 vqid, u64 submission,
                                               VAddr source, const u32* origin) {
    auto task = ProcessCompute(acb, vqid, submission, source, origin);
    while (!task.handle.done() && !stopping) {
        task.handle.resume();
        if (!task.handle.done())
            co_yield {};
    }
    if (task.handle.promise().error)
        std::rethrow_exception(task.handle.promise().error);
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    const auto submission = Core::Diagnostics::Handoff::NextId();
    const auto source = reinterpret_cast<VAddr>(acb.data());
    if (Pm4Trace::Active())
        Pm4Trace::NoteSubmit(gnm_vqid, submission, source, static_cast<u32>(acb.size()), 0, 0);
    auto task = owned_submissions ? ProcessOwnedCompute({acb.begin(), acb.end()}, vqid,
                                                        submission, source, acb.data())
                                  : ProcessCompute(acb, vqid, submission, source);
    const auto generation = diagnostics ? diagnostics->Generation() : 0;
    task.handle.promise().diagnostic_id = submission;
    task.handle.promise().replay_submit = {
        static_cast<u32>(VideoCore::Replay::SubmitQueue::Compute), gnm_vqid, submission, source,
        acb.size(), 0, 0, source};
    {
        std::scoped_lock lock{queue.m_access};
        SHAD_HANDOFF(generation, "queue_enqueue", gnm_vqid, task.handle.promise().diagnostic_id, acb.size());
        queue.submits.emplace(task.Release());
        ++num_submits;
    }

    std::scoped_lock lk{submit_mutex};
    num_mapped_queues = std::max(num_mapped_queues.load(), gnm_vqid + 1);
    submit_cv.notify_all();
}

} // namespace AmdGpu
