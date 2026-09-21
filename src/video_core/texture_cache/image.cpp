// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ranges>
#include "common/assert.h"
#include "common/div_ceil.h"
#include "core/emulator_settings.h"
#include "video_core/texture_cache/internal_scale.h"
#include "video_core/memory_diagnostics.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>
#include "video_core/vma_diagnostics.h"

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance* instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance->IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used.
            usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

// Storage is provisioned speculatively for compute clears, but its sample-count
// limit must not silently turn an MSAA attachment into a single-sample image.
// Query the concrete format/usage tuple again without that optional use. Views
// must use the selected backing's usage, and unsupported MSAA storage stays explicit.
static vk::SampleCountFlags ConfigureImageSamples(const Vulkan::Instance& instance,
                                                   vk::ImageCreateInfo& ci, u32 requested) {
    const auto query = [&](vk::ImageUsageFlags usage) {
        const auto result = instance.GetPhysicalDevice().getImageFormatProperties2({
            .format = ci.format, .type = ci.imageType, .tiling = ci.tiling,
            .usage = usage, .flags = ci.flags});
        return result.result == vk::Result::eSuccess
            ? result.value.imageFormatProperties.sampleCounts : vk::SampleCountFlags{};
    };
    auto supported = query(ci.usage);
    auto selected = LiverpoolToVK::NumSamples(requested, supported);
    if (requested > 1 && (ci.usage & vk::ImageUsageFlagBits::eStorage)) {
        const auto attachment_usage = ci.usage & ~vk::ImageUsageFlagBits::eStorage;
        const auto attachment_supported = query(attachment_usage);
        const auto attachment_samples = LiverpoolToVK::NumSamples(requested, attachment_supported);
        if (u32(attachment_samples) > u32(selected)) {
            ci.usage = attachment_usage;
            supported = attachment_supported;
            selected = attachment_samples;
        }
    }
    ASSERT_MSG(bool(supported), "Image format {} is unsupported for usage {}",
               vk::to_string(ci.format), vk::to_string(ci.usage));
    ASSERT_MSG(u32(selected) == requested,
               "Unsupported image sample count: format={} usage={} requested={} supported={:#x}",
               vk::to_string(ci.format), vk::to_string(ci.usage), requested, u32(supported));
    ci.samples = selected;
    return supported;
}

