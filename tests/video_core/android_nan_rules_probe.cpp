// SPDX-License-Identifier: GPL-2.0-or-later
// GCN float min/max/med3/clamp NaN rules through the production SPIR-V emitter on the GPU.
// GCN min/max return the operand that is not NaN, med3 with a NaN input is min3, and an output
// clamp turns NaN into 0. Games rely on this to sanitize 0 * inf or x / 0 (MHR material shaders
// clamp a NaN blend weight); a driver free to propagate NaN renders those pixels black.
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
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

constexpr std::array<u32, 10> Values{
    0x7fc00000u, // NaN
    0xffc00000u, // -NaN
    0x7f800000u, // +inf
    0xff800000u, // -inf
    0xc0000000u, // -2
    0x80000000u, // -0
    0x00000000u, // +0
    0x3e800000u, // 0.25
    0x3f800000u, // 1
    0x40400000u, // 3
};
constexpr u32 Cases = Values.size() * Values.size() * Values.size();
constexpr u32 Lanes = 64, Batches = (Cases + Lanes - 1) / Lanes;
constexpr u32 Outputs = 12;
constexpr u32 OutputBase = 3 * Batches * Lanes;

static std::vector<u32> Build(u32 batch, bool nan_preserve) {
    Profile profile{};
    profile.supported_spirv = 0x10600;
    profile.subgroup_size = 64;
    profile.support_fp32_signed_zero_inf_nan_preserve = nan_preserve;
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    info.flattened_ud_buf.resize(4);
    const AmdGpu::Buffer null_buffer = AmdGpu::Buffer::Null();
    std::memcpy(info.flattened_ud_buf.data(), &null_buffer, sizeof(null_buffer));
    info.buffers.push_back({.used_types = IR::Type::U32, .is_written = true});
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.workgroup_size = {Lanes, 1, 1};
    runtime.props.num_allocated_vgprs = 8;
    Pools pools;
    IR::Program p{info};
    auto* b = pools.block_pool.Create(pools.inst_pool);
    p.blocks = {b};
    p.post_order_blocks = {b};
    p.syntax_list.push_back({.data = {.block = b}, .type = IR::AbstractSyntaxNode::Type::Block});
    p.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Gcn::Translator t(p.info, runtime, profile);
    t.EmitPrologue(b);
    IR::IREmitter ir{*b};
    const auto handle = ir.Imm32(0u);
    const IR::U32 item{ir.IAdd(ir.LaneId(), ir.Imm32(batch * Lanes))};
    const auto load = [&](u32 operand) {
        const auto index = ir.IAdd(ir.IMul(item, ir.Imm32(3u)), ir.Imm32(operand));
        return ir.BitCast<IR::F32>(IR::U32{ir.LoadBufferU32(1, handle, index, {})});
    };
    const IR::F32 a = load(0), b_ = load(1), c = load(2);
    const IR::F32 results[Outputs] = {
        IR::F32{ir.FPSaturate(a)},
        IR::F32{ir.FPMax(a, b_)},
        IR::F32{ir.FPMin(a, b_)},
        IR::F32{ir.FPMaxTri(a, b_, c)},
        IR::F32{ir.FPMinTri(a, b_, c)},
        IR::F32{ir.FPMedTri(a, b_, c)},
        IR::F32{ir.FPClamp(a, b_, c)},
        // v_rcp_f32 + v_mad_f32 ... clamp, as in the MHR blend weight.
        IR::F32{ir.FPSaturate(ir.FPFma(ir.FPRecip(b_), ir.FPNeg(a), ir.Imm32(1.0f)))},
        // The same as separate v_mul_f32 / v_add_f32, the clamp on the last one.
        IR::F32{ir.FPSaturate(ir.FPAdd(ir.FPMul(ir.FPRecip(b_), ir.FPNeg(a)), ir.Imm32(1.0f)))},
        IR::F32{ir.FPSaturate(ir.FPMul(a, b_))},
        IR::F32{ir.FPSaturate(ir.FPAdd(a, b_))},
        IR::F32{ir.FPSaturate(ir.FPSub(ir.Imm32(1.0f), a))},
    };
    for (u32 i = 0; i < Outputs; ++i) {
        const auto index = ir.IAdd(ir.IMul(item, ir.Imm32(Outputs)), ir.Imm32(OutputBase + i));
        ir.StoreBufferU32(1, handle, index, ir.BitCast<IR::U32>(results[i]), {});
    }
    Optimization::SsaRewritePass(p);
    Optimization::ConstantPropagationPass(p.post_order_blocks);
    Optimization::DeadCodeEliminationPass(p);
    Optimization::CollectShaderInfoPass(p, profile);
    Backend::Bindings bindings{};
    return Backend::SPIRV::EmitSPIRV(profile, runtime, p, bindings);
}

static float F(u32 bits) {
    return std::bit_cast<float>(bits);
}
static float NMin(float x, float y) {
    return std::isnan(x) ? y : std::isnan(y) ? x : std::fmin(x, y);
}
static float NMax(float x, float y) {
    return std::isnan(x) ? y : std::isnan(y) ? x : std::fmax(x, y);
}
static float Med3(float a, float b, float c) {
    if (std::isnan(a) || std::isnan(b) || std::isnan(c))
        return NMin(NMin(a, b), c);
    return std::fmax(std::fmin(a, b), std::fmin(std::fmax(a, b), c));
}
static float Sat(float x) {
    return std::isnan(x) ? 0.f : std::fmin(std::fmax(x, 0.f), 1.f);
}

