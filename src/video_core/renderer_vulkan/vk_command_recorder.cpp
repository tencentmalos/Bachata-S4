// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fmt/format.h>

#include "common/profiler.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/render_pass_stats.h"
#include "video_core/renderer_vulkan/vk_command_recorder.h"

namespace Vulkan {

namespace {
// Payload estimate for an array: its bytes plus worst-case alignment padding.
template <typename T>
constexpr size_t Bytes(size_t count) {
    return count * sizeof(T) + alignof(T) + 16;
}
} // namespace

CommandRecorder::CommandRecorder() {
    worker = std::jthread([this](std::stop_token stop) { Worker(stop); });
}

CommandRecorder::~CommandRecorder() {
    try {
        Sync();
    } catch (...) {
    }
    worker.request_stop();
    work_cv.notify_all();
}

void CommandRecorder::ApplyRequestedMode() noexcept {
    const int mode = requested_mode.exchange(-1, std::memory_order_relaxed);
    if (mode >= 0)
        enabled = mode == 1;
}

void CommandRecorder::EnsureRoom(size_t payload_bytes) {
    auto& slot = prepass ? pre_current : current;
    if (slot->HasRoom(payload_bytes))
        return;
    if (holding) {
        // Keep the full chunk on this thread; too many and the pass is released.
        (prepass ? pre : held).push_back(std::move(slot));
        slot = TakeChunk();
        // Never while recording pre-pass work: its tail would land inside the pass.
        if (!prepass && held.size() + pre.size() > MaxHeldChunks) {
            stats.hold_overflows.fetch_add(1, std::memory_order_relaxed);
            ReleaseHeld();
        }
    } else {
        Dispatch();
    }
    // A command larger than a whole chunk cannot be deferred; callers fall back first.
    ASSERT_MSG(slot->HasRoom(payload_bytes), "recorded command payload {} too large",
               payload_bytes);
}

std::unique_ptr<CommandChunk> CommandRecorder::TakeChunk() {
    {
        std::scoped_lock lk{mutex};
        if (!free_chunks.empty()) {
            auto chunk = std::move(free_chunks.back());
            free_chunks.pop_back();
            return chunk;
        }
    }
    return std::make_unique<CommandChunk>();
}

void CommandRecorder::BeginPass() {
    if (!Deferring() || holding)
        return;
    Dispatch(); // everything before the pass is ordered ahead of any pre-pass command
    holding = true;
    stats.held_passes.fetch_add(1, std::memory_order_relaxed);
}

void CommandRecorder::EndPass() {
    if (holding)
        ReleaseHeld();
}

void CommandRecorder::ReleaseHeld() {
    // Order: pre-pass chunks, then the held pass chunks; the current chunk (the pass
    // tail) follows in the normal stream.
    std::vector<std::unique_ptr<CommandChunk>> order;
    order.reserve(pre.size() + held.size() + 1);
    for (auto& c : pre)
        order.push_back(std::move(c));
    if (!pre_current->Empty()) {
        order.push_back(std::move(pre_current));
        pre_current = TakeChunk();
    }
    for (auto& c : held)
        order.push_back(std::move(c));
    pre.clear();
    held.clear();
    holding = false;
    prepass = false;
    if (order.empty())
        return;
    u64 commands = 0;
    for (auto& c : order)
        commands += c->Count();
    stats.chunks.fetch_add(order.size(), std::memory_order_relaxed);
    stats.commands.fetch_add(commands, std::memory_order_relaxed);
    {
        std::scoped_lock lk{mutex};
        ThrowIfFailed();
        for (auto& c : order)
            queue.push_back(std::move(c));
    }
    work_cv.notify_one();
}

void CommandRecorder::Dispatch() {
    if (current->Empty())
        return;
    stats.chunks.fetch_add(1, std::memory_order_relaxed);
    stats.commands.fetch_add(current->Count(), std::memory_order_relaxed);
    std::unique_ptr<CommandChunk> next;
    {
        std::scoped_lock lk{mutex};
        ThrowIfFailed();
        queue.push_back(std::move(current));
        if (!free_chunks.empty()) {
            next = std::move(free_chunks.back());
            free_chunks.pop_back();
        }
    }
    work_cv.notify_one();
    current = next ? std::move(next) : std::make_unique<CommandChunk>();
}


void CommandRecorder::Sync() {
    if (holding)
        ReleaseHeld();
    Dispatch();
    stats.syncs.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lk{mutex};
    if (busy || !queue.empty()) {
        stats.sync_waits.fetch_add(1, std::memory_order_relaxed);
        Common::Profiler::Scope scope{"Vulkan.RecorderSync"};
        idle_cv.wait(lk, [this] { return !busy && queue.empty(); });
    }
    ThrowIfFailed();
}

void CommandRecorder::ThrowIfFailed() {
    if (error) {
        auto e = error;
        error = nullptr;
        std::rethrow_exception(e);
    }
}

void CommandRecorder::Worker(std::stop_token stop) {
    Common::SetCurrentThreadName("shadPS4:VkRecord");
    while (true) {
        std::unique_ptr<CommandChunk> chunk;
        vk::CommandBuffer cmdbuf;
        bool failed = false;
        {
            std::unique_lock lk{mutex};
            work_cv.wait(lk, stop, [this] { return !queue.empty(); });
            if (queue.empty())
                return; // stop requested
            chunk = std::move(queue.front());
            queue.pop_front();
            busy = true;
            cmdbuf = target;
            failed = error != nullptr;
        }
        try {
            if (!failed) {
                Common::Profiler::Scope scope{"Vulkan.RecordChunk"};
                chunk->Execute(cmdbuf);
            }
        } catch (...) {
            std::scoped_lock lk{mutex};
            if (!error)
                error = std::current_exception();
        }
        chunk->Reset();
        {
            std::scoped_lock lk{mutex};
            free_chunks.push_back(std::move(chunk));
            busy = false;
            if (queue.empty())
                idle_cv.notify_all();
        }
    }
}

std::string CommandRecorder::Command(const std::vector<std::string>& args) {
    if (!args.empty() && (args[0] == "on" || args[0] == "off")) {
        requested_mode.store(args[0] == "on" ? 1 : 0, std::memory_order_relaxed);
        return fmt::format("vk_recorder {} requested (applies at the next submission)\n", args[0]);
    }
    if (args.size() > 1 && args[0] == "hoist") {
        render_pass_stats.hoist_off.store(args[1] == "off", std::memory_order_relaxed);
        return fmt::format("pass hoisting {}\n", args[1] == "off" ? "off" : "on");
    }
    if (!args.empty() && args[0] != "status")
        return "usage: vk_recorder status | on | off | hoist on|off\n";
    constexpr auto o = std::memory_order_relaxed;
    return fmt::format("chunks={} commands={} syncs={} sync_waits={} raw_syncs={} "
                       "held_passes={} hold_overflows={}\n",
                       stats.chunks.load(o), stats.commands.load(o), stats.syncs.load(o),
                       stats.sync_waits.load(o), stats.raw_syncs.load(o),
                       stats.held_passes.load(o), stats.hold_overflows.load(o));
}

// ---- RecordingCommandBuffer: commands with pointed-to data ----

void RecordingCommandBuffer::pipelineBarrier2(const vk::DependencyInfo& info) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.pipelineBarrier2(info);
        return;
    }
    const size_t bytes = Bytes<vk::MemoryBarrier2>(info.memoryBarrierCount) +
                         Bytes<vk::BufferMemoryBarrier2>(info.bufferMemoryBarrierCount) +
                         Bytes<vk::ImageMemoryBarrier2>(info.imageMemoryBarrierCount);
    recorder->Record(bytes, [&](CommandChunk& chunk) {
        const auto mem = Copy(chunk, info.pMemoryBarriers, info.memoryBarrierCount);
        const auto buf = Copy(chunk, info.pBufferMemoryBarriers, info.bufferMemoryBarrierCount);
        const auto img = Copy(chunk, info.pImageMemoryBarriers, info.imageMemoryBarrierCount);
        const auto flags = info.dependencyFlags;
        return [=](vk::CommandBuffer c) {
            c.pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = flags,
                .memoryBarrierCount = u32(mem.size()),
                .pMemoryBarriers = mem.data(),
                .bufferMemoryBarrierCount = u32(buf.size()),
                .pBufferMemoryBarriers = buf.data(),
                .imageMemoryBarrierCount = u32(img.size()),
                .pImageMemoryBarriers = img.data(),
            });
        };
    });
}

