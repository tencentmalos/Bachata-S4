// SPDX-License-Identifier: GPL-2.0-or-later
// Run the PS4 compiler's whole-wave reduction (s_orn2_saveexec_b64 + DS_SWIZZLE xor butterfly +
// v_readlane 31/63) on the GPU inside divergent control flow, where the lanes the guest had
// disabled are absent from the SPIR-V control flow, and compare with the GCN result.
// Each case is translated twice: with clustered subgroup reductions and with the plain
// shuffle translation (profile flag off) as the counterexample.
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

constexpr u32 Lanes = 64, Width = 4;

struct Variant {
    const char* name;
    std::vector<u32> words;
    u32 input_vgpr;
    u32 value_vgpr;
    u32 low_sgpr;
    u32 high_sgpr;
    bool is_min; // UMin with -1 fill, otherwise Or with 0 fill.
    u64 contribution_mask = ~u64{0};
};

// cs 0xa38aae6c at 0xc48 (Monster Hunter: World), min-reduction of v4.
static const Variant MinVariant{"umin",
                                {0xbeea287e, 0x000c08c1, 0xd8d4401f, 0x05000006, 0xbf8c007f,
                                 0x260c0b06, 0xd8d4201f, 0x05000006, 0xbf8c007f, 0x260c0b06,
                                 0xd8d4101f, 0x05000006, 0xbf8c007f, 0x260c0b06, 0xd8d4081f,
                                 0x05000006, 0xbf8c007f, 0x260c0b06, 0xd8d4041f, 0x05000006,
                                 0xbf8c007f, 0x260c0b06, 0x02093f06, 0x020b7f06, 0xbefe046a},
                                4,
                                6,
                                4,
                                5,
                                true};
// fs 0x15e44dcc at 0xaac, or-reduction of v24 (the restore uses vcc instead of s[6:7]).
static const Variant OrVariant{
    "or",
    {0xbeea287e, 0xbf8c0f70, 0x00043080, 0xd8d4401f, 0x01000002, 0xbf8c007f, 0x38040302,
     0xd8d4201f, 0x01000002, 0xbf8c007f, 0x38040302, 0xd8d4101f, 0x01000002, 0xbf8c007f,
     0x38040302, 0xd8d4081f, 0x01000002, 0xbf8c007f, 0x38040302, 0xd8d4041f, 0x01000002,
     0xbf8c007f, 0x38040302, 0x02053f02, 0x02077f02, 0xbefe046a},
    24,
    2,
    2,
    3,
    false};

// MHR fs b0260f9a at 0x580: the fill uses EXEC & s[68:69], not the saved VCC.
static const Variant OrSubsetVariant{
    "or-exec-subset",
    {0x8784447e, 0xbeea287e, 0xbf8c0f70, 0xd2000004, 0x00120a80, 0xd8d4401f, 0x05000004,
     0xbf8c007f, 0x38080b04, 0xd8d4201f, 0x05000004, 0xbf8c007f, 0x38080b04, 0xd8d4101f,
     0x05000004, 0xbf8c007f, 0x38080b04, 0xd8d4081f, 0x05000004, 0xbf8c007f, 0x38080b04,
     0xd8d4041f, 0x05000004, 0xbf8c007f, 0x38080b04, 0x02093f04, 0x020b7f04, 0xbefe046a},
    5,
    4,
    4,
    5,
    false,
    0x55ff00ff7f0f00ffULL};
static const Variant OrSubsetGapVariant = [] {
    auto variant = OrSubsetVariant;
    variant.name = "or-exec-subset-gap";
    variant.words.insert(variant.words.begin() + 1, {0xd2160034, 0x00005ca0});
    return variant;
}();

static u32 InputOf(const Variant& v, u32 lane, u32 seed) {
    const u32 hash = (lane + 1) * 0x9e3779b1u ^ seed;
    return v.is_min ? (hash >> 8) : (1u << ((hash >> 27) & 31)) | (hash & 0x100u);
}

