// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <memory>
#include <span>
#include "common/div_ceil.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

#include "video_core/host_shaders/tiling_macro_8_comp.h"
#include "video_core/host_shaders/tiling_macro_16_comp.h"
#include "video_core/host_shaders/tiling_macro_32_comp.h"
#include "video_core/host_shaders/tiling_macro_64_comp.h"
#include "video_core/host_shaders/tiling_macro_96_comp.h"
#include "video_core/host_shaders/tiling_macro_128_comp.h"
#include "video_core/host_shaders/tiling_micro_8_comp.h"
#include "video_core/host_shaders/tiling_micro_16_comp.h"
#include "video_core/host_shaders/tiling_micro_32_comp.h"
#include "video_core/host_shaders/tiling_micro_64_comp.h"
#include "video_core/host_shaders/tiling_micro_96_comp.h"
#include "video_core/host_shaders/tiling_micro_128_comp.h"
#include "video_core/host_shaders/tiling_image_macro_8_comp.h"
#include "video_core/host_shaders/tiling_image_macro_16_comp.h"
#include "video_core/host_shaders/tiling_image_macro_32_comp.h"
#include "video_core/host_shaders/tiling_image_macro_64_comp.h"
#include "video_core/host_shaders/tiling_image_macro_128_comp.h"
#include "video_core/host_shaders/tiling_image_micro_8_comp.h"
#include "video_core/host_shaders/tiling_image_micro_16_comp.h"
#include "video_core/host_shaders/tiling_image_micro_32_comp.h"
#include "video_core/host_shaders/tiling_image_micro_64_comp.h"
#include "video_core/host_shaders/tiling_image_micro_128_comp.h"

#include <magic_enum/magic_enum.hpp>
#include <vk_mem_alloc.h>
#include "video_core/vma_diagnostics.h"

namespace VideoCore {

struct TilingInfo {
    u32 bank_swizzle;
    u32 num_slices;
    u32 num_mips;
    u32 micro_mip_mask;
    std::array<ImageInfo::MipInfo, 16> mips;
    // Fused readback only (ignored by the buffer-fed shaders).
    u32 native_width{};
    u32 native_height{};
    u32 pack_kind{};
    u32 reserved{};
};
static_assert(offsetof(TilingInfo, mips) == 16 && sizeof(ImageInfo::MipInfo) == 16);
static_assert(sizeof(TilingInfo) == 16 + 16 * 16 + 16);

// Specialization constants of tiling.comp, indexed by constant_id: 4-11 exist only in the
// macro-tiled variants and 12-13 only in the fused readback ones; entries for ids a variant
// does not declare are ignored by Vulkan.
static constexpr u32 NUM_TILING_SPEC_CONSTANTS = 14;

static std::array<u32, NUM_TILING_SPEC_CONSTANTS> TilingSpecData(const ImageInfo& info,
                                                                 bool is_tiler, u32 pack_kind,
                                                                 bool pack_bgra) {
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    std::array<u32, NUM_TILING_SPEC_CONSTANTS> data{
        info.num_samples,
        u32(micro_tile_mode),
        AmdGpu::GetMicroTileThickness(info.array_mode),
        u32(is_tiler),
    };
    if (AmdGpu::IsMacroTiled(info.array_mode)) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        data[4] = u32(info.array_mode);
        data[5] = u32(AmdGpu::GetPipeConfig(info.tile_mode));
        data[6] = AmdGpu::GetBankWidth(macro_tile_mode);
        data[7] = AmdGpu::GetBankHeight(macro_tile_mode);
        data[8] = AmdGpu::GetNumBanks(macro_tile_mode);
        data[9] = std::bit_width(data[8]) - 1;
        data[10] = AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode, micro_tile_mode,
                                              info.num_bits);
        data[11] = AmdGpu::GetMacrotileAspect(macro_tile_mode);
    }
    data[12] = pack_kind;
    data[13] = u32(pack_bgra);
    return data;
}