void RecordingCommandBuffer::pipelineBarrier(
    vk::PipelineStageFlags src, vk::PipelineStageFlags dst, vk::DependencyFlags flags,
    vk::ArrayProxy<const vk::MemoryBarrier> const& memory,
    vk::ArrayProxy<const vk::BufferMemoryBarrier> const& buffers,
    vk::ArrayProxy<const vk::ImageMemoryBarrier> const& images) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.pipelineBarrier(src, dst, flags, memory, buffers, images);
        return;
    }
    const size_t bytes = Bytes<vk::MemoryBarrier>(memory.size()) +
                         Bytes<vk::BufferMemoryBarrier>(buffers.size()) +
                         Bytes<vk::ImageMemoryBarrier>(images.size());
    recorder->Record(bytes, [&](CommandChunk& chunk) {
        const auto mem = Copy(chunk, memory.data(), memory.size());
        const auto buf = Copy(chunk, buffers.data(), buffers.size());
        const auto img = Copy(chunk, images.data(), images.size());
        return [=](vk::CommandBuffer c) { c.pipelineBarrier(src, dst, flags, mem, buf, img); };
    });
}

void RecordingCommandBuffer::copyBuffer(vk::Buffer src, vk::Buffer dst,
                                        vk::ArrayProxy<const vk::BufferCopy> const& regions) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.copyBuffer(src, dst, regions);
        return;
    }
    recorder->Record(Bytes<vk::BufferCopy>(regions.size()), [&](CommandChunk& chunk) {
        const auto r = Copy(chunk, regions.data(), regions.size());
        return [=](vk::CommandBuffer c) { c.copyBuffer(src, dst, r); };
    });
}

