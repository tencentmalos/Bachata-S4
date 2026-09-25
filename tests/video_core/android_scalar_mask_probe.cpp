// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise decoded GCN mask writes followed by ordinary scalar reads on the GPU.
#include <cstdio>
#include <fstream>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
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
constexpr u32 Width = 23, Lanes = 64;
static std::vector<u32> Build(u32 threshold) {
    Profile profile{};
    profile.supported_spirv = 0x10600;
    profile.subgroup_size = 64;
    Info info{};
    info.stage = Stage::Compute;
    info.l_stage = LogicalStage::Compute;
    info.buffers.push_back({.used_types = IR::Type::U32, .is_written = true});
    RuntimeInfo runtime{};
    runtime.Initialize(Stage::Compute);
    runtime.cs_info.workgroup_size = {64, 1, 1};
    runtime.num_allocated_vgprs = 32;
    Pools pools;
    IR::Program p{info};
    auto* b = pools.block_pool.Create(pools.inst_pool);
    p.blocks = {b};
    p.post_order_blocks = {b};
    p.syntax_list.push_back({.data = {.block = b}, .type = IR::AbstractSyntaxNode::Type::Block});
    p.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Gcn::Translator t(info, runtime, profile);
    t.EmitPrologue(b);
    IR::IREmitter ir{*b};
    auto emit = [&](std::initializer_list<u32> words) {
        Gcn::GcnCodeSlice slice(words.begin(), words.end());
        Gcn::GcnDecodeContext decoder;
        t.TranslateInstruction(decoder.decodeInstruction(slice));
    };
    auto scalar = [](u32 n) { return IR::ScalarReg(n); };
    const auto lane = ir.LaneId();
    const auto bit = threshold <= 64
                         ? ir.ILessThan(lane, ir.Imm32(threshold), false)
                         : ir.INotEqual(ir.BitwiseAnd(lane, ir.Imm32(1u)), ir.Imm32(0u));
    // Poison old numeric definitions: a mask write must replace both halves.
    ir.SetScalarReg(scalar(0), ir.Imm32(0x12345678u));
    ir.SetScalarReg(scalar(1), ir.Imm32(0x9abcdef0u));
    ir.SetScalarReg(scalar(20), ir.Imm32(0x11111111u));
    ir.SetScalarReg(scalar(21), ir.Imm32(0x22222222u));
    ir.SetExec(ir.Imm1(true));
    ir.SetVcc(ir.Imm1(true));
    emit({0xbe94246a});             // s_and_saveexec_b64 s[20:21], vcc
    ir.SetVectorReg(IR::VectorReg::V18, IR::U32{ir.Select(bit, ir.Imm32(1u), ir.Imm32(0u))});
    emit({0xd18a0000, 0x00022480}); // v_cmp_ne_u32 s[0:1], 0, v18
    emit({0x81eb0014});             // s_sub_i32 vcc_hi, s20, s0
    emit({0x81ea0115});             // s_sub_i32 vcc_lo, s21, s1
    emit({0x886a6a6b});             // s_or_b32 vcc_lo, vcc_hi, vcc_lo
    emit({0xbf06806a});             // s_cmp_eq_u32 vcc_lo, 0
    const auto equal = IR::U32{ir.Select(ir.GetScc(), ir.Imm32(1u), ir.Imm32(0u))};
    emit({0xbe880400});             // s_mov_b64 s[8:9], s[0:1], copy mask views
    ir.SetScalarReg(scalar(4), ir.Imm32(0xdeadbeefu));
    ir.SetScalarReg(scalar(5), ir.Imm32(0x76543210u));
    emit({0xbe8c0404}); // s_mov_b64 s[12:13], s[4:5], ordinary 64-bit payload
    // Spill both data and masks to VGPR lanes, as the resource descriptor path does.
    ir.SetVectorReg(IR::VectorReg::V15, ir.Imm32(0u));
    emit({0x041f0004}); // v_writelane_b32 v15, s4, 0
    emit({0x041f0205}); // v_writelane_b32 v15, s5, 1
    ir.SetScalarReg(scalar(17), ir.Imm32(0xaabbccddu));
    emit({0x0221010f}); // v_readlane_b32 s16, v15, 0
    const auto neighbor = ir.GetScalarReg(scalar(17));
    emit({0x0223030f}); // v_readlane_b32 s17, v15, 1
    emit({0x041f0400}); // v_writelane_b32 v15, s0, 2
    emit({0x041f0601}); // v_writelane_b32 v15, s1, 3
    emit({0x0225050f}); // v_readlane_b32 s18, v15, 2
    emit({0x0227070f}); // v_readlane_b32 s19, v15, 3
    // VOPC writes VCC, then scalar ALU reads vcc_lo/vcc_hi (MHW's second loop).
    ir.SetVccLo(ir.Imm32(0x11223344u));
    ir.SetVccHi(ir.Imm32(0x55667788u));
    emit({0x7d842480}); // v_cmp_eq_u32 vcc, 0, v18
    const auto vcc_lo = ir.GetVccLo();
    const auto vcc_hi = ir.GetVccHi();
    emit({0xbe96046a}); // s_mov_b64 s[22:23], vcc
    emit({0xbeea0404}); // s_mov_b64 vcc, s[4:5], ordinary 64-bit payload
    const auto vcc_data_lo = ir.GetVccLo();
    const auto vcc_data_hi = ir.GetVccHi();
    ir.SetVccHi(ir.Imm32(0xaabbccddu));
    emit({0x02d5010f}); // v_readlane_b32 vcc_lo, v15, 0; preserve vcc_hi
    const IR::U32 output[] = {ir.GetScalarReg(scalar(0)),
                              ir.GetScalarReg(scalar(1)),
                              ir.GetScalarReg(scalar(20)),
                              ir.GetScalarReg(scalar(21)),
                              equal,
                              ir.GetScalarReg(scalar(8)),
                              ir.GetScalarReg(scalar(9)),
                              ir.GetScalarReg(scalar(12)),
                              ir.GetScalarReg(scalar(13)),
                              lane,
                              ir.GetScalarReg(scalar(16)),
                              ir.GetScalarReg(scalar(17)),
                              neighbor,
                              ir.GetScalarReg(scalar(18)),
                              ir.GetScalarReg(scalar(19)),
                              vcc_lo,
                              vcc_hi,
                              ir.GetScalarReg(scalar(22)),
                              ir.GetScalarReg(scalar(23)),
                              vcc_data_lo,
                              vcc_data_hi,
                              ir.GetVccLo(),
                              ir.GetVccHi()};
    for (u32 i = 0; i < Width; ++i) {
        const auto index = ir.IAdd(ir.IMul(lane, ir.Imm32(Width)), ir.Imm32(i));
        ir.StoreBufferU32(1, ir.Imm32(0u), IR::U32{index}, output[i], {});
    }
    Optimization::SsaRewritePass(p);
    Optimization::ConstantPropagationPass(p.post_order_blocks);
    Optimization::ReadLaneEliminationPass(p);
    Optimization::DeadCodeEliminationPass(p);
    Optimization::CollectShaderInfoPass(p, profile);
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime, p, bindings);
}
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    setbuf(stdout, nullptr);
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("scalar-mask-probe");
    Window w;
    auto driver = std::string_view(argv[1]) == "system"
                      ? Vulkan::LoadAndroidSystemDriver()
                      : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(w, 0, false, false, driver);
    if (!instance.IsSubgroupSize64Supported())
        return 3;
    auto d = instance.GetDevice();
    auto buffer = Vulkan::Check(d.createBufferUnique(
        {.size = Lanes * Width * 4, .usage = vk::BufferUsageFlagBits::eStorageBuffer}));
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
    auto pl =
        Vulkan::Check(d.createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &*layout}));
    vk::DescriptorPoolSize ps{vk::DescriptorType::eStorageBuffer, 1};
    auto pool = Vulkan::Check(
        d.createDescriptorPoolUnique({.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps}));
    auto sets = Vulkan::Check(d.allocateDescriptorSets(
        {.descriptorPool = *pool, .descriptorSetCount = 1, .pSetLayouts = &*layout}));
    vk::DescriptorBufferInfo bi{*buffer, 0, Lanes * Width * 4};
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
    for (u32 threshold : {0u, 1u, 31u, 32u, 33u, 63u, 64u, 65u}) {
        auto code = Build(threshold);
        std::ofstream out(std::string(argv[2]) + "/mask-" + std::to_string(threshold) + ".spv",
                          std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
        out.close();
        auto sm = Vulkan::Check(
            d.createShaderModuleUnique({.codeSize = code.size() * 4, .pCode = code.data()}));
        vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{.requiredSubgroupSize = 64};
        auto pipe = Vulkan::Check(
            d.createComputePipelineUnique({}, {.stage = {.pNext = &subgroup,
                                                         .stage = vk::ShaderStageFlagBits::eCompute,
                                                         .module = *sm,
                                                         .pName = "main"},
                                               .layout = *pl}));
        std::fill(mapped, mapped + Lanes * Width, 0xcdcdcdcdu);
        auto cmd = commands[0];
        Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{}));
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pl, 0, sets, {});
        cmd.dispatch(1, 1, 1);
        Vulkan::Check(cmd.end());
        vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
        Vulkan::Check(instance.GetGraphicsQueue().submit(submit, *fence));
        if (d.waitForFences(*fence, true, 5000000000ULL) != vk::Result::eSuccess)
            return 5;
        const u64 mask = threshold == 65   ? 0xaaaaaaaaaaaaaaaaULL
                         : threshold >= 64 ? ~u64{0}
                                           : (u64{1} << threshold) - 1;
        for (u32 lane = 0; lane < Lanes; ++lane) {
            const u32 expected[] = {u32(mask),
                                    u32(mask >> 32),
                                    ~0u,
                                    ~0u,
                                    threshold == 64,
                                    u32(mask),
                                    u32(mask >> 32),
                                    0xdeadbeefu,
                                    0x76543210u,
                                    lane,
                                    0xdeadbeefu,
                                    0x76543210u,
                                    0xaabbccddu,
                                    u32(mask),
                                    u32(mask >> 32),
                                    u32(~mask),
                                    u32(~mask >> 32),
                                    u32(~mask),
                                    u32(~mask >> 32),
                                    0xdeadbeefu,
                                    0x76543210u,
                                    0xdeadbeefu,
                                    0xaabbccddu};
            for (u32 i = 0; i < Width; ++i) {
                ++checks;
                if (mapped[lane * Width + i] != expected[i]) {
                    if (failures++ < 16)
                        std::printf("FAIL mask=%u lane=%u field=%u got=%08x want=%08x\n", threshold,
                                    lane, i, mapped[lane * Width + i], expected[i]);
                }
            }
        }
        Vulkan::Check(d.resetFences(*fence));
        Vulkan::Check(d.resetCommandPool(*cp));
    }
    d.unmapMemory(*mem);
    std::printf("SCALAR_MASK %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
