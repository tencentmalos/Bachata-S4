// SPDX-License-Identifier: GPL-2.0-or-later
// Execute the production SPIR-V emitter on a real Vulkan device. Inputs are
// storage loads, not constants folded by a host-side model of the lowering.
#include <bit>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "frontend/window.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "video_core/renderer_vulkan/vk_instance.h"

using namespace Shader;
using namespace Shader::Backend::SPIRV;
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
constexpr u32 Rows = 256, Stride = 96, DataOffset = 512;
struct Expected {
    u32 offset;
    u32 value;
    const char* name;
};
std::vector<Expected> expected;
std::vector<u32> Build(bool native, std::vector<u32>& data, bool fp64 = false) {
    Profile profile{};
    profile.supported_spirv = 0x10600;
    profile.support_int64 = native;
    RuntimeInfo runtime{};
    runtime.stage = Stage::Compute;
    runtime.cs_info.workgroup_size = {1, 1, 1};
    Info info{};
    info.stage = Stage::Compute;
    info.l_stage = LogicalStage::Compute;
    info.uses_dma = true;
    info.uses_fp64 = fp64;
    info.shared_types = IR::Type::U64;
    runtime.cs_info.shared_memory_size = 8;
    info.buffers.push_back({.used_types = IR::Type::U64, .buffer_type = BufferType::BdaPagetable});
    info.buffers.push_back(
        {.used_types = IR::Type::U32, .buffer_type = BufferType::FaultBuffer, .is_written = true});
    info.buffers.push_back({.used_types = IR::Type::U32 | IR::Type::U64, .is_written = true});
    Shader::Backend::Bindings bindings{};
    EmitContext c(profile, runtime, info, bindings);
    c.AddCapability(spv::Capability::Int8);
    c.AddCapability(spv::Capability::Int16);
    if (fp64)
        c.AddCapability(spv::Capability::Float64);
    if (native)
        c.AddCapability(spv::Capability::Int64);
    c.AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
    c.AddExtension("SPV_KHR_physical_storage_buffer");
    auto gid =
        c.DefineVariable(c.U32[3], spv::BuiltIn::GlobalInvocationId, spv::StorageClass::Input);
    auto [buf, ptr] = c.buffers[2].Alias(EmitContext::PointerType::U32);
    auto main =
        c.OpFunction(c.void_id, spv::FunctionControlMask::MaskNone, c.TypeFunction(c.void_id));
    c.AddLabel();
    auto row = c.OpCompositeExtract(c.U32[1], c.OpLoad(c.U32[3], gid), 0U);
    auto base = c.OpIMul(c.U32[1], row, c.ConstU32(Stride));
    auto index = [&](u32 i) { return c.OpIAdd(c.U32[1], base, c.ConstU32(i)); };
    auto load = [&](u32 i) {
        return c.OpLoad(c.U32[1], c.OpAccessChain(ptr, buf, c.u32_zero_value, index(i)));
    };
    auto a = c.PackU64(c.OpCompositeConstruct(c.U32[2], load(0), load(1)));
    auto b = c.PackU64(c.OpCompositeConstruct(c.U32[2], load(2), load(3)));
    auto shift = load(4);
    u32 output = 8;
    std::vector<std::pair<const char*, std::function<u64(u64, u64, u32)>>> ops;
    auto store32 = [&](Id x) {
        c.OpStore(c.OpAccessChain(ptr, buf, c.u32_zero_value, index(output++)), x);
    };
    auto check64 = [&](Id x, const char* name, auto fn) {
        auto pair = c.UnpackU64(x);
        store32(c.OpCompositeExtract(c.U32[1], pair, 0U));
        store32(c.OpCompositeExtract(c.U32[1], pair, 1U));
        ops.emplace_back(name, fn);
    };
    check64(EmitIAdd64(c, a, b), "add", [](u64 x, u64 y, u32) { return x + y; });
    check64(EmitISub64(c, a, b), "sub", [](u64 x, u64 y, u32) { return x - y; });
    check64(EmitIMul64(c, a, b), "mul", [](u64 x, u64 y, u32) { return x * y; });
    check64(EmitINeg64(c, a), "neg", [](u64 x, u64, u32) { return 0 - x; });
    check64(EmitShiftLeftLogical64(c, a, shift), "shl", [](u64 x, u64, u32 n) { return x << n; });
    check64(EmitShiftRightLogical64(c, a, shift), "shr", [](u64 x, u64, u32 n) { return x >> n; });
    check64(EmitShiftRightArithmetic64(c, a, shift), "sar",
            [](u64 x, u64, u32 n) { return u64(s64(x) >> n); });
    check64(EmitBitwiseAnd64(c, nullptr, a, b), "and", [](u64 x, u64 y, u32) { return x & y; });
    check64(EmitBitwiseOr64(c, nullptr, a, b), "or", [](u64 x, u64 y, u32) { return x | y; });
    check64(EmitConvertU64U32(c, c.NarrowU64(a)), "widen",
            [](u64 x, u64, u32) { return u64(u32(x)); });
    // Exercise actual storage aliases and workgroup loads/stores, not just pairs.
    EmitWriteSharedU64(c, c.u32_zero_value, a);
    check64(EmitLoadSharedU64(c, c.u32_zero_value), "shared", [](u64 x, u64, u32) { return x; });
    auto addr64 = c.OpShiftRightLogical(c.U32[1], index(6), c.ConstU32(1U));
    EmitStoreBufferU64(c, nullptr, 2, addr64, b);
    check64(EmitLoadBufferU64(c, nullptr, 2, addr64), "buffer", [](u64, u64 y, u32) { return y; });
    const u32 scalar_start = output;
    std::vector<std::pair<const char*, std::function<u32(u64, u64, u32)>>> scalar_ops;
    auto check32 = [&](Id x, const char* name, auto fn) {
        store32(x);
        scalar_ops.emplace_back(name, fn);
    };
    auto check_bool = [&](Id x, const char* name, auto fn) {
        check32(c.OpSelect(c.U32[1], x, c.u32_one_value, c.u32_zero_value), name, fn);
    };
    check_bool(EmitIEqual64(c, a, b), "eq", [](u64 x, u64 y, u32) { return x == y; });
    check_bool(EmitINotEqual64(c, a, b), "ne", [](u64 x, u64 y, u32) { return x != y; });
    check_bool(EmitSLessThan64(c, a, b), "slt", [](u64 x, u64 y, u32) { return s64(x) < s64(y); });
    check_bool(EmitULessThan64(c, a, b), "ult", [](u64 x, u64 y, u32) { return x < y; });
    check_bool(EmitSLessThanEqual64(c, a, b), "sle",
               [](u64 x, u64 y, u32) { return s64(x) <= s64(y); });
    check_bool(EmitULessThanEqual64(c, a, b), "ule", [](u64 x, u64 y, u32) { return x <= y; });
    check_bool(EmitSGreaterThan64(c, a, b), "sgt",
               [](u64 x, u64 y, u32) { return s64(x) > s64(y); });
    check_bool(EmitUGreaterThan64(c, a, b), "ugt", [](u64 x, u64 y, u32) { return x > y; });
    check_bool(EmitSGreaterThanEqual64(c, a, b), "sge",
               [](u64 x, u64 y, u32) { return s64(x) >= s64(y); });
    check_bool(EmitUGreaterThanEqual64(c, a, b), "uge", [](u64 x, u64 y, u32) { return x >= y; });
    check32(EmitBitCount64(c, a), "popcount", [](u64 x, u64, u32) { return std::popcount(x); });
    check32(EmitFindILsb64(c, a), "lsb",
            [](u64 x, u64, u32) { return x ? std::countr_zero(x) : -1; });
    check32(EmitFindUMsb64(c, a), "msb",
            [](u64 x, u64, u32) { return x ? 63 - std::countl_zero(x) : -1; });
    check32(c.OpBitcast(c.U32[1], EmitConvertF32U64(c, a)), "u64-f32",
            [](u64 x, u64, u32) { return std::bit_cast<u32>(float(x)); });
    check32(c.OpBitcast(c.U32[1], EmitConvertF32S64(c, a)), "s64-f32",
            [](u64 x, u64, u32) { return std::bit_cast<u32>(float(s64(x))); });
    // Compare directed modes against host nextafter with an exact wide reference,
    // not a second copy of the shader's integer rounding algorithm.
    if (!native) {
        for (auto mode : {AmdGpu::FpRoundMode::ToZero, AmdGpu::FpRoundMode::PlusInf,
                          AmdGpu::FpRoundMode::MinInf}) {
            runtime.fp_round_mode32 = mode;
            for (bool sign : {false, true}) {
                check32(c.OpBitcast(c.U32[1], c.U64ToFloat(a, sign, false)), "directed-f32",
                        [mode, sign](u64 x, u64, u32) {
                            const long double exact = sign ? (long double)s64(x) : (long double)x;
                            float result = sign ? float(s64(x)) : float(x);
                            const bool down = mode == AmdGpu::FpRoundMode::MinInf ||
                                              (mode == AmdGpu::FpRoundMode::ToZero && exact >= 0);
                            if (down && (long double)result > exact)
                                result = std::nextafter(result, -INFINITY);
                            if (!down && (long double)result < exact)
                                result = std::nextafter(result, INFINITY);
                            return std::bit_cast<u32>(result);
                        });
            }
        }
        runtime.fp_round_mode32 = AmdGpu::FpRoundMode::NearestEven;
    }
    if (fp64) {
        // Validation-only on Adreno (shaderFloat64=0); do not dispatch this module.
        check32(c.NarrowU64(EmitConvertU64F64(c, EmitConvertF64U64(c, a))), "fp64-validation",
                [](u64 x, u64, u32) { return u32(x); });
        check32(c.NarrowU64(EmitConvertS64F64(c, EmitConvertF64S64(c, a))), "fp64-validation",
                [](u64 x, u64, u32) { return u32(x); });
    }
    // Page 0 is backed by the real 64-bit BDA, page 1 deliberately faults.
    check32(c.EmitDwordMemoryRead(c.ConstU64(16), [&] { return c.ConstU32(0xdeadu); }),
            "bda-backed", [](u64, u64, u32) { return 0x1234abcdu; });
    check32(c.EmitDwordMemoryRead(
                c.WidenU32(c.OpShiftLeftLogical(
                    c.U32[1],
                    c.OpIAdd(c.U32[1], c.OpUMod(c.U32[1], row, c.ConstU32(31U)), c.u32_one_value),
                    c.ConstU32(14U))),
                [&] { return c.ConstU32(0xbeefu); }),
            "bda-fault", [](u64, u64, u32) { return 0xbeefu; });
    const u32 float_start = output;
    auto f = c.OpBitcast(c.F32[1], load(5));
    auto fu = c.UnpackU64(EmitConvertU64F32(c, c.OpFAbs(c.F32[1], f)));
    auto fs = c.UnpackU64(EmitConvertS64F32(c, f));
    store32(c.OpCompositeExtract(c.U32[1], fu, 0U));
    store32(c.OpCompositeExtract(c.U32[1], fu, 1U));
    store32(c.OpCompositeExtract(c.U32[1], fs, 0U));
    store32(c.OpCompositeExtract(c.U32[1], fs, 1U));
    c.OpReturn();
    c.OpFunctionEnd();
    c.AddEntryPoint(spv::ExecutionModel::GLCompute, main, "main", c.interfaces);
    c.AddExecutionMode(main, spv::ExecutionMode::LocalSize, 1U, 1U, 1U);
    std::mt19937_64 rng(0x64ad740);
    constexpr u64 edges[] = {0,
                             1,
                             0xffffffffULL,
                             0x100000000ULL,
                             0x8000000000000000ULL,
                             UINT64_MAX,
                             0x100000001ULL,
                             0x7fffffffffffffffULL,
                             0x1ffffff00000000ULL,
                             0x1ffffff00000001ULL,
                             0x1fffffd00000000ULL};
    expected.clear();
    for (u32 r = 0; r < Rows; r++) {
        u64 x = r < std::size(edges) * 8 ? edges[r / 8] : rng(), y = r % 4 == 0 ? x : rng();
        u32 n = std::array<u32, 8>{0, 1, 7, 31, 32, 33, 62, 63}[r % 8];
        float fv = r % 3 == 0 ? -float(r) * 4294967296.f : float(r) * 16777216.f + 0.5f;
        u32 pos = r * Stride;
        data[pos] = u32(x);
        data[pos + 1] = u32(x >> 32);
        data[pos + 2] = u32(y);
        data[pos + 3] = u32(y >> 32);
        data[pos + 4] = n;
        data[pos + 5] = std::bit_cast<u32>(fv);
        u32 off = pos + 8;
        for (auto& [name, fn] : ops) {
            u64 v = fn(x, y, n);
            expected.push_back({off++, u32(v), name});
            expected.push_back({off++, u32(v >> 32), name});
        }
        if (off != pos + scalar_start)
            std::abort();
        for (auto& [name, fn] : scalar_ops)
            expected.push_back({off++, fn(x, y, n), name});
        if (off != pos + float_start)
            std::abort();
        for (u64 v : {u64(std::abs(fv)), u64(s64(fv))}) {
            expected.push_back({off++, u32(v), "f32-int64"});
            expected.push_back({off++, u32(v >> 32), "f32-int64"});
        }
    }
    return c.Assemble();
}
std::vector<u32> BuildThroughIR() {
    Profile p{};
    p.supported_spirv = 0x10600;
    RuntimeInfo r{};
    r.stage = Stage::Compute;
    r.cs_info.workgroup_size = {1, 1, 1};
    Info i{};
    i.stage = Stage::Compute;
    i.l_stage = LogicalStage::Compute;
    i.buffers.push_back({.used_types = IR::Type::U64, .is_written = true});
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    auto a = ir.LoadBufferU64(ir.Imm32(0U), ir.Imm32(0U), {});
    auto sum = IR::U64{ir.IAdd(a, ir.Imm64(u64{0x1ffffffffULL}))};
    ir.StoreBufferU64(ir.Imm32(0U), ir.Imm32(1U), sum, {});
    IR::Program program(i);
    program.blocks.push_back(&block);
    program.syntax_list.push_back(
        {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Shader::Backend::Bindings bindings{};
    return EmitSPIRV(p, r, program, bindings);
}
int main(int argc, char** argv) {
    if (argc < 3)
        return 2;
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("int64-probe");
    Window w;
    auto driver = std::string_view(argv[1]) == "system"
                      ? Vulkan::LoadAndroidSystemDriver()
                      : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(w, 0, false, false, driver);
    if (argc > 3 && std::string_view(argv[3]) == "wait-reshape") {
        for (unsigned i = 0; i < 600 &&
             instance.GpuReshapeAdapter().StatusText().find("transport=connected") == std::string::npos; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto d = instance.GetDevice();
    auto flags =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    auto buffer = Vulkan::Check(
        d.createBufferUnique({.size = Rows * Stride * 4 + DataOffset,
                              .usage = vk::BufferUsageFlagBits::eStorageBuffer |
                                       vk::BufferUsageFlagBits::eShaderDeviceAddress}));
    auto req = d.getBufferMemoryRequirements(*buffer);
    u32 mt = 0;
    auto mp = instance.GetMemoryProperties();
    while (mt < mp.memoryTypeCount && (!(req.memoryTypeBits & (1u << mt)) ||
                                       (mp.memoryTypes[mt].propertyFlags & flags) != flags))
        ++mt;
    if (mt == mp.memoryTypeCount)
        return 3;
    vk::MemoryAllocateFlagsInfo af{.flags = vk::MemoryAllocateFlagBits::eDeviceAddress};
    auto mem = Vulkan::Check(
        d.allocateMemoryUnique({.pNext = &af, .allocationSize = req.size, .memoryTypeIndex = mt}));
    Vulkan::Check(d.bindBufferMemory(*buffer, *mem, 0));
    auto* mapped = static_cast<u32*>(Vulkan::Check(d.mapMemory(*mem, 0, req.size)));
    auto bda = d.getBufferAddress({.buffer = *buffer});
    std::printf("PROBE device=%s nativeInt64=%d BDA=%llx\n",
                instance.GetPhysicalDevice().getProperties().deviceName.data(),
                instance.IsShaderInt64Supported(), (unsigned long long)bda);
    if (bda <= UINT32_MAX)
        return 4; // The test must exercise high address bits.
    std::vector<vk::DescriptorSetLayoutBinding> db;
    for (u32 i = 0; i < 3; i++)
        db.push_back({.binding = i,
                      .descriptorType = vk::DescriptorType::eStorageBuffer,
                      .descriptorCount = 1,
                      .stageFlags = vk::ShaderStageFlagBits::eCompute});
    auto layout = Vulkan::Check(
        d.createDescriptorSetLayoutUnique({.bindingCount = 3, .pBindings = db.data()}));
    auto pl =
        Vulkan::Check(d.createPipelineLayoutUnique({.setLayoutCount = 1, .pSetLayouts = &*layout}));
    vk::DescriptorPoolSize ps{vk::DescriptorType::eStorageBuffer, 3};
    auto pool = Vulkan::Check(
        d.createDescriptorPoolUnique({.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps}));
    auto sets = Vulkan::Check(d.allocateDescriptorSets(
        {.descriptorPool = *pool, .descriptorSetCount = 1, .pSetLayouts = &*layout}));
    std::array<vk::DescriptorBufferInfo, 3> bi{
        {{*buffer, 0, 256}, {*buffer, 256, 4}, {*buffer, DataOffset, Rows * Stride * 4}}};
    for (u32 i = 0; i < 3; i++) {
        vk::WriteDescriptorSet wr{.dstSet = sets[0],
                                  .dstBinding = i,
                                  .descriptorCount = 1,
                                  .descriptorType = vk::DescriptorType::eStorageBuffer,
                                  .pBufferInfo = &bi[i]};
        d.updateDescriptorSets(wr, {});
    }
    auto cp = Vulkan::Check(
        d.createCommandPoolUnique({.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()}));
    auto commands = Vulkan::Check(d.allocateCommandBuffers(
        {.commandPool = *cp, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}));
    auto fence = Vulkan::Check(d.createFenceUnique({}));
    unsigned failures = 0, checks = 0;
    for (bool native : {false, true}) {
        if (native && !instance.IsShaderInt64Supported())
            continue;
        std::vector<u32> data(Rows * Stride, 0xcdcdcdcd);
        auto code = Build(native, data);
        std::ofstream out(std::string(argv[2]) + (native ? "/native.spv" : "/lowered.spv"),
                          std::ios::binary);
        out.write(reinterpret_cast<char*>(code.data()), code.size() * 4);
        out.close();
        bool invalid = false;
        for (size_t i = 5; i < code.size();) {
            u32 wc = code[i] >> 16, op = code[i] & 0xffff;
            if (!wc)
                return 5;
            if (!native && ((op == u32(spv::Op::OpTypeInt) && code[i + 2] == 64) ||
                            (op == u32(spv::Op::OpCapability) &&
                             (code[i + 1] == u32(spv::Capability::Int64) ||
                              code[i + 1] == u32(spv::Capability::Int64Atomics)))))
                invalid = true;
            i += wc;
        }
        if (invalid) {
            puts("FAIL Int64 leaked into lowered module");
            return 6;
        }
        std::memset(mapped, 0, DataOffset);
        std::memcpy(mapped + DataOffset / 4, data.data(), data.size() * 4);
        const u64 backed_page = bda + 272;
        std::memcpy(mapped, &backed_page, 8);
        mapped[288 / 4] = 0x1234abcd;
        auto sm = Vulkan::Check(
            d.createShaderModuleUnique({.codeSize = code.size() * 4, .pCode = code.data()}));
        auto pipe = Vulkan::Check(d.createComputePipelineUnique(
            {},
            {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *sm, .pName = "main"},
             .layout = *pl}));
        auto cmd = commands[0];
        Vulkan::Check(cmd.begin(vk::CommandBufferBeginInfo{}));
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pl, 0, sets, {});
        cmd.dispatch(Rows, 1, 1);
        Vulkan::Check(cmd.end());
        vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
        Vulkan::Check(instance.GetGraphicsQueue().submit(submit, *fence));
        if (d.waitForFences(*fence, true, 5000000000ULL) != vk::Result::eSuccess)
            return 7;
        for (auto e : expected) {
            ++checks;
            if (mapped[DataOffset / 4 + e.offset] != e.value) {
                if (failures++ < 20)
                    std::printf("FAIL native=%d row=%u op=%s got=%08x want=%08x\n", native,
                                e.offset / Stride, e.name, mapped[DataOffset / 4 + e.offset],
                                e.value);
            }
        }
        ++checks;
        if (mapped[256 / 4] != 0xfffffffeU) {
            ++failures;
            std::printf("FAIL fault bit %x\n", mapped[256 / 4]);
        }
        Vulkan::Check(d.resetFences(*fence));
        Vulkan::Check(d.resetCommandPool(*cp));
    }
    // Early, explicit refusal for atomic64; never emit vector atomics or two stores.
    for (bool shared : {false, true}) {
        Profile p{};
        p.supported_spirv = 0x10600;
        Info i{};
        i.stage = Stage::Compute;
        i.l_stage = LogicalStage::Compute;
        i.uses_buffer_int64_atomics = !shared;
        i.uses_shared_int64_atomics = shared;
        RuntimeInfo r{};
        Shader::Backend::Bindings b{};
        ++checks;
        try {
            EmitContext c(p, r, i, b);
            ++failures;
        } catch (const std::runtime_error&) {
        }
    }
    for (const auto* name : {"fp64-validation.spv", "full-emitter.spv"}) {
        std::vector<u32> data(Rows * Stride);
        auto code = std::string_view(name) == "full-emitter.spv" ? BuildThroughIR()
                                                                 : Build(false, data, true);
        std::ofstream out(std::string(argv[2]) + "/" + name, std::ios::binary);
        out.write(reinterpret_cast<char*>(code.data()), code.size() * 4);
    }
    d.unmapMemory(*mem);
    if (argc > 3 && std::string_view(argv[3]) == "wait-reshape")
        std::this_thread::sleep_for(std::chrono::seconds(2)); // Drain diagnostic transport only.
    instance.GpuReshapeAdapter().PublishStatus(true);
    std::printf("%s\n", instance.GpuReshapeAdapter().StatusText().c_str());
    std::printf("INT64_DEVICE %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