void RecordingCommandBuffer::copyImage(vk::Image src, vk::ImageLayout sl, vk::Image dst,
                                       vk::ImageLayout dl,
                                       vk::ArrayProxy<const vk::ImageCopy> const& regions) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.copyImage(src, sl, dst, dl, regions);
        return;
    }
    recorder->Record(Bytes<vk::ImageCopy>(regions.size()), [&](CommandChunk& chunk) {
        const auto r = Copy(chunk, regions.data(), regions.size());
        return [=](vk::CommandBuffer c) { c.copyImage(src, sl, dst, dl, r); };
    });
}

void RecordingCommandBuffer::copyImageToBuffer(
    vk::Image src, vk::ImageLayout sl, vk::Buffer dst,
    vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.copyImageToBuffer(src, sl, dst, regions);
        return;
    }
    recorder->Record(Bytes<vk::BufferImageCopy>(regions.size()), [&](CommandChunk& chunk) {
        const auto r = Copy(chunk, regions.data(), regions.size());
        return [=](vk::CommandBuffer c) { c.copyImageToBuffer(src, sl, dst, r); };
    });
}

void RecordingCommandBuffer::copyBufferToImage(
    vk::Buffer src, vk::Image dst, vk::ImageLayout dl,
    vk::ArrayProxy<const vk::BufferImageCopy> const& regions) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.copyBufferToImage(src, dst, dl, regions);
        return;
    }
    recorder->Record(Bytes<vk::BufferImageCopy>(regions.size()), [&](CommandChunk& chunk) {
        const auto r = Copy(chunk, regions.data(), regions.size());
        return [=](vk::CommandBuffer c) { c.copyBufferToImage(src, dst, dl, r); };
    });
}

