// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <cmath>
#include <cstdio>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/host_runtime/vr_geometry.h"
#include "core/libraries/move/move.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
using namespace Core::HostRuntime;
using namespace Libraries::VrTracker;
using namespace Libraries::Move;
static unsigned checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; printf("FAIL %d: %s\n",__LINE__,#__VA_ARGS__); } } while(0)
static bool Near(float a, float b) { return std::abs(a-b)<1e-5f; }
static void CheckOrder(bool swap) {
    SetXrSwapHands(swap);
    auto& sensor=GuestVrSensor::Instance();
    GuestVrSensor::HardwareFrame f;
    f.generation=sensor.BeginOpenXr();
    f.received_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    f.predicted_ns=f.received_ns+10'000'000;
    f.running=f.focused=true;
    f.head.position={2,1.6f,-3};
    f.head.orientation_valid=f.head.position_valid=true;
    for(unsigned i=0;i<2;++i) {
        auto& h=f.hands[i];
        h.active=true;
        h.grip=f.head;
        h.grip.position={i?.4f:-.3f,1.2f,-.7f};
        h.grip.orientation={0,std::sqrt(.5f),0,std::sqrt(.5f)};
        h.grip.angular_velocity={0,0,-2}; // body +X at yaw +90
        h.grip.linear_velocity={1,2,3};
        h.aim=h.grip; h.aim.position[0]=9; // distinct aim must never become the Move pose
        h.trigger=i?.75f:.25f;
        h.buttons=i?0x4000:0x8000;
    }
    CHECK(sensor.PublishOpenXr(f));
    CHECK(sceMoveInit()==0);
    // Open right first: registration/opening order cannot swap the hands.
    const s32 right=sceMoveOpen(1000,0,swap?0:1), left=sceMoveOpen(1000,0,swap?1:0);
    CHECK(right>0 && left>0 && right!=left);
    CHECK(HandIndexForHandle(left)==0 && HandIndexForHandle(right)==1);
    // A settings update is for the next session, not a mid-frame remap.
    SetXrSwapHands(!swap);
    CHECK(HandIndexForHandle(left)==0 && HandIndexForHandle(right)==1);
    OrbisVrTrackerInitParam init{};
    init.size=sizeof(init);
    // In hardware-provider mode the HLE does not access optical work buffers.
    int dummy;
    init.direct_memory_garlic=init.direct_memory_onion=init.work_memory=&dummy;
    init.direct_memory_garlic_alignment=init.direct_memory_onion_alignment=init.work_memory_alignment=ORBIS_VR_TRACKER_MEMORY_ALIGNMENT;
    init.direct_memory_garlic_size=ORBIS_VR_TRACKER_GARLIC_SIZE;
    init.direct_memory_onion_size=ORBIS_VR_TRACKER_BASE_ONION_SIZE;
    init.work_memory_size=ORBIS_VR_TRACKER_WORK_SIZE;
    CHECK(sceVrTrackerInit(&init)==0);
    CHECK(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_MOVE,right)==0);
    CHECK(sceVrTrackerRegisterDevice(ORBIS_VR_TRACKER_DEVICE_MOVE,left)==0);
    for(unsigned i=0;i<2;++i) {
        const auto handle=i?right:left;
        OrbisVrTrackerGetResultParam query{};
        query.size=sizeof(query);query.handle=handle;query.user_frame_number=37;
        OrbisVrTrackerResultData result{};
        CHECK(sceVrTrackerGetResult(&query,&result)==0);
        const auto& p=result.move_info.device_pose;
        CHECK(result.connected && result.status==ORBIS_VR_TRACKER_STATUS_TRACKING);
        // At yaw +90, the sphere is 75 mm left of the palm, not a world-Z shift.
        CHECK(Near(p.position_x,(i?.4f:-.3f)-.075f) && Near(p.position_y,1.2f) && Near(p.position_z,-.7f));
        CHECK(Near(p.orientation_y,std::sqrt(.5f)) && Near(p.orientation_w,std::sqrt(.5f)));
        CHECK(result.user_frame_number==37 && Near(result.velocity_x,1) && Near(result.velocity_y,2.15f) && Near(result.velocity_z,3));
        OrbisMoveData data{};
        CHECK(sceMoveReadStateLatest(handle,&data)==0);
        CHECK(Near(data.gyro[0],2) && Near(data.gyro[1],0) && Near(data.gyro[2],0));
        CHECK(data.button_data.trigger_data==(i?191:64));
        CHECK(data.button_data.button_data==(i?ORBIS_MOVE_BUTTON_CROSS:ORBIS_MOVE_BUTTON_SQUARE));
        CHECK(sceMoveSetVibration(handle, i?191:64)==0);
        const auto haptics=sensor.TakeHaptics(f.generation);
        CHECK(Near(haptics[i],float(i?191:64)/255.f) && haptics[1-i]==-1.f);
        f.hands[i].grip.orientation_valid=false;
        CHECK(sensor.PublishOpenXr(f));
        CHECK(sceVrTrackerGetResult(&query,&result)==0 &&
              result.orientation_quality==ORBIS_VR_TRACKER_QUALITY_NONE &&
              result.position_quality==ORBIS_VR_TRACKER_QUALITY_NONE);
        f.hands[i].grip.orientation_valid=true;
        f.hands[i].grip.position_valid=false;
        CHECK(sensor.PublishOpenXr(f));
        CHECK(sceVrTrackerGetResult(&query,&result)==0 && result.position_quality==ORBIS_VR_TRACKER_QUALITY_NONE);
        f.hands[i].active=false;
        CHECK(sensor.PublishOpenXr(f));
        CHECK(sceVrTrackerGetResult(&query,&result)==0 && !result.connected && result.status==ORBIS_VR_TRACKER_STATUS_NOT_TRACKING);
    }
    CHECK(sceVrTrackerTerm()==0);
    CHECK(sceMoveTerm()==0);
    sensor.EndOpenXr(f.generation);
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    Common::FS::InitializeAndroidUserPaths(argv[1]);
    Common::Log::Setup("openxr-pose-tests");
    // Three known rotations prove this is a controller-local origin transform.
    GuestVrSensor::Pose grip;
    grip.position={2,3,4};
    grip.position_valid=grip.orientation_valid=true;
    auto sphere=VrGeometry::MoveSpherePose(grip);
    CHECK(Near(sphere.position[0],2) && Near(sphere.position[1],3) && Near(sphere.position[2],3.925f));
    grip.orientation={std::sqrt(.5f),0,0,std::sqrt(.5f)};
    grip.angular_velocity={2,0,0};
    sphere=VrGeometry::MoveSpherePose(grip);
    CHECK(Near(sphere.position[0],2) && Near(sphere.position[1],3.075f) && Near(sphere.position[2],4));
    CHECK(Near(sphere.linear_velocity[0],0) && Near(sphere.linear_velocity[1],0) && Near(sphere.linear_velocity[2],.15f));
    grip.orientation={1,0,0,0};
    sphere=VrGeometry::MoveSpherePose(grip);
    CHECK(Near(sphere.position[0],2) && Near(sphere.position[1],3) && Near(sphere.position[2],4.075f));
    CheckOrder(false);
    CheckOrder(true);
    // The XR preference must not swap the diagnostic non-XR SBS provider.
    auto& sensor=GuestVrSensor::Instance();
    sensor.SetSbsEnabled(true);
    SetXrSwapHands(true);
    CHECK(sceMoveInit()==0);
    const s32 first=sceMoveOpen(1000,0,0), second=sceMoveOpen(1000,0,1);
    CHECK(HandIndexForHandle(first)==0 && HandIndexForHandle(second)==1);
    CHECK(sceMoveTerm()==0);
    sensor.SetSbsEnabled(false);
    SetXrSwapHands(false);
    printf("openxr_pose_tests: %u checks / %u failures\n",checks,failures);
    Common::Log::Shutdown();
    return failures?1:0;
}
