// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "video_core/renderer_vulkan/openxr/status_projection.h"

namespace p = Vulkan::OpenXr::StatusProjection;
namespace m = spatial::xr::math;

static bool Near(float a, float b) { return std::abs(a-b)<.0001f; }
static void Same(const XrVector3f& a, const XrVector3f& b) {
    assert(Near(a.x,b.x) && Near(a.y,b.y) && Near(a.z,b.z));
}
int main() {
    unsigned checks=0;
    // Bounds enclose the scaled, tilted model; production uses actual mesh bounds.
    const XrVector3f lo{-.36f,-.60f,-1.23f},hi{.36f,-.24f,-1.03f};
    for(float yaw:{-2.8f,-.8f,0.f,1.3f,3.f})
    for(float pitch:{-1.1f,-.4f,0.f,.25f,.5f})
    for(float roll:{-.5f,0.f,.5f})
    for(float ipd:{.05f,.064f,.078f}) {
        const XrPosef head{m::Multiply(m::QuaternionFromAxisAngle({0,1,0},yaw),
            m::Multiply(m::QuaternionFromAxisAngle({1,0,0},pitch),
                        m::QuaternionFromAxisAngle({0,0,1},roll))),{3,1.7f,-4}};
        std::array<XrView,2> eyes{XrView{XR_TYPE_VIEW},XrView{XR_TYPE_VIEW}};
        for(unsigned e=0;e<2;++e) {
            const float side=e?1.f:-1.f;
            eyes[e].pose=p::ToWorld(head,{m::QuaternionFromAxisAngle({0,1,0},-side*.09f),{side*ipd*.5f,0,0}});
            eyes[e].fov={-1.5f,1.5f,1.5f,-1.5f};
        }
        float heading=0;
        const auto anchor=p::Anchor(eyes,heading);
        assert(Near(heading,yaw)); // no left-eye cant bias
        Same(anchor.position,head.position);
        const auto full=p::LocalViews(eyes,anchor);
        auto crop=full;
        Same(p::ToWorld(anchor,{m::IdentityQuaternion(),{0,-.42f,-1.15f}}).position,
             m::Transform({m::QuaternionFromAxisAngle({0,1,0},yaw),head.position},{0,-.42f,-1.15f}));
        for(unsigned e=0;e<2;++e) {
            Same(p::ToWorld(anchor,crop[e].pose).position,eyes[e].pose.position);
            Same(m::Rotate(p::ToWorld(anchor,full[e].pose).orientation,{0,0,-1}),
                 m::Rotate(eyes[e].pose.orientation,{0,0,-1}));
            assert(p::Crop(lo,hi,crop[e]));
            for(unsigned c=0;c<8;++c) {
                const XrVector3f corner{c&1?hi.x:lo.x,c&2?hi.y:lo.y,c&4?hi.z:lo.z};
                const auto eye=m::Transform(m::Inverse(crop[e].pose),corner);
                const float x=eye.x/-eye.z,y=eye.y/-eye.z;
                const auto& f=crop[e].fov;
                assert(Near(f.angleLeft,-f.angleRight) && Near(f.angleUp,-f.angleDown));
                assert(x>std::tan(f.angleLeft) && x<std::tan(f.angleRight));
                assert(y>std::tan(f.angleDown) && y<std::tan(f.angleUp));
                // Same point, same rays after moving panel-space back to LOCAL.
                Same(eye,m::Transform(m::Inverse(p::ToWorld(anchor,crop[e].pose)),
                                     m::Transform(anchor,corner)));
                ++checks;
            }
        }
        assert(Near(m::EyeSeparation(crop[0].pose.position,crop[1].pose.position),ipd));
    }
    std::array<XrView,2> vertical{XrView{XR_TYPE_VIEW},XrView{XR_TYPE_VIEW}};
    for(auto& eye:vertical)eye.pose={m::QuaternionFromAxisAngle({1,0,0},m::Pi*.5f),{0,0,0}};
    float heading=.6f; (void)p::Anchor(vertical,heading); assert(Near(heading,.6f));
    XrView invalid{XR_TYPE_VIEW}; invalid.pose=m::IdentityPose();
    assert(!p::Crop({-1,-1,-1},{1,1,1},invalid));
    assert(!p::Crop({0,0,-1},{0,0,-1},invalid));
    XrView clipped{XR_TYPE_VIEW}; clipped.pose=m::IdentityPose();
    clipped.fov={-.5f,.5f,.5f,-.5f};
    assert(p::Crop({-.9f,-.9f,-1.f},{.9f,.9f,-.8f},clipped));
    assert(Near(clipped.fov.angleLeft,-.5f) && Near(clipped.fov.angleRight,.5f));
    assert(Near(clipped.fov.angleUp,.5f) && Near(clipped.fov.angleDown,-.5f));
    // A captured anchor never moves with later head motion. The exact same
    // world point must project correctly after camera translation and rotation.
    const XrPosef fixed{m::QuaternionFromAxisAngle({0,1,0},.4f),{1,1.6f,2}};
    const auto point=m::Transform(fixed,{0,-.42f,-1.15f});
    for(float motion:{-.3f,0.f,.3f}) {
        std::array<XrView,2> eyes{XrView{XR_TYPE_VIEW},XrView{XR_TYPE_VIEW}};
        for(unsigned e=0;e<2;++e)eyes[e].pose={m::QuaternionFromAxisAngle({0,1,0},motion),{motion+(e?.032f:-.032f),1.7f,2}};
        const auto local=p::LocalViews(eyes,fixed);
        for(unsigned e=0;e<2;++e)
            Same(m::Transform(m::Inverse(local[e].pose),{0,-.42f,-1.15f}),
                 m::Transform(m::Inverse(eyes[e].pose),point));
    }
    printf("PASS: %u corner/ray checks, canted eyes, 50/64/78 mm IPD, yaw/pitch/roll, vertical fallback, invalid bounds\n",checks);
}
