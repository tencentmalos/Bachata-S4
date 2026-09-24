// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

// Deferred Vulkan command recording (the CommandChunk/worker scheme of Citron/yuzu).
//
// The PM4 owner encodes commands with their arguments copied by value into chunks;
// one worker thread replays the chunks, in order, into the real command buffer. The
// producer and the worker never touch the command buffer or its pool at the same
// time: the producer only uses them directly after Sync(), which waits for the worker
// to drain. When disabled every command runs immediately on the caller.
class CommandChunk {
public:
    static constexpr size_t Capacity = 128 * 1024;
    static constexpr size_t MaxCommandSize = 512;

    CommandChunk() = default;
    CommandChunk(const CommandChunk&) = delete;
    CommandChunk& operator=(const CommandChunk&) = delete;
    ~CommandChunk() {
        Reset();
    }

    bool Empty() const noexcept {
        return first == nullptr;
    }
    u32 Count() const noexcept {
        return count;
    }

    // Space for one command of at most MaxCommandSize plus payload_bytes of copied data.
    bool HasRoom(size_t payload_bytes) const noexcept {
        return front + MaxCommandSize + payload_bytes + alignof(std::max_align_t) * 2 <= back;
    }

    // Payload is carved from the back; it lives until the chunk is reset after execution.
    void* AllocPayload(size_t bytes, size_t align) noexcept {
        size_t start = (back - bytes) & ~(align - 1);
        ASSERT(start >= front);
        back = start;
        return storage.get() + start;
    }

    template <typename F>
    void Record(F&& func) {
        using Cmd = TypedCommand<std::decay_t<F>>;
        static_assert(sizeof(Cmd) <= MaxCommandSize, "recorded command captures too much");
        size_t start = (front + alignof(Cmd) - 1) & ~(alignof(Cmd) - 1);
        ASSERT(start + sizeof(Cmd) <= back);
        auto* cmd = new (storage.get() + start) Cmd(std::forward<F>(func));
        front = start + sizeof(Cmd);
        if (last)
            last->next = cmd;
        else
            first = cmd;
        last = cmd;
        ++count;
    }

    void Execute(vk::CommandBuffer cmdbuf) {
        for (Command* cmd = first; cmd; cmd = cmd->next)
            cmd->Execute(cmdbuf);
    }

    void Reset() noexcept {
        for (Command* cmd = first; cmd;) {
            Command* next = cmd->next;
            cmd->~Command();
            cmd = next;
        }
        first = last = nullptr;
        front = 0;
        back = Capacity;
        count = 0;
    }

private:
    struct Command {
        virtual ~Command() = default;
        virtual void Execute(vk::CommandBuffer cmdbuf) = 0;
        Command* next{};
    };
    template <typename F>
    struct TypedCommand final : Command {
        explicit TypedCommand(F&& f) : func{std::move(f)} {}
        explicit TypedCommand(const F& f) : func{f} {}
        void Execute(vk::CommandBuffer cmdbuf) override {
            func(cmdbuf);
        }
        F func;
    };

    std::unique_ptr<std::byte[]> storage{new std::byte[Capacity]};
    size_t front{};
    size_t back{Capacity};
    Command* first{};
    Command* last{};
    u32 count{};
};

class CommandRecorder {
public:
    // Chunks are handed to the worker once they hold this many commands (or are full).
    static constexpr u32 DispatchThreshold = 256;

    CommandRecorder();
    ~CommandRecorder();

    bool Enabled() const noexcept {
        return enabled;
    }

    // Commands are deferred unless disabled or the real command buffer was handed out
    // (then everything records immediately until the next command buffer).
    bool Deferring() const noexcept {
        return enabled && !raw_checked_out;
    }

    // Producer: the command buffer the worker replays into. Only while drained.
    void SetTarget(vk::CommandBuffer cmdbuf) noexcept {
        target = cmdbuf;
        raw_checked_out = false;
    }

    // Applies a pending on/off request. Only while drained.
    void ApplyRequestedMode() noexcept;

    template <typename F>
    void Record(size_t payload_bytes, F&& build) {
        EnsureRoom(payload_bytes); // may replace the current chunk
        auto& chunk = prepass ? *pre_current : *current;
        chunk.Record(build(chunk));
        if (!holding && chunk.Count() >= DispatchThreshold)
            Dispatch();
    }

