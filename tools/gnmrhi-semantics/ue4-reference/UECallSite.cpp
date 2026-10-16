// Illustration using the real UE command-list interface. Not a recovered KH3 pass.
#include "RHICommandList.h"

void RecordGnmReferenceCompute(FRHICommandList& RHICmdList,
    FComputeShaderRHIParamRef Shader, FUniformBufferRHIParamRef UniformBuffer,
    uint32 UniformSlot, uint32 LooseByteOffset, const void* LooseBytes, uint32 NumLooseBytes,
    uint32 GroupsX, uint32 GroupsY, uint32 GroupsZ)
{
    // Shader/reflection and UB layout must agree. Slot 0 may be the shader's
    // loose-constant slot; callers must select the actual reflected UB slot.
    RHICmdList.SetComputeShader(Shader);
    RHICmdList.SetShaderUniformBuffer(Shader, UniformSlot, UniformBuffer);
    RHICmdList.SetShaderParameter(Shader, 0, LooseByteOffset, NumLooseBytes, LooseBytes);
    RHICmdList.DispatchComputeShader(GroupsX, GroupsY, GroupsZ);

    // No submit/flush inserted here. The real renderer also supplies transitions,
    // direct texture/UAV bindings and shader-specific parameters as needed.
}

// The caller supplies a working pipeline/BSS factory and render-target/fixed
// state. The reference context remains abstract; this does not register an RHI.
PRAGMA_DISABLE_DEPRECATION_WARNINGS
void RecordGnmReferenceGraphics(FRHICommandList& RHICmdList,
    FGraphicsPipelineStateRHIParamRef Pipeline, FVertexShaderRHIParamRef VS,
    FPixelShaderRHIParamRef PS, FVertexBufferRHIParamRef Vertices,
    FIndexBufferRHIParamRef Indices, uint32 Stride, uint32 VertexOffset,
    FUniformBufferRHIParamRef VertexUniform, uint32 UniformSlot,
    FTextureRHIParamRef Texture, FSamplerStateRHIParamRef Sampler,
    uint32 TextureSlot, uint32 SamplerSlot, uint32 FirstInstance,
    uint32 NumVertices, uint32 NumTriangles, uint32 NumInstances)
{
    // UE's fallback links pipeline -> CreateBoundShaderState -> SetBoundShaderState.
    // KH3's correlated wrapper also has a stencil-ref argument; its ABI is not
    // the same as this public one-parameter SetGraphicsPipelineState interface.
    RHICmdList.SetGraphicsPipelineState(Pipeline);
    // KH3 form; deprecated in the 4.18 headers used for signature comparison.
    RHICmdList.SetStreamSource(0, Vertices, Stride, VertexOffset);
    RHICmdList.SetShaderUniformBuffer(VS, UniformSlot, VertexUniform);
    RHICmdList.SetShaderTexture(PS, TextureSlot, Texture);
    RHICmdList.SetShaderSampler(PS, SamplerSlot, Sampler);
    RHICmdList.DrawIndexedPrimitive(Indices, PT_TriangleList, 0, FirstInstance,
                                    NumVertices, 0, NumTriangles, NumInstances);
}
PRAGMA_ENABLE_DEPRECATION_WARNINGS

// Pipeline must contain a decoded GS + embedded copyVS and the VS alternate ES
// variant. UE continues to address its logical VS and GS; it never binds the
// copy shader as an additional FRHIVertexShader from renderer code.
void RecordGnmReferenceGeometry(FRHICommandList& RHICmdList,
    FGraphicsPipelineStateRHIParamRef Pipeline, FVertexShaderRHIParamRef VS,
    FGeometryShaderRHIParamRef GS, FUniformBufferRHIParamRef VertexUniform,
    FUniformBufferRHIParamRef GeometryUniform, uint32 VertexSlot, uint32 GeometrySlot,
    FTextureRHIParamRef GeometryTexture, uint32 TextureSlot, uint32 NumTriangles)
{
    // Caller has set stream, PS resources, targets and fixed state.
    RHICmdList.SetGraphicsPipelineState(Pipeline);
    RHICmdList.SetShaderUniformBuffer(VS, VertexSlot, VertexUniform); // hardware ES=4
    RHICmdList.SetShaderUniformBuffer(GS, GeometrySlot, GeometryUniform); // GS=3
    RHICmdList.SetShaderTexture(GS, TextureSlot, GeometryTexture);
    RHICmdList.DrawPrimitive(PT_TriangleList, 0, NumTriangles, 1);
}