static bool AstcLdrSource(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbUnormBlock: case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc1RgbaUnormBlock: case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc2UnormBlock: case vk::Format::eBc2SrgbBlock:
    case vk::Format::eBc3UnormBlock: case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc4UnormBlock: case vk::Format::eBc5UnormBlock:
    case vk::Format::eBc7UnormBlock: case vk::Format::eBc7SrgbBlock: return true;
    default: return false; // Signed channels and HDR must not be silently clamped to LDR.
    }
}
static bool IsSrgbBlock(vk::Format format) {
    return format == vk::Format::eBc1RgbSrgbBlock || format == vk::Format::eBc1RgbaSrgbBlock ||
           format == vk::Format::eBc2SrgbBlock || format == vk::Format::eBc3SrgbBlock ||
           format == vk::Format::eBc7SrgbBlock || format == vk::Format::eAstc4x4SrgbBlock || format == vk::Format::eAstc6x6SrgbBlock;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

UniqueImage::~UniqueImage() {
    if (image) {
        VideoCore::VmaDiagnostics::DestroyImage(allocator, image, allocation);
    }
}

void UniqueImage::Destroy() {
    if (image) {
        VideoCore::VmaDiagnostics::DestroyImage(allocator, image, allocation);
        image = vk::Image{};
        allocation = {};
    }
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VkResult result = VideoCore::VmaDiagnostics::CreateImage(allocator, &image_ci_unsafe, &alloc_info, &unsafe_image,
                                     &allocation, nullptr);
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating image with error {}",
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
}

Image::Image(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
             BlitHelper& blit_helper_, Common::SlotVector<ImageView>& slot_image_views_,
             const ImageInfo& info_, TextureCache* owner_, ScaleUse use,
             std::optional<ScalePolicySnapshot> probe_policy)
    : instance{&instance_}, scheduler{&scheduler_}, blit_helper{&blit_helper_},
      slot_image_views{&slot_image_views_}, info{info_}, owner{owner_},
      policy{probe_policy.value_or(instance_.ScalePolicy())},
      scale_plan{owner_ && info_.pixel_format != vk::Format::eUndefined ? owner_->AcquireScalePlan(info_, use) : std::make_shared<ResourceScalePlan>()} {
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }
    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    // Here we force `eExtendedUsage` as don't know all image usage cases beforehand. In normal case
    // the texture cache should re-create the resource with the usage requested
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance->Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance->IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    usage_flags = ImageUsageFlags(instance, info);
    format_features = FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance->GetSupportedFormat(info.pixel_format, format_features);
    vk::ImageCreateInfo image_ci = {
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = supported_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    supported_samples = ConfigureImageSamples(*instance, image_ci, instance->HostSamples(info.num_samples));

    const bool render = use == ScaleUse::RenderTarget || use == ScaleUse::DepthTarget || use == ScaleUse::VideoOut;
    scale_plan->history |= 1u << u32(use);
    if (scale_plan->domain != ScaleDomain::NativeRequired) {
        if (render) {
            scale_plan->domain = ScaleDomain::Render;
            scale_plan->render_transition = true;
            scale_plan->reason = ScaleReason::None;
        } else if (use == ScaleUse::Texture && scale_plan->domain == ScaleDomain::Unknown) {
            scale_plan->domain = ScaleDomain::Asset;
            scale_plan->reason = ScaleReason::None;
        } else if (use == ScaleUse::Storage) scale_plan->RequireNative(ScaleReason::SemanticNative);
    }
    u32 selected = 8;
    bool direct_drop = false;
    if (scale_plan->domain != ScaleDomain::NativeRequired) {
        if (policy.legacy) {
            selected = policy.render_eighths;
            scale_plan->reason = ScaleReason::Legacy;
        } else if (scale_plan->domain == ScaleDomain::Render) {
            if (std::min(info.size.width, info.size.height) > 64) selected = policy.render_eighths;
            else scale_plan->reason = ScaleReason::SizeProtect;
        } else if (scale_plan->domain == ScaleDomain::Asset) {
            if (info.props.is_depth) scale_plan->RequireNative(ScaleReason::SemanticNative);
            else if (std::min(info.size.width, info.size.height) < 128) scale_plan->reason = ScaleReason::SizeProtect;
            else if (info.resources.levels < 2) scale_plan->reason = ScaleReason::InsufficientMips;
            else if (policy.texture == TextureQuality::Medium) selected = 6;
            else if (policy.texture == TextureQuality::Low) { selected = 4; direct_drop = true; }
        }
    }
    const InternalScale scale{selected};
    // BC1/BC4 have eight-byte blocks: 4x4 ASTC at 0.75 would grow their storage.
    const auto astc_format = info.num_bits == 64
        ? (IsSrgbBlock(supported_format) ? vk::Format::eAstc6x6SrgbBlock : vk::Format::eAstc6x6UnormBlock)
        : (IsSrgbBlock(supported_format) ? vk::Format::eAstc4x4SrgbBlock : vk::Format::eAstc4x4UnormBlock);
    // Integer, volume, tiny LUT and multisampled resources keep their exact data layout.
    // Format capability checks also exclude integer formats from filtered resampling.
    const bool eligible = scale.eighths < 8 && image_ci.imageType == vk::ImageType::e2D &&
        info.num_samples == 1 && info.size.width >= 16 && info.size.height >= 16;
    const auto blit_features = vk::FormatFeatureFlagBits2::eBlitSrc |
                               vk::FormatFeatureFlagBits2::eBlitDst;
    if (eligible && direct_drop &&
        instance->IsFormatSupported(supported_format, vk::FormatFeatureFlagBits2::eSampledImageFilterLinear)) {
        scale_eighths = 4;
        mip_skip = 1;
        image_ci.extent.width = std::max(info.size.width >> 1, 1u);
        image_ci.extent.height = std::max(info.size.height >> 1, 1u);
        image_ci.mipLevels -= 1;
    } else if (eligible && !direct_drop && !info.props.is_block && instance->IsFormatSupported(supported_format, blit_features) &&
        (info.props.is_depth || instance->IsFormatSupported(supported_format, vk::FormatFeatureFlagBits2::eSampledImageFilterLinear))) {
        scale_eighths = scale.eighths;
        image_ci.extent.width = scale.Size(info.size.width);
        image_ci.extent.height = scale.Size(info.size.height);
        image_ci.mipLevels = scale.Levels(info.size.width, info.size.height, info.resources.levels);
    } else if (eligible && policy.legacy && info.props.is_block && scale.MipDrop() &&
               info.resources.levels > scale.MipDrop()) {
        scale_eighths = scale.eighths;
        mip_skip = scale.MipDrop();
        image_ci.extent.width = std::max(info.size.width >> mip_skip, 1u);
        image_ci.extent.height = std::max(info.size.height >> mip_skip, 1u);
        image_ci.mipLevels -= mip_skip;
    } else if (eligible && !direct_drop && !render && AstcLdrSource(supported_format) &&
        instance->IsFormatSupported(supported_format, vk::FormatFeatureFlagBits2::eSampledImageFilterLinear) &&
        instance->IsFormatSupported(astc_format,
            vk::FormatFeatureFlagBits2::eSampledImage | vk::FormatFeatureFlagBits2::eTransferDst)) {
        scale_eighths = scale.eighths;
        astc_encoded = true;
        image_ci.format = astc_format;
        image_ci.extent.width = scale.Size(info.size.width);
        image_ci.extent.height = scale.Size(info.size.height);
        image_ci.mipLevels = scale.Levels(info.size.width, info.size.height, info.resources.levels);
    }

    backing = &backing_images.emplace_back();
    backing->num_samples = u32(image_ci.samples);
    backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
    backing->image.Create(image_ci);
    if (selected < 8 && !IsScaled()) scale_plan->RequireNative(ScaleReason::SemanticNative);
    PublishScalePlan();
    if (IsScaled()) {
        static std::atomic<u32> reports{};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 64)
            LOG_INFO(Render_Vulkan, "Internal scale allocation: {}x{} {} -> {}x{} {} levels {} -> {} mode {}",
                info.size.width, info.size.height, vk::to_string(info.pixel_format),
                image_ci.extent.width, image_ci.extent.height, vk::to_string(image_ci.format),
                info.resources.levels, image_ci.mipLevels, mip_skip ? "mip-drop" : astc_encoded ? "ASTC" : "resample");
    }

    Vulkan::SetObjectName(instance->GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;


void Image::BlitBacking(BackingImage& source, BackingImage& dest,
                        std::span<const vk::BufferImageCopy> uploaded) {
    auto* saved = backing;
    backing = &source;
    Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    backing = &dest;
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    const auto& src = source.image.image_ci;
    const auto& dst = dest.image.image_ci;
    std::vector<vk::ImageBlit> regions;
    for (u32 mip = 0; mip < dst.mipLevels; ++mip) {
        const u32 src_mip = std::min(mip, src.mipLevels - 1);
        if (!uploaded.empty() && std::ranges::none_of(uploaded, [=](const auto& copy) {
                return copy.imageSubresource.mipLevel == src_mip;
            })) continue;
        for (const auto aspect : {vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth,
                                  vk::ImageAspectFlagBits::eStencil}) {
            if (!(aspect_mask & aspect)) continue;
            // CPU depth uploads do not contain a stencil plane.
            if (!uploaded.empty() && aspect == vk::ImageAspectFlagBits::eStencil) continue;
            regions.push_back(vk::ImageBlit{
                .srcSubresource = {aspect, src_mip, 0, src.arrayLayers},
                .srcOffsets = std::array{vk::Offset3D{}, vk::Offset3D{
                    s32(std::max(src.extent.width >> src_mip, 1u)),
                    s32(std::max(src.extent.height >> src_mip, 1u)), 1}},
                .dstSubresource = {aspect, mip, 0, dst.arrayLayers},
                .dstOffsets = std::array{vk::Offset3D{}, vk::Offset3D{
                    s32(std::max(dst.extent.width >> mip, 1u)),
                    s32(std::max(dst.extent.height >> mip, 1u)), 1}},
            });
        }
    }
    if (!regions.empty()) {
        scheduler->CommandBuffer().blitImage(source.image, vk::ImageLayout::eTransferSrcOptimal,
            dest.image, vk::ImageLayout::eTransferDstOptimal, regions,
            info.props.is_depth ? vk::Filter::eNearest : vk::Filter::eLinear);
    }
    Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead, {});
    backing = saved;
}

void Image::PublishScalePlan() {
    scale_plan->eighths = scale_eighths;
    scale_plan->drop = mip_skip;
    scale_plan->physical_width = backing->image.image_ci.extent.width;
    scale_plan->physical_height = backing->image.image_ci.extent.height;
    scale_plan->physical_format = u32(backing->image.image_ci.format);
    ++scale_plan->plan_version;
    const auto category = "image/" + std::string(ScaleDomainName(scale_plan->domain));
    VmaDiagnostics::Tag(instance->GetAllocator(), backing->image.allocation, category.c_str());
}

void Image::ReallocateScale(u32 eighths) {
    if (eighths == scale_eighths && !mip_skip && !astc_encoded) return;
    scheduler->EndRendering();
    auto ci = backing->image.image_ci;
    const InternalScale scale{eighths};
    ci.extent = vk::Extent3D{scale.Size(info.size.width), scale.Size(info.size.height), info.size.depth};
    ci.mipLevels = scale.Levels(info.size.width, info.size.height, info.resources.levels);
    const bool reload = mip_skip != 0 || astc_encoded;
    if (astc_encoded) ci.format = instance->GetSupportedFormat(info.pixel_format, format_features);
    auto* source = backing;
    auto retired = std::make_shared<std::deque<BackingImage>>(std::move(backing_images));
    backing_images.clear();
    backing = &backing_images.emplace_back();
    backing->num_samples = source->num_samples;
    backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
    backing->image.Create(ci);
    scale_eighths = eighths;
    mip_skip = 0;
    astc_encoded = false;
    if (!reload && source->state.layout != vk::ImageLayout::eUndefined) {
        BlitBacking(*source, *backing);
    }
    auto* views = slot_image_views;
    scheduler->DeferOperation([retired, views] {
        for (auto& image : *retired) {
            for (auto id : image.image_view_ids) views->erase(id);
        }
    });
    for (auto& image : *retired) VmaDiagnostics::Tag(instance->GetAllocator(), image.image.allocation, nullptr, true);
    PublishScalePlan();
    if (reload) {
        // Asset mip/codec conversion cannot contain GPU-produced data. Restore
        // from the current guest/buffer contents before publishing a render use.
        ASSERT(owner && False(flags & ImageFlagBits::GpuModified));
        flags |= ImageFlagBits::CpuDirty;
        owner->RefreshImage(*this);
    }
    // This retains the cache ImageId. Every subsequent FindView reads the new
    // backing; needs_rebind is reserved for overlap replacement of the ImageId.
    // In particular, depth attachment publication requires that flag to be clear.
}

void Image::ForceNative(const char* reason) {
    const std::string_view why{reason};
    const auto code = why.find("streaming") != why.npos ? ScaleReason::Streaming :
        why.find("readback") != why.npos ? ScaleReason::Readback :
        why.find("mixed") != why.npos ? ScaleReason::MixedPass :
        why.find("cost") != why.npos ? ScaleReason::UpdateCost :
        why.find("budget") != why.npos ? ScaleReason::Budget :
        why.find("alias") != why.npos || why.find("mismatch") != why.npos || why.find("reinterpretation") != why.npos
            ? ScaleReason::Alias : ScaleReason::SemanticNative;
    if (code == ScaleReason::Readback && IsScaled()) scale_plan->upscaled_readback = true;
    const bool changed = scale_plan->domain != ScaleDomain::NativeRequired;
    if (changed) {
        scale_plan->RequireNative(code);
        if (owner) owner->RecordNativeFallback(code);
    }
    if (IsScaled()) ReallocateScale(8);
    else if (changed) PublishScalePlan();
}

void Image::ObserveUsage(ScaleUse use) {
    scale_plan->history |= 1u << u32(use);
    if (use == ScaleUse::Storage) { ForceNative("storage"); return; }
    const bool render = use == ScaleUse::RenderTarget || use == ScaleUse::DepthTarget || use == ScaleUse::VideoOut;
    if (!render || scale_plan->domain == ScaleDomain::NativeRequired || scale_plan->render_transition) return;
    scale_plan->render_transition = true;
    scale_plan->domain = ScaleDomain::Render;
    scale_plan->reason = policy.legacy ? ScaleReason::Legacy : ScaleReason::None;
    const u32 eighths = policy.render_eighths;
    const auto format = instance->GetSupportedFormat(info.pixel_format, format_features);
    if (info.num_samples != 1 || info.props.is_block ||
        ConvertImageType(info.type) != vk::ImageType::e2D ||
        !instance->IsFormatSupported(format, vk::FormatFeatureFlagBits2::eBlitSrc | vk::FormatFeatureFlagBits2::eBlitDst) ||
        (!info.props.is_depth && !instance->IsFormatSupported(format, vk::FormatFeatureFlagBits2::eSampledImageFilterLinear))) {
        ForceNative("attachment format"); return;
    }
    if (!policy.legacy && std::min(info.size.width, info.size.height) <= 64) {
        scale_plan->reason = ScaleReason::SizeProtect;
        return;
    }
    // Dirty CPU data is uploaded by UpdateImage before the attachment is used;
    // clean GPU data is copied from the existing backing, never stale RAM.
    ReallocateScale(eighths);
}

bool Image::InheritCopyPlan(Image& source) {
    scale_plan->history |= 1u << 8;
    source.scale_plan->history |= 1u << 7;
    const bool replan = scale_eighths != source.scale_eighths;
    if (scale_plan->domain == ScaleDomain::NativeRequired ||
        (replan && (scale_plan->sampled ||
         (scale_plan->origin != ScaleOrigin::Unknown && scale_plan->origin != ScaleOrigin::Copy))) ||
        source.mip_skip || source.astc_encoded || mip_skip || astc_encoded ||
        info.size != source.info.size || info.num_bits != source.info.num_bits ||
        info.props.is_block != source.info.props.is_block || info.type != source.info.type ||
        info.resources.layers != source.info.resources.layers) return false;
    if (source.IsScaled() && info.props.is_block) return false;
    ReallocateScale(source.scale_eighths);
    scale_plan->domain = source.scale_plan->domain;
    scale_plan->origin = ScaleOrigin::Copy;
    scale_plan->source_generation = source.scale_plan->mapping_generation;
    scale_plan->render_transition = source.scale_plan->render_transition;
    scale_plan->reason = ScaleReason::CopyInherit;
    ++scale_plan->content_version;
    PublishScalePlan();
    return true;
}

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples != instance->HostSamples(info.num_samples)) {
        SetBackingSamples(info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        const auto view_id = backing->image_view_ids[std::distance(view_infos.begin(), it)];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->insert(*instance, view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

Image::Barriers Image::GetBarriers(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                                   vk::PipelineStageFlags2 dst_stage,
                                   std::optional<SubresourceRange> subres_range) {
    if (subres_range) subres_range = HostRange(*subres_range);
    const SubresourceExtent host_resources{backing->image.image_ci.mipLevels, info.resources.layers};
    auto& last_state = backing->state;
    auto& subresource_states = backing->subresource_states;

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != host_resources);
    const bool partially_transited = !subresource_states.empty();

    Barriers barriers;
    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(host_resources.levels * host_resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
        }

        // In case of partial transition, we need to change the specified subresources only.
        // Otherwise all subresources need to be set to the same state so we can use a full
        // resource transition for the next time.
        const auto mips =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.level,
                                           subres_range->base.level + subres_range->extent.levels)
                : std::views::iota(0u, host_resources.levels);
        const auto layers =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.layer,
                                           subres_range->base.layer + subres_range->extent.layers)
                : std::views::iota(0u, host_resources.layers);

        for (u32 mip : mips) {
            for (u32 layer : layers) {
                // NOTE: these loops may produce a lot of small barriers.
                // If this becomes a problem, we can optimize it by merging adjacent barriers.
                const auto subres_idx = mip * host_resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    barriers.emplace_back(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = state.layout,
                        .newLayout = dst_layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = GetImage(),
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
        }
    } else { // Full resource transition
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask && !is_write) {
            return {};
        }

        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = last_state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;

    return barriers;
}

void Image::Transit(vk::ImageLayout dst_layout, vk::AccessFlags2 dst_mask,
                    std::optional<SubresourceRange> range, vk::CommandBuffer cmdbuf /*= {}*/) {
    // Adjust pipeline stage
    const vk::PipelineStageFlags2 dst_pl_stage =
        (dst_mask == vk::AccessFlagBits2::eTransferRead ||
         dst_mask == vk::AccessFlagBits2::eTransferWrite)
            ? vk::PipelineStageFlagBits2::eTransfer
            : vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader;

    const auto barriers = GetBarriers(dst_layout, dst_mask, dst_pl_stage, range);
    if (barriers.empty()) {
        return;
    }

    if (!cmdbuf) {
        // When using external cmdbuf you are responsible for ending rp.
        scheduler->EndRendering();
        cmdbuf = scheduler->CommandBuffer();
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    });
}

