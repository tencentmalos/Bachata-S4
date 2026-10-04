// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>
#include <queue>

#include "common/assert.h"
#include "core/diagnostics/diagnostics_hub_registry.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/fence_labels.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/replay/gpu_replay_format.h"
#include "video_core/replay/gpu_replay_hooks.h"

namespace Vulkan {
class Rasterizer;
}

namespace Libraries::VideoOut {
struct VideoOutPort;
}

namespace VideoCore::Replay {
class PayloadBuilder;
class Player;
}

namespace AmdGpu {

struct PM4CmdWaitRegMem;

struct Liverpool {
    // PM4 owner only. Identifies intervals ending in an accepted VideoOut flip,
    // not a CPU function invocation or a host present.
    u64 diagnostic_guest_flip{};
    std::shared_ptr<Core::Diagnostics::DiagnosticsPublisher> diagnostics{Core::Diagnostics::DiagnosticsHub::Instance().Acquire()};
    // PM4 progress is published in batches: one clock read per packet cost ~3% of the
    // command processor. The owner flushes whenever a queue task suspends or returns.
    static constexpr u32 Pm4ProgressBatch = 64;
    u32 pm4_progress_pending{};
    void NotePm4Consumed() noexcept {
        if (diagnostics && ++pm4_progress_pending >= Pm4ProgressBatch) {
            FlushPm4Progress();
        }
    }
    void FlushPm4Progress() noexcept {
        if (pm4_progress_pending == 0 || !diagnostics) {
            return;
        }
        diagnostics->Advance(Core::Diagnostics::AdvanceSignal::Pm4Consumed,
                             Core::Diagnostics::DiagnosticNowNs(), pm4_progress_pending);
        pm4_progress_pending = 0;
    }
    static constexpr u32 GfxQueueId = 0u;
    static constexpr u32 NumGfxRings = 1u;     // actually 2, but HP is reserved by system software
    static constexpr u32 NumComputePipes = 7u; // actually 8, but #7 is reserved by system software
    static constexpr u32 NumQueuesPerPipe = 8u;
    static constexpr u32 NumComputeRings = NumComputePipes * NumQueuesPerPipe;
    static constexpr u32 NumTotalQueues = NumGfxRings + NumComputeRings;
    static_assert(NumTotalQueues < 64u); // need to fit into u64 bitmap for ffs

    enum ContextRegs : u32 {
        DbZInfo = 0xA010,
        CbColor0Base = 0xA318,
        CbColor1Base = 0xA327,
        CbColor2Base = 0xA336,
        CbColor3Base = 0xA345,
        CbColor4Base = 0xA354,
        CbColor5Base = 0xA363,
        CbColor6Base = 0xA372,
        CbColor7Base = 0xA381,
        CbColor0Cmask = 0xA31F,
        CbColor1Cmask = 0xA32E,
        CbColor2Cmask = 0xA33D,
        CbColor3Cmask = 0xA34C,
        CbColor4Cmask = 0xA35B,
        CbColor5Cmask = 0xA36A,
        CbColor6Cmask = 0xA379,
        CbColor7Cmask = 0xA388,
    };

    std::atomic<bool> stopping{};
    /// Set by a queue task that yields to wait for memory; command processor thread only.
    bool waiting_yield{};
    std::atomic<bool> processing{};
    Regs regs{};
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> last_cb_extent{};
    CbDbExtent last_db_extent{};

public:
    explicit Liverpool();
    ~Liverpool();
    void RequestStop();
    std::function<void(std::exception_ptr)> fault_handler;
    std::mutex failure_mutex;
    std::exception_ptr failure;
    void CheckFault() {
        std::scoped_lock lock(failure_mutex);
        if (failure) std::rethrow_exception(failure);
    }
    void ReportFault(std::exception_ptr error) {
        { std::scoped_lock lock(failure_mutex); if (failure) return; failure = error; }
        if (fault_handler) fault_handler(error);
        else std::rethrow_exception(error);
    }
    void UseOwnedSubmissions() {
        owned_submissions = true;
    }
    /// Submitted DCB/CCB/ACBs are copied at submission and parsed from the copy, so a guest
    /// write after submitting cannot change what the command processor reads, and a GPU replay
    /// sees the commands of the submission.
    bool owned_submissions{true};
    bool StopRequested() const {
        return stopping.load();
    }