    // Pass holding: commands of an open render pass stay on this thread until the pass
    // ends, so operations found independent of it can be placed before its begin.
    // BeginPass is called right before the pass begin command is recorded.
    // 32 x 128 KiB: large guest passes (a 400-draw G-buffer) stay held to their end.
    static constexpr size_t MaxHeldChunks = 32;
    void BeginPass();
    void EndPass();
    bool Holding() const noexcept {
        return holding;
    }
    // Commands recorded between BeginPrePass/EndPrePass execute before the held pass.
    void BeginPrePass() noexcept {
        prepass = true;
    }
    void EndPrePass() noexcept {
        prepass = false;
    }

    // Hands the current chunk to the worker.
    void Dispatch();

    // Dispatch + wait until every recorded command has been replayed.
    void Sync();

    static std::string Command(const std::vector<std::string>& args);

    struct Stats { // static storage: zero-initialized
        std::atomic<u64> chunks, commands, syncs, sync_waits, raw_syncs, held_passes,
            hold_overflows;
    };
    static inline Stats stats;
    static inline std::atomic<int> requested_mode{-1}; // -1 keep, 0 off, 1 on

    // Counts producer-side direct command buffer accesses (each forces a Sync).
    void CheckOutRaw() {
        if (!Deferring())
            return;
        stats.raw_syncs.fetch_add(1, std::memory_order_relaxed);
        Sync();
        raw_checked_out = true;
    }

private:
    void EnsureRoom(size_t payload_bytes);
    void ReleaseHeld(); // queue pre-pass then held chunks, stop holding
    std::unique_ptr<CommandChunk> TakeChunk();
    void Worker(std::stop_token stop);
    void ThrowIfFailed();

    bool enabled{};
    bool raw_checked_out{};
    bool holding{};
    bool prepass{};
    std::vector<std::unique_ptr<CommandChunk>> held;     // pass chunks (in order)
    std::vector<std::unique_ptr<CommandChunk>> pre;      // pre-pass chunks (in order)
    std::unique_ptr<CommandChunk> pre_current{std::make_unique<CommandChunk>()};
    vk::CommandBuffer target{};
    std::unique_ptr<CommandChunk> current{std::make_unique<CommandChunk>()};

    std::mutex mutex;
    std::condition_variable_any work_cv;
    std::condition_variable idle_cv;
    std::deque<std::unique_ptr<CommandChunk>> queue;
    std::vector<std::unique_ptr<CommandChunk>> free_chunks;
    bool busy{};
    std::exception_ptr error;
    std::jthread worker;
};

// Command buffer facade used by all renderer code. Methods mirror vk::CommandBuffer;
// arrays and pointed-to structures are deep-copied into the chunk when recording.
class RecordingCommandBuffer {
public:
    RecordingCommandBuffer(CommandRecorder* recorder_, vk::CommandBuffer raw_)
        : recorder_ptr{recorder_}, raw{raw_} {}