void Image::CheckUploadBudget() {
    if (!owner || policy.legacy || !IsScaled() || mip_skip ||
        scale_plan->domain != ScaleDomain::Asset) return;
    // A bounded per-identity conversion budget prevents perpetually dirty assets
    // from allocating a native upload image and encoding on every refresh.
    if (scale_plan->domain == ScaleDomain::Asset &&
        (scale_plan->uploads >= 8 || scale_plan->upload_bytes >= 64ull * 1024 * 1024)) {
        ForceNative("update cost");
    } else if (u64(info.guest_size) > 64ull * 1024 * 1024 ||
        MemoryDiagnostics::upload_image_bytes.load(std::memory_order_relaxed) + info.guest_size > 256ull * 1024 * 1024) {
        ForceNative("upload budget");
    }
}

void Image::Upload(std::span<const vk::BufferImageCopy> copies, vk::Buffer buffer, u64 offset, u64 buffer_size) {
    if (!buffer_size) buffer_size = info.guest_size;
    ++scale_plan->content_version;
    ++scale_plan->uploads;
    scale_plan->upload_bytes += buffer_size;
    if (scale_plan->domain == ScaleDomain::Asset) scale_plan->origin = ScaleOrigin::Upload;
    if (!IsScaled()) {
        UploadRegions(copies, buffer, offset, buffer_size);
        return;
    }
    if (mip_skip) {
        std::vector<vk::BufferImageCopy> mapped;
        for (auto copy : copies) {
            if (copy.imageSubresource.mipLevel < mip_skip) continue;
            copy.imageSubresource.mipLevel -= mip_skip;
            mapped.push_back(copy);
        }
        if (!mapped.empty()) UploadRegions(mapped, buffer, offset, buffer_size);
        flags &= ~ImageFlagBits::Dirty;
        return;
    }
    // Upload/resample only when guest memory changed. The full-size staging image
    // retires after this submission; it is not a second persistent backing.
    struct UploadImage : BackingImage {
        u64 allocation_bytes{};
        ~UploadImage() {
            MemoryDiagnostics::upload_image_bytes.fetch_sub(allocation_bytes, std::memory_order_relaxed);
        }
    };
    auto temporary = std::make_shared<UploadImage>();
    temporary->num_samples = 1;
    temporary->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
    auto ci = backing->image.image_ci;
    ci.extent = vk::Extent3D{info.size.width, info.size.height, info.size.depth};
    ci.mipLevels = info.resources.levels;
    if (astc_encoded) ci.format = instance->GetSupportedFormat(info.pixel_format, format_features);
    temporary->image.Create(ci);
    VmaDiagnostics::Tag(instance->GetAllocator(), temporary->image.allocation, "image/upload-source", true);
    VmaAllocationInfo temporary_info{};
    vmaGetAllocationInfo(instance->GetAllocator(), temporary->image.allocation, &temporary_info);
    temporary->allocation_bytes = temporary_info.size;
    MemoryDiagnostics::upload_image_bytes.fetch_add(temporary_info.size, std::memory_order_relaxed);
    MemoryDiagnostics::upload_image_created_bytes.fetch_add(temporary_info.size, std::memory_order_relaxed);
    MemoryDiagnostics::upload_image_created_count.fetch_add(1, std::memory_order_relaxed);
    auto* scaled = backing;
    backing = temporary.get();
    UploadRegions(copies, buffer, offset, buffer_size);
    if (astc_encoded) {
        Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {});
        backing = scaled;
        Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
        for (const auto& copy : copies) {
            const u32 mip = copy.imageSubresource.mipLevel;
            if (mip >= backing->image.image_ci.mipLevels) continue;
            const auto extent = HostExtent(mip);
            blit_helper->EncodeAstc(temporary->image, ci.format, mip, backing->image, mip,
                extent.width, extent.height, info.resources.layers, IsSrgbBlock(ci.format), info.num_bits == 64 ? 6 : 4);
        }
        Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead, {});
    } else {
        backing = scaled;
        BlitBacking(*temporary, *scaled, copies);
    }
    scheduler->DeferOperation([temporary] {});
}