static IR::U1 MaskBit(IR::IREmitter& ir, const IR::U32& lane, u64 mask) {
    const IR::U32 word{ir.Select(ir.ILessThan(lane, ir.Imm32(32u), false), ir.Imm32(u32(mask)),
                                 ir.Imm32(u32(mask >> 32)))};
    const IR::U32 bit{ir.BitwiseAnd(ir.ShiftRightLogical(word, ir.BitwiseAnd(lane, ir.Imm32(31u))),
                                    ir.Imm32(1u))};
    return ir.INotEqual(bit, ir.Imm32(0u));
}

static std::vector<u32> Build(const Variant& v, u64 present, u64 exec, u32 seed, bool clustered,
                              u32* reduction_count = nullptr) {
    Profile profile{};
    profile.supported_spirv = 0x10600;
    profile.subgroup_size = 64;
    profile.supports_subgroup_clustered_reduce = clustered;
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    info.flattened_ud_buf.resize(4);
    const AmdGpu::Buffer null_buffer = AmdGpu::Buffer::Null();
    std::memcpy(info.flattened_ud_buf.data(), &null_buffer, sizeof(null_buffer));
    info.buffers.push_back({.used_types = IR::Type::U32, .is_written = true});
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.workgroup_size = {64, 1, 1};
    runtime.props.num_allocated_vgprs = 64;
    Pools pools;
    IR::Program p{info};
    auto* entry = pools.block_pool.Create(pools.inst_pool);
    auto* body = pools.block_pool.Create(pools.inst_pool);
    auto* merge = pools.block_pool.Create(pools.inst_pool);
    p.blocks = {entry, body, merge};
    Gcn::Translator t(p.info, runtime, profile);
    t.EmitPrologue(entry);

    IR::IREmitter ir{*entry};
    const IR::U32 lane = ir.LaneId();
    const IR::U32 hash{
        ir.BitwiseXor(ir.IMul(ir.IAdd(lane, ir.Imm32(1u)), ir.Imm32(0x9e3779b1u)), ir.Imm32(seed))};
    const IR::U32 input =
        v.is_min
            ? ir.ShiftRightLogical(hash, ir.Imm32(8u))
            : ir.BitwiseOr(ir.ShiftLeftLogical(
                               ir.Imm32(1u), ir.BitFieldExtract(hash, ir.Imm32(27u), ir.Imm32(5u))),
                           ir.BitwiseAnd(hash, ir.Imm32(0x100u)));
    ir.SetVectorReg(IR::VectorReg(v.input_vgpr), input);
    // Lanes of the wave the guest disabled before this region never enter the SPIR-V body;
    // present lanes outside exec model a narrowing without a control-flow split.
    const IR::U1 enter = ir.ConditionRef(MaskBit(ir, lane, present));
    ir.SetExec(MaskBit(ir, lane, exec));
    ir.SetScalarReg(IR::ScalarReg(68), ir.Imm32(u32(v.contribution_mask)));
    ir.SetScalarReg(IR::ScalarReg(69), ir.Imm32(u32(v.contribution_mask >> 32)));
    ir.SetScalarReg(IR::ScalarReg(46), ir.Imm32(1U));

    std::vector<Gcn::GcnInst> insts;
    Gcn::GcnCodeSlice slice(v.words.data(), v.words.data() + v.words.size());
    Gcn::GcnDecodeContext decoder;
    while (!slice.atEnd()) {
        insts.push_back(decoder.decodeInstruction(slice));
    }
    t.Translate(body, 0, IR::Condition::True, insts);

    IR::IREmitter out{*body};
    const IR::U32 output[] = {out.GetScalarReg(IR::ScalarReg(v.low_sgpr)),
                              out.GetScalarReg(IR::ScalarReg(v.high_sgpr)),
                              out.GetVectorReg(IR::VectorReg(v.value_vgpr)), out.Imm32(1u)};
    const IR::U32 out_lane = out.LaneId();
    for (u32 i = 0; i < Width; ++i) {
        const auto index = out.IAdd(out.IMul(out_lane, out.Imm32(Width)), out.Imm32(i));
        out.StoreBufferU32(1, out.Imm32(0u), IR::U32{index}, output[i], {});
    }
    entry->AddBranch(body);
    entry->AddBranch(merge);
    body->AddBranch(merge);

    using Node = IR::AbstractSyntaxNode;
    Node n{};
    n.type = Node::Type::Block;
    n.data.block = entry;
    p.syntax_list.push_back(n);
    n = {};
    n.type = Node::Type::If;
    n.data.if_node.cond = enter;
    n.data.if_node.body = body;
    n.data.if_node.merge = merge;
    p.syntax_list.push_back(n);
    n = {};
    n.type = Node::Type::Block;
    n.data.block = body;
    p.syntax_list.push_back(n);
    n = {};
    n.type = Node::Type::EndIf;
    n.data.end_if.merge = merge;
    p.syntax_list.push_back(n);
    n = {};
    n.type = Node::Type::Block;
    n.data.block = merge;
    p.syntax_list.push_back(n);
    n = {};
    n.type = Node::Type::Return;
    p.syntax_list.push_back(n);
    p.post_order_blocks = IR::PostOrder(p.syntax_list.front().data.block);

    Optimization::SsaRewritePass(p);
    Optimization::ConstantPropagationPass(p.post_order_blocks);
    Optimization::DeadCodeEliminationPass(p);
    Optimization::CollectShaderInfoPass(p, profile);
    if (reduction_count) {
        *reduction_count = 0;
        for (auto* block : p.blocks)
            for (const auto& inst : block->Instructions())
                *reduction_count += inst.GetOpcode() == IR::Opcode::ClusteredOr32 ||
                                    inst.GetOpcode() == IR::Opcode::ClusteredUMin32;
    }
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime, p, bindings);
}

