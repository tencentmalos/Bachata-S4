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
    layout(offset = 32) vec4 eye_uv[4];
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
    // Base and overlay can independently use separate views or a shared image.
    // Preserve a shared image's UVs, rather than duplicating its full width into
    // each half. Array eyes arrive as separate single-layer 2D image views.
    vec2 sample_uv = uv;
    if (pp.flip_y != 0u) sample_uv.y = 1.0 - sample_uv.y;
    vec2 eye_uv = vec2(fract(sample_uv.x * 2.0), sample_uv.y);
    bool right_eye = uv.x >= 0.5;
    bool explicit_uv = (pp.sbs & 16u) != 0u;
    uint eye = right_eye ? 1u : 0u;
    vec4 color_linear;
    if (explicit_uv) {
        vec2 base_uv = eye_uv * pp.eye_uv[eye].xy + pp.eye_uv[eye].zw;
        color_linear = right_eye ? texture(rightSampler, base_uv) : texture(texSampler, base_uv);
    } else {
        bool split_base = (pp.sbs & 1u) != 0u;
        color_linear = split_base && right_eye
            ? texture(rightSampler, eye_uv)
            : texture(texSampler, split_base ? eye_uv : sample_uv);
    }
    if ((pp.sbs & 2u) != 0u) {
        vec4 overlay;
        if (explicit_uv) {
            vec2 overlay_uv = eye_uv * pp.eye_uv[eye + 2u].xy + pp.eye_uv[eye + 2u].zw;
            overlay = right_eye ? texture(overlayRightSampler, overlay_uv)
                                : texture(overlayLeftSampler, overlay_uv);
        } else {
            bool split_overlay = (pp.sbs & 4u) != 0u;
            overlay = split_overlay && right_eye
                ? texture(overlayRightSampler, eye_uv)
                : texture(overlayLeftSampler, split_overlay ? eye_uv : sample_uv);
        }
        color_linear = vec4(overlay.rgb + color_linear.rgb * (1.0 - overlay.a), 1.0);
    }
    // VR scene alpha belongs to the guest's intermediate rendering, not the
    // Android UI. Keep overlay blending above, then present an opaque frame.
    if ((pp.sbs & 8u) != 0u) color_linear.a = 1.0;
    if (pp.hdr != 0u) {
        color = color_linear;
    } else {
        if (pp.srgb_input != 0u) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