void Image::UploadRegions(std::span<const vk::BufferImageCopy> upload_copies, vk::Buffer buffer,
                   u64 offset, u64 buffer_size) {
    SetBackingSamples(info.num_samples, false);
    scheduler->EndRendering();

    const vk::BufferMemoryBarrier2 pre_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer,
        .offset = offset,
        .size = buffer_size,
    };
    const vk::BufferMemoryBarrier2 post_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .buffer = buffer,
        .offset = offset,
        .size = buffer_size,
    };
    const auto image_barriers =
        GetBarriers(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eCopy, {});
    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
        .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    });
    cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal,
                             upload_copies);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
    flags &= ~ImageFlagBits::Dirty;
}

void Image::Download(std::span<const vk::BufferImageCopy> download_copies, vk::Buffer buffer,
                     u64 offset, u64 download_size) {
    ForceNative("guest readback");
    SetBackingSamples(info.num_samples);
    scheduler->EndRendering();

    const vk::BufferMemoryBarrier2 pre_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = download_size,
    };
    const vk::BufferMemoryBarrier2 post_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .buffer = buffer,
        .offset = offset,
        .size = download_size,
    };
    const auto image_barriers =
        GetBarriers(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                    vk::PipelineStageFlagBits2::eCopy, {});
    auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_barrier,
        .imageMemoryBarrierCount = static_cast<u32>(image_barriers.size()),
        .pImageMemoryBarriers = image_barriers.data(),
    });
    cmdbuf.copyImageToBuffer(GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer,
                             download_copies);
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_barrier,
    });
}

