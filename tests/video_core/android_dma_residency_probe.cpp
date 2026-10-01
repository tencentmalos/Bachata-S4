// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production fault-manager retirement path, not just its parser.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/mman.h>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "frontend/window.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"
struct Window : Frontend::Window {
    s32 GetWidth() const override {
        return 64;
    }
    s32 GetHeight() const override {
        return 64;
    }
    Frontend::WindowSystemInfo GetWindowInfo() const override {
        return {};
    }
    bool RequestKeyboard() override {
        return false;
    }
    void ReleaseKeyboard() override {}
};
struct Backend : Core::GuestMemoryBackend {
    static constexpr u64 Size = 0x80000;
    void* allocation =
        mmap(std::getenv("DMA_PROBE_LOW_VA") ? reinterpret_cast<void*>(0x220000000ULL) : nullptr,
             Size + 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    VAddr base = (u64(allocation) + 0x3fff) & ~0x3fffULL;
    ~Backend() {
        munmap(allocation, Size + 0x4000);
    }
    u8* BackingBase() const override {
        return reinterpret_cast<u8*>(base);
    }
    boost::icl::interval_set<VAddr> UsableRegions() const override {
        boost::icl::interval_set<VAddr> out;
        out.add(boost::icl::interval<VAddr>::right_open(base, base + Size));
        return out;
    }
    bool OwnsRange(VAddr a, u64 n) const override {
        return a >= base && a - base < Size && n <= Size - (a - base);
    }
    void* Map(VAddr a, u64 n, PAddr, bool) override {
        if (!OwnsRange(a, n) || mprotect(reinterpret_cast<void*>(a), n, PROT_READ | PROT_WRITE))
            throw std::runtime_error("map");
        return reinterpret_cast<void*>(a);
    }
    void* MapFile(VAddr, u64, u64, u32, uintptr_t, bool) override {
        throw std::runtime_error("unused");
    }
    void Unmap(VAddr a, u64 n) override {
        mprotect(reinterpret_cast<void*>(a), n, PROT_NONE);
    }
    void Protect(VAddr a, u64 n, Core::MemoryPermission p) override {
        mprotect(reinterpret_cast<void*>(a), n, int(p));
    }
};

std::vector<u32> ReadShader(u32 page_shift, bool int64) {
    using namespace Shader;
    Common::ObjectPool<IR::Inst> pool;
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    info.buffers.push_back({.used_types = IR::Type::U32, .is_written = true});
    IR::Block block(pool);
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back(
        {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    IR::IREmitter ir(block);
    const auto lane = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
    const auto base =
        ir.CompositeConstruct(ir.GetUserData(IR::ScalarReg(0)), ir.GetUserData(IR::ScalarReg(1)));
    const auto value = ir.ReadConst(base, IR::U32{ir.IAdd(lane, ir.GetUserData(IR::ScalarReg(2)))});
    ir.StoreBufferU32(1, ir.Imm32(0U), lane, value, {});
    Profile profile{};
    profile.supported_spirv = 0x10500;
    profile.support_int64 = int64;
    profile.sparse_page_shift = page_shift;
    profile.subgroup_size = 64;
    Optimization::CollectShaderInfoPass(program, profile);
    if (info.buffers.size() != 4 || !info.uses_dma)
        throw std::runtime_error("unexpected DMA shader layout");
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.workgroup_size = {64, 1, 1};
    Shader::Backend::Bindings bindings{};
    return Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
}

int Run(int argc, char** argv) {
    if (argc != 3)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("dma-residency-probe");
    Common::ElfInfo::Instance().InitializeGuestMetadata(argv[2], 0x11000000, "DMA-PROBE");
    Backend backend;
    if (backend.allocation == MAP_FAILED)
        return 2;
    Core::MemoryManager memory(&backend);
    Core::Memory::Binding binding(memory);
    void* mapped{};
    if (memory.MapMemory(&mapped, backend.base, Backend::Size, Core::MemoryProt::CpuReadWrite,
                         Core::MemoryMapFlags::NoFlags, Core::VMAType::File, "dma-probe") != 0)
        return 2;
    for (u32 n = 0; n < Backend::Size / 4; ++n)
        static_cast<u32*>(mapped)[n] = n * 0x9e3779b9U + 17;
    Window window;
    Vulkan::Instance instance(window, 0, false, false, Vulkan::LoadAndroidTurnip(argv[1], argv[2]));
    std::printf("DMA probe base=%llx maxStorageBufferRange=%u\n",
                static_cast<unsigned long long>(backend.base),
                instance.GetPhysicalDevice().getProperties().limits.maxStorageBufferRange);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    AmdGpu::Liverpool liverpool;
    Vulkan::Rasterizer rasterizer(instance, scheduler, runtime, &liverpool);
    rasterizer.MapMemory(backend.base, Backend::Size);
    auto& bc = rasterizer.GetBufferCache();
    const u64 page_size = 1ULL << bc.GetSparsePageShift();
    if (page_size != 0x4000)
        throw std::runtime_error("test requires 16 KiB sparse pages");
    unsigned checks{}, failures{};
    auto check = [&](bool ok) {
        ++checks;
        if (!ok)
            ++failures;
    };
    VideoCore::Buffer download(instance, 0, 0x20000, VideoCore::MemoryType::HostCached);
    auto finish = [&] {
        runtime.FlushBarriers();
        const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                         .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                         .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                         .dstAccessMask = vk::AccessFlagBits2::eHostRead};
        scheduler.CommandBuffer().pipelineBarrier2(
            vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        scheduler.Finish();
        scheduler.PopPendingOperations();
        download.Invalidate(0, download.SizeBytes());
    };
    // Normal tiny descriptor reads must retain their stream fast path.
    const auto [stream, off] = bc.ObtainBuffer(backend.base, 32, false);
    check(bc.IsStreamBuffer(stream));
    // Separated faults stay individual 16 KiB spans: the old implementation
    // incorrectly streams them and leaves both page-table entries at zero.
    for (u32 round = 0; round < 3; ++round) {
        const std::array<VAddr, 2> addresses{backend.base + (2 + round * 8) * page_size,
                                             backend.base + (5 + round * 8) * page_size};
        std::map<u64, u32> marks;
        for (VAddr a : addresses) {
            const u64 page = a >> bc.GetSparsePageShift();
            marks[page / 32] |= 1U << (page % 32);
        }
        for (const auto& [word, bits] : marks)
            runtime.InlineData(bc.GetFaultBuffer(), word * 4, bits);
        runtime.FlushBarriers();
        const vk::MemoryBarrier2 marked{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                                        .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                                                         vk::AccessFlagBits2::eShaderWrite};
        scheduler.CommandBuffer().pipelineBarrier2(
            vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &marked});
        runtime.FlushBarriers();
        bc.ProcessFaultBuffer();
        scheduler.Finish();
        scheduler.PopPendingOperations();
        const auto reads = bc.SynchronizeDmaBuffers();
        for (u32 i = 0; i < addresses.size(); ++i) {
            const u64 page = addresses[i] >> bc.GetSparsePageShift();
            const vk::BufferCopy copy{page * 8, i * 8, 8};
            runtime.CopyBuffer(bc.GetBdaPageTableBuffer(), &download, {&copy, 1});
        }
        finish();
        for (u32 i = 0; i < addresses.size(); ++i) {
            u64 address{};
            std::memcpy(&address, download.mapped_data.data() + i * 8, 8);
            check(address != 0);
            bool found{};
            for (const auto& r : reads) {
                const u64 begin = r.buffer->CpuAddr() + r.offset;
                if (addresses[i] >= begin && addresses[i] + page_size <= begin + r.size) {
                    const vk::BufferCopy copy{r.offset + addresses[i] - begin,
                                              0x1000 + i * page_size, page_size};
                    runtime.CopyBuffer(r.buffer, &download, {&copy, 1});
                    found = true;
                    break;
                }
            }
            check(found);
            if (!found)
                continue;
            finish();
            const auto* expected = reinterpret_cast<const u32*>(addresses[i]);
            const auto* actual =
                reinterpret_cast<const u32*>(download.mapped_data.data() + 0x1000 + i * page_size);
            for (u32 n = 0; n < page_size / 4; ++n)
                check(actual[n] == expected[n]);
        }
    }

    // Read through the production shader's BDA page-table lookup as well as the
    // ordinary buffer copy above. This covers address formation and fault retry.
    EmulatorSettings.SetDirectMemoryAccessEnabled(true);
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 4> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i)
        bindings[i] = {.binding = i,
                       .descriptorType = vk::DescriptorType::eStorageBuffer,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    auto layout = Vulkan::Check(device.createDescriptorSetLayoutUnique(
        {.bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
    const vk::PushConstantRange push_range{vk::ShaderStageFlagBits::eCompute, 0, 128};
    auto pipeline_layout =
        Vulkan::Check(device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                         .pSetLayouts = &*layout,
                                                         .pushConstantRangeCount = 1,
                                                         .pPushConstantRanges = &push_range}));
    const vk::DescriptorPoolSize count{vk::DescriptorType::eStorageBuffer, 4};
    auto descriptor_pool = Vulkan::Check(device.createDescriptorPoolUnique(
        {.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &count}));
    const auto sets = Vulkan::Check(device.allocateDescriptorSets(
        {.descriptorPool = *descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &*layout}));
    const auto* table = bc.GetBdaPageTableRootBuffer();
    const auto* faults = bc.GetFaultBuffer();
    check(table->SizeBytes() <=
          instance.GetPhysicalDevice().getProperties().limits.maxStorageBufferRange);
    const std::array<vk::DescriptorBufferInfo, 4> buffer_infos{
        {{download.Handle(), 0, 256},
         {download.Handle(), 0x1000, 256},
         {table->Handle(), 0, table->SizeBytes()},
         {faults->Handle(), 0, faults->SizeBytes()}}};
    for (u32 i = 0; i < buffer_infos.size(); ++i)
        device.updateDescriptorSets(
            vk::WriteDescriptorSet{.dstSet = sets[0],
                                   .dstBinding = i,
                                   .descriptorCount = 1,
                                   .descriptorType = vk::DescriptorType::eStorageBuffer,
                                   .pBufferInfo = &buffer_infos[i]},
            {});
    for (bool int64 : {true, false}) {
        auto code = ReadShader(bc.GetSparsePageShift(), int64);
        std::ofstream file(int64 ? "dma-read-u64.spv" : "dma-read-pair.spv", std::ios::binary);
        file.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
        auto module = Vulkan::Check(
            device.createShaderModuleUnique({.codeSize = code.size() * 4, .pCode = code.data()}));
        auto pipeline = Vulkan::Check(device.createComputePipelineUnique(
            {}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                           .module = *module,
                           .pName = "main"},
                 .layout = *pipeline_layout}));
        const VAddr address = backend.base + (int64 ? 26 : 29) * page_size + 64;
        for (u32 round = 0; round < 4; ++round) {
            std::array<u32, 32> push{};
            push[4] = u32(address);
            push[5] = u32(address >> 32);
            push[6] = round * 64;
            const auto reads = bc.SynchronizeDmaBuffers();
            runtime.FlushBarriers();
            const vk::MemoryBarrier2 ready{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                           .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                           .dstStageMask =
                                               vk::PipelineStageFlagBits2::eComputeShader,
                                           .dstAccessMask = vk::AccessFlagBits2::eShaderRead |
                                                            vk::AccessFlagBits2::eShaderWrite};
            const auto cmd = scheduler.CommandBuffer();
            cmd.pipelineBarrier2(
                vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &ready});
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, sets, {});
            cmd.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                              push.data());
            cmd.dispatch(1, 1, 1);
            finish();
            const auto* actual = reinterpret_cast<const u32*>(download.mapped_data.data());
            const auto* expected = reinterpret_cast<const u32*>(address) + round * 64;
            for (u32 n = 0; n < 64; ++n) {
                if (actual[n] != (round ? expected[n] : 0U) && failures < 8)
                    std::printf("DMA read int64=%u round=%u lane=%u got=%08x expected=%08x\n",
                                int64, round, n, actual[n], round ? expected[n] : 0U);
                check(actual[n] == (round ? expected[n] : 0U));
            }
            bc.ProcessFaultBuffer();
            scheduler.Finish();
            scheduler.PopPendingOperations();
            const vk::BufferCopy entry_copy{(address >> bc.GetSparsePageShift()) * 8, 0x10000, 8};
            runtime.CopyBuffer(bc.GetBdaPageTableBuffer(), &download, {&entry_copy, 1});
            finish();
            u64 entry{};
            std::memcpy(&entry, download.mapped_data.data() + 0x10000, 8);
            std::printf("DMA read int64=%u round=%u table_offset=%llx entry=%llx\n", int64, round,
                        static_cast<unsigned long long>(entry_copy.srcOffset),
                        static_cast<unsigned long long>(entry));
        }
        // A physical table read must reject guest addresses outside its 40-bit
        // coverage before accessing either the table or the fault bitmap.
        for (u64 invalid : {1ULL << 40, 0xffffffffffffff00ULL}) {
            std::array<u32, 32> push{};
            push[4] = u32(invalid);
            push[5] = u32(invalid >> 32);
            const auto cmd = scheduler.CommandBuffer();
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, sets, {});
            cmd.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                              push.data());
            cmd.dispatch(1, 1, 1);
            finish();
            const auto* actual = reinterpret_cast<const u32*>(download.mapped_data.data());
            for (u32 n = 0; n < 64; ++n)
                check(actual[n] == 0);
        }
    }
    scheduler.Finish();
    scheduler.PopPendingOperations();
    scheduler.SetSubmitCallback({});
    memory.Protect(backend.base, Backend::Size, Core::MemoryProt::CpuReadWrite);
    std::printf("dma residency: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int result = Run(argc, argv);
    Common::Log::Shutdown();
    return result;
}