    // Scalar-argument commands.
    void bindPipeline(vk::PipelineBindPoint bp, vk::Pipeline p) const {
        Emit([=](vk::CommandBuffer c) { c.bindPipeline(bp, p); });
    }
    void draw(u32 vc, u32 ic, u32 fv, u32 fi) const {
        Emit([=](vk::CommandBuffer c) { c.draw(vc, ic, fv, fi); });
    }
    void drawIndexed(u32 ic, u32 inst, u32 fi, s32 vo, u32 finst) const {
        Emit([=](vk::CommandBuffer c) { c.drawIndexed(ic, inst, fi, vo, finst); });
    }
    void drawIndirect(vk::Buffer b, vk::DeviceSize o, u32 n, u32 s) const {
        Emit([=](vk::CommandBuffer c) { c.drawIndirect(b, o, n, s); });
    }
    void drawIndexedIndirect(vk::Buffer b, vk::DeviceSize o, u32 n, u32 s) const {
        Emit([=](vk::CommandBuffer c) { c.drawIndexedIndirect(b, o, n, s); });
    }
    void drawIndirectCount(vk::Buffer b, vk::DeviceSize o, vk::Buffer cb, vk::DeviceSize co,
                           u32 n, u32 s) const {
        Emit([=](vk::CommandBuffer c) { c.drawIndirectCount(b, o, cb, co, n, s); });
    }
    void drawIndexedIndirectCount(vk::Buffer b, vk::DeviceSize o, vk::Buffer cb,
                                  vk::DeviceSize co, u32 n, u32 s) const {
        Emit([=](vk::CommandBuffer c) { c.drawIndexedIndirectCount(b, o, cb, co, n, s); });
    }
    void dispatch(u32 x, u32 y, u32 z) const {
        Emit([=](vk::CommandBuffer c) { c.dispatch(x, y, z); });
    }
    void dispatchIndirect(vk::Buffer b, vk::DeviceSize o) const {
        Emit([=](vk::CommandBuffer c) { c.dispatchIndirect(b, o); });
    }
    void bindIndexBuffer(vk::Buffer b, vk::DeviceSize o, vk::IndexType t) const {
        Emit([=](vk::CommandBuffer c) { c.bindIndexBuffer(b, o, t); });
    }
    void fillBuffer(vk::Buffer b, vk::DeviceSize o, vk::DeviceSize s, u32 v) const {
        Emit([=](vk::CommandBuffer c) { c.fillBuffer(b, o, s, v); });
    }
    void endRendering() const {
        Emit([](vk::CommandBuffer c) { c.endRendering(); });
    }
    void endDebugUtilsLabelEXT() const {
        Emit([](vk::CommandBuffer c) { c.endDebugUtilsLabelEXT(); });
    }
    void setDepthTestEnable(bool v) const {
        Emit([=](vk::CommandBuffer c) { c.setDepthTestEnable(v); });
    }
    void setDepthWriteEnable(bool v) const {
        Emit([=](vk::CommandBuffer c) { c.setDepthWriteEnable(v); });
    }
    void setDepthCompareOp(vk::CompareOp v) const {
        Emit([=](vk::CommandBuffer c) { c.setDepthCompareOp(v); });
    }
    void setDepthBoundsTestEnable(bool v) const {
        Emit([=](vk::CommandBuffer c) { c.setDepthBoundsTestEnable(v); });
    }
    void setDepthBounds(float a, float b) const {
        Emit([=](vk::CommandBuffer c) { c.setDepthBounds(a, b); });
    }
    void setDepthBiasEnable(bool v) const {
        Emit([=](vk::CommandBuffer c) { c.setDepthBiasEnable(v); });
    }
    void setDepthBias(float a, float b, float s) const {
        Emit([=](vk::CommandBuffer c) { c.setDepthBias(a, b, s); });
    }
    void setStencilTestEnable(bool v) const {
        Emit([=](vk::CommandBuffer c) { c.setStencilTestEnable(v); });
    }
    void setStencilOp(vk::StencilFaceFlags f, vk::StencilOp a, vk::StencilOp b, vk::StencilOp d,
                      vk::CompareOp op) const {
        Emit([=](vk::CommandBuffer c) { c.setStencilOp(f, a, b, d, op); });
    }
    void setStencilReference(vk::StencilFaceFlags f, u32 v) const {
        Emit([=](vk::CommandBuffer c) { c.setStencilReference(f, v); });
    }
    void setStencilWriteMask(vk::StencilFaceFlags f, u32 v) const {
        Emit([=](vk::CommandBuffer c) { c.setStencilWriteMask(f, v); });
    }
    void setStencilCompareMask(vk::StencilFaceFlags f, u32 v) const {
        Emit([=](vk::CommandBuffer c) { c.setStencilCompareMask(f, v); });
    }
    void setPrimitiveRestartEnable(bool v) const {
        Emit([=](vk::CommandBuffer c) { c.setPrimitiveRestartEnable(v); });
    }
    void setRasterizerDiscardEnable(bool v) const {
        Emit([=](vk::CommandBuffer c) { c.setRasterizerDiscardEnable(v); });
    }
    void setCullMode(vk::CullModeFlags v) const {
        Emit([=](vk::CommandBuffer c) { c.setCullMode(v); });
    }
    void setFrontFace(vk::FrontFace v) const {
        Emit([=](vk::CommandBuffer c) { c.setFrontFace(v); });
    }
    void setLineWidth(float v) const {
        Emit([=](vk::CommandBuffer c) { c.setLineWidth(v); });
    }
    void setAttachmentFeedbackLoopEnableEXT(vk::ImageAspectFlags v) const {
        Emit([=](vk::CommandBuffer c) { c.setAttachmentFeedbackLoopEnableEXT(v); });
    }
    void setBlendConstants(const float constants[4]) const {
        std::array<float, 4> v{constants[0], constants[1], constants[2], constants[3]};
        Emit([=](vk::CommandBuffer c) { c.setBlendConstants(v.data()); });
    }