static std::pair<u32, u32> SanitizeCopyLayers(const ImageInfo& src_info, const ImageInfo& dst_info,
                                              const u32 depth) {
    const auto vk_src_type = ConvertImageType(src_info.type);
    const auto vk_dst_type = ConvertImageType(dst_info.type);

    u32 src_layers = src_info.resources.layers;
    u32 dst_layers = dst_info.resources.layers;

    // 3D images can only use 1 layer.
    if (vk_src_type == vk::ImageType::e3D && src_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D source layers {} to 1.", src_layers);
        src_layers = 1;
    }
    if (vk_dst_type == vk::ImageType::e3D && dst_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D destination layers {} to 1.", dst_layers);
        dst_layers = 1;
    }

    // If the image type is equal, layer count must match. Take the minimum of both.
    if (vk_src_type == vk_dst_type) {
        if (src_layers != dst_layers) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy source layers {} and destination layers {} to minimum.",
                        src_layers, dst_layers);
            src_layers = dst_layers = std::min(src_layers, dst_layers);
        }
    } else {
        // For 2D <-> 3D copies, 2D layer count must equal 3D depth.
        if (vk_src_type == vk::ImageType::e2D && vk_dst_type == vk::ImageType::e3D &&
            src_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D source layers {} to 3D destination depth {}", src_layers,
                        depth);
            src_layers = depth;
        }
        if (vk_src_type == vk::ImageType::e3D && vk_dst_type == vk::ImageType::e2D &&
            dst_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D destination layers {} to 3D source depth {}", dst_layers,
                        depth);
            dst_layers = depth;
        }
    }

    return std::make_pair(src_layers, dst_layers);
}

