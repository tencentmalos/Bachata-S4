// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "video_core/amdgpu/depth_range.h"
using namespace AmdGpu;
struct Registers {
    ViewportControl viewport_control{};
    ClipperControl clipper_control{};
    std::array<ViewportBounds, NUM_VIEWPORTS> viewports{};
    std::array<ViewportDepth, NUM_VIEWPORTS> viewport_depths{};
    struct {
        bool disable_viewport_clamp{};
    } depth_render_override;
    bool IsClipDisabled() const {
        return clipper_control.clip_disable;
    }
};
int main() {
    unsigned count = 0, failed = 0;
    auto check = [&](bool ok) {
        ++count;
        if (!ok)
            ++failed;
    };
    Registers regs{};
    regs.viewports[0] = {1, 0, 1, 0, 1, 0};
    regs.viewport_depths[0] = {0, 1};
    regs.viewport_control.zscale_enable = regs.viewport_control.zoffset_enable = 1;
    check(BuildDepthRangeEmulation(regs, false).enabled);
    check(!BuildDepthRangeEmulation(regs, true).enabled);
    regs.viewports[0].zscale = .5f;
    regs.viewports[0].zoffset = .5f;
    check(!BuildDepthRangeEmulation(regs, false).enabled);
    regs.viewports[0].zscale = 1.f;
    regs.viewports[0].zoffset = 0.f;
    regs.clipper_control.clip_space = ClipSpace::ZeroToW;
    check(!BuildDepthRangeEmulation(regs, false).enabled);
    regs.viewports[1] = {1, 0, 1, 0, -1, 0};
    regs.viewport_depths[1] = {.2f, .8f};
    auto two = BuildDepthRangeEmulation(regs, false);
    check(two.enabled && two.viewports[1].scale < 0.f && two.clip_near && two.clip_far);
    regs.clipper_control.zclip_near_disable = 1;
    check(!BuildDepthRangeEmulation(regs, false).clip_near &&
          BuildDepthRangeEmulation(regs, false).clip_far);
    regs.clipper_control.clip_disable = 1;
    check(!BuildDepthRangeEmulation(regs, false).clip_far);
    auto same = MakeDepthRangeTransform(1, 0, .3f, .3f);
    check(same.scale == 0 && std::isfinite(same.offset));
    // Independent hardware affine evaluation must recover the guest result for
    // every nondegenerate range, including reversed scale and clamp endpoints.
    for (float scale : {-2.f, -1.f, .5f, 1.f, 2.f})
        for (float offset : {-.5f, 0.f, .5f, 1.f})
            for (auto bounds : {std::array{0.f, 1.f}, std::array{.2f, .8f}, std::array{.8f, .2f}})
                for (int z = -40; z <= 40; ++z)
                    for (float w : {.5f, 1.f, 10.f}) {
                        auto t = MakeDepthRangeTransform(scale, offset, bounds[0], bounds[1]);
                        const float guest = scale * (z / 20.f) / w + offset;
                        const float host = (t.scale * (z / 20.f) + t.offset * w) / w *
                                               (t.max_depth - t.min_depth) +
                                           t.min_depth;
                        check(std::abs(guest - host) < 2e-6f);
                    }
    printf("DEPTH_RANGE %u checks / %u failures\n", count, failed);
    return failed ? 1 : 0;
}