// Build-time SPIR-V of tiling.comp for one pixel width (host_shaders/CMakeLists.txt).
static std::span<const u32> TilingCode(u32 num_bits, bool is_macro, bool from_image) {
#define TILING_CODE(bits)                                                                      \
    case bits:                                                                                 \
        if (from_image) {                                                                      \
            return is_macro ? std::span<const u32>{TILING_IMAGE_MACRO_##bits##_COMP}           \
                            : std::span<const u32>{TILING_IMAGE_MICRO_##bits##_COMP};          \
        }                                                                                      \
        return is_macro ? std::span<const u32>{TILING_MACRO_##bits##_COMP}                     \
                        : std::span<const u32>{TILING_MICRO_##bits##_COMP};
    switch (num_bits) {
        TILING_CODE(8)
        TILING_CODE(16)
        TILING_CODE(32)
        TILING_CODE(64)
        TILING_CODE(128)
    case 96:
        ASSERT_MSG(!from_image, "No fused readback tiler for 96-bit pixels");
        return is_macro ? std::span<const u32>{TILING_MACRO_96_COMP}
                        : std::span<const u32>{TILING_MICRO_96_COMP};
    default:
        UNREACHABLE_MSG("Unsupported tiling pixel width {}", num_bits);
    }
#undef TILING_CODE
}

// Which raw packing reproduces the guest bytes of `backing` from a sampled vec4, and the
// view format to sample through (sRGB backings are read through their UNORM sibling so
// the sampler does not decode them).
static TileManager::ReadbackPack SelectReadbackPack(vk::Format backing, u32 num_bits) {
    using F = vk::Format;
    TileManager::ReadbackPack p{};
    p.view_format = backing;
    switch (backing) {
    case F::eR8G8B8A8Unorm:
    case F::eA8B8G8R8UnormPack32:
        p.kind = 1;
        break;
    case F::eR8G8B8A8Srgb:
        p.kind = 1;
        p.view_format = F::eR8G8B8A8Unorm;
        break;
    case F::eA8B8G8R8SrgbPack32:
        p.kind = 1;
        p.view_format = F::eA8B8G8R8UnormPack32;
        break;
    case F::eB8G8R8A8Unorm:
        p.kind = 1;
        p.bgra = true;
        break;
    case F::eB8G8R8A8Srgb:
        p.kind = 1;
        p.bgra = true;
        p.view_format = F::eB8G8R8A8Unorm;
        break;
    case F::eR16G16B16A16Sfloat:
        p.kind = 2;
        break;
    case F::eR32Sfloat:
    case F::eD32Sfloat:
    case F::eD32SfloatS8Uint:
        p.kind = 3;
        break;
    case F::eR16G16Sfloat:
        p.kind = 4;
        break;
    case F::eR8G8Unorm:
        p.kind = 5;
        break;
    case F::eR8Unorm:
        p.kind = 6;
        break;
    case F::eR32G32Sfloat:
        p.kind = 7;
        break;
    case F::eR32G32B32A32Sfloat:
        p.kind = 8;
        break;
    case F::eR16Sfloat:
        p.kind = 9;
        break;
    case F::eR16Unorm:
    case F::eD16Unorm:
        p.kind = 10;
        break;
    case F::eR16G16Unorm:
        p.kind = 11;
        break;
    case F::eR16G16B16A16Unorm:
        p.kind = 12;
        break;
    case F::eB10G11R11UfloatPack32:
        p.kind = 13;
        break;
    default:
        return {};
    }
    static constexpr std::array<u32, 14> kind_bits{0, 32, 64, 32, 32, 16, 8, 64,
                                                   128, 16, 16, 32, 64, 32};
    if (kind_bits[p.kind] != num_bits) {
        return {};
    }
    return p;
}

TileManager::TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         StreamBuffer& stream_buffer_)
    : instance{instance}, scheduler{scheduler}, stream_buffer{stream_buffer_} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = instance.HostDescriptorFlags(),
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto desc_layout_result = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(desc_layout_result.result == vk::Result::eSuccess,
               "Failed to create descriptor set layout: {}",
               vk::to_string(desc_layout_result.result));
    desc_layout = std::move(desc_layout_result.value);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(layout_result));
    pl_layout = std::move(layout);

    // Fused readback: tiled output buffer, sampled source image, tiling info.
    fused_readback = instance.ScalePolicy().fused_readback;
    const std::array<vk::DescriptorSetLayoutBinding, 3> image_bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};
    const vk::DescriptorSetLayoutCreateInfo image_desc_layout_ci = {
        .flags = instance.HostDescriptorFlags(),
        .bindingCount = static_cast<u32>(image_bindings.size()),
        .pBindings = image_bindings.data(),
    };
    auto image_desc_result = device.createDescriptorSetLayoutUnique(image_desc_layout_ci);
    ASSERT_MSG(image_desc_result.result == vk::Result::eSuccess,
               "Failed to create readback descriptor set layout: {}",
               vk::to_string(image_desc_result.result));
    image_desc_layout = std::move(image_desc_result.value);
    const vk::DescriptorSetLayout image_set_layout = *image_desc_layout;
    const vk::PipelineLayoutCreateInfo image_layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &image_set_layout,
    };
    auto [image_layout_result, image_layout] = device.createPipelineLayoutUnique(image_layout_info);
    ASSERT_MSG(image_layout_result == vk::Result::eSuccess,
               "Failed to create readback pipeline layout: {}", vk::to_string(image_layout_result));
    image_pl_layout = std::move(image_layout);
    const auto make_sampler = [&](vk::Filter filter) {
        const vk::SamplerCreateInfo sampler_ci = {
            .magFilter = filter,
            .minFilter = filter,
            .mipmapMode = vk::SamplerMipmapMode::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
            .maxLod = 16.f,
        };
        auto [result, sampler] = device.createSamplerUnique(sampler_ci);
        ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create readback sampler: {}",
                   vk::to_string(result));
        return std::move(sampler);
    };
    linear_sampler = make_sampler(vk::Filter::eLinear);
    nearest_sampler = make_sampler(vk::Filter::eNearest);
}