void Image::CopyImage(Image& src_image) {
    if (!InheritCopyPlan(src_image)) {
        ForceNative("copy alias");
        src_image.ForceNative("copy alias");
    }
    if ((IsScaled() || src_image.IsScaled()) &&
        (mip_skip || src_image.mip_skip || astc_encoded || src_image.astc_encoded || scale_eighths != src_image.scale_eighths ||
         info.size != src_image.info.size || info.num_bits != src_image.info.num_bits ||
         info.props.is_block != src_image.info.props.is_block)) {
        ForceNative("image alias");
        src_image.ForceNative("image alias");
    }
    const auto& src_info = src_image.info;

    const u32 num_mips = std::min(src_image.backing->image.image_ci.mipLevels, backing->image.image_ci.mipLevels);

    // Format mismatch warning (safe but useful)
    if (src_info.pixel_format != info.pixel_format) {
        LOG_DEBUG(Render_Vulkan,
                  "Copy between different formats: src={}, dst={}. "
                  "Result may be undefined.",
                  vk::to_string(src_info.pixel_format), vk::to_string(info.pixel_format));
    }

    const u32 base_width = src_image.HostExtent().width;
    const u32 base_height = src_image.HostExtent().height;
    const u32 base_depth =
        info.type == AmdGpu::ImageType::Color3D ? info.size.depth : src_info.size.depth;

    // Match sample count before copying
    SetBackingSamples(info.num_samples, false);
    src_image.SetBackingSamples(src_info.num_samples);

    boost::container::small_vector<vk::ImageCopy, 8> regions;

    const vk::ImageAspectFlags src_aspect =
        src_image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const vk::ImageAspectFlags dst_aspect = aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const bool src_is_2d = ConvertImageType(src_info.type) == vk::ImageType::e2D;
    const bool src_is_3d = ConvertImageType(src_info.type) == vk::ImageType::e3D;

    const bool dst_is_2d = ConvertImageType(info.type) == vk::ImageType::e2D;
    const bool dst_is_3d = ConvertImageType(info.type) == vk::ImageType::e3D;

    const bool is_2d_to_3d = src_is_2d && dst_is_3d;
    const bool is_3d_to_2d = src_is_3d && dst_is_2d;
    const bool is_same_type = !is_2d_to_3d && !is_3d_to_2d;

    for (u32 mip = 0; mip < num_mips; ++mip) {
        const u32 mip_w = std::max(base_width >> mip, 1u);
        const u32 mip_h = std::max(base_height >> mip, 1u);
        const u32 mip_d = std::max(base_depth >> mip, 1u);

        auto [src_layers, dst_layers] = SanitizeCopyLayers(src_info, info, mip_d);

        vk::ImageCopy region{};

        region.srcSubresource.aspectMask = src_aspect;
        region.srcSubresource.mipLevel = mip;
        region.srcSubresource.baseArrayLayer = 0;

        region.dstSubresource.aspectMask = dst_aspect;
        region.dstSubresource.mipLevel = mip;
        region.dstSubresource.baseArrayLayer = 0;

        if (is_same_type) {
            // 2D->2D OR 3D->3D
            if (src_is_3d) {
                // 3D images must use layerCount=1
                region.srcSubresource.layerCount = 1;
                region.dstSubresource.layerCount = 1;
                region.extent = vk::Extent3D(mip_w, mip_h, mip_d);
            } else {
                // Array images
                const u32 copy_layers = std::min(src_layers, dst_layers);
                region.srcSubresource.layerCount = copy_layers;
                region.dstSubresource.layerCount = copy_layers;
                region.extent = vk::Extent3D(mip_w, mip_h, 1);
            }
        } else if (is_2d_to_3d) {
            // 2D array -> 3D volume
            region.srcSubresource.layerCount = src_layers;
            region.dstSubresource.layerCount = 1;
            region.extent = vk::Extent3D(mip_w, mip_h, src_layers);
        } else if (is_3d_to_2d) {
            // 3D volume -> 2D array
            region.srcSubresource.layerCount = 1;
            region.dstSubresource.layerCount = dst_layers;
            region.extent = vk::Extent3D(mip_w, mip_h, dst_layers);
        }

        regions.push_back(region);
    }

    scheduler->EndRendering();

    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});

    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});

    auto cmdbuf = scheduler->CommandBuffer();

    if (!regions.empty()) {
        cmdbuf.copyImage(src_image.GetImage(), src_image.backing->state.layout, GetImage(),
                         backing->state.layout, regions);
    }

    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}