void RecordingCommandBuffer::blitImage(vk::Image src, vk::ImageLayout sl, vk::Image dst,
                                       vk::ImageLayout dl,
                                       vk::ArrayProxy<const vk::ImageBlit> const& regions,
                                       vk::Filter filter) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.blitImage(src, sl, dst, dl, regions, filter);
        return;
    }
    recorder->Record(Bytes<vk::ImageBlit>(regions.size()), [&](CommandChunk& chunk) {
        const auto r = Copy(chunk, regions.data(), regions.size());
        return [=](vk::CommandBuffer c) { c.blitImage(src, sl, dst, dl, r, filter); };
    });
}

void RecordingCommandBuffer::resolveImage(
    vk::Image src, vk::ImageLayout sl, vk::Image dst, vk::ImageLayout dl,
    vk::ArrayProxy<const vk::ImageResolve> const& regions) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.resolveImage(src, sl, dst, dl, regions);
        return;
    }
    recorder->Record(Bytes<vk::ImageResolve>(regions.size()), [&](CommandChunk& chunk) {
        const auto r = Copy(chunk, regions.data(), regions.size());
        return [=](vk::CommandBuffer c) { c.resolveImage(src, sl, dst, dl, r); };
    });
}

void RecordingCommandBuffer::clearColorImage(
    vk::Image image, vk::ImageLayout layout, const vk::ClearColorValue& color,
    vk::ArrayProxy<const vk::ImageSubresourceRange> const& ranges) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.clearColorImage(image, layout, color, ranges);
        return;
    }
    recorder->Record(Bytes<vk::ImageSubresourceRange>(ranges.size()), [&](CommandChunk& chunk) {
        const auto r = Copy(chunk, ranges.data(), ranges.size());
        const vk::ClearColorValue value = color;
        return [=](vk::CommandBuffer c) { c.clearColorImage(image, layout, value, r); };
    });
}

void RecordingCommandBuffer::pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages,
                                           u32 offset, u32 size, const void* data) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.pushConstants(layout, stages, offset, size, data);
        return;
    }
    recorder->Record(Bytes<u8>(size), [&](CommandChunk& chunk) {
        const auto bytes = Copy(chunk, static_cast<const u8*>(data), size);
        return [=](vk::CommandBuffer c) {
            c.pushConstants(layout, stages, offset, size, bytes.data());
        };
    });
}

void RecordingCommandBuffer::pushDescriptorSetKHR(
    vk::PipelineBindPoint bp, vk::PipelineLayout layout, u32 set,
    vk::ArrayProxy<const vk::WriteDescriptorSet> const& writes) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.pushDescriptorSetKHR(bp, layout, set, writes);
        return;
    }
    size_t bytes = Bytes<vk::WriteDescriptorSet>(writes.size());
    for (const auto& w : writes) {
        if (w.pNext) { // not deep-copyable here: keep ordering and run it directly
            recorder->Sync();
            raw.pushDescriptorSetKHR(bp, layout, set, writes);
            return;
        }
        if (w.pImageInfo)
            bytes += Bytes<vk::DescriptorImageInfo>(w.descriptorCount);
        if (w.pBufferInfo)
            bytes += Bytes<vk::DescriptorBufferInfo>(w.descriptorCount);
        if (w.pTexelBufferView)
            bytes += Bytes<vk::BufferView>(w.descriptorCount);
    }
    recorder->Record(bytes, [&](CommandChunk& chunk) {
        const auto copied = Copy(chunk, writes.data(), writes.size());
        auto* out = const_cast<vk::WriteDescriptorSet*>(copied.data());
        for (auto& w : std::span{out, copied.size()}) {
            if (w.pImageInfo)
                w.pImageInfo = Copy(chunk, w.pImageInfo, w.descriptorCount).data();
            if (w.pBufferInfo)
                w.pBufferInfo = Copy(chunk, w.pBufferInfo, w.descriptorCount).data();
            if (w.pTexelBufferView)
                w.pTexelBufferView = Copy(chunk, w.pTexelBufferView, w.descriptorCount).data();
        }
        return [=](vk::CommandBuffer c) { c.pushDescriptorSetKHR(bp, layout, set, copied); };
    });
}