    void SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb, VAddr source = 0);
    void SubmitAsc(u32 gnm_vqid, std::span<const u32> acb);

    void SubmitDone() noexcept {
        std::scoped_lock lk{submit_mutex};
        mapped_queues[GfxQueueId].ccb_buffer_offset = 0;
        mapped_queues[GfxQueueId].dcb_buffer_offset = 0;
        submit_done = true;
        submit_cv.notify_all();
    }

    void WaitGpuIdle() noexcept {
        std::unique_lock lk{submit_mutex};
        submit_cv.wait(lk, [this] { return stopping || num_submits == 0; });
    }

    bool IsGpuIdle() const {
        return num_submits == 0 && num_commands == 0 && !processing && !submit_done;
    }

    /// Waits until the completion thread performed every deferred fence. Command processor
    /// thread, after the scheduler finished the work the fences follow.
    void DrainFences();
    /// DebugBus gpu_fences: status | completion | parse | flip_wait on|off.
    std::string FenceCommand(const std::vector<std::string>& args);

    /// The fences deferred so far, for a flip prepared now. Command processor thread.
    u64 FenceMark() const {
        return fence_labels->Deferred();
    }
    /// Blocks until every fence deferred before `mark` took effect. VideoOut completes a flip
    /// (flip status and events, display buffer labels) only then: on hardware the flip follows
    /// the frame's end-of-pipe writes, and a guest that recycles a frame's memory once it
    /// flipped must not find late label writes landing in it.
    void WaitFences(u64 mark);

    /// GPU replay: the state that outlives a submission (registers, compute queue programs,
    /// CE RAM and counters, ring queues). Command processor thread, with every queue idle.
    void SaveReplayState(VideoCore::Replay::PayloadBuilder& state,
                         VideoCore::Replay::PayloadBuilder& ring_queues);
    /// GPU replay: restores what SaveReplayState wrote, before any submission.
    bool LoadReplayState(std::span<const u8> state, std::span<const u8> ring_queues,
                         std::string& error);
    /// GPU replay: the command processor replays the player's events, then returns to normal
    /// operation. The queues run only when the trace resumes them.
    void StartReplay(VideoCore::Replay::Player* player);

    Vulkan::Rasterizer* GetRasterizer() const {
        return rasterizer;
    }

    void SetVoPort(Libraries::VideoOut::VideoOutPort* port) {
        vo_port = port;
    }

    /// GPU replay capture: injected commands run only between queue resumes, and the VideoOut
    /// label wait yields like other waits instead of blocking. Command processor thread.
    void SetReplayCapture(bool capture) {
        replay_capture = capture;
    }

    bool IsGpuThread() const {
        return std::this_thread::get_id() == gpu_id;
    }