TileManager::~TileManager() = default;

TileManager::ScratchBuffer TileManager::GetScratchBuffer(u32 size) {
    constexpr auto usage =
        vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;

    const vk::BufferCreateInfo buffer_ci = {
        .size = size,
        .usage = usage,
    };

    const VmaAllocationCreateInfo alloc_info{
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkBuffer buffer;
    VmaAllocation allocation;
    const auto buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
    const auto result = VideoCore::VmaDiagnostics::CreateBuffer(instance.GetAllocator(), &buffer_ci_unsafe, &alloc_info,
                                        &buffer, &allocation, nullptr, "scratch/detile-tile");
    ASSERT(result == VK_SUCCESS);
    VmaDiagnostics::Tag(instance.GetAllocator(), allocation, nullptr, true);
    return {buffer, allocation};
}

vk::Pipeline TileManager::GetTilingPipeline(const ImageInfo& info, bool is_tiler) {
    const TilingKey key{
        .tile_mode = info.tile_mode,
        .num_bits = info.num_bits,
        .num_samples = info.num_samples,
        .is_tiler = is_tiler,
    };
    if (const auto it = tiling_pipelines.find(key); it != tiling_pipelines.end()) {
        return *it->second;
    }
    return CreateTilingPipeline(key, info);
}

vk::Pipeline TileManager::CreateTilingPipeline(const TilingKey& key, const ImageInfo& info) {
    const auto device = instance.GetDevice();
    const bool from_image = key.pack_kind != 0;
    const bool is_macro = AmdGpu::IsMacroTiled(info.array_mode);
    const auto spec_data = TilingSpecData(info, key.is_tiler, key.pack_kind, key.pack_bgra);
    static constexpr auto spec_entries = [] {
        std::array<vk::SpecializationMapEntry, NUM_TILING_SPEC_CONSTANTS> entries{};
        for (u32 i = 0; i < entries.size(); ++i) {
            entries[i] = vk::SpecializationMapEntry{i, i * u32(sizeof(u32)), sizeof(u32)};
        }
        return entries;
    }();
    const vk::SpecializationInfo specialization{
        .mapEntryCount = static_cast<u32>(spec_entries.size()),
        .pMapEntries = spec_entries.data(),
        .dataSize = sizeof(spec_data),
        .pData = spec_data.data(),
    };

    const auto module =
        Vulkan::CompileSPV(TilingCode(info.num_bits, is_macro, from_image), device);
    const auto module_name =
        from_image ? fmt::format("{}_{} readback pack{}{}", magic_enum::enum_name(info.tile_mode),
                                 info.num_bits, key.pack_kind, key.pack_bgra ? " bgra" : "")
                   : fmt::format("{}_{}x{} {}", magic_enum::enum_name(info.tile_mode),
                                 info.num_bits, info.num_samples,
                                 key.is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Creating tiling pipeline {}", module_name);
    Vulkan::SetObjectName(device, module, module_name);
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
        .pSpecializationInfo = &specialization,
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = from_image ? *image_pl_layout : *pl_layout,
    };
    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    device.destroyShaderModule(module);
    ASSERT_MSG(result == vk::Result::eSuccess, "Tiling pipeline {} creation failed {}",
               module_name, vk::to_string(result));
    const auto handle = *pipeline;
    tiling_pipelines.emplace(key, std::move(pipeline));
    return handle;
}

TileManager::Result TileManager::DetileImage(vk::Buffer in_buffer, u32 in_offset,
                                             const ImageInfo& info) {
    if (!info.props.is_tiled) {
        return {in_buffer, in_offset};
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.micro_mip_mask = info.micro_mip_mask;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = info.resources.levels;
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.CopyHost(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto [out_buffer, out_allocation] = GetScratchBuffer(info.guest_size);
    scheduler.DeferOperation([this, out_buffer, out_allocation]() {
        VideoCore::VmaDiagnostics::DestroyBuffer(instance.GetAllocator(), out_buffer, out_allocation);
    });

    scheduler.EndRendering(Vulkan::RenderBreak::Detile);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, GetTilingPipeline(info, false));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = in_buffer,
        .offset = in_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = out_buffer,
        .offset = 0,
        .range = info.guest_size,
    };

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    scheduler.BindHostDescriptors(vk::PipelineBindPoint::eCompute, *pl_layout, *desc_layout, set_writes);

    const u32 texels_per_invocation = info.num_bits == 8 ? 4 : 1;
    const auto dim_x = Common::DivCeil(info.guest_size / (info.num_bits / 8),
                                      64U * texels_per_invocation);
    Vulkan::GpuZoneScope gpu_zone{scheduler, Vulkan::GpuProfiler::Stage::Transfer};
    cmdbuf.dispatch(dim_x, 1, 1);
    return {out_buffer, 0};
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            vk::Buffer out_buffer, u64 out_offset, u32 copy_size) {
    const auto& info = in_image.info;
    if (!info.props.is_tiled) {
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        in_image.Download(buffer_copies, out_buffer, out_offset, copy_size);
        return;
    }

    if (TileImageFromScaled(in_image, static_cast<u32>(buffer_copies.size()), out_buffer,
                            out_offset)) {
        return;
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.micro_mip_mask = info.micro_mip_mask;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = static_cast<u32>(buffer_copies.size());
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.CopyHost(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto [temp_buffer, temp_allocation] = GetScratchBuffer(info.guest_size);
    scheduler.DeferOperation([this, temp_buffer, temp_allocation]() {
        VideoCore::VmaDiagnostics::DestroyBuffer(instance.GetAllocator(), temp_buffer, temp_allocation);
    });

    const auto cmdbuf = scheduler.CommandBuffer();
    in_image.Download(buffer_copies, temp_buffer, 0, copy_size);

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, GetTilingPipeline(info, true));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer,
        .offset = out_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = temp_buffer,
        .offset = 0,
        .range = info.guest_size,
    };

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    scheduler.BindHostDescriptors(vk::PipelineBindPoint::eCompute, *pl_layout, *desc_layout, set_writes);

    const auto dim_x = (info.guest_size / (info.num_bits / 8)) / 64;
    Vulkan::GpuZoneScope gpu_zone{scheduler, Vulkan::GpuProfiler::Stage::Readback};
    cmdbuf.dispatch(dim_x, 1, 1);
}

vk::Pipeline TileManager::GetImageTilingPipeline(const ImageInfo& info, const ReadbackPack& pack) {
    const TilingKey key{
        .tile_mode = info.tile_mode,
        .num_bits = info.num_bits,
        .num_samples = info.num_samples,
        .pack_kind = pack.kind,
        .is_tiler = true,
        .pack_bgra = pack.bgra,
    };
    if (const auto it = tiling_pipelines.find(key); it != tiling_pipelines.end()) {
        return *it->second;
    }
    return CreateTilingPipeline(key, info);
}

bool TileManager::TileImageFromScaled(Image& in_image, u32 num_mips, vk::Buffer out_buffer,
                                      u64 out_offset) {
    const auto& info = in_image.info;
    if (!fused_readback || !in_image.IsScaled() || in_image.DroppedMips() ||
        in_image.IsReencoded() || !info.props.is_tiled || info.props.is_block ||
        info.props.is_volume || info.num_samples != 1 || num_mips == 0 ||
        num_mips > info.resources.levels) {
        return false;
    }
    using AmdGpu::ImageType;
    if (info.type != ImageType::Color2D && info.type != ImageType::Color2DArray &&
        info.type != ImageType::Cube) {
        return false;
    }
    const auto& backing_ci = in_image.backing->image.image_ci;
    if (backing_ci.mipLevels < num_mips || backing_ci.arrayLayers < info.resources.layers) {
        return false;
    }
    const auto pack = SelectReadbackPack(backing_ci.format, info.num_bits);
    if (pack.kind == 0) {
        return false;
    }
    const bool depth = info.props.is_depth;
    const vk::ImageLayout layout = depth ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                         : vk::ImageLayout::eShaderReadOnlyOptimal;
    // Sampling the scaled backing needs a shader-read layout on the whole image; this
    // ends any open pass exactly like the blit of the old chain did.
    in_image.Transit(layout, vk::AccessFlagBits2::eShaderRead, {}, {},
                     Vulkan::RenderBreak::Download);

    const vk::ImageViewUsageCreateInfo usage_ci{.usage = vk::ImageUsageFlagBits::eSampled};
    const vk::ImageViewCreateInfo view_ci = {
        .pNext = &usage_ci,
        .image = in_image.GetImage(),
        .viewType = vk::ImageViewType::e2DArray,
        .format = pack.view_format,
        .subresourceRange{
            .aspectMask = depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = num_mips,
            .baseArrayLayer = 0,
            .layerCount = info.resources.layers,
        },
    };
    auto [view_result, view] = instance.GetDevice().createImageViewUnique(view_ci);
    if (view_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Fused readback view failed ({}), using blit chain",
                    vk::to_string(view_result));
        return false;
    }
    // The view is only referenced by this command buffer; retire it with the tick.
    auto view_holder = std::make_shared<vk::UniqueImageView>(std::move(view));
    scheduler.DeferOperation([view_holder] {});

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.micro_mip_mask = info.micro_mip_mask;
    params.num_slices = info.resources.layers;
    params.num_mips = num_mips;
    for (u32 mip = 0; mip < num_mips; ++mip) {
        params.mips[mip] = info.mips_layout[mip];
    }
    params.native_width = info.size.width;
    params.native_height = info.size.height;
    params.pack_kind = pack.kind;
    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.CopyHost(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };
    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer,
        .offset = out_offset,
        .range = info.guest_size,
    };
    const vk::DescriptorImageInfo source_info{
        .sampler = depth ? *nearest_sampler : *linear_sampler,
        .imageView = **view_holder,
        .imageLayout = layout,
    };
    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &source_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};

    if (scheduler.TakePassBreakLog())
        LOG_INFO(Render_Vulkan,
                 "Internal scale: fused readback {}x{} {} L:{} M:{} {:#x} bytes={} pack={}{}",
                 info.size.width, info.size.height, vk::to_string(info.pixel_format),
                 info.resources.layers, num_mips, info.guest_address, info.guest_size, pack.kind,
                 pack.bgra ? " bgra" : "");

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, GetImageTilingPipeline(info, pack));
    scheduler.BindHostDescriptors(vk::PipelineBindPoint::eCompute, *image_pl_layout,
                                  *image_desc_layout, set_writes);
    const auto dim_x = (info.guest_size / (info.num_bits / 8)) / 64;
    Vulkan::GpuZoneScope gpu_zone{scheduler, Vulkan::GpuProfiler::Stage::Readback};
    cmdbuf.dispatch(dim_x, 1, 1);
    in_image.RecordFusedReadback();
    return true;
}

} // namespace VideoCore
