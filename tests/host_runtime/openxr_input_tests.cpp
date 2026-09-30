// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include "core/host_runtime/openxr_pad_mapping.h"
#include "spatial/xr/XrInputSystem.h"
using namespace spatial::xr;
namespace {
unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)
uintptr_t next_id{10};
std::map<XrAction, std::string> actions;
std::map<XrSpace, std::pair<std::string, unsigned>> spaces;
std::map<std::string, XrPath> paths;
bool focused{true};
bool hand_active[2]{true, true};
unsigned created{}, destroyed{}, stopped{}, haptic_hand{99}, attached{};
float last_amplitude{};
unsigned HandOf(XrPath path) {
    return path == paths["/user/hand/left"] ? 0 : 1;
}
XrResult XRAPI_PTR MakeSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* out) {
    *out = reinterpret_cast<XrActionSet>(++next_id);
    return XR_SUCCESS;
}
XrResult XRAPI_PTR DropSet(XrActionSet) {
    return XR_SUCCESS;
}
XrResult XRAPI_PTR MakeAction(XrActionSet, const XrActionCreateInfo* info, XrAction* out) {
    *out = reinterpret_cast<XrAction>(++next_id);
    actions[*out] = info->actionName;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR DropAction(XrAction action) {
    actions.erase(action);
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Path(XrInstance, const char* text, XrPath* out) {
    auto [it, inserted] = paths.try_emplace(text, ++next_id);
    *out = it->second;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Suggest(XrInstance, const XrInteractionProfileSuggestedBinding* info) {
    unsigned grip{}, trigger{}, haptic{};
    for (unsigned i = 0; i < info->countSuggestedBindings; ++i) {
        const auto& name = actions[info->suggestedBindings[i].action];
        grip += name == "grip_pose";
        trigger += name == "trigger_value";
        haptic += name == "haptic";
    }
    CHECK(grip == 2 && trigger == 2 && haptic == 2);
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Attach(XrSession, const XrSessionActionSetsAttachInfo*) {
    ++attached;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR MakeSpace(XrSession, const XrActionSpaceCreateInfo* info, XrSpace* out) {
    *out = reinterpret_cast<XrSpace>(++next_id);
    spaces[*out] = {actions[info->action], HandOf(info->subactionPath)};
    ++created;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR DropSpace(XrSpace space) {
    CHECK(spaces.erase(space) == 1);
    ++destroyed;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Sync(XrSession, const XrActionsSyncInfo*) {
    return focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}
XrResult XRAPI_PTR Bool(XrSession, const XrActionStateGetInfo* info, XrActionStateBoolean* out) {
    const auto hand = HandOf(info->subactionPath);
    out->isActive = hand_active[hand];
    out->currentState = actions[info->action] == (hand ? "a_button" : "x_button");
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Float(XrSession, const XrActionStateGetInfo* info, XrActionStateFloat* out) {
    auto hand = HandOf(info->subactionPath);
    out->isActive = hand_active[hand];
    out->currentState = actions[info->action] == "trigger_value" ? (hand ? .75f : .25f) : .6f;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Vector(XrSession, const XrActionStateGetInfo* info, XrActionStateVector2f* out) {
    auto hand = HandOf(info->subactionPath);
    out->isActive = hand_active[hand];
    out->currentState = {hand ? .5f : -.5f, .25f};
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Pose(XrSession, const XrActionStateGetInfo* info, XrActionStatePose* out) {
    out->isActive = hand_active[HandOf(info->subactionPath)];
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Locate(XrSpace space, XrSpace, XrTime, XrSpaceLocation* out) {
    auto [name, hand] = spaces.at(space);
    out->locationFlags =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    out->pose.orientation.w = 1;
    out->pose.position = {hand ? .3f : -.3f, name == "grip_pose" ? 1.f : 2.f, -1.f};
    if (out->next) {
        auto& velocity = *static_cast<XrSpaceVelocity*>(out->next);
        velocity.velocityFlags = XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
        velocity.angularVelocity = {1, 2, 3};
    }
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Haptic(XrSession, const XrHapticActionInfo* info,
                          const XrHapticBaseHeader* out) {
    haptic_hand = HandOf(info->subactionPath);
    last_amplitude = reinterpret_cast<const XrHapticVibration*>(out)->amplitude;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Stop(XrSession, const XrHapticActionInfo*) {
    ++stopped;
    return XR_SUCCESS;
}
} // namespace
int main() {
    XrInputFunctions f{};
    f.createActionSet = MakeSet;
    f.destroyActionSet = DropSet;
    f.createAction = MakeAction;
    f.destroyAction = DropAction;
    f.stringToPath = Path;
    f.suggestInteractionProfileBindings = Suggest;
    f.attachSessionActionSets = Attach;
    f.createActionSpace = MakeSpace;
    f.destroySpace = DropSpace;
    f.syncActions = Sync;
    f.getActionStateBoolean = Bool;
    f.getActionStateFloat = Float;
    f.getActionStateVector2f = Vector;
    f.getActionStatePose = Pose;
    f.locateSpace = Locate;
    f.applyHapticFeedback = Haptic;
    f.stopHapticFeedback = Stop;
    {
        XrInputSystem input(reinterpret_cast<XrInstance>(1), reinterpret_cast<XrSession>(2), f);
        CHECK(input.IsValid());
        CHECK(attached == 1 && created == 4);
        auto s = input.Sync(reinterpret_cast<XrSpace>(3), 1000);
        std::array<Core::HostRuntime::GuestVrSensor::Hand, 2> mapped{};
        for (unsigned i = 0; i < 2; ++i) {
            mapped[i].active = true;
            Core::HostRuntime::MapXrHand(s.hands[i], i, mapped[i]);
        }
        CHECK((mapped[0].buttons & 0x8000) && (mapped[1].buttons & 0x4000));
        CHECK(mapped[0].trigger == .25f && mapped[1].trigger == .75f);
        CHECK(mapped[0].stick_x == -.5f && mapped[0].stick_y == -.25f);
        CHECK(Core::HostRuntime::MapXrPad(mapped).axes[4] == 0); // grip modifier consumes analog
        mapped[0].squeeze = mapped[1].squeeze = 0;
        auto pad = Core::HostRuntime::MapXrPad(mapped);
        CHECK(pad.axes[4] == .25f && pad.axes[5] == .75f);
        CHECK(pad.buttons & 0x800); // right grip+trigger was mapped to R1
        mapped[0].buttons = 0x10;
        mapped[1].buttons = 0x40;
        CHECK(Core::HostRuntime::MapXrPad(mapped).buttons == 0x40);
        auto invalid = s.hands[0];
        invalid.trigger = std::numeric_limits<float>::quiet_NaN();
        Core::HostRuntime::MapXrHand(invalid, 0, mapped[0]);
        CHECK(mapped[0].trigger == 0);
        CHECK(s.hands[0].trigger == .25f && s.hands[1].trigger == .75f);
        CHECK(s.hands[0].squeeze == .6f);
        CHECK(s.IsPressed(Hand::Left, ControllerButton::X));
        CHECK(!s.IsPressed(Hand::Right, ControllerButton::X));
        CHECK(s.IsPressed(Hand::Right, ControllerButton::A));
        CHECK(s.hands[0].grip_pose.position.x == -.3f && s.hands[1].grip_pose.position.x == .3f);
        CHECK(s.hands[0].grip_pose.position.y == 1 && s.hands[0].aim_pose.position.y == 2);
        CHECK(s.hands[0].grip_velocity.angularVelocity.z == 3);
        CHECK(s.hands[0].pose_valid && s.hands[0].grip_flags != 0);
        input.RequestHaptic(Hand::Left, 100, 0, .2f);
        CHECK(haptic_hand == 0 && last_amplitude == .2f);
        input.RequestHaptic(Hand::Right, 100, 0, .8f);
        CHECK(haptic_hand == 1 && last_amplitude == .8f);
        input.RequestHaptic(Hand::Left, 100, 0, 0);
        CHECK(stopped == 1);
        hand_active[0] = false;
        s = input.Sync(reinterpret_cast<XrSpace>(3), 2000);
        CHECK(!s.hands[0].active && s.hands[0].buttons == 0 && s.hands[0].trigger == 0);
        CHECK(s.hands[1].active && s.hands[1].trigger == .75f);
        focused = false;
        s = input.Sync(reinterpret_cast<XrSpace>(3), 3000);
        CHECK(!s.hands[0].active && !s.hands[1].active && !s.hands[1].buttons);
        focused = true;
        input.ResetState();
        s = input.Sync(reinterpret_cast<XrSpace>(3), 4000);
        CHECK(s.WasPressedThisFrame(Hand::Right, ControllerButton::A));
    }
    CHECK(created == destroyed && actions.empty() && spaces.empty());
    std::printf("openxr_input_tests: %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
