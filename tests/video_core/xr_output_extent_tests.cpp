// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "video_core/renderer_vulkan/openxr/output_extent.h"
using namespace Vulkan::OpenXr;
int main() {
    const EyeExtent expected[]{{2592, 2400}, {3376, 2976}, {4160, 3552}};
    unsigned checks = 0;
    for (unsigned tier = 0; tier < 3; ++tier) {
        const auto r = ChooseOutputExtent({2592, 2400}, {4160, 3552}, tier);
        assert(r.width == expected[tier].width && r.height == expected[tier].height);
        ++checks;
    }
    for (auto limit : {EyeExtent{4096, 4096}, EyeExtent{2048, 4096}, EyeExtent{4000, 2200}, EyeExtent{1, 1}}) {
        auto previous = ChooseOutputExtent({2592, 2400}, limit, 0);
        for (unsigned tier = 0; tier < 3; ++tier) {
            const auto r = ChooseOutputExtent({2592, 2400}, limit, tier);
            assert(r.width && r.height && r.width <= limit.width && r.height <= limit.height);
            assert(r.width >= previous.width && r.height >= previous.height);
            if (tier == 2) assert(r.width == limit.width && r.height == limit.height);
            previous = r;
            checks += 3;
        }
    }
    assert(ChooseOutputExtent({0, 2400}, {4096, 4096}, 1).width == 0);
    assert(ChooseOutputExtent({2592, 2400}, {4096, 0}, 2).width == 0);
    assert(ChooseOutputExtent({2592, 2400}, {4096, 4096}, 3).width == 0);
    printf("XR output extent: %u boundary checks passed\n", checks + 3);
}
