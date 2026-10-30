// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_ordered_dispatch.h"
#include <bit>
#include <cstdlib>
using namespace Shader;
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


constexpr u32 Lanes = 64, Groups = 2052, Width = 8;
// Binding 0 also acts as the GDS backing: first 64 KiB are reserved for it.
constexpr u32 OutputBase = 16384, Words = OutputBase + Groups * Lanes * Width;

static std::vector<u32> Build(u64 mask, u32 mode) {
    Profile profile{};
    profile.supported_spirv = 0x10600;
    profile.subgroup_size = 64;
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.workgroup_size = {64, 1, 1};
    runtime.hw.cs.tgid_enable = {true, true, true};
    runtime.hw.cs.tg_size_enable = true;
    Pools pools;
    IR::Program p{info};
    auto* b = pools.block_pool.Create(pools.inst_pool);
    p.blocks = {b};
    p.syntax_list.emplace_back();
    p.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Block;
    p.syntax_list.back().data.block = b;
    p.syntax_list.emplace_back();
    p.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Return;
    p.post_order_blocks = IR::PostOrder(b);
    Gcn::Translator t(p.info, runtime, profile);
    t.EmitPrologue(b);
    IR::IREmitter ir{*b};
    const auto lane = ir.LaneId();
    const IR::U32 word{ir.Select(ir.ILessThan(lane, ir.Imm32(32u), false),
                                 ir.Imm32(u32(mask)), ir.Imm32(u32(mask >> 32)))};
    ir.SetExec(ir.INotEqual(ir.BitwiseAnd(ir.ShiftRightLogical(
        word, ir.BitwiseAnd(lane, ir.Imm32(31u))), ir.Imm32(1u)), ir.Imm32(0u)));
    // Use a nonzero GDS base, and distinct lane values to detect wrong election.
    ir.SetM0(ir.Imm32(8u << 16));
    ir.SetVectorReg(IR::VectorReg::V1, ir.IAdd(lane, ir.Imm32(1u)));
    // Exact ADD/SWAP instruction encodings, no hand-built substitute atomic.
    const u32 words[] = {0xd8fe0000u | (mode << 8), 0x03000001u};
    Gcn::GcnCodeSlice slice(words, words + 2);
    Gcn::GcnDecodeContext decoder;
    t.TranslateInstruction(decoder.decodeInstruction(slice));
    ASSERT(!info.translation_failed && info.uses_ordered_count);
    const auto group = ir.GetAttributeU32(IR::Attribute::WorkgroupIndex);
    const IR::U32 previous{ir.Select(ir.IEqual(group, ir.Imm32(0u)),
        ir.Imm32(OutputBase - 1),
        ir.IAdd(ir.Imm32(OutputBase + 3), ir.IMul(ir.IAdd(
            ir.IMul(ir.ISub(group, ir.Imm32(1u)), ir.Imm32(Lanes)),
            ir.BitwiseAnd(ir.IAdd(lane, ir.Imm32(1u)), ir.Imm32(Lanes - 1))), ir.Imm32(Width))))};
    const IR::U32 fields[] = {ir.GetVectorReg(IR::VectorReg::V3),
                              ir.GetScalarReg(IR::ScalarReg(3)), group, lane,
                              ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0),
                              ir.GetAttributeU32(IR::Attribute::WorkgroupId, 1),
                              ir.GetAttributeU32(IR::Attribute::WorkgroupId, 2),
                              ir.ReadConstBuffer(ir.Imm32(0u), previous, {})};
    for (u32 i = 0; i < Width; ++i) {
        const IR::U32 index{ir.IAdd(ir.Imm32(OutputBase + i), ir.IMul(
            ir.IAdd(ir.IMul(group, ir.Imm32(Lanes)), lane), ir.Imm32(Width)))};
        ir.StoreBufferU32(1, ir.Imm32(0u), index, fields[i], {});
    }
    Optimization::SsaRewritePass(p);
    Optimization::ConstantPropagationPass(p.post_order_blocks);
    Optimization::ResourceDiscoveryList resources;
    for (auto& inst : b->Instructions()) {
        if (inst.GetOpcode() == IR::Opcode::DataOrderedCount)
            resources.push_back({.user = &inst});
    }
    Optimization::ResourcePatchingPass(info, resources, profile);
    ASSERT(info.buffers.size() == 1 && info.buffers[0].buffer_type == BufferType::GdsBuffer);
    Optimization::DeadCodeEliminationPass(p);
    Optimization::CollectShaderInfoPass(p, profile);
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime, p, bindings);
}
static int Run(int argc, char** argv) {
    if (argc != 3)
        return 2;
    setbuf(stdout, nullptr);
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("ordered-count-probe");
    Window w;
    auto driver = std::string_view(argv[1]) == "system"
                      ? Vulkan::LoadAndroidSystemDriver()
                      : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(w, 0, false, false, driver);
    if (!instance.IsSubgroupSize64Supported())
        return 3;
    auto d = instance.GetDevice();
    auto buffer = Vulkan::Check(d.createBufferUnique(
        {.size = Words * 4, .usage = vk::BufferUsageFlagBits::eStorageBuffer}));
    const auto req = d.getBufferMemoryRequirements(*buffer);
    const auto mp = instance.GetMemoryProperties();
    const auto flags =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    u32 mt = 0;
    while (mt < mp.memoryTypeCount && (!(req.memoryTypeBits & (1u << mt)) ||
                                       (mp.memoryTypes[mt].propertyFlags & flags) != flags))
        ++mt;
    if (mt == mp.memoryTypeCount)
        return 4;
    auto mem =
        Vulkan::Check(d.allocateMemoryUnique({.allocationSize = req.size, .memoryTypeIndex = mt}));
    Vulkan::Check(d.bindBufferMemory(*buffer, *mem, 0));
    auto* mapped = static_cast<u32*>(Vulkan::Check(d.mapMemory(*mem, 0, req.size)));
    vk::DescriptorSetLayoutBinding db{.binding = 0,
                                      .descriptorType = vk::DescriptorType::eStorageBuffer,
                                      .descriptorCount = 1,
                                      .stageFlags = vk::ShaderStageFlagBits::eCompute};
    auto layout =
        Vulkan::Check(d.createDescriptorSetLayoutUnique({.bindingCount = 1, .pBindings = &db}));
    vk::PushConstantRange pc{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                              .offset = 0, .size = sizeof(PushData)};
    auto pl = Vulkan::Check(d.createPipelineLayoutUnique({.setLayoutCount = 1,
        .pSetLayouts = &*layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc}));
    vk::DescriptorPoolSize ps{vk::DescriptorType::eStorageBuffer, 1};
    auto pool = Vulkan::Check(
        d.createDescriptorPoolUnique({.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps}));
    auto sets = Vulkan::Check(d.allocateDescriptorSets(
        {.descriptorPool = *pool, .descriptorSetCount = 1, .pSetLayouts = &*layout}));
    vk::DescriptorBufferInfo bi{*buffer, 0, Words * 4};
    vk::WriteDescriptorSet wr{.dstSet = sets[0],
                              .dstBinding = 0,
                              .descriptorCount = 1,
                              .descriptorType = vk::DescriptorType::eStorageBuffer,
                              .pBufferInfo = &bi};
    d.updateDescriptorSets(wr, {});
    auto cp = Vulkan::Check(
        d.createCommandPoolUnique({.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()}));
    auto commands = Vulkan::Check(d.allocateCommandBuffers(
        {.commandPool = *cp, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}));
    auto fence = Vulkan::Check(d.createFenceUnique({}));


    u32 checks = 0, failures = 0;
    for (u64 mask : {~u64{0}, u64{1} << 39, u64{0}}) {
        for (u32 mode : {3u, 0x13u}) {
            auto code = Build(mask, mode);
            std::ofstream spv(std::string(argv[2]) + "/ordered-" + std::to_string(mask) + "-" +
                                  std::to_string(mode) + ".spv", std::ios::binary);
            spv.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
            spv.close();
            if (std::getenv("SHAD_TEST_EMIT_ONLY")) continue;
            auto sm = Vulkan::Check(d.createShaderModuleUnique(
                {.codeSize = code.size() * 4, .pCode = code.data()}));
            vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{.requiredSubgroupSize = 64};
            auto pipe = Vulkan::Check(d.createComputePipelineUnique({}, {
                .flags = vk::PipelineCreateFlagBits::eDispatchBase,
                .stage = {.pNext = &subgroup, .stage = vk::ShaderStageFlagBits::eCompute,
                          .module = *sm, .pName = "main"}, .layout = *pl}));
            for (const auto dims : {std::array<u32, 3>{Groups, 1, 1},
                                    std::array<u32, 3>{19, 9, 12},
                                    std::array<u32, 3>{7, 3, 2},
                                    std::array<u32, 3>{1, 1, 1},
                                    std::array<u32, 3>{0, 1, 1},
                                    std::array<u32, 3>{1, 0, 1},
                                    std::array<u32, 3>{1, 1, 0}}) {
                const u32 groups = dims[0] * dims[1] * dims[2];
                std::fill(mapped, mapped + Words, 0xcdcdcdcdu);
                mapped[2] = 17;
                PushData push{};
                push.xoffset = std::bit_cast<float>(dims[0]);
                push.yoffset = std::bit_cast<float>(dims[1]);
                push.xscale = std::bit_cast<float>(dims[2]);
                const bool batch = std::getenv("SHAD_TEST_BATCH");
                push.yscale = std::bit_cast<float>(u32(batch));
                auto cmd = commands[0];
                Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{}));
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pl, 0, sets, {});
                cmd.pushConstants(*pl, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                if (batch) {
                    cmd.dispatch(1, 1, 1);
                } else if (std::getenv("SHAD_TEST_REVERSE")) {
                    for (u32 g = groups; g-- > 0;) {
                        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                            vk::PipelineStageFlagBits::eComputeShader, {},
                            vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                                .dstAccessMask = vk::AccessFlagBits::eShaderRead |
                                                 vk::AccessFlagBits::eShaderWrite}, {}, {});
                        cmd.dispatchBase(g % dims[0], (g / dims[0]) % dims[1],
                                         g / (dims[0] * dims[1]), 1, 1, 1);
                    }
                } else {
                    Vulkan::DispatchOrderedGroups(cmd, dims[0], dims[1], dims[2]);
                }
                cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                    vk::PipelineStageFlagBits::eHost, {},
                    vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                                      .dstAccessMask = vk::AccessFlagBits::eHostRead}, {}, {});
                Vulkan::Check(cmd.end());
                vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
                Vulkan::Check(instance.GetGraphicsQueue().submit(submit, *fence));
                if (d.waitForFences(*fence, true, 10000000000ULL) != vk::Result::eSuccess)
                    return 5;
                const u32 contribution = mask ? std::countr_zero(mask) + 1 : 0;
                u32 counter = 17;
                for (u32 g = 0; g < groups; ++g) {
                    for (u32 lane = 0; lane < Lanes; ++lane) {
                        const u32 expect[] = {counter, 0x80000001u | ((g & 2047u) << 6), g, lane,
                            g % dims[0], (g / dims[0]) % dims[1], g / (dims[0] * dims[1]),
                            g ? (lane + 1) % Lanes : 0xcdcdcdcdu};
                        for (u32 i = 0; i < Width; ++i) {
                            ++checks;
                            const u32 got = mapped[OutputBase + (g * Lanes + lane) * Width + i];
                            if (got != expect[i] && failures++ < 12)
                                printf("FAIL mask=%llx mode=%u group=%u lane=%u field=%u got=%x want=%x\n",
                                    static_cast<unsigned long long>(mask), mode, g, lane, i, got, expect[i]);
                        }
                    }
                    if (mask) counter = mode == 3 ? counter + contribution : contribution;
                }
                ++checks;
                if (mapped[2] != counter) ++failures;
                if (groups < Groups) {
                    ++checks;
                    if (mapped[OutputBase + groups * Lanes * Width] != 0xcdcdcdcdu) ++failures;
                }
                // Neighbouring GDS words must remain untouched.
                for (u32 i : {0u, 1u, 3u, 16383u}) {
                    ++checks;
                    if (mapped[i] != 0xcdcdcdcdu) ++failures;
                }
                Vulkan::Check(d.resetFences(*fence));
                Vulkan::Check(d.resetCommandPool(*cp));
            }
        }
    }
    d.unmapMemory(*mem);
    printf("ORDERED_COUNT %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    int result = 3;
    try { result = Run(argc, argv); }
    catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); }
    Common::Log::Shutdown();
    return result;
}
