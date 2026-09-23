// SPDX-License-Identifier: GPL-2.0-or-later
#include <nlohmann/json.hpp>
#include "core/host_runtime/guest_pad.h"
#include "imgui/renderer/imgui_core.h"
#include <cstdio>
#include <future>
#include <thread>
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::Pad;
using Json = nlohmann::json;
static unsigned checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#__VA_ARGS__); } } while(0)
int main() {
    OrbisPadAdapter p;
    auto command=[&](std::vector<std::string> a) { return Json::parse(p.DebugCommand(a)); };
    auto state=[](u64 id, u64 mask=0x4000) { return std::vector<std::string>{"state","123","7","run","ocr",std::to_string(id),"0","2000",std::to_string(mask),"0","0","0","0","0","0","0","0","0"}; };
    auto query=[&](u64 id) { return command({"status","123","7","run","ocr",std::to_string(id)}); };
    auto release=[&](u64 id) { return command({"release_all","123","7","run","ocr",std::to_string(id)}); };
    CHECK(command({"capabilities"})["buttons"].size()==18);
    p.DebugLifecycle(123,7,"run",1);
    const auto token=p.BeginSession();
    CHECK(command(state(1))["status"]=="not_running");
    p.DebugLifecycle(123,7,"run",3);
    CHECK(p.SetConnected(token,0,true)==PadResult::Ok);
    CHECK(p.Initialize()==0);
    const int handle=p.Open(1000,0,0,0);
    CHECK(handle>0);
    auto trace_start=command({"trace","start","123","7","run","test-game"});
    CHECK(trace_start.value("status", "") == "started" && trace_start.value("active", false));
    const auto trace_id=trace_start["trace_id"].get<std::string>();
    auto trace_read=command({"trace","read","123","7","run",trace_id,"0","64"});
    CHECK(trace_read["status"]=="ok" && trace_read["events"].size()>=2);
    CHECK(trace_read["events"][0]["schema"]=="game-input-event.v1" &&
          trace_read["events"][0]["event"]=="initial_state");
    auto first=command(state(1)); CHECK(first["status"]=="dispatched" && !first.value("duplicate", true));
    CHECK(p.ReadButtons(0)==0x4000);
    OrbisPadData d{}; CHECK(p.Read(handle,&d,1,true)==1);
    CHECK(!query(1).value("guest_poll_seen", true)); // Diagnostic read is not a guest acknowledgment.
    CHECK(p.Read(handle,&d,1,true,true)==1);
    CHECK(query(1).value("guest_poll_seen", false));
    auto duplicate=command(state(1));
    CHECK(duplicate.value("duplicate", false) && duplicate["deadline_ns"]==first["deadline_ns"]);
    auto bad=state(1,0x8000); CHECK(command(bad)["status"]=="id_conflict");
    bad=state(2); bad[4]="other"; CHECK(command(bad)["status"]=="busy");
    for(int i:{1,2,3}) { bad=state(2); bad[i]="999"; CHECK(command(bad)["status"]=="wrong_session"); }
    CHECK(command({"release_all","123","7","run","other","1"})["status"]=="busy");
    for(auto [index,value]:std::vector<std::pair<int,std::string>>{{6,"4"},{7,"0"},{7,"2001"},{8,"-1"},{9,"NaN"},{10,"inf"},{11,"1.01"},{12,"-1.01"},{13,"-0.1"},{14,"2"},{15,"2"},{16,"1.1"},{17,"-0.1"},{5,"0"},{4,"invalid/owner"}}) {
        bad=state(2); bad[index]=value; CHECK(command(bad)["status"]=="invalid_arguments");
        CHECK(p.ReadButtons(0)==0x4000);
    }
    CHECK(command(state(2,0x80000000))["status"]=="unsupported_buttons");
    CHECK(release(2)["status"]=="dispatched"); CHECK(p.ReadButtons(0)==0);
    CHECK(query(1)["release_reason"]=="release_all");
    // Real checked Guest ABI, not just native Read, confirms pointer failure cannot consume a receipt.
    auto space_result=GuestAddressSpace::Create({.reservation_size=0x1000000}); CHECK(bool(space_result));
    auto space=std::move(space_result).Value(); const auto base=space->ReservationBase();
    CHECK(space->Map({base,0x4000},GuestPermission::Read|GuestPermission::Write));
    std::array<GuestUser,4> users{}; users[0]={1000,"debug pad"};
    GuestPlatform platform(users,0x9000000,1,false); CHECK(platform.Initialize()==0);
    GuestPad guest(p,platform); CHECK(guest.Dispatch(*space,"hv1luiJrqQM",{})==0);
    u64 id=3;
    for(u64 bit:{0x4000,0x2000,0x8000,0x1000,0x10,0x40,0x80,0x20,0x400,0x800,0x100,0x200,2,4,8,1,0x10000,0x100000}) {
        CHECK(command(state(id,bit))["status"]=="dispatched");
        CHECK(guest.Dispatch(*space,"YndgXqQVV7c",{u64(handle),1})==u32(ORBIS_PAD_ERROR_INVALID_ARG));
        CHECK(!query(id).value("guest_poll_seen", true));
        CHECK(guest.Dispatch(*space,"YndgXqQVV7c",{u64(handle),base.value})==0);
        CHECK(space->ReadData(base,std::as_writable_bytes(std::span{&d,1})));
        CHECK(u32(d.buttons)==bit && query(id).value("guest_poll_count", 0u)==1);
        ++id;
    }
    auto axes=state(id,0); axes[9]="-1";axes[10]="1";axes[11]="0.5";axes[12]="-0.5";
    axes[13]="1";axes[14]="0.25";axes[15]="1";axes[16]="1";axes[17]="0.5";
    CHECK(command(axes)["status"]=="dispatched"); CHECK(p.Read(handle,&d,1,true,true)==1);
    CHECK(d.leftStick.x==0 && d.leftStick.y==255 && d.rightStick.x==192 && d.rightStick.y==64);
    CHECK(d.analogButtons.l2==255 && d.analogButtons.r2==64 && u32(d.buttons)==0x100);
    CHECK(d.touchData.touchNum==1 && d.touchData.touch[0].x==1919 && d.touchData.touch[0].y==475);
    CHECK(p.Submit(token,0,{.buttons=0x8000,.left_x=1})==PadResult::Ok);
    CHECK(release(++id)["status"]=="dispatched");
    CHECK(p.ReadState(0,&d) && u32(d.buttons)==0x8000 && d.leftStick.x==255); // Own source only.
    CHECK(p.Submit(token,0,{})==PadResult::Ok);
    // Independent ports, but one owner across the process device.
    for(int port=0;port<4;++port) {auto a=state(++id);a[6]=std::to_string(port);CHECK(command(a)["status"]=="dispatched"); CHECK(p.ReadButtons(port)==0x4000);}
    CHECK(release(++id)["status"]=="dispatched"); for(int port=0;port<4;++port) CHECK(p.ReadButtons(port)==0);
    // Loss of ADB requires no final host command: native watchdog neutralizes input.
    auto timed=state(++id); timed[7]="40"; CHECK(command(timed)["status"]=="dispatched");
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(p.ReadButtons(0) && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(p.ReadButtons(0)==0); CHECK(query(id)["release_reason"]=="timeout");
    CHECK(command(timed).value("duplicate", false) && p.ReadButtons(0)==0);
    CHECK(command(state(++id))["status"]=="dispatched"); p.FocusLost(token); CHECK(p.ReadButtons(0)==0);
    CHECK(query(id)["release_reason"]=="focus_lost");
    CHECK(command(state(++id))["status"]=="not_focused");
    CHECK(!command({"status"}).value("focused", true));
    auto trace_stop=command({"trace","stop","123","7","run",trace_id});
    CHECK(trace_stop.value("status", "") == "stopped" && !trace_stop.value("active", true));
    p.SetConnected(token,0,true);
    auto overflow_start=command({"trace","start","123","7","run","overflow-test"});
    CHECK(overflow_start["status"]=="started");
    const auto overflow_id=overflow_start["trace_id"].get<std::string>();
    for (u64 i=0; i<2100; ++i)
        CHECK(p.Submit(token, 0, PadSnapshot{.buttons = (i & 1) ? 0x4000u : 0u}) == PadResult::Ok);
    auto gap=command({"trace","read","123","7","run",overflow_id,"0","8"});
    CHECK(gap.value("status", "") == "gap" && gap.value("overflow", false) &&
          gap.value("dropped", 0u) > 0);
    CHECK(command({"trace","stop","123","7","run",overflow_id})["status"]=="stopped");
    CHECK(command(state(++id))["status"]=="dispatched"); p.SetConnected(token,0,false); CHECK(p.ReadButtons(0)==0);
    CHECK(query(id)["release_reason"]=="focus_lost");
    CHECK(command(state(++id))["status"]=="not_focused");
    p.SetConnected(token,0,true);
    ImGui::Core::AcquireGamepadInputCapture();
    CHECK(command(state(++id))["status"]=="ui_captured"); ImGui::Core::ReleaseGamepadInputCapture();
    CHECK(command(state(++id))["status"]=="dispatched"); ImGui::Core::AcquireGamepadInputCapture();
    CHECK(p.Read(handle,&d,1,true,true)==1 && u32(d.buttons)==0x80000000);
    CHECK(!query(id).value("guest_poll_seen", true));
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(p.ReadButtons(0) && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(query(id)["release_reason"]=="ui_captured"); ImGui::Core::ReleaseGamepadInputCapture();
    // Concurrent retries execute once and cannot extend the timeout.
    const auto repeated=state(++id);
    std::array<std::future<Json>,8> futures;
    for(auto& f:futures) f=std::async(std::launch::async,[&]{return command(repeated);});
    unsigned sent{};for(auto& f:futures){auto j=f.get();CHECK(j["status"]=="dispatched");sent+=!j.value("duplicate", true);}CHECK(sent==1);
    for(int i=0;i<260;++i) CHECK(command(state(++id))["status"]=="dispatched");
    CHECK(command(repeated)["status"]=="stale_action_id");
    p.DebugLifecycle(123,7,"run",4); CHECK(p.ReadButtons(0)==0); CHECK(command(state(++id))["status"]=="not_running");
    p.EndSession(token); const auto next=p.BeginSession(); p.DebugLifecycle(123,8,"run",3); p.SetConnected(next,0,true);
    CHECK(command(state(++id))["status"]=="wrong_session");
    auto fresh=state(1);fresh[2]="8"; CHECK(command(fresh)["status"]=="dispatched");
    p.DebugLifecycle(123,7,"run",6); CHECK(p.ReadButtons(0)==0x4000);
    p.EndSession(token);CHECK(p.ReadButtons(0)==0x4000);p.EndSession(next);CHECK(p.ReadButtons(0)==0);
    std::printf("debug pad: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