    // Array/pointer commands.
    void pipelineBarrier2(const vk::DependencyInfo& info) const;
    void pipelineBarrier(vk::PipelineStageFlags src, vk::PipelineStageFlags dst,
                         vk::DependencyFlags flags,
                         vk::ArrayProxy<const vk::MemoryBarrier> const& memory,
                         vk::ArrayProxy<const vk::BufferMemoryBarrier> const& buffers,
                         vk::ArrayProxy<const vk::ImageMemoryBarrier> const& images) const;
    void copyBuffer(vk::Buffer src, vk::Buffer dst,
                    vk::ArrayProxy<const vk::BufferCopy> const& regions) const;
    void copyImage(vk::Image src, vk::ImageLayout sl, vk::Image dst, vk::ImageLayout dl,
                   vk::ArrayProxy<const vk::ImageCopy> const& regions) const;
    void copyImageToBuffer(vk::Image src, vk::ImageLayout sl, vk::Buffer dst,
                           vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const;
    void copyBufferToImage(vk::Buffer src, vk::Image dst, vk::ImageLayout dl,
                           vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const;
    void blitImage(vk::Image src, vk::ImageLayout sl, vk::Image dst, vk::ImageLayout dl,
                   vk::ArrayProxy<const vk::ImageBlit> const& regions, vk::Filter filter) const;
    void resolveImage(vk::Image src, vk::ImageLayout sl, vk::Image dst, vk::ImageLayout dl,
                      vk::ArrayProxy<const vk::ImageResolve> const& regions) const;
    void clearColorImage(vk::Image image, vk::ImageLayout layout, const vk::ClearColorValue& color,
                         vk::ArrayProxy<const vk::ImageSubresourceRange> const& ranges) const;
    void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 offset, u32 size,
                       const void* data) const;
    void pushDescriptorSetKHR(vk::PipelineBindPoint bp, vk::PipelineLayout layout, u32 set,
                              vk::ArrayProxy<const vk::WriteDescriptorSet> const& writes) const;
    void bindDescriptorSets(vk::PipelineBindPoint bp, vk::PipelineLayout layout, u32 first,
                            vk::ArrayProxy<const vk::DescriptorSet> const& sets,
                            vk::ArrayProxy<const u32> const& offsets) const;
    void beginRendering(const vk::RenderingInfo& info) const;
    void beginDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const;
    void insertDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const;
    void setViewportWithCount(vk::ArrayProxy<const vk::Viewport> const& v) const;
    void setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> const& v) const;
    void setViewport(u32 first, vk::ArrayProxy<const vk::Viewport> const& v) const;
    void setScissor(u32 first, vk::ArrayProxy<const vk::Rect2D> const& v) const;
    void setColorWriteMaskEXT(u32 first, vk::ArrayProxy<const vk::ColorComponentFlags> const& v) const;
    void bindVertexBuffers(u32 first, u32 count, const vk::Buffer* buffers,
                           const vk::DeviceSize* offsets) const;
    void bindVertexBuffers2(u32 first, u32 count, const vk::Buffer* buffers,
                            const vk::DeviceSize* offsets, const vk::DeviceSize* sizes,
                            const vk::DeviceSize* strides) const;
    void setVertexInputEXT(
        vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> const& bindings,
        vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> const& attributes) const;

    // Runs func with the real command buffer, in recording order (a lambda with value captures).
    template <typename F>
    void Custom(size_t payload_bytes, F&& func) const {
        CommandRecorder* const recorder = Active();
        if (!recorder) {
            func(raw);
            return;
        }
        recorder->Record(payload_bytes, [&](CommandChunk&) { return std::forward<F>(func); });
    }

private:
    CommandRecorder* Active() const noexcept {
        return recorder_ptr && recorder_ptr->Deferring() ? recorder_ptr : nullptr;
    }

    template <typename F>
    void Emit(F&& func) const {
        CommandRecorder* const recorder = Active();
        if (!recorder) {
            func(raw);
            return;
        }
        recorder->Record(0, [&](CommandChunk&) { return std::forward<F>(func); });
    }

    template <typename T>
    static std::span<const T> Copy(CommandChunk& chunk, const T* data, size_t count) {
        if (count == 0 || !data)
            return {};
        auto* dst = static_cast<T*>(chunk.AllocPayload(sizeof(T) * count, alignof(T)));
        std::memcpy(static_cast<void*>(dst), data, sizeof(T) * count);
        return {dst, count};
    }

    CommandRecorder* recorder_ptr;
    vk::CommandBuffer raw;
};

} // namespace Vulkan
