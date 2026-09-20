// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise real Image upload/resampling/retirement and the production SPIR-V
// image-fetch/query emitters on the device, including dynamic guest descriptors.
#include <cmath>
#include <cstdio>
#include <fstream>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "frontend/window.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/internal_scale.h"
#include "video_core/texture_cache/blit_helper.h"
#include "resource_policy_gpu_checks.h"
#include "texture_cache_gpu_checks.h"
using namespace Shader;
using namespace Shader::Backend::SPIRV;
struct Window : Frontend::Window {
    s32 GetWidth() const override { return 64; }
    s32 GetHeight() const override { return 64; }
    Frontend::WindowSystemInfo GetWindowInfo() const override { return {}; }
    bool RequestKeyboard() override { return false; }
    void ReleaseKeyboard() override {}
};
constexpr u32 W = 65, H = 49, Levels = 3, Layers = 2, Words = 9;
std::vector<u32> ComputeShader(const AmdGpu::Image& sharp, u32 mip) {
    Profile profile{};
    profile.supported_spirv = 0x10600;
    profile.internal_scale = true;
    RuntimeInfo runtime{};
    runtime.stage = Stage::Compute;
    runtime.cs_info.workgroup_size = {1, 1, 1};
    Info info{};
    info.stage = Stage::Compute;
    info.l_stage = LogicalStage::Compute;
    info.buffers.push_back({.used_types = IR::Type::U32, .is_written = true});
    info.buffers.push_back({.used_types = IR::Type::U32, .buffer_type = BufferType::Flatbuf});
    info.flattened_ud_buf.resize(8);
    std::memcpy(info.flattened_ud_buf.data(), &sharp, sizeof(sharp));
    ImageResource resource{};
    resource.is_array = true;
    resource.sharp_fetch.load_mask = 0xff;
    for (u32 i = 0; i < 8; i++) resource.sharp_fetch.offsets[i] = i;
    info.images.push_back(resource);
    Shader::Backend::Bindings bindings{};
    EmitContext c(profile, runtime, info, bindings);
    for (auto cap : {spv::Capability::ImageQuery, spv::Capability::Int8, spv::Capability::Int16})
        c.AddCapability(cap);
    auto gid = c.DefineVariable(c.U32[3], spv::BuiltIn::GlobalInvocationId, spv::StorageClass::Input);
    auto [buf, ptr] = c.buffers[0].Alias(EmitContext::PointerType::U32);
    auto main = c.OpFunction(c.void_id, spv::FunctionControlMask::MaskNone, c.TypeFunction(c.void_id));
    c.AddLabel();
    const auto coord = c.OpLoad(c.U32[3], gid);
    const auto x = c.OpCompositeExtract(c.U32[1], coord, 0);
    const auto y = c.OpCompositeExtract(c.U32[1], coord, 1);
    const auto z = c.OpCompositeExtract(c.U32[1], coord, 2);
    const auto index = c.OpIMul(c.U32[1], c.ConstU32(Words), c.OpIAdd(c.U32[1], x,
        c.OpIMul(c.U32[1], c.ConstU32(std::max(W >> mip, 1u)), c.OpIAdd(c.U32[1], y,
        c.OpIMul(c.U32[1], z, c.ConstU32(std::max(H >> mip, 1u)))))));
    const auto dims = EmitImageQueryDimensions(c, nullptr, 0, c.ConstU32(mip), true);
    const auto pixel = EmitImageRead(c, nullptr, 0, coord, c.ConstU32(mip), {});
    for (u32 i = 0; i < Words; i++) {
        auto value = i < 4 ? c.OpCompositeExtract(c.U32[1], dims, i)
            : i < 8 ? c.OpBitcast(c.U32[1], c.OpCompositeExtract(c.F32[1], pixel, i - 4))
                    : c.RenderScaleEighths();
        c.OpStore(c.OpAccessChain(ptr, buf, c.u32_zero_value,
            c.OpIAdd(c.U32[1], index, c.ConstU32(i))), value);
    }
    c.OpReturn();
    c.OpFunctionEnd();
    c.AddEntryPoint(spv::ExecutionModel::GLCompute, main, "main", c.interfaces);
    c.AddExecutionMode(main, spv::ExecutionMode::LocalSize, 1U, 1U, 1U);
    return c.Assemble();
}
int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 2;
    if (argc == 4) {
        EmulatorSettings.SetInternalScalePercent(50);
        EmulatorSettings.SetTextureQuality(2);
    }
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("internal-scale-probe");
    Window window;
    auto driver = std::string_view(argv[1]) == "system" ? Vulkan::LoadAndroidSystemDriver()
        : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
    Vulkan::Scheduler scheduler(instance);
    if (argc == 4) return TextureCacheGpuChecks(instance, scheduler, argv[2]);
    VideoCore::BlitHelper blit(instance, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    const auto device = instance.GetDevice();
    struct Buffer {
        vk::UniqueDeviceMemory memory;
        vk::UniqueBuffer buffer;
        void* mapped;
    };
    auto make_buffer = [&](u64 size) {
        Buffer result;
        result.buffer = Vulkan::Check(device.createBufferUnique({.size = size,
            .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                     vk::BufferUsageFlagBits::eTransferDst}));
        const auto req = device.getBufferMemoryRequirements(*result.buffer);
        const auto props = instance.GetMemoryProperties();
        const auto flags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        u32 type = 0;
        for (; type < props.memoryTypeCount; ++type)
            if ((req.memoryTypeBits & (1u << type)) && (props.memoryTypes[type].propertyFlags & flags) == flags) break;
        result.memory = Vulkan::Check(device.allocateMemoryUnique({.allocationSize = req.size, .memoryTypeIndex = type}));
        Vulkan::Check(device.bindBufferMemory(*result.buffer, *result.memory, 0));
        result.mapped = Vulkan::Check(device.mapMemory(*result.memory, 0, VK_WHOLE_SIZE));
        return result;
    };
    auto upload = make_buffer(W * H * Layers * 8);
    auto output = make_buffer(W * H * Layers * Words * 4);
    auto flat = make_buffer(32);
    auto* src = static_cast<u8*>(upload.mapped);
    std::vector<vk::BufferImageCopy> copies;
    u32 bytes = 0;
    for (u32 mip = 0; mip < Levels; mip++) {
        const u32 w = W >> mip, h = H >> mip;
        copies.push_back({.bufferOffset = bytes,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, Layers},
            .imageExtent = {w, h, 1}});
        for (u32 layer = 0; layer < Layers; ++layer) for (u32 y = 0; y < h; y++) for (u32 x = 0; x < w; x++) {
            src[bytes++] = std::lround(x * 255.f / (w - 1));
            src[bytes++] = std::lround(y * 255.f / (h - 1));
            src[bytes++] = layer * 64 + mip * 16;
            src[bytes++] = 255;
        }
    }
    auto sharp = AmdGpu::Image::Null(false);
    sharp.type = u64(AmdGpu::ImageType::Color2DArray);
    sharp.width = W - 1; sharp.height = H - 1; sharp.pitch = W - 1;
    sharp.last_level = Levels - 1; sharp.last_array = Layers - 1; sharp.depth = Layers - 1;
    std::memcpy(flat.mapped, &sharp, 32);
    std::array bindings{
        vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute}};
    auto set_layout = Vulkan::Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = u32(bindings.size()), .pBindings = bindings.data()}));
    vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(PushData)};
    auto layout = Vulkan::Check(device.createPipelineLayoutUnique({.setLayoutCount = 1,
        .pSetLayouts = &*set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range}));
    std::array<vk::UniquePipeline, Levels> pipelines;
    for (u32 mip = 0; mip < Levels; ++mip) {
        const auto code = ComputeShader(sharp, mip);
        std::ofstream file(std::string(argv[2]) + "/internal-scale-" + std::to_string(mip) + ".spv", std::ios::binary);
        file.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
        auto module = Vulkan::Check(device.createShaderModuleUnique({.codeSize = code.size() * 4, .pCode = code.data()}));
        pipelines[mip] = Vulkan::Check(device.createComputePipelineUnique({}, {
            .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"}, .layout = *layout}));
    }
    u32 checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        checks++;
        if (!ok && failures++ < 10) printf("FAIL %s\n", label);
    };
    const std::vector<u8> rgba(src, src + bytes);
    const auto rgba_copies = copies;
    for (u32 compressed : {0u, 1u, 3u, 4u, 5u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u}) for (float percent : {100.f, 75.f, 50.f, 37.5f, 25.f}) {
        const bool encoder_gradient = compressed >= 10;
        const bool srgb = compressed == 12 || compressed == 15 || compressed == 5;
        const bool alpha_gradient = compressed==11 || compressed==12 || compressed==14 || compressed==15;
        const bool independent_gradient = compressed==16 || compressed==17;
        const u32 block_dim = (compressed>=13 && compressed<=15) || compressed==17 ? 6 : 4;
        float max_rgb_error=0, max_alpha_error=0;
        const u32 test_levels = compressed == 4 ? 1 : Levels;
        sharp.last_level = test_levels - 1;
        std::memcpy(flat.mapped, &sharp, 32);
        if (!compressed || encoder_gradient) {
            bytes = rgba.size(); copies = rgba_copies;
            std::memcpy(src, rgba.data(), bytes);
            if (encoder_gradient) for (u32 i=0; i<bytes; i+=4) {
                if (!independent_gradient) src[i+1] = 255 - src[i]; // Anti-correlated channels exercise endpoint ordering.
                if (alpha_gradient) src[i+3] = 51 + src[i]*0.7f;
            }
        } else {
            bytes = 0; copies.clear();
            for (u32 mip=0; mip<test_levels; mip++) {
                const u32 w=W>>mip, h=H>>mip;
                copies.push_back({.bufferOffset=bytes,
                    .imageSubresource={vk::ImageAspectFlagBits::eColor,mip,0,Layers}, .imageExtent={w,h,1}});
                for (u32 layer=0; layer<Layers; layer++) for (u32 block=0; block<((w+3)/4)*((h+3)/4); block++) {
                    if (compressed==3) {
                        const u64 alpha=128; std::memcpy(src+bytes,&alpha,8); bytes+=8;
                    }
                    const u64 color=u64(((mip+1)*6)<<11 | ((layer+1)*20)<<5 | 16);
                    std::memcpy(src+bytes,&color,8); bytes+=8;
                }
            }
        }
        const auto scale = VideoCore::InternalScale::FromPercent(percent);
        EmulatorSettings.SetInternalScalePercent(percent);
        check(EmulatorSettings.GetInternalScalePercent() == percent, "native percentage precision");
        PushData packed{};
        packed.SetRenderScale(scale.eighths);
        for (u32 binding = 0; binding < PushData::MaxScaledBinding; ++binding)
            packed.SetImageScale(binding, 3);
        packed.SetImageScale(PushData::MaxScaledBinding, 3); // Refused; must not overwrite render scale.
        check((packed.image_scales[1] >> 28) == scale.eighths, "push scale packing boundary");
        VideoCore::ImageInfo info{};
        info.size = {W, H, 1}; info.resources = {test_levels, Layers};
        info.pixel_format = compressed == 3 ? vk::Format::eBc3UnormBlock :
            compressed == 5 ? vk::Format::eBc1RgbaSrgbBlock :
            compressed && !encoder_gradient ? vk::Format::eBc1RgbaUnormBlock : vk::Format::eR8G8B8A8Unorm;
        if (encoder_gradient) info.pixel_format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
        info.props.is_block = compressed && !encoder_gradient;
        info.type = AmdGpu::ImageType::Color2D;
        info.num_bits = compressed == 3 ? 128 : compressed && !encoder_gradient ? 64 : 32; info.pitch = W; info.guest_size = bytes;
        std::unique_ptr<VideoCore::Image> source_image;
        if (encoder_gradient && percent != 100) {
            EmulatorSettings.SetInternalScalePercent(100);
            source_image = std::make_unique<VideoCore::Image>(instance, scheduler, blit, views, info);
            source_image->Upload(copies, *upload.buffer, 0);
            source_image->Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {});
            info.size.width = scale.Size(W); info.size.height = scale.Size(H);
            info.pixel_format = block_dim == 6
                ? (srgb ? vk::Format::eAstc6x6SrgbBlock : vk::Format::eAstc6x6UnormBlock)
                : (srgb ? vk::Format::eAstc4x4SrgbBlock : vk::Format::eAstc4x4UnormBlock);
            info.props.is_block = true; info.num_bits = 128;
        }
        const VideoCore::ScalePolicySnapshot probe_policy{
            encoder_gradient ? 8u : scale.eighths, VideoCore::TextureQuality::High, true};
        VideoCore::Image image(instance, scheduler, blit, views, info, nullptr,
            VideoCore::ScaleUse::Texture, probe_policy);
        std::unique_ptr<VideoCore::Image> copy_source;
        if (!compressed) {
            copy_source = std::make_unique<VideoCore::Image>(instance, scheduler, blit, views, info, nullptr,
                VideoCore::ScaleUse::Texture, probe_policy);
            copy_source->Upload(copies, *upload.buffer, 0);
            image.CopyImageWithBuffer(*copy_source, *upload.buffer, 0);
        } else if (source_image) {
            image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite, {});
            for (u32 mip=0; mip<Levels; ++mip) {
                auto extent=image.HostExtent(mip);
                blit.EncodeAstc(source_image->GetImage(), source_image->info.pixel_format, mip,
                    image.GetImage(), mip, extent.width, extent.height, Layers, srgb, block_dim);
            }
        } else image.Upload(copies, *upload.buffer, 0);
        check(image.HostExtent().width == scale.Size(W), "physical width");
        check(image.HostExtent().height == scale.Size(H), "physical height");
        VideoCore::ImageViewInfo vi{};
        vi.format = info.pixel_format; vi.type = AmdGpu::ImageType::Color2DArray;
        vi.range.extent = info.resources;
        auto view = image.FindView(vi).image_view.get();
        image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {});
        for (u32 mip = 0; mip < test_levels; mip++) {
            auto cmd = scheduler.CommandBuffer();
            const u32 w = W >> mip, h = H >> mip;
            const u32 hw = image.HostExtent(mip).width, hh = image.HostExtent(mip).height;
            PushData push{}; push.SetImageScale(2, source_image ? 2u : image.ShaderScaleCode(0));
            push.SetRenderScale(scale.eighths);
            std::array buffers{vk::DescriptorBufferInfo{*output.buffer, 0, VK_WHOLE_SIZE},
                               vk::DescriptorBufferInfo{*flat.buffer, 0, 32}};
            vk::DescriptorImageInfo texture{{}, view, vk::ImageLayout::eShaderReadOnlyOptimal};
            std::array writes{
                vk::WriteDescriptorSet{.dstBinding=0, .descriptorCount=1, .descriptorType=vk::DescriptorType::eStorageBuffer, .pBufferInfo=&buffers[0]},
                vk::WriteDescriptorSet{.dstBinding=1, .descriptorCount=1, .descriptorType=vk::DescriptorType::eStorageBuffer, .pBufferInfo=&buffers[1]},
                vk::WriteDescriptorSet{.dstBinding=2, .descriptorCount=1, .descriptorType=vk::DescriptorType::eSampledImage, .pImageInfo=&texture}};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipelines[mip]);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes);
            cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
            cmd.dispatch(w, h, Layers);
            scheduler.Finish();
            auto* result = static_cast<u32*>(output.mapped);
            for (u32 layer=0; layer<Layers; layer++) for (u32 y=0; y<h; y++) for (u32 x=0; x<w; x++) {
                const auto* p = result + ((layer*h+y)*w+x)*Words;
                check(p[0]==w && p[1]==h && p[2]==Layers && p[3]==test_levels, "logical dimensions");
                check(p[8] == scale.eighths, "GPU render scale decode");
                const u32 hx = (x+0.5f)*hw/w, hy=(y+0.5f)*hh/h;
                const float expected_x = ((hx+0.5f)*w/hw-0.5f)/(w-1);
                const float expected_y = ((hy+0.5f)*h/hh-0.5f)/(h-1);
                if (encoder_gradient) {
                    const auto linear = [srgb](float v) { return !srgb ? v : v <= .04045f ? v/12.92f : std::pow((v+.055f)/1.055f,2.4f); };
                    const float red_error=std::abs(std::bit_cast<float>(p[4])-linear(expected_x));
                    const float green_error=std::abs(std::bit_cast<float>(p[5])-linear(independent_gradient?expected_y:1.f-expected_x));
                    const float alpha_error=std::abs(std::bit_cast<float>(p[7])-(alpha_gradient?.2f+.7f*expected_x:1.f));
                    max_rgb_error=std::max({max_rgb_error,red_error,green_error});
                    max_alpha_error=std::max(max_alpha_error,alpha_error);
                    // Quality budgets, not lossless assertions: one endpoint axis cannot
                    // represent two independent color ramps in a large low-mip block.
                    const float budget=independent_gradient ? .5f*std::max(float(block_dim)/hw,float(block_dim)/hh)+.02f
                        : srgb && alpha_gradient && block_dim==6 ? .12f : .08f;
                    check(red_error<budget, "ASTC gradient X quality budget");
                    check(green_error<budget, "ASTC gradient green quality budget");
                    check(std::abs(std::bit_cast<float>(p[6])-linear((layer*64+mip*16)/255.f))<.015f, "ASTC gradient layer/mip");
                    check(alpha_error<.06f, "ASTC gradient alpha");
                } else if (compressed) {
                    const u32 actual_mip=image.DroppedMips() ? std::max(mip,image.DroppedMips()) : mip;
                    const auto linear = [srgb](float v) { return !srgb ? v : v <= .04045f ? v/12.92f : std::pow((v+.055f)/1.055f,2.4f); };
                    check(std::abs(std::bit_cast<float>(p[4])-linear((actual_mip+1)*6.f/31))<0.018f, "BC/ASTC red mip");
                    check(std::abs(std::bit_cast<float>(p[5])-linear((layer+1)*20.f/63))<0.018f, "BC/ASTC green layer");
                    check(std::abs(std::bit_cast<float>(p[6])-linear(16.f/31))<0.018f, "BC/ASTC blue");
                    check(std::abs(std::bit_cast<float>(p[7])-(compressed==3?128.f/255:1.f))<0.012f, "BC/ASTC alpha");
                } else {
                    check(std::abs(std::bit_cast<float>(p[4])-expected_x)<0.009f, "guest X fetch");
                    check(std::abs(std::bit_cast<float>(p[5])-expected_y)<0.009f, "guest Y fetch");
                    check(std::abs(std::bit_cast<float>(p[6])-(layer*64+mip*16)/255.f)<0.005f, "layer and mip");
                }
            }
        }
        // This must retain GPU contents and return original readback extents.
        if (!compressed) {
        image.ForceNative("probe readback promotion");
        check(image.HostExtent().width==W && image.HostExtent().height==H, "promotion extent");
        image.Download(std::span{copies}.first(1), *output.buffer, 0, W*H*Layers*4);
        scheduler.Finish();
        const auto* pixels=static_cast<const u8*>(output.mapped);
        const u32 center=((H/2)*W+W/2)*4;
        check(std::abs(int(pixels[center])-128)<6 && std::abs(int(pixels[center+1])-128)<6, "promotion content");
        }
        check((!compressed || encoder_gradient || percent == 100 || image.DroppedMips()) || image.IsAstcEncoded(), "compressed resample really ASTC");
        if (compressed && !encoder_gradient && scale.MipDrop() && test_levels > scale.MipDrop()) {
            check(image.DroppedMips() == scale.MipDrop(), "mip-drop count");
            check(image.backing->image.image_ci.mipLevels == test_levels-scale.MipDrop(), "mip-drop allocation");
            check(image.ShaderScaleCode(0) == (scale.MipDrop() == 2 ? 3u : 1u), "mip-drop shader code");
            check(image.ShaderScaleCode(1) == (scale.MipDrop() == 2 ? 1u : 0u), "view base mip compensation");
            check(image.ShaderScaleCode(2) == 0, "native mip view");
        }
        if (encoder_gradient) printf("ASTC_QUALITY case%u scale%g block%u max_rgb_error=%.6f max_alpha_error=%.6f\n",
            compressed,percent,block_dim,max_rgb_error,max_alpha_error);
        for(auto& backing:image.backing_images) for(auto id:backing.image_view_ids) views.erase(id);
        printf("SCALE %g case%u: host %ux%u, guest %ux%u; cumulative %u checks / %u failures\n",
            percent, compressed, scale.Size(W), scale.Size(H),W,H,checks,failures);
    }
    printf("LEGACY_SCALE_DEVICE %u checks / %u failures\n", checks, failures);
    ResourcePolicyGpuChecks(instance, scheduler, blit, views, check);
    EmulatorSettings.SetInternalScalePercent(100);
    scheduler.Finish();
    printf("INTERNAL_SCALE_DEVICE %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
