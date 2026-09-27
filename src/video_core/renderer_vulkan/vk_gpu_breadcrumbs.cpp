// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_command_recorder.h"
#include "video_core/renderer_vulkan/vk_gpu_breadcrumbs.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan {

namespace {

constexpr vk::DeviceSize BeginOffset = 0;
constexpr vk::DeviceSize EndOffset = 4;

std::string_view Setting(const char* property, const char* env) {
#ifdef __ANDROID__
    static thread_local char value[PROP_VALUE_MAX]{};
    if (__system_property_get(property, value) > 0) {
        return value;
    }
    return {};
#else
    const char* value = std::getenv(env);
    return value ? std::string_view{value} : std::string_view{};
#endif
}

// Everything recorded before is finished and visible to the fill; the fill is visible to the
// host and finished before anything recorded after it starts.
void Mark(vk::CommandBuffer cmd, vk::Buffer buffer, vk::DeviceSize offset, u32 value) {
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &before});
    cmd.fillBuffer(buffer, offset, sizeof(u32), value);
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands | vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
                         vk::AccessFlagBits2::eHostRead,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &after});
}

} // Anonymous namespace

bool GpuBreadcrumbs::Requested() {
    return Setting("debug.shadps4.gpu_breadcrumbs", "SHADPS4_GPU_BREADCRUMBS") == "1";
}

GpuBreadcrumbs::GpuBreadcrumbs(const Instance& instance) : device{instance.GetDevice()} {
    const auto [buffer_result, created] = device.createBuffer(vk::BufferCreateInfo{
        .size = 256,
        .usage = vk::BufferUsageFlagBits::eTransferDst,
    });
    ASSERT_MSG(buffer_result == vk::Result::eSuccess, "Breadcrumb buffer creation failed");
    buffer = created;
    const auto requirements = device.getBufferMemoryRequirements(buffer);
    const auto& properties = instance.GetMemoryProperties();
    const auto wanted =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    u32 type = 0;
    while (type < properties.memoryTypeCount &&
           (!(requirements.memoryTypeBits & (1u << type)) ||
            (properties.memoryTypes[type].propertyFlags & wanted) != wanted)) {
        ++type;
    }
    ASSERT_MSG(type < properties.memoryTypeCount, "No host-coherent memory for breadcrumbs");
    const auto [memory_result, allocated] = device.allocateMemory(vk::MemoryAllocateInfo{
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    });
    ASSERT_MSG(memory_result == vk::Result::eSuccess, "Breadcrumb memory allocation failed");
    memory = allocated;
    ASSERT(device.bindBufferMemory(buffer, memory, 0) == vk::Result::eSuccess);
    const auto [map_result, pointer] = device.mapMemory(memory, 0, requirements.size);
    ASSERT_MSG(map_result == vk::Result::eSuccess, "Breadcrumb memory map failed");
    mapped = static_cast<volatile u32*>(pointer);
    mapped[0] = 0;
    mapped[1] = 0;

    if (const auto value = Setting("debug.shadps4.gpu_breadcrumbs_ms", "SHADPS4_GPU_BREADCRUMBS_MS");
        !value.empty()) {
        threshold = std::chrono::milliseconds{std::strtoul(std::string{value}.c_str(), nullptr, 10)};
    }
    LOG_WARNING(Render_Vulkan,
                "GPU breadcrumbs on: dispatches are serialized, stalls over {} ms are reported",
                threshold.count());
    watchdog = std::jthread([this](std::stop_token stop) { Watch(stop); });
}

GpuBreadcrumbs::~GpuBreadcrumbs() {
    watchdog = {};
    device.unmapMemory(memory);
    device.destroyBuffer(buffer);
    device.freeMemory(memory);
}

void GpuBreadcrumbs::BeforeDispatch(const RecordingCommandBuffer& cmdbuf, u64 shader_hash,
                                    u32 dim_x, u32 dim_y, u32 dim_z, bool indirect,
                                    const Context& context) {
    const u32 serial = ++next_serial;
    current_serial = serial;
    if (context.predicated) {
        predicated_dispatches.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::scoped_lock lock{records_mutex};
        records[serial % records.size()] = {serial,      indirect, shader_hash, dim_x,
                                            dim_y,       dim_z,    context};
    }
    cmdbuf.Custom(0, [buffer = buffer, serial](vk::CommandBuffer cmd) {
        Mark(cmd, buffer, BeginOffset, serial);
    });
}