void Image::CopyImageWithBuffer(Image& src_image, vk::Buffer buffer, u64 offset) {
    if (!InheritCopyPlan(src_image)) {
        ForceNative("copy alias");
        src_image.ForceNative("copy alias");
    }
    if (mip_skip || src_image.mip_skip || astc_encoded || src_image.astc_encoded || scale_eighths != src_image.scale_eighths ||
        info.size != src_image.info.size || info.num_bits != src_image.info.num_bits ||
        info.props.is_block != src_image.info.props.is_block) {
        ForceNative("byte reinterpretation");
        src_image.ForceNative("byte reinterpretation");
    }
    const auto& src_info = src_image.info;
    const u32 num_mips = std::min(src_image.backing->image.image_ci.mipLevels, backing->image.image_ci.mipLevels);
    const u32 num_layers = std::min(src_info.resources.layers, info.resources.layers);
    ASSERT(src_info.resources.layers == info.resources.layers || num_mips == 1);

    SetBackingSamples(info.num_samples, false);
    src_image.SetBackingSamples(src_info.num_samples);

    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    u64 mip_offset = offset;
    for (u32 mip = 0; mip < num_mips; ++mip) {
        const auto mip_w = src_image.HostExtent(mip).width;
        const auto mip_h = src_image.HostExtent(mip).height;
        const auto mip_d = std::max(src_info.size.depth >> mip, 1u);

        buffer_copies.emplace_back(vk::BufferImageCopy{
            .bufferOffset = mip_offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource{
                .aspectMask = src_image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {mip_w, mip_h, mip_d},
        });
        // Each mip needs its own byte range. Reusing the base offset overwrites
        // all earlier levels before the subsequent buffer-to-image copies.
        const u32 block = src_info.props.is_block ? 4 : 1;
        const u64 mip_bytes = u64(Common::DivCeil(mip_w, block)) *
            Common::DivCeil(mip_h, block) * mip_d * num_layers * (src_info.num_bits / 8);
        mip_offset = Common::AlignUp(mip_offset + mip_bytes, u64(16));
    }

    const vk::BufferMemoryBarrier2 pre_copy_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .buffer = buffer,
        .offset = offset,
        .size = VK_WHOLE_SIZE,
    };

    const vk::BufferMemoryBarrier2 post_copy_barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = buffer,
        .offset = offset,
        .size = VK_WHOLE_SIZE,
    };

    scheduler->EndRendering();
    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});

    auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &pre_copy_barrier,
    });

    cmdbuf.copyImageToBuffer(src_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, buffer,
                             buffer_copies);

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .dependencyFlags = vk::DependencyFlagBits::eByRegion,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &post_copy_barrier,
    });

    for (auto& copy : buffer_copies) {
        copy.imageSubresource.aspectMask = aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    }

    cmdbuf.copyBufferToImage(buffer, GetImage(), vk::ImageLayout::eTransferDstOptimal,
                             buffer_copies);
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}

void Image::CopyMip(Image& src_image, u32 mip, u32 slice) {
    ForceNative("mip alias");
    src_image.ForceNative("mip alias");
    const auto& src_info = src_image.info;

    const auto dst_dim = info.props.is_block ? 2 : 0;
    const auto mip_block_w = std::max(info.size.width >> (mip + dst_dim), 1u);
    const auto mip_block_h = std::max(info.size.height >> (mip + dst_dim), 1u);
    const auto mip_block_p = std::max(info.mips_layout[mip].pitch >> dst_dim, 1u);

    const auto src_dim = src_info.props.is_block ? 2 : 0;
    ASSERT(mip_block_w == (src_info.size.width >> src_dim));
    ASSERT(mip_block_h == (src_info.size.height >> src_dim));
    ASSERT(mip_block_p == (src_info.pitch >> src_dim));

    const auto [src_layers, dst_layers] = SanitizeCopyLayers(src_info, info, src_info.size.depth);

    const vk::ImageCopy image_copy{
        .srcSubresource{
            .aspectMask = src_image.aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = src_layers,
        },
        .dstSubresource{
            .aspectMask = src_image.aspect_mask,
            .mipLevel = mip,
            .baseArrayLayer = slice,
            .layerCount = dst_layers,
        },
        .extent = {src_info.size.width, src_info.size.height, src_info.size.depth},
    };

    SetBackingSamples(info.num_samples);
    src_image.SetBackingSamples(src_info.num_samples);

    scheduler->EndRendering();
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});

    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.copyImage(src_image.GetImage(), src_image.backing->state.layout, GetImage(),
                     backing->state.layout, image_copy);
    Transit(vk::ImageLayout::eGeneral,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead, {});
}

