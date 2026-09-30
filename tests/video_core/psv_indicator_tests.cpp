// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cmath>
#include "video_core/renderer_vulkan/openxr/psv_indicator.h"
int main() {
    using namespace Vulkan::OpenXr;
    for(double time : {0., .1, .3, 10., 3600.}) {
        const auto running=IndicatorLight(PsvIndicator::Running,time);
        const auto charging=IndicatorLight(PsvIndicator::Charging,time);
        assert(running.brightness==1 && running.colour[2]>running.colour[0]);
        assert(charging.brightness==1 && charging.colour[0]>charging.colour[2]);
        assert(IndicatorLight(PsvIndicator::Off,time).brightness==0);
    }
    for(auto state : {PsvIndicator::Standby,PsvIndicator::ChargeLow}) {
        assert(IndicatorLight(state,.1).brightness==1);
        assert(IndicatorLight(state,.3).brightness==0);
        assert(IndicatorLight(state,.5).brightness==1);
    }
    assert(IndicatorLight(PsvIndicator::Notification,0).brightness==0);
    assert(IndicatorLight(PsvIndicator::Notification,1.2).brightness>.999f);
    assert(IndicatorLight(PsvIndicator::Notification,2.4).brightness<.001f);
    for(int state=0;state<6;++state) for(int i=0;i<10000;++i) {
        const auto light=IndicatorLight(static_cast<PsvIndicator>(state),i*.007);
        assert(std::isfinite(light.brightness) && light.brightness>=0 && light.brightness<=1);
    }
}
