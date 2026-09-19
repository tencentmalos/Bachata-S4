// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;
layout (binding = 1) uniform sampler2D rightSampler;
layout (binding = 2) uniform sampler2D overlayLeftSampler;
layout (binding = 3) uniform sampler2D overlayRightSampler;

layout (push_constant) uniform settings {
    float gamma;
    uint hdr;
    uint srgb_input;
    uint sbs;
    uint flip_y;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

void main() {
    // Mode 1 mirrors a flat image. Modes 2/3 consume distinct guest eyes in
    // the existing presentation pass, without PSVR warp or CPU readback.
    vec2 sample_uv = uv;
    if (pp.flip_y != 0u) sample_uv.y = 1.0 - sample_uv.y;
    if (pp.sbs != 0u) sample_uv.x = fract(uv.x * 2.0);
    vec4 color_linear = pp.sbs >= 2u && uv.x >= 0.5
        ? texture(rightSampler, sample_uv) : texture(texSampler, sample_uv);
    if (pp.sbs == 3u) {
        vec4 overlay = uv.x >= 0.5 ? texture(overlayRightSampler, sample_uv)
                                   : texture(overlayLeftSampler, sample_uv);
        color_linear = vec4(overlay.rgb + color_linear.rgb * (1.0 - overlay.a), 1.0);
    }
    if (pp.hdr != 0u) {
        color = color_linear;
    } else {
        if (pp.srgb_input != 0u) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
