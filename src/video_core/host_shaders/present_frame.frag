// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

// The game picture into its rectangle of the window image. The viewport covers exactly that
// rectangle, so uv spans the picture. A pre-rotated window image (VkSurfaceTransformFlagBitsKHR)
// stores the picture turned clockwise by `rotation` quarter turns; uv addresses the stored
// rectangle and is mapped back to the picture's own orientation.
//
// `encode` set: the input is linear (a guest image through an sRGB view, or a reconstruction)
// and gets the post-process transfer to the window's UNORM format, with the user gamma. This is
// the whole post-process of a 2D frame, so the guest image reaches the window in one write.
// Clear: the input already holds window values (a composed frame, the splash) and is copied.

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D picture;

layout (push_constant) uniform settings {
    uint rotation;
    uint encode;
    // sRGB-encoded samples through a UNORM view (Vulkan has no sRGB 10-bit format): decode first.
    uint srgb_input;
    float gamma;
} pc;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;

// Same transfer as post_process.frag.
vec3 encode_srgb(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pc.gamma))) - b,
        d * rgb / pc.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

vec3 decode_srgb(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

void main() {
    vec2 p = uv;
    if (pc.rotation == 1u) p = vec2(uv.y, 1.0 - uv.x);
    else if (pc.rotation == 2u) p = vec2(1.0 - uv.x, 1.0 - uv.y);
    else if (pc.rotation == 3u) p = vec2(1.0 - uv.y, uv.x);
    vec3 rgb = texture(picture, p).rgb;
    if (pc.encode != 0u) {
        if (pc.srgb_input != 0u) rgb = decode_srgb(rgb);
        rgb = encode_srgb(rgb);
    }
    color = vec4(rgb, 1.0);
}