static int Run(int argc, char** argv) {
    if (argc != 3)
        return 2;
    setbuf(stdout, nullptr);
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("wave-reduction-probe");
    Window w;
    auto driver = std::string_view(argv[1]) == "system"
                      ? Vulkan::LoadAndroidSystemDriver()
                      : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(w, 0, false, false, driver);
    if (!instance.IsSubgroupSize64Supported() || !instance.IsSubgroupClusteredReduceSupported())
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

    struct Case {
        u64 present;
        u64 exec;
    };
    const Case cases[] = {
        {~u64{0}, ~u64{0}},                             // whole wave
        {0x000000ffffffffffULL, 0x000000ffffffffffULL}, // lanes 0..39
        {0xaaaaaaaaaaaaaaaaULL, 0xaaaaaaaaaaaaaaaaULL}, // odd lanes
        {0xfffffffe00000000ULL, 0xfffffffe00000000ULL}, // high half except lane 32
        {0x0000000000000020ULL, 0x0000000000000020ULL}, // one lane in the low half
        {0x8000000000000001ULL, 0x8000000000000001ULL}, // lanes 0 and 63
        {~u64{0}, 0x0f0f0f0f0f0f0f0fULL},               // narrowing without a split
        {0x7fffffff7fffffffULL, 0x3fffffff3fffffffULL}, // lanes 31/63 absent
        {0x00ff00ff00ff00ffULL, 0x00f000ff00f000ffULL}, // mixed
    };
    u32 checks[2]{}, failures[2]{};
    for (bool overwrite : {false, true}) {
        auto unrelated_mask = OrSubsetVariant;
        if (overwrite) {
            unrelated_mask.words.insert(unrelated_mask.words.begin() + 1, 0x8884447e);
        } else {
            unrelated_mask.words[0] = 0x8884447e; // OR with EXEC is not a subset proof.
        }
        u32 reductions{};
        Build(unrelated_mask, ~u64{0}, ~u64{0}, 1U, true, &reductions);
        ++checks[1];
        if (reductions) {
            ++failures[1];
            printf("FAIL non-subset/overwritten mask accepted\n");
        }
    }
    for (const Variant* v : {&MinVariant, &OrVariant, &OrSubsetVariant, &OrSubsetGapVariant}) {
        for (u32 c = 0; c < std::size(cases); ++c) {
            for (u32 seed : {0x12345678u, 0xdeadbeefu, 0x0u}) {
                for (bool clustered : {false, true}) {
                    const auto& [present, exec] = cases[c];
                    const auto code = Build(*v, present, exec, seed, clustered);
                    if (seed == 0x12345678u) {
                        std::ofstream out(std::string(argv[2]) + "/wave-" + v->name + "-" +
                                              std::to_string(c) + (clustered ? "-new" : "-old") +
                                              ".spv",
                                          std::ios::binary);
                        out.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
                    }
                    auto sm = Vulkan::Check(d.createShaderModuleUnique(
                        {.codeSize = code.size() * 4, .pCode = code.data()}));
                    vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{
                        .requiredSubgroupSize = 64};
                    auto pipe = Vulkan::Check(d.createComputePipelineUnique(
                        {}, {.stage = {.pNext = &subgroup,
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
                    cmd.pipelineBarrier(
                        vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
                        {},
                        vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                                          .dstAccessMask = vk::AccessFlagBits::eHostRead},
                        {}, {});
                    Vulkan::Check(cmd.end());
                    vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
                    Vulkan::Check(instance.GetGraphicsQueue().submit(submit, *fence));
                    if (d.waitForFences(*fence, true, 5000000000ULL) != vk::Result::eSuccess)
                        return 5;

                    // GCN: every lane runs the butterfly; lanes outside SAVE hold the identity.
                    const u32 identity = v->is_min ? ~0u : 0u;
                    u32 half[2] = {identity, identity};
                    for (u32 lane = 0; lane < Lanes; ++lane) {
                        if ((present >> lane & 1) && (exec >> lane & 1) &&
                            (v->contribution_mask >> lane & 1)) {
                            const u32 x = InputOf(*v, lane, seed);
                            half[lane >> 5] =
                                v->is_min ? std::min(half[lane >> 5], x) : half[lane >> 5] | x;
                        }
                    }
                    for (u32 lane = 0; lane < Lanes; ++lane) {
                        const bool entered = present >> lane & 1;
                        const u32 expected[Width] = {
                            entered ? half[0] : 0xcdcdcdcdu, entered ? half[1] : 0xcdcdcdcdu,
                            entered ? half[lane >> 5] : 0xcdcdcdcdu, entered ? 1u : 0xcdcdcdcdu};
                        for (u32 i = 0; i < Width; ++i) {
                            ++checks[clustered];
                            const u32 got = mapped[lane * Width + i];
                            if (got != expected[i] && failures[clustered]++ < 12) {
                                std::printf("FAIL %s %s case=%u seed=%08x lane=%u field=%u "
                                            "got=%08x want=%08x\n",
                                            clustered ? "new" : "old", v->name, c, seed, lane, i,
                                            got, expected[i]);
                            }
                        }
                    }
                    Vulkan::Check(d.resetFences(*fence));
                    Vulkan::Check(d.resetCommandPool(*cp));
                }
            }
        }
    }
    d.unmapMemory(*mem);
    std::printf("WAVE_REDUCTION old %u checks / %u failures, new %u checks / %u failures\n",
                checks[0], failures[0], checks[1], failures[1]);
    return failures[1] ? 1 : 0;
}

int main(int argc, char** argv) {
    int result = 3;
    try {
        result = Run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
    }
    Common::Log::Shutdown();
    return result;
}