void RecordingCommandBuffer::bindDescriptorSets(vk::PipelineBindPoint bp, vk::PipelineLayout layout,
                                                u32 first,
                                                vk::ArrayProxy<const vk::DescriptorSet> const& sets,
                                                vk::ArrayProxy<const u32> const& offsets) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.bindDescriptorSets(bp, layout, first, sets, offsets);
        return;
    }
    recorder->Record(Bytes<vk::DescriptorSet>(sets.size()) + Bytes<u32>(offsets.size()),
                     [&](CommandChunk& chunk) {
                         const auto s = Copy(chunk, sets.data(), sets.size());
                         const auto o = Copy(chunk, offsets.data(), offsets.size());
                         return [=](vk::CommandBuffer c) {
                             c.bindDescriptorSets(bp, layout, first, s, o);
                         };
                     });
}

void RecordingCommandBuffer::beginRendering(const vk::RenderingInfo& info) const {
    CommandRecorder* const recorder = Active();
    const bool plain = !info.pNext &&
                       (!info.pDepthAttachment || !info.pDepthAttachment->pNext) &&
                       (!info.pStencilAttachment || !info.pStencilAttachment->pNext);
    if (!recorder || !plain) {
        if (recorder)
            recorder->Sync();
        raw.beginRendering(info);
        return;
    }
    const size_t bytes = Bytes<vk::RenderingAttachmentInfo>(info.colorAttachmentCount + 2);
    recorder->Record(bytes, [&](CommandChunk& chunk) {
        const auto colors = Copy(chunk, info.pColorAttachments, info.colorAttachmentCount);
        const auto depth = Copy(chunk, info.pDepthAttachment, info.pDepthAttachment ? 1 : 0);
        const auto stencil = Copy(chunk, info.pStencilAttachment, info.pStencilAttachment ? 1 : 0);
        vk::RenderingInfo copy = info;
        copy.pColorAttachments = colors.data();
        copy.pDepthAttachment = depth.empty() ? nullptr : depth.data();
        copy.pStencilAttachment = stencil.empty() ? nullptr : stencil.data();
        return [=](vk::CommandBuffer c) { c.beginRendering(copy); };
    });
}

namespace {
template <typename Func>
void RecordLabel(CommandRecorder* recorder, const vk::DebugUtilsLabelEXT& label, Func&& call) {
    const std::string_view name = label.pLabelName ? label.pLabelName : "";
    recorder->Record(name.size() + 1 + 16, [&](CommandChunk& chunk) {
        auto* text = static_cast<char*>(chunk.AllocPayload(name.size() + 1, 1));
        std::memcpy(text, name.data(), name.size());
        text[name.size()] = '\0';
        vk::DebugUtilsLabelEXT copy = label;
        copy.pNext = nullptr;
        copy.pLabelName = text;
        return [=](vk::CommandBuffer c) { call(c, copy); };
    });
}
} // namespace

void RecordingCommandBuffer::beginDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.beginDebugUtilsLabelEXT(label);
        return;
    }
    RecordLabel(recorder, label, [](vk::CommandBuffer c, const vk::DebugUtilsLabelEXT& l) {
        c.beginDebugUtilsLabelEXT(l);
    });
}

void RecordingCommandBuffer::insertDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.insertDebugUtilsLabelEXT(label);
        return;
    }
    RecordLabel(recorder, label, [](vk::CommandBuffer c, const vk::DebugUtilsLabelEXT& l) {
        c.insertDebugUtilsLabelEXT(l);
    });
}

void RecordingCommandBuffer::setViewportWithCount(vk::ArrayProxy<const vk::Viewport> const& v) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.setViewportWithCount(v);
        return;
    }
    recorder->Record(Bytes<vk::Viewport>(v.size()), [&](CommandChunk& chunk) {
        const auto s = Copy(chunk, v.data(), v.size());
        return [=](vk::CommandBuffer c) { c.setViewportWithCount(s); };
    });
}