void GpuBreadcrumbs::AfterDispatch(const RecordingCommandBuffer& cmdbuf) {
    cmdbuf.Custom(0, [buffer = buffer, serial = current_serial](vk::CommandBuffer cmd) {
        Mark(cmd, buffer, EndOffset, serial);
    });
}

void GpuBreadcrumbs::Watch(std::stop_token stop) {
    using Clock = std::chrono::steady_clock;
    u32 last_begin = 0;
    u32 last_end = 0;
    u32 reported = 0;
    auto changed = Clock::now();
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        const u32 begin = mapped[0];
        const u32 end = mapped[1];
        const auto now = Clock::now();
        if (begin != last_begin || end != last_end) {
            last_begin = begin;
            last_end = end;
            changed = now;
            continue;
        }
        const auto stalled = std::chrono::duration_cast<std::chrono::milliseconds>(now - changed);
        if (begin != end && begin != reported && stalled >= threshold) {
            reported = begin;
            Report(begin, end, stalled);
        }
    }
}

void GpuBreadcrumbs::Report(u32 begin, u32 end, std::chrono::milliseconds stalled) {
    const auto describe = [this](u32 serial) {
        std::scoped_lock lock{records_mutex};
        const Record& record = records[serial % records.size()];
        return record.serial == serial ? record : Record{serial, false, 0, 0, 0, 0, {}};
    };
    const Record running = describe(begin);
    const Record previous = describe(end);
    // stderr first: a hung GPU can take the process down before the log is flushed.
    std::fprintf(stderr,
                 "GPU_BREADCRUMB_STALL %lld ms: dispatch #%u cs %#018llx %s %ux%ux%u started, "
                 "not finished; last finished #%u cs %#018llx %ux%ux%u\n",
                 static_cast<long long>(stalled.count()), running.serial,
                 static_cast<unsigned long long>(running.shader_hash),
                 running.indirect ? "indirect" : "direct", running.dim_x, running.dim_y,
                 running.dim_z, previous.serial,
                 static_cast<unsigned long long>(previous.shader_hash), previous.dim_x,
                 previous.dim_y, previous.dim_z);
    std::fprintf(stderr,
                 "GPU_BREADCRUMB_CONTEXT dispatch #%u queue %u predicated %d; last SET_PREDICATION "
                 "%08x %08x (%u seen); predicated dispatches so far %u\n",
                 running.serial, running.context.queue, running.context.predicated ? 1 : 0,
                 running.context.predication_dw1, running.context.predication_dw2,
                 running.context.predications,
                 predicated_dispatches.load(std::memory_order_relaxed));
    // The dispatches that fed the stalled one, oldest first.
    constexpr u32 History = 48;
    for (u32 serial = begin > History ? begin - History : 1; serial < begin; ++serial) {
        const Record record = describe(serial);
        std::fprintf(stderr, "GPU_BREADCRUMB_HISTORY #%u queue %u cs %#018llx %s %ux%ux%u\n",
                     record.serial, record.context.queue,
                     static_cast<unsigned long long>(record.shader_hash),
                     record.indirect ? "indirect" : "direct", record.dim_x, record.dim_y,
                     record.dim_z);
    }
    std::fflush(stderr);
    LOG_CRITICAL(Render_Vulkan,
                 "GPU breadcrumb stall {} ms: dispatch #{} cs {:#018x} {} {}x{}x{} started, not "
                 "finished; last finished #{} cs {:#018x} {}x{}x{}",
                 stalled.count(), running.serial, running.shader_hash,
                 running.indirect ? "indirect" : "direct", running.dim_x, running.dim_y,
                 running.dim_z, previous.serial, previous.shader_hash, previous.dim_x,
                 previous.dim_y, previous.dim_z);
}

} // namespace Vulkan
