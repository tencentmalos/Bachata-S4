// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include "spatial/xr/XrMath.h"

namespace Vulkan::OpenXr::StatusProjection {
namespace xm = spatial::xr::math;

// A gravity-aligned panel frame captured at creation / explicit recenter only.
// It stays in LOCAL space when the viewer turns or moves.
// Average both forwards: the runtime may cant the individual eyes outwards.
inline XrPosef Anchor(const std::array<XrView, 2>& eyes, float& last_yaw) {
    const auto f = xm::Add(xm::Rotate(eyes[0].pose.orientation, {0, 0, -1}),
                           xm::Rotate(eyes[1].pose.orientation, {0, 0, -1}));
    if (f.x * f.x + f.z * f.z > 0.0001f)
        last_yaw = std::atan2(-f.x, -f.z);
    return {xm::QuaternionFromAxisAngle({0, 1, 0}, last_yaw),
            xm::Scale(xm::Add(eyes[0].pose.position, eyes[1].pose.position), .5f)};
}

inline XrPosef ToWorld(const XrPosef& anchor, const XrPosef& local) {
    return {xm::Multiply(anchor.orientation, local.orientation),
            xm::Transform(anchor, local.position)};
}

// All eight corners of the model's scene-space AABB, including shoulder caps
// and controls. Padding in tangent space keeps filtering away from image edges.
inline bool Crop(const XrVector3f& minimum, const XrVector3f& maximum,
                 XrView& view) {
    float left = std::numeric_limits<float>::infinity(), right = -left;
    float down = left, up = -left;
    const auto inverse = xm::Inverse(view.pose);
    for (unsigned i = 0; i < 8; ++i) {
        const auto v = xm::Transform(inverse, {i & 1 ? maximum.x : minimum.x,
                                               i & 2 ? maximum.y : minimum.y,
                                               i & 4 ? maximum.z : minimum.z});
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
            v.z >= -.05f)
            return false;
        left = std::min(left, v.x / -v.z); right = std::max(right, v.x / -v.z);
        down = std::min(down, v.y / -v.z); up = std::max(up, v.y / -v.z);
    }
    const float px = (right - left) * .06f, py = (up - down) * .06f;
    if (px <= 0 || py <= 0) return false;
    // Swan's compositor shifts the scene with off-centre FOVs (including
    // crops shared by both eyes). A centred crop keeps full/cropped projection
    // coincident on device. Keep the runtime eye pose and narrow symmetrically
    // about its optical axis; never enlarge beyond the original located FOV.
    const float x = std::atan(std::max(std::abs(left-px), std::abs(right+px)));
    const float y = std::atan(std::max(std::abs(down-py), std::abs(up+py)));
    view.fov = {std::max(view.fov.angleLeft, -x), std::min(view.fov.angleRight, x),
                std::min(view.fov.angleUp, y), std::max(view.fov.angleDown, -y)};
    return true;
}

inline std::array<XrView, 2> LocalViews(const std::array<XrView, 2>& eyes,
                                       const XrPosef& anchor) {
    auto views = eyes;
    const auto inverse = xm::Inverse(anchor);
    for (auto& view : views) {
        view.pose = ToWorld(inverse, view.pose);
        // Keep the runtime's full eye pose, including cant and head roll.
        // Only FOV changes; compositor reprojection gets the original pose.
    }
    return views;
}
} // namespace Vulkan::OpenXr::StatusProjection