void RecordingCommandBuffer::setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> const& v) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.setScissorWithCount(v);
        return;
    }
    recorder->Record(Bytes<vk::Rect2D>(v.size()), [&](CommandChunk& chunk) {
        const auto s = Copy(chunk, v.data(), v.size());
        return [=](vk::CommandBuffer c) { c.setScissorWithCount(s); };
    });
}

void RecordingCommandBuffer::setViewport(u32 first,
                                         vk::ArrayProxy<const vk::Viewport> const& v) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.setViewport(first, v);
        return;
    }
    recorder->Record(Bytes<vk::Viewport>(v.size()), [&](CommandChunk& chunk) {
        const auto s = Copy(chunk, v.data(), v.size());
        return [=](vk::CommandBuffer c) { c.setViewport(first, s); };
    });
}

void RecordingCommandBuffer::setScissor(u32 first, vk::ArrayProxy<const vk::Rect2D> const& v) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.setScissor(first, v);
        return;
    }
    recorder->Record(Bytes<vk::Rect2D>(v.size()), [&](CommandChunk& chunk) {
        const auto s = Copy(chunk, v.data(), v.size());
        return [=](vk::CommandBuffer c) { c.setScissor(first, s); };
    });
}

void RecordingCommandBuffer::setColorWriteMaskEXT(
    u32 first, vk::ArrayProxy<const vk::ColorComponentFlags> const& v) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.setColorWriteMaskEXT(first, v);
        return;
    }
    recorder->Record(Bytes<vk::ColorComponentFlags>(v.size()), [&](CommandChunk& chunk) {
        const auto s = Copy(chunk, v.data(), v.size());
        return [=](vk::CommandBuffer c) { c.setColorWriteMaskEXT(first, s); };
    });
}

void RecordingCommandBuffer::bindVertexBuffers(u32 first, u32 count, const vk::Buffer* buffers,
                                               const vk::DeviceSize* offsets) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.bindVertexBuffers(first, count, buffers, offsets);
        return;
    }
    recorder->Record(Bytes<vk::Buffer>(count) + Bytes<vk::DeviceSize>(count),
                     [&](CommandChunk& chunk) {
                         const auto b = Copy(chunk, buffers, count);
                         const auto o = Copy(chunk, offsets, count);
                         return [=](vk::CommandBuffer c) {
                             c.bindVertexBuffers(first, count, b.data(), o.data());
                         };
                     });
}

void RecordingCommandBuffer::bindVertexBuffers2(u32 first, u32 count, const vk::Buffer* buffers,
                                                const vk::DeviceSize* offsets,
                                                const vk::DeviceSize* sizes,
                                                const vk::DeviceSize* strides) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.bindVertexBuffers2(first, count, buffers, offsets, sizes, strides);
        return;
    }
    recorder->Record(Bytes<vk::Buffer>(count) + 3 * Bytes<vk::DeviceSize>(count),
                     [&](CommandChunk& chunk) {
                         const auto b = Copy(chunk, buffers, count);
                         const auto o = Copy(chunk, offsets, count);
                         const auto s = Copy(chunk, sizes, sizes ? count : 0);
                         const auto st = Copy(chunk, strides, strides ? count : 0);
                         return [=](vk::CommandBuffer c) {
                             c.bindVertexBuffers2(first, count, b.data(), o.data(),
                                                  s.empty() ? nullptr : s.data(),
                                                  st.empty() ? nullptr : st.data());
                         };
                     });
}

void RecordingCommandBuffer::setVertexInputEXT(
    vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> const& bindings,
    vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> const& attributes) const {
    CommandRecorder* const recorder = Active();
    if (!recorder) {
        raw.setVertexInputEXT(bindings, attributes);
        return;
    }
    recorder->Record(Bytes<vk::VertexInputBindingDescription2EXT>(bindings.size()) +
                         Bytes<vk::VertexInputAttributeDescription2EXT>(attributes.size()),
                     [&](CommandChunk& chunk) {
                         const auto b = Copy(chunk, bindings.data(), bindings.size());
                         const auto a = Copy(chunk, attributes.data(), attributes.size());
                         return [=](vk::CommandBuffer c) { c.setVertexInputEXT(b, a); };
                     });
}

} // namespace Vulkan
