// SPDX-License-Identifier: GPL-2.0-or-later
// Production MSAA image types, dimension queries and wrapped sample fetches.
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
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/internal_scale.h"
#include "video_core/texture_cache/blit_helper.h"


using namespace Shader;
using namespace Shader::Backend::SPIRV;
struct Window : Frontend::Window {
    s32 GetWidth() const override { return 64; }
    s32 GetHeight() const override { return 64; }
    Frontend::WindowSystemInfo GetWindowInfo() const override { return {}; }
    bool RequestKeyboard() override { return false; }
    void ReleaseKeyboard() override {}
};
constexpr u32 W = 16, H = 8, Layers = 2, Words = 8;
std::vector<u32> ComputeShader(const AmdGpu::Image& sharp, bool array, u32 sample, bool has_mips, bool disable_msaa) {
    Profile profile{};
    profile.supported_spirv = 0x10600;
    profile.internal_scale = true;
    profile.force_disable_msaa = disable_msaa;
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
    resource.is_array = array;
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
        c.OpIMul(c.U32[1], c.ConstU32(W), c.OpIAdd(c.U32[1], y,
        c.OpIMul(c.U32[1], z, c.ConstU32(H))))));
    const auto dims = EmitImageQueryDimensions(c, nullptr, 0, c.ConstU32(7u), has_mips);
    const auto xy = c.OpCompositeConstruct(c.U32[2], x, y);
    const auto pixel = EmitImageRead(c, nullptr, 0, array ? coord : xy, {}, c.ConstU32(sample));
    for (u32 i = 0; i < Words; i++) {
        auto value = i < 4 ? c.OpCompositeExtract(c.U32[1], dims, i)
            : c.OpBitcast(c.U32[1], c.OpCompositeExtract(c.F32[1], pixel, i - 4));
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
    const bool disable_msaa = argc == 4 && std::string_view(argv[3]) == "off";
    EmulatorSettings.SetMsaaDisabled(disable_msaa);
    Common::FS::InitializeAndroidUserPaths(argv[2]);
    Common::Log::Setup("msaa-image-probe");
    Window window;
    auto driver = std::string_view(argv[1]) == "system" ? Vulkan::LoadAndroidSystemDriver()
        : Vulkan::LoadAndroidTurnip(argv[1], argv[2]);
    Vulkan::Instance instance(window, 0, false, false, driver);
    Vulkan::Scheduler scheduler(instance);
    VideoCore::BlitHelper blit(instance, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    const auto device = instance.GetDevice();
    // Per-sample render writes detect mismatches hidden by uniform clear tests.
    const auto vs = Vulkan::Compile(R"(#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.5, 1.0);
})", vk::ShaderStageFlagBits::eVertex, device);
    const auto fs = Vulkan::Compile(R"(#version 450
layout(push_constant) uniform Color { vec4 value; } color;
layout(location=0) out vec4 result;
void main() { result = color.value; }
)", vk::ShaderStageFlagBits::eFragment, device);
    vk::PushConstantRange render_range{vk::ShaderStageFlagBits::eFragment, 0, 16};
    auto render_layout = Vulkan::Check(device.createPipelineLayoutUnique({
        .pushConstantRangeCount=1, .pPushConstantRanges=&render_range}));
    const auto color_for = [](u32 sample, u32 layer) {
        return std::array<float,4>{float((sample+layer)%4==0 || sample==3),
            float((sample+layer)%4==1 || sample==3), float((sample+layer)%4==2 || sample==3),1};
    };
    const auto memory_type = [&](u32 bits, vk::MemoryPropertyFlags flags) {
        const auto props = instance.GetMemoryProperties();
        for (u32 i = 0; i < props.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
        std::abort();
    };
    struct Buffer { vk::UniqueDeviceMemory memory; vk::UniqueBuffer buffer; void* mapped; };
    const auto make_buffer = [&](u32 bytes) {
        Buffer b;
        b.buffer = Vulkan::Check(device.createBufferUnique({.size = bytes,
            .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst}));
        const auto req = device.getBufferMemoryRequirements(*b.buffer);
        b.memory = Vulkan::Check(device.allocateMemoryUnique({.allocationSize = req.size,
            .memoryTypeIndex = memory_type(req.memoryTypeBits,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)}));
        Vulkan::Check(device.bindBufferMemory(*b.buffer, *b.memory, 0));
        b.mapped = Vulkan::Check(device.mapMemory(*b.memory, 0, VK_WHOLE_SIZE));
        return b;
    };
    auto output = make_buffer(W * H * Layers * Words * 4), flat = make_buffer(32);
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
    unsigned checks{}, failures{};
    const auto check = [&](bool ok) { ++checks; if (!ok) ++failures; };
    // Regression: optional storage usage on Adreno reduces the queried mask to
    // 1x, despite render/sample images supporting 2x and 4x. Check the actual
    // VkImage rather than only uniform clear colors (which hid this mismatch).
    for (const auto format : {vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb}) {
        const auto caps = instance.GetPhysicalDevice().getImageFormatProperties2({
            .format=format, .type=vk::ImageType::e2D, .tiling=vk::ImageTiling::eOptimal,
            .usage=vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            .flags=vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage});
        check(caps.result == vk::Result::eSuccess);
        if (caps.result != vk::Result::eSuccess) continue;
        for (u32 requested : {2u,4u}) {
            if (!(u32(caps.value.imageFormatProperties.sampleCounts) & requested)) continue;
            VideoCore::ImageInfo info{};
            info.type=AmdGpu::ImageType::Color2D; info.pixel_format=format;
            info.size={128,128,1}; info.resources={1,1}; info.num_samples=requested;
            info.num_bits=32; info.pitch=128; info.guest_size=128*128*4*requested;
            VideoCore::Image direct(instance,scheduler,blit,views,info);
            check(u32(direct.backing->image.image_ci.samples) == instance.HostSamples(requested));
            info.num_samples=1; info.guest_size=128*128*4;
            VideoCore::Image promoted(instance,scheduler,blit,views,info);
            for (u32 samples : {requested,1u,requested}) {
                promoted.SetBackingSamples(samples, false);
                check(u32(promoted.backing->image.image_ci.samples) == instance.HostSamples(samples));
                VideoCore::ImageViewInfo vi{};
                vi.type=AmdGpu::ImageType::Color2D; vi.format=format;
                vi.range.extent={1,1};
                check(bool(promoted.FindView(vi,false).image_view));
                if (samples == 1)
                    check(bool(promoted.backing->image.image_ci.usage & vk::ImageUsageFlagBits::eStorage));
            }
        }
    }
    for (const u32 requested : {2u, 4u}) for (const bool array : {false, true}) {
        const u32 layers = array ? Layers : 1;
        VideoCore::ImageInfo info{};
        info.type=AmdGpu::ImageType::Color2D;
        info.pixel_format=vk::Format::eR8G8B8A8Unorm;
        info.size={W,H,1}; info.resources={1,layers}; info.num_samples=requested;
        info.num_bits=32; info.pitch=W; info.guest_size=W*H*layers*4*requested;
        VideoCore::Image owned_image(instance,scheduler,blit,views,info);
        check(u32(owned_image.backing->image.image_ci.samples) == instance.HostSamples(info.num_samples));
        const u32 samples=owned_image.backing->num_samples;
        const auto image=owned_image.GetImage();
        VideoCore::ImageViewInfo vi{};
        vi.type=array ? AmdGpu::ImageType::Color2DMsaaArray : AmdGpu::ImageType::Color2DMsaa;
        vi.format=info.pixel_format; vi.range.extent=info.resources;
        const auto view=owned_image.FindView(vi).image_view.get();
        owned_image.Transit(vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits2::eColorAttachmentWrite, {});
        auto depth_info=info;
        depth_info.pixel_format=vk::Format::eD32Sfloat;
        depth_info.props.is_depth=true;
        VideoCore::Image depth(instance,scheduler,blit,views,depth_info);
        check(u32(depth.backing->image.image_ci.samples)==samples);
        depth.Transit(vk::ImageLayout::eDepthStencilAttachmentOptimal,
            vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {});
        std::vector<vk::UniqueImageView> depth_views;
        std::vector<vk::UniquePipeline> pipelines;
        for(u32 sample=0;sample<samples;++sample) {
            const std::array stages{
                vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eVertex,.module=vs,.pName="main"},
                vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eFragment,.module=fs,.pName="main"}};
            const vk::PipelineVertexInputStateCreateInfo vertex{};
            const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology=vk::PrimitiveTopology::eTriangleList};
            const vk::Viewport viewport{0,0,float(W),float(H),0,1};
            const vk::Rect2D scissor{{0,0},{W,H}};
            const vk::PipelineViewportStateCreateInfo viewport_state{.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
            const vk::PipelineRasterizationStateCreateInfo raster{.lineWidth=1};
            const u32 mask=1u<<sample;
            const vk::PipelineMultisampleStateCreateInfo multisample{.rasterizationSamples=vk::SampleCountFlagBits(samples),.pSampleMask=&mask};
            const vk::PipelineDepthStencilStateCreateInfo depth_state{.depthTestEnable=true,.depthWriteEnable=true,.depthCompareOp=vk::CompareOp::eAlways};
            const vk::PipelineColorBlendAttachmentState blend{.colorWriteMask=vk::ColorComponentFlags(15)};
            const vk::PipelineColorBlendStateCreateInfo blending{.attachmentCount=1,.pAttachments=&blend};
            const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount=1,.pColorAttachmentFormats=&info.pixel_format,.depthAttachmentFormat=depth_info.pixel_format};
            pipelines.push_back(Vulkan::Check(device.createGraphicsPipelineUnique({}, {
                .pNext=&rendering,.stageCount=2,.pStages=stages.data(),.pVertexInputState=&vertex,
                .pInputAssemblyState=&assembly,.pViewportState=&viewport_state,.pRasterizationState=&raster,
                .pMultisampleState=&multisample,.pDepthStencilState=&depth_state,.pColorBlendState=&blending,.layout=*render_layout})));
        }
        std::vector<vk::UniqueImageView> layer_views;
        for (u32 layer = 0; layer < layers; ++layer) {
            layer_views.push_back(Vulkan::Check(device.createImageViewUnique({.image = image,
                .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8G8B8A8Unorm,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,layer,1}})));
            depth_views.push_back(Vulkan::Check(device.createImageViewUnique({.image=depth.GetImage(),
                .viewType=vk::ImageViewType::e2D,.format=depth_info.pixel_format,
                .subresourceRange={vk::ImageAspectFlagBits::eDepth,0,1,layer,1}})));
            vk::RenderingAttachmentInfo depth_attachment{.imageView=*depth_views.back(),
                .imageLayout=vk::ImageLayout::eDepthStencilAttachmentOptimal,
                .loadOp=vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore,
                .clearValue={.depthStencil={1,0}}};
            vk::RenderingAttachmentInfo color{.imageView = *layer_views.back(),
                .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .loadOp = vk::AttachmentLoadOp::eClear, .storeOp = vk::AttachmentStoreOp::eStore,
                .clearValue = vk::ClearColorValue(std::array<float,4>{float(layer==0),float(layer==1),0,1})};
            auto cmd = scheduler.CommandBuffer();
            cmd.beginRendering({.renderArea={{0,0},{W,H}},.layerCount=1,
                .colorAttachmentCount=1,.pColorAttachments=&color,.pDepthAttachment=&depth_attachment});
            for(u32 sample=0;sample<samples;++sample) {
                cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,*pipelines[sample]);
                const auto c=color_for(sample,layer);
                cmd.pushConstants(*render_layout,vk::ShaderStageFlagBits::eFragment,0,16,c.data());
                cmd.draw(3,1,0,0);
            }
            cmd.endRendering();
        }
        owned_image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {});
        auto sharp = AmdGpu::Image::Null(false);
        sharp.type = u64(array ? AmdGpu::ImageType::Color2DMsaaArray : AmdGpu::ImageType::Color2DMsaa);
        sharp.width=W-1; sharp.height=H-1; sharp.pitch=W-1;
        sharp.last_level=std::countr_zero(requested); sharp.last_array=layers-1; sharp.depth=layers-1;
        std::memcpy(flat.mapped,&sharp,32);
        for (const u32 sample : {0u,3u,6u}) for (const bool has_mips : {false,true}) {
            const auto code = ComputeShader(sharp,array,sample,has_mips,disable_msaa);
            const auto name = std::string(argv[2])+"/msaa-"+std::to_string(disable_msaa)+"-"+std::to_string(requested)+"-"+std::to_string(array)+"-"+
                std::to_string(sample)+"-"+std::to_string(has_mips)+".spv";
            std::ofstream file(name,std::ios::binary);
            file.write(reinterpret_cast<const char*>(code.data()),code.size()*4);
            auto module=Vulkan::Check(device.createShaderModuleUnique({.codeSize=code.size()*4,.pCode=code.data()}));
            auto pipeline=Vulkan::Check(device.createComputePipelineUnique({}, {
                .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*module,.pName="main"},.layout=*layout}));
            std::array buffers{vk::DescriptorBufferInfo{*output.buffer,0,VK_WHOLE_SIZE},
                vk::DescriptorBufferInfo{*flat.buffer,0,32}};
            vk::DescriptorImageInfo texture{{},view,vk::ImageLayout::eShaderReadOnlyOptimal};
            std::array writes{
                vk::WriteDescriptorSet{.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffers[0]},
                vk::WriteDescriptorSet{.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffers[1]},
                vk::WriteDescriptorSet{.dstBinding=2,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&texture}};
            auto cmd=scheduler.CommandBuffer();
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*layout,0,writes);
            PushData push{};
            cmd.pushConstants(*layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(push),&push);
            cmd.dispatch(W,H,layers);
            vk::MemoryBarrier2 done{.srcStageMask=vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask=vk::AccessFlagBits2::eShaderWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask=vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&done});
            scheduler.Finish();
            auto* result=static_cast<u32*>(output.mapped);
            for(u32 l=0;l<layers;++l) for(u32 i=0;i<W*H;++i) {
                auto* p=result+(l*W*H+i)*Words;
                check(p[0]==W && p[1]==H && p[2]==(array?layers:0) && p[3]==u32(has_mips));
                for(u32 c=0;c<4;++c) check(std::bit_cast<float>(p[4+c])==color_for(sample%samples,l)[c]);
            }
        }

        // Exercise the production resolve path: multisample resolve normally,
        // single-sample copy when forced off, with exact per-layer readback.
        auto resolved_info=info; resolved_info.num_samples=1;
        VideoCore::Image resolved(instance,scheduler,blit,views,resolved_info);
        VideoCore::SubresourceRange all{}; all.extent={1,layers};
        resolved.Resolve(owned_image,all,all);
        resolved.Transit(vk::ImageLayout::eTransferSrcOptimal,vk::AccessFlagBits2::eTransferRead,{});
        const vk::BufferImageCopy region{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,layers},.imageExtent={W,H,1}};
        auto cmd=scheduler.CommandBuffer();
        cmd.copyImageToBuffer(resolved.GetImage(),vk::ImageLayout::eTransferSrcOptimal,*output.buffer,region);
        const vk::MemoryBarrier2 host_barrier{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host_barrier});
        scheduler.Finish();
        auto* bytes=static_cast<u8*>(output.mapped);
        for(u32 l=0;l<layers;++l) for(u32 i=0;i<W*H;++i) for(u32 c=0;c<4;++c) {
            float expected=0;
            for(u32 sample=0;sample<samples;++sample) expected+=color_for(sample,l)[c]*255/samples;
            check(std::abs(float(bytes[(l*W*H+i)*4+c])-expected)<=1);
        }
    }
    device.destroyShaderModule(vs); device.destroyShaderModule(fs);
    device.unmapMemory(*output.memory); device.unmapMemory(*flat.memory);
    printf("msaa_image_probe (%s): %u checks / %u failures\n",disable_msaa?"off":"game",checks,failures);
    return failures?1:0;
}
