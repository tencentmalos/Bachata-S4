// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Nearest-texel copy of a depth (and stencil) subresource into a depth attachment of
// another size, for depth formats the driver cannot blit into. The texel choice is the
// blit rule floor((x + 0.5) * src / dst), in integers so ratios like 2:1 are exact.

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require
#if defined(STENCIL)
#extension GL_ARB_shader_stencil_export : require
#endif

layout (binding = 0, set = 0) uniform texture2D depth_in;
#if defined(STENCIL)
layout (binding = 1, set = 0) uniform utexture2D stencil_in;
#endif

layout (push_constant) uniform Extent {
    ivec2 dst_size;
};

void main()
{
    const ivec2 dst = ivec2(gl_FragCoord.xy);
    const ivec2 src_size = textureSize(depth_in, 0);
    const ivec2 coord = min(((2 * dst + 1) * src_size) / (2 * dst_size), src_size - 1);
    gl_FragDepth = texelFetch(depth_in, coord, 0).r;
#if defined(STENCIL)
    gl_FragStencilRefARB = int(texelFetch(stencil_in, coord, 0).r);
#endif
}