    void BindRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }

    template <bool wait_done = false>
    void SendCommand(auto&& func) {
        CheckFault();
        if (std::this_thread::get_id() == gpu_id) {
            return func();
        }
        if constexpr (wait_done) {
            std::binary_semaphore sem{0};
            std::exception_ptr error;
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace([this, &sem, &func, &error] {
                    try {
                        CheckFault();
                    func();
                    } catch (...) {
                        error = std::current_exception();
                    }
                    sem.release();
                });
                ++num_commands;
                submit_cv.notify_all();
            }
            sem.acquire();
            if (error)
                std::rethrow_exception(error);
        } else {
            std::scoped_lock lk{submit_mutex};
            command_queue.emplace(std::move(func));
            ++num_commands;
            submit_cv.notify_all();
        }
    }

    void ReserveCopyBufferSpace() {
        GpuQueue& gfx_queue = mapped_queues[GfxQueueId];
        std::scoped_lock lk(gfx_queue.m_access);
        constexpr size_t GfxReservedSize = 2_MB >> 2;
        gfx_queue.ccb_buffer.reserve(GfxReservedSize);
        gfx_queue.dcb_buffer.reserve(GfxReservedSize);
    }

    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }

    /// PM4 context of the dispatch being processed, for GPU hang diagnostics only.
    struct DispatchDiagnostics {
        u32 queue{};           ///< 0: graphics ring, otherwise compute vqid + 1.
        bool predicated{};     ///< Type-3 header predicate bit of the dispatch packet.
        u32 predication_dw1{}; ///< Last SET_PREDICATION on the graphics ring (raw dwords).
        u32 predication_dw2{};
        u32 predications{};    ///< SET_PREDICATION packets seen.
    };
    DispatchDiagnostics dispatch_diag{};
    /// Type-3 header predicate bit of the draw packet being executed (direct draws).
    bool draw_predicated{};
    /// Patched flip packets processed on the graphics ring: frame boundaries in command order.
    u64 flip_epoch{};

    struct AscQueueInfo {
        static constexpr size_t Pm4BufferSize = 1024;
        VAddr map_addr;
        u32* read_addr;
        u32 ring_size_dw;
        u32 pipe_id;
        std::array<u32, Pm4BufferSize> tmp_packet;
        u32 tmp_dwords;
    };
    Common::SlotVector<AscQueueInfo> asc_queues{};