void Image::Resolve(Image& src_image, const VideoCore::SubresourceRange& mrt0_range,
                    const VideoCore::SubresourceRange& mrt1_range) {
    if (!InheritCopyPlan(src_image)) {
        ForceNative("resolve alias");
        src_image.ForceNative("resolve alias");
    }
    if (scale_eighths != src_image.scale_eighths || info.size != src_image.info.size) {
        ForceNative("resolve mismatch");
        src_image.ForceNative("resolve mismatch");
    }
    SetBackingSamples(1, false);
    scheduler->EndRendering();

    src_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                      mrt0_range);
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, mrt1_range);

    const auto [src_layers, dst_layers] = SanitizeCopyLayers(src_image.info, info, 1);
    if (src_image.backing->num_samples == 1) {
        const vk::ImageCopy region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt0_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt1_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = HostExtent(),
        };
        scheduler->CommandBuffer().copyImage(src_image.GetImage(),
                                             vk::ImageLayout::eTransferSrcOptimal, GetImage(),
                                             vk::ImageLayout::eTransferDstOptimal, region);
    } else {
        const vk::ImageResolve region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt0_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = mrt1_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = HostExtent(),
        };
        scheduler->CommandBuffer().resolveImage(src_image.GetImage(),
                                                vk::ImageLayout::eTransferSrcOptimal, GetImage(),
                                                vk::ImageLayout::eTransferDstOptimal, region);
    }

    flags |= VideoCore::ImageFlagBits::GpuModified;
    flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Image::Clear(const vk::ClearValue& clear_value, const VideoCore::SubresourceRange& guest_range) {
    const auto range = HostRange(guest_range);
    const vk::ImageSubresourceRange vk_range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = range.base.level,
        .levelCount = range.extent.levels,
        .baseArrayLayer = range.base.layer,
        .layerCount = range.extent.layers,
    };
    scheduler->EndRendering();
    Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
    const auto cmdbuf = scheduler->CommandBuffer();
    cmdbuf.clearColorImage(GetImage(), vk::ImageLayout::eTransferDstOptimal, clear_value.color,
                           vk_range);
}

void Image::SetBackingSamples(u32 num_samples, bool copy_backing) {
    num_samples = instance->HostSamples(num_samples);
    if (num_samples > 1) ForceNative("multisample backing");
    if (!backing || backing->num_samples == num_samples) {
        return;
    }
    ASSERT_MSG(!info.props.is_depth, "Swapping samples is only valid for color images");
    BackingImage* new_backing;
    auto it = std::ranges::find(backing_images, num_samples, &BackingImage::num_samples);
    if (it == backing_images.end()) {
        auto new_image_ci = backing->image.image_ci;
        // Returning from an attachment backing to a single-sample storage view
        // must restore its storage usage; promotion must re-query the MSAA tuple.
        new_image_ci.usage = usage_flags;
        ConfigureImageSamples(*instance, new_image_ci, num_samples);

        new_backing = &backing_images.emplace_back();
        new_backing->num_samples = num_samples;
        new_backing->image = UniqueImage{instance->GetDevice(), instance->GetAllocator()};
        new_backing->image.Create(new_image_ci);

        Vulkan::SetObjectName(instance->GetDevice(), new_backing->image.image,
                              "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{} (backing)",
                              info.size.width, info.size.height, info.size.depth,
                              AmdGpu::NameOf(info.tile_mode), vk::to_string(info.pixel_format),
                              info.guest_address, info.guest_size, info.resources.layers,
                              info.resources.levels, num_samples);
    } else {
        new_backing = std::addressof(*it);
    }

    if (copy_backing) {
        scheduler->EndRendering();
        ASSERT(info.resources.levels == 1 && info.resources.layers == 1);

        // Transition current backing to shader read layout
        auto barriers =
            GetBarriers(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead,
                        vk::PipelineStageFlagBits2::eFragmentShader, std::nullopt);

        // Transition dest backing to color attachment layout, not caring of previous contents
        constexpr auto dst_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        constexpr auto dst_access = vk::AccessFlagBits2::eColorAttachmentWrite;
        constexpr auto dst_layout = vk::ImageLayout::eColorAttachmentOptimal;
        barriers.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = new_backing->image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = info.resources.layers,
            },
        });
        const auto cmdbuf = scheduler->CommandBuffer();
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
            .pImageMemoryBarriers = barriers.data(),
        });

        // Copy between ms and non ms backing images
        blit_helper->CopyBetweenMsImages(
            info.size.width, info.size.height, new_backing->num_samples, info.pixel_format,
            backing->num_samples > 1, backing->image, new_backing->image);

        // Update current layout in tracker to new backings layout
        new_backing->state.layout = dst_layout;
        new_backing->state.access_mask = dst_access;
        new_backing->state.pl_stage = dst_stage;
    }

    backing = new_backing;
}

} // namespace VideoCore