static int Run(int argc, char** argv) {
    if (argc != 3)
        return 2;
    setbuf(stdout, nullptr);
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("nan-rules-probe");
    Window w;
    auto driver = std::string_view(argv[1]) == "system"
                      ? Vulkan::LoadAndroidSystemDriver()
                      : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(w, 0, false, false, driver);
    if (!instance.IsSubgroupSize64Supported())
        return 3;
    const auto float_controls =
        instance.GetPhysicalDevice()
            .getProperties2<vk::PhysicalDeviceProperties2,
                            vk::PhysicalDeviceFloatControlsProperties>()
            .get<vk::PhysicalDeviceFloatControlsProperties>();
    const bool nan_preserve = float_controls.shaderSignedZeroInfNanPreserveFloat32;
    std::printf("shaderSignedZeroInfNanPreserveFloat32=%d\n", nan_preserve);
    auto d = instance.GetDevice();
    const u32 words = OutputBase + Batches * Lanes * Outputs;
    auto buffer = Vulkan::Check(d.createBufferUnique(
        {.size = words * 4, .usage = vk::BufferUsageFlagBits::eStorageBuffer}));
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
    std::fill(mapped, mapped + words, 0xcdcdcdcdu);
    for (u32 i = 0; i < Cases; ++i) {
        mapped[i * 3 + 0] = Values[i % 10];
        mapped[i * 3 + 1] = Values[i / 10 % 10];
        mapped[i * 3 + 2] = Values[i / 100];
    }
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
    vk::DescriptorBufferInfo bi{*buffer, 0, words * 4};
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
    for (u32 batch = 0; batch < Batches; ++batch) {
        auto code = Build(batch, nan_preserve);
        if (batch == 0) {
            std::ofstream out(std::string(argv[2]) + "/nan-rules.spv", std::ios::binary);
            out.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
        }
        auto sm = Vulkan::Check(
            d.createShaderModuleUnique({.codeSize = code.size() * 4, .pCode = code.data()}));
        vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{.requiredSubgroupSize = 64};
        auto pipe = Vulkan::Check(
            d.createComputePipelineUnique({}, {.stage = {.pNext = &subgroup,
                                                         .stage = vk::ShaderStageFlagBits::eCompute,
                                                         .module = *sm,
                                                         .pName = "main"},
                                               .layout = *pl}));
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
        Vulkan::Check(d.resetFences(*fence));
        Vulkan::Check(d.resetCommandPool(*cp));
    }
    static constexpr const char* Names[Outputs] = {
        "sat",  "max",   "min",           "max3",          "min3",    "med3",
        "clamp", "rcp-fma-sat", "rcp-mul-add-sat", "mul-sat", "add-sat", "sub-sat"};
    u32 checks = 0, failures = 0, per_op[Outputs]{};
    for (u32 i = 0; i < Cases; ++i) {
        const float a = F(mapped[i * 3]), b = F(mapped[i * 3 + 1]), c = F(mapped[i * 3 + 2]);
        const float expected[Outputs] = {
            Sat(a),
            NMax(a, b),
            NMin(a, b),
            NMax(NMax(a, b), c),
            NMin(NMin(a, b), c),
            Med3(a, b, c),
            NMin(NMax(a, b), c),
            Sat(std::fma(1.f / b, -a, 1.f)),
            Sat(1.f / b * -a + 1.f),
            Sat(a * b),
            Sat(a + b),
            Sat(1.f - a),
        };
        for (u32 op = 0; op < Outputs; ++op) {
            // NClamp is undefined for crossed or NaN bounds.
            if (op == 6 && (std::isnan(b) || std::isnan(c) || b > c))
                continue;
            const float got = F(mapped[OutputBase + i * Outputs + op]);
            const float want = expected[op];
            bool ok;
            if (std::isnan(want) || std::isnan(got)) {
                ok = std::isnan(want) && std::isnan(got);
            } else {
                // Signed zeros compare equal; the fused multiply-add may round differently.
                const float tolerance = op == 7 || op == 8 ? 1e-6f : 0.f;
                ok = got == want || std::fabs(got - want) <= tolerance;
            }
            ++checks;
            per_op[op] += !ok;
            if (!ok && failures++ < 24)
                std::printf("FAIL %s a=%08x b=%08x c=%08x got=%08x want=%08x\n", Names[op],
                            mapped[i * 3], mapped[i * 3 + 1], mapped[i * 3 + 2],
                            std::bit_cast<u32>(got), std::bit_cast<u32>(want));
        }
    }
    d.unmapMemory(*mem);
    for (u32 op = 0; op < Outputs; ++op)
        std::printf("  %s failures=%u\n", Names[op], per_op[op]);
    std::printf("NAN_RULES %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    const int result = Run(argc, argv);
    Common::Log::Shutdown();
    return result;
}