private:
    struct Task {
        struct promise_type {
            auto get_return_object() {
                Task task{};
                task.handle = std::coroutine_handle<promise_type>::from_promise(*this);
                return task;
            }
            static constexpr std::suspend_always initial_suspend() noexcept {
                // We want the task to be suspended at start
                return {};
            }
            static constexpr std::suspend_always final_suspend() noexcept {
                return {};
            }
            std::exception_ptr error;
            void unhandled_exception() {
                error = std::current_exception();
            }
            u64 diagnostic_id{};
            /// The top-level command buffer this task runs, for GPU replay capture.
            VideoCore::Replay::SubmitRecord replay_submit{};
            bool replay_resumed{};
            void return_void() {}
            struct empty {};
            std::suspend_always yield_value(empty&&) {
                return {};
            }
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle{};
        Task() = default;
        Task(const Task&) = delete;
        Task(Task&& other) noexcept : handle(std::exchange(other.handle, {})) {}
        Task& operator=(Task&& other) noexcept {
            if (this != &other) {
                if (handle)
                    handle.destroy();
                handle = std::exchange(other.handle, {});
            }
            return *this;
        }
        ~Task() {
            if (handle)
                handle.destroy();
        }
        Handle Release() {
            return std::exchange(handle, {});
        }
    };

    using CmdBuffer = std::pair<std::span<const u32>, std::span<const u32>>;
    // `origin` is the guest's command buffer when the task parses an owned copy of it: a
    // REWIND packet waits for the guest to validate it there, and later packets are read again.
    Task ProcessOwnedCompute(std::vector<u32> acb, u32 vqid, u64 submission, VAddr source,
                             const u32* origin);
    CmdBuffer CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb);
    Task ProcessOwnedGraphics(std::vector<u32> dcb, std::vector<u32> ccb, u64 submission,
                              VAddr source, const u32* origin);
    Task ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb, u64 submission,
                         VAddr source, const u32* origin = nullptr);
    Task ProcessCeUpdate(std::span<const u32> ccb, u64 submission = 0);
    template <bool is_indirect = false>
    Task ProcessCompute(std::span<const u32> acb, u32 vqid, u64 submission, VAddr source,
                        const u32* origin = nullptr);

    void ProcessCommands();
    /// Evaluates a wait packet's condition. A GPU replay capture records the result; a replay
    /// first applies the guest writes recorded before the wait and returns the recorded result.
    template <typename Condition>
    bool Poll(VideoCore::Replay::WaitKind kind, const void* address, Condition&& condition) {
        if (replay_player) [[unlikely]] {
            BeforeReplayPoll();
            return AfterReplayPoll(kind, address, condition());
        }
        const bool satisfied = condition();
        if (VideoCore::Replay::CaptureHooksActive()) [[unlikely]] {
            RecordPoll(kind, address, satisfied);
        }
        return satisfied;
    }
    void RecordPoll(VideoCore::Replay::WaitKind kind, const void* address, bool satisfied);
    void BeforeReplayPoll();
    bool AfterReplayPoll(VideoCore::Replay::WaitKind kind, const void* address, bool satisfied);
    void RunReplay();
    /// Every queue with work is waiting on memory: spin briefly, then yield, then sleep until a
    /// new submission or command (bounded).
    void IdleBackoff(u32 idle_rounds);
    void Process(std::stop_token stoken);

    struct GpuQueue {
        std::mutex m_access{};
        std::atomic<u32> dcb_buffer_offset;
        std::atomic<u32> ccb_buffer_offset;
        std::vector<u32> dcb_buffer;
        std::vector<u32> ccb_buffer;
        std::queue<Task::Handle> submits{};
        ComputeProgram cs_state{};
    };
    /// Resumes a queue's front task once (curr_qid is set). True when it yielded to wait.
    bool ResumeTask(GpuQueue& queue, Task::Handle task);
    std::array<GpuQueue, NumTotalQueues> mapped_queues{};
    std::atomic<u32> num_mapped_queues{1u}; // GFX is always available

    VAddr indirect_args_addr{};
    u32 num_counter_pairs{};
    u64 pixel_counter{};

    struct ConstantEngine {
        void Reset() {
            ce_count = 0;
            de_count = 0;
            ce_compare_count = 0;
        }

        [[nodiscard]] u32 Diff() const {
            ASSERT_MSG(ce_count >= de_count, "DE counter is ahead of CE");
            return ce_count - de_count;
        }

        u32 ce_compare_count{};
        u32 ce_count{};
        u32 de_count{};
        static std::array<u8, 48_KB> constants_heap;
    } cblock{};

    Vulkan::Rasterizer* rasterizer{};
    Libraries::VideoOut::VideoOutPort* vo_port{};
    const bool guest_markers_enabled;
    std::jthread process_thread{};
    std::atomic<u32> num_submits{};
    std::atomic<u32> num_commands{};
    std::atomic<bool> submit_done{};
    std::mutex submit_mutex;
    std::condition_variable_any submit_cv;
    std::queue<Common::UniqueFunction<void>> command_queue{};
    std::thread::id gpu_id;
    s32 curr_qid{-1};
    bool replay_capture{};
    VideoCore::Replay::Player* replay_player{};

    /// Performs an EOP/EOS/RELEASE_MEM write and interrupt when the GPU completed the work
    /// recorded before it (or at once, without completion fences or a rasterizer).
    void SignalFence(Fence fence);
    /// The open command buffer holds work a deferred fence waits for.
    bool FencesUnsubmitted() const;
    /// Submits the open command buffer when a deferred fence waits for it.
    void FlushFences(std::atomic<u64>& reason);
    /// Shared with the operations the scheduler's completion thread runs.
    std::shared_ptr<FenceLabels> fence_labels{std::make_shared<FenceLabels>()};
    /// The condition of a WAIT_REG_MEM packet, reading memory as the command processor sees it.
    /// A wait met by a pending fence value raises wait_debt to that fence.
    bool TestWait(const PM4CmdWaitRegMem& wait);
    /// Before a write the guest sees at once (WRITE_DATA, a semaphore signal): performs the
    /// fences that waits met by pending values relied on. On hardware those waits held the
    /// command processor until the fences took effect, so the guest never sees the write first
    /// (a game recycles a command buffer once a WRITE_DATA after such a wait says the GPU is
    /// done with it, and a late label write would land in the new commands).
    void SettleWaits();
    /// Newest fence sequence a met wait read a pending value of (0: none). Command processor
    /// thread.
    u64 wait_debt{};
    /// Session setting (GPU.completion_fences); the DebugBus can change it for comparisons.
    std::atomic<bool> completion_fences{true};
    /// VideoOut completes flips after the fences before them (WaitFences); DebugBus switch for
    /// comparisons.
    std::atomic<bool> flip_waits_fences{true};
    /// Scheduler tick the newest deferred fence waits for (0: none).
    u64 fence_tick{};
};

} // namespace AmdGpu
