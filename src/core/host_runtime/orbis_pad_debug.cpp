// SPDX-License-Identifier: GPL-2.0-or-later
#include <nlohmann/json.hpp>
#include "core/host_runtime/orbis_pad_adapter.h"
#include "imgui/renderer/imgui_core.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <thread>
#include <array>

namespace Core::HostRuntime {
using Json = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;
using namespace spatial::input;
namespace {
constexpr std::array<u32, 18> Bits{0x4000,0x2000,0x8000,0x1000,0x10,0x40,0x80,0x20,
    0x400,0x800,0x100,0x200,2,4,8,1,0x10000,0x100000};
constexpr std::array<std::string_view, 18> Names{"cross","circle","square","triangle",
    "up","down","left","right","l1","r1","l2","r2","l3","r3","options","share","ps","touchpad"};
constexpr u64 ButtonMask = 0x11ffff;
u64 Ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
bool Uint(std::string_view s, u64& out, bool hex = false) {
    int base = 10;
    if (hex && s.starts_with("0x")) { base = 16; s.remove_prefix(2); }
    if (s.empty()) return false;
    const auto [p,e] = std::from_chars(s.data(), s.data()+s.size(), out, base);
    return e == std::errc{} && p == s.data()+s.size();
}
bool Float(std::string_view s, float& out, float min, float max) {
    const auto [p,e] = std::from_chars(s.data(), s.data()+s.size(), out);
    return !s.empty() && e == std::errc{} && p == s.data()+s.size() &&
           std::isfinite(out) && out >= min && out <= max;
}
bool Owner(std::string_view s) {
    return !s.empty() && s.size() <= 48 && std::ranges::all_of(s, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
Json Snapshot(const PadSnapshot& s) {
    return {{"buttons",s.buttons},{"left_x",s.left_x},{"left_y",s.left_y},
        {"right_x",s.right_x},{"right_y",s.right_y},{"left_trigger",s.left_trigger},
        {"right_trigger",s.right_trigger},{"touch_down",s.touch_down},
        {"touch_x",s.touch_x},{"touch_y",s.touch_y}};
}
const char* TraceSourceName(OrbisPadAdapter::TraceSource s) {
    switch (s) {
    case OrbisPadAdapter::TraceSource::Physical: return "physical";
    case OrbisPadAdapter::TraceSource::Overlay: return "overlay";
    case OrbisPadAdapter::TraceSource::Debugbus: return "debugbus";
    case OrbisPadAdapter::TraceSource::Merged: return "merged";
    }
    return "merged";
}
const char* TraceKindName(OrbisPadAdapter::TraceKind k) {
    switch (k) {
    case OrbisPadAdapter::TraceKind::InitialState: return "initial_state";
    case OrbisPadAdapter::TraceKind::CommandRequested: return "command_requested";
    case OrbisPadAdapter::TraceKind::StatePublished: return "state_published";
    case OrbisPadAdapter::TraceKind::GuestPolled: return "guest_polled";
    case OrbisPadAdapter::TraceKind::Released: return "released";
    case OrbisPadAdapter::TraceKind::Gap: return "gap";
    case OrbisPadAdapter::TraceKind::SessionChanged: return "session_changed";
    case OrbisPadAdapter::TraceKind::TraceStopped: return "trace_stopped";
    }
    return "state_published";
}
bool Same(const PadSnapshot& a, const PadSnapshot& b) {
    return a.buttons == b.buttons && a.left_x == b.left_x && a.left_y == b.left_y &&
           a.right_x == b.right_x && a.right_y == b.right_y &&
           a.left_trigger == b.left_trigger && a.right_trigger == b.right_trigger &&
           a.touch_down == b.touch_down && a.touch_x == b.touch_x && a.touch_y == b.touch_y;
}
Json TraceValueJson(const OrbisPadAdapter::TraceValue& v) {
    auto j = Snapshot(v.state);
    j["connected"] = v.connected;
    j["valid"] = v.valid;
    return j;
}
Json TraceEdges(const OrbisPadAdapter::TraceValue& before,
                const OrbisPadAdapter::TraceValue& after) {
    Json pressed = Json::array(), released = Json::array();
    if (before.valid && after.valid) {
        const u64 rise = after.state.buttons & ~before.state.buttons;
        const u64 fall = before.state.buttons & ~after.state.buttons;
        for (size_t i = 0; i < Names.size(); ++i) {
            if (rise & Bits[i]) pressed.push_back(Names[i]);
            if (fall & Bits[i]) released.push_back(Names[i]);
        }
        if (!before.state.touch_down && after.state.touch_down) pressed.push_back("touch");
        if (before.state.touch_down && !after.state.touch_down) released.push_back("touch");
    }
    return {{"pressed", pressed}, {"released", released}};
}
Json TraceEntryJson(const OrbisPadAdapter::TraceEntry& e) {
    Json j{{"schema", "game-input-event.v1"}, {"seq", e.seq},
           {"event", TraceKindName(e.kind)}, {"source", TraceSourceName(e.source)},
           {"source_id", e.source_id}, {"port", e.port}, {"pid", 0},
           {"generation", 0}, {"run_uuid", ""}, {"game_id", ""},
           {"clock", "CLOCK_MONOTONIC"}, {"device_monotonic_ns", e.monotonic_ns},
           {"host_receive_started_ns", e.host_receive_started_ns},
           {"host_received_ns", e.host_received_ns}, {"related_seq", e.related_seq},
           {"action_id", e.action_id}, {"intercepted", e.intercepted},
           {"state", TraceValueJson(e.value)}, {"edge", TraceEdges(e.previous, e.value)}};
    if (!e.reason.empty()) j["reason"] = e.reason;
    return j;
}
}
struct OrbisPadAdapter::DebugReceipt {
    std::string owner, request, state{"held"}, release_reason;
    u64 id{}, deadline_ns{}, published_timestamp_us{}, guest_poll_count{}, first_guest_poll_ns{};
    int port{};
    PadSnapshot snapshot;
    Json JsonValue() const {
        return {{"owner",owner},{"action_id",id},{"port",port},{"state",state},
            {"published_timestamp_us",published_timestamp_us},{"deadline_ns",deadline_ns},
            {"guest_poll_seen",guest_poll_count != 0},{"guest_poll_count",guest_poll_count},
            {"first_guest_poll_ns",first_guest_poll_ns},{"release_reason",release_reason},
            {"requested",Snapshot(snapshot)}};
    }
};
struct OrbisPadAdapter::DebugState {
    std::condition_variable cv;
    std::thread worker;
    bool closing{}, focused{};
    u64 pid{}, generation{};
    u32 phase{};
    std::string uuid, owner;
    std::map<std::string,u64> high_water;
    std::deque<std::shared_ptr<DebugReceipt>> receipts;
    static constexpr std::size_t kTraceCapacity = 4096;
    std::array<TraceEntry, kTraceCapacity> trace_ring{};
    std::array<TraceValue, kMaxPadPorts * 4> trace_previous{};
    std::array<u64, kMaxPadPorts> trace_merged_seq{};
    std::uint64_t trace_next_seq{1};
    std::uint64_t trace_dropped{};
    bool trace_overflow{};
    bool trace_active{};
    std::string trace_id, trace_game_id;
};
OrbisPadAdapter::OrbisPadAdapter() : debug_(std::make_unique<DebugState>()) {
    debug_->worker = std::thread([this] { DebugWatchdog(); });
}
OrbisPadAdapter::~OrbisPadAdapter() {
    { std::lock_guard lock(mutex_); debug_->closing = true; debug_->cv.notify_all(); }
    if (debug_->worker.joinable()) debug_->worker.join();
}
void OrbisPadAdapter::ResetDebugLocked() {
    ReleaseDebugLocked("session_reset", true);
    debug_->focused = false;
    debug_->high_water.clear();
    debug_->receipts.clear();
    debug_->trace_active = false;
    debug_->trace_id.clear();
    debug_->trace_game_id.clear();
    debug_->trace_next_seq = 1;
    debug_->trace_dropped = 0;
    debug_->trace_overflow = false;
    debug_->trace_merged_seq.fill(0);
    for (auto& v : debug_->trace_previous) v = {};
}

void OrbisPadAdapter::TraceAppendLocked(TraceEntry entry) {
    if (!debug_->trace_active && entry.kind != TraceKind::TraceStopped)
        return;
    const auto now = Ns();
    entry.monotonic_ns = now;
    entry.host_receive_started_ns = now;
    entry.host_received_ns = now;
    entry.seq = debug_->trace_next_seq++;
    if (entry.seq > DebugState::kTraceCapacity)
        debug_->trace_overflow = true;
    if (debug_->trace_next_seq > DebugState::kTraceCapacity + 1)
        ++debug_->trace_dropped;
    debug_->trace_ring[(entry.seq - 1) % DebugState::kTraceCapacity] = std::move(entry);
}

void OrbisPadAdapter::TracePublishLocked(int port) {
    if (!debug_->trace_active)
        return;
    auto& p = ports_[port];
    const std::array<std::optional<ControllerState>, 3> states = {
        p.physical ? hub_.Snapshot(*p.physical) : std::nullopt,
        p.overlay ? hub_.Snapshot(*p.overlay) : std::nullopt,
        p.debug ? hub_.Snapshot(*p.debug) : std::nullopt};
    const std::array<TraceSource, 3> sources = {TraceSource::Physical, TraceSource::Overlay,
                                                 TraceSource::Debugbus};
    const std::array<std::int64_t, 3> ids = {
        p.physical ? p.physical->backend_id : 0, p.overlay ? p.overlay->backend_id : 0,
        p.debug ? p.debug->backend_id : 0};
    for (size_t i = 0; i < states.size(); ++i) {
        TraceValue value;
        value.valid = true;
        value.connected = states[i] && states[i]->connected;
        if (value.connected) {
            for (size_t b = 0; b < Bits.size(); ++b)
                if (states[i]->buttons[b]) value.state.buttons |= Bits[b];
            for (size_t a = 0; a < 6; ++a) {
                float* dst = (&value.state.left_x) + a;
                *dst = states[i]->axes[a];
            }
        }
        const size_t slot = port * 4 + i;
        auto& previous = debug_->trace_previous[slot];
        if (!previous.valid || !Same(previous.state, value.state) || previous.connected != value.connected) {
            TraceEntry e;
            e.kind = previous.valid ? TraceKind::StatePublished : TraceKind::InitialState;
            e.source = sources[i]; e.port = port;
            e.source_id = ids[i]; e.value = value; e.previous = previous;
            e.action_id = (i == 2 && p.debug_receipt) ? p.debug_receipt->id : 0;
            TraceAppendLocked(std::move(e));
            previous = value;
        }
    }
    TraceValue merged;
    merged.valid = true; merged.connected = p.data.connected;
    merged.state.buttons = u32(p.data.buttons);
    merged.state.left_x = (int(p.data.leftStick.x) - 128) / (p.data.leftStick.x < 128 ? 128.f : 127.f);
    merged.state.left_y = (int(p.data.leftStick.y) - 128) / (p.data.leftStick.y < 128 ? 128.f : 127.f);
    merged.state.right_x = (int(p.data.rightStick.x) - 128) / (p.data.rightStick.x < 128 ? 128.f : 127.f);
    merged.state.right_y = (int(p.data.rightStick.y) - 128) / (p.data.rightStick.y < 128 ? 128.f : 127.f);
    merged.state.left_trigger = p.data.analogButtons.l2 / 255.f;
    merged.state.right_trigger = p.data.analogButtons.r2 / 255.f;
    merged.state.touch_down = p.data.touchData.touchNum != 0;
    if (merged.state.touch_down) {
        merged.state.touch_x = p.data.touchData.touch[0].x / 1919.f;
        merged.state.touch_y = p.data.touchData.touch[0].y / 949.f;
    }
    auto& previous = debug_->trace_previous[port * 4 + 3];
    if (!previous.valid || !Same(previous.state, merged.state) || previous.connected != merged.connected) {
        TraceEntry e; e.kind = previous.valid ? TraceKind::StatePublished : TraceKind::InitialState;
        e.source = TraceSource::Merged;
        e.port = port; e.source_id = port; e.value = merged; e.previous = previous;
        TraceAppendLocked(std::move(e));
        previous = merged;
        debug_->trace_merged_seq[port] = debug_->trace_next_seq - 1;
    }
}

void OrbisPadAdapter::TraceGuestPollLocked(int port, const Sample& sample, bool intercepted) {
    if (!debug_->trace_active) return;
    TraceEntry e; e.kind = TraceKind::GuestPolled; e.source = TraceSource::Merged; e.port = port;
    e.source_id = port; e.related_seq = debug_->trace_merged_seq[port]; e.intercepted = intercepted;
    e.value = debug_->trace_previous[port * 4 + 3];
    e.previous = e.value;
    TraceAppendLocked(std::move(e));
}

void OrbisPadAdapter::TraceReleasedLocked(int port, std::string_view reason) {
    if (!debug_->trace_active) return;
    TraceEntry e; e.kind = TraceKind::Released; e.source = TraceSource::Debugbus; e.port = port;
    e.source_id = kMaxPadPorts + port; e.reason = reason;
    e.action_id = ports_[port].debug_receipt ? ports_[port].debug_receipt->id : 0;
    e.value = debug_->trace_previous[port * 4 + 2];
    e.previous = e.value;
    TraceAppendLocked(std::move(e));
}

void OrbisPadAdapter::TraceCommandLocked(TraceKind kind, int port, u64 action_id,
                                         std::string_view reason) {
    if (!debug_->trace_active) return;
    TraceEntry e; e.kind = kind; e.source = TraceSource::Debugbus; e.port = port;
    e.source_id = kMaxPadPorts + std::max(port, 0); e.action_id = action_id; e.reason = reason;
    TraceAppendLocked(std::move(e));
}
void OrbisPadAdapter::DebugFocusLocked(bool focused) {
    debug_->focused = focused;
    if (!focused) ReleaseDebugLocked("focus_lost", true);
}
void OrbisPadAdapter::DebugLifecycle(u64 pid, u64 generation, std::string_view uuid, u32 phase) {
    std::lock_guard lock(mutex_);
    if (generation < debug_->generation) return; // Late old teardown cannot revoke a new lease.
    if (generation != debug_->generation || pid != debug_->pid || uuid != debug_->uuid)
        ResetDebugLocked();
    debug_->pid = pid; debug_->generation = generation; debug_->uuid = uuid; debug_->phase = phase;
    if (phase != 3) {
        ReleaseDebugLocked("session_not_running", true);
        if (debug_->trace_active) {
            TraceEntry e; e.kind = TraceKind::TraceStopped; e.source = TraceSource::Merged;
            e.reason = "session_not_running"; TraceAppendLocked(std::move(e));
            debug_->trace_active = false;
        }
    }
    debug_->cv.notify_all();
}
void OrbisPadAdapter::ReleaseDebugPortLocked(int port, std::string_view reason, bool discard_history) {
    auto& p = ports_[port];
    if (!p.debug) return;
    TraceReleasedLocked(port, reason);
    if (p.debug_receipt) {
        p.debug_receipt->state = "released";
        p.debug_receipt->release_reason = reason;
    }
    hub_.RemoveDevice(*p.debug);
    p.debug.reset(); p.debug_touch = {}; p.debug_receipt.reset();
    if (discard_history) p.history.clear();
    Publish(port);
}
void OrbisPadAdapter::ReleaseDebugLocked(std::string_view reason, bool discard_history) {
    for (int i=0; i<kMaxPadPorts; ++i) ReleaseDebugPortLocked(i, reason, discard_history);
    debug_->owner.clear();
    debug_->cv.notify_all();
}
void OrbisPadAdapter::ObserveDebugLocked(const Sample& sample, bool intercepted) {
    // Called only from the checked GuestPad adapter after output admission. The
    // JNI diagnostic reads do not increment this receipt, nor does modal input.
    if (!sample.debug) return;
    const auto it = std::find_if(ports_.begin(), ports_.end(), [&](const Port& p) {
        return p.debug_receipt == sample.debug;
    });
    if (it != ports_.end())
        TraceGuestPollLocked(int(it - ports_.begin()), sample, intercepted);
    if (intercepted) return;
    ++sample.debug->guest_poll_count;
    if (!sample.debug->first_guest_poll_ns) sample.debug->first_guest_poll_ns = Ns();
}
void OrbisPadAdapter::DebugWatchdog() {
    std::unique_lock lock(mutex_);
    while (!debug_->closing) {
        auto deadline = Clock::time_point::max();
        const auto now = Ns();
        const bool captured = ImGui::Core::IsGamepadInputCaptured();
        for (int i=0; i<kMaxPadPorts; ++i) {
            const auto& receipt = ports_[i].debug_receipt;
            if (!receipt) continue;
            if (captured || now >= receipt->deadline_ns)
                ReleaseDebugPortLocked(i, captured ? "ui_captured" : "timeout", captured);
            else
                deadline = std::min(deadline, Clock::time_point(std::chrono::nanoseconds(receipt->deadline_ns)));
        }
        const bool any = std::ranges::any_of(ports_, [](const Port& p) { return bool(p.debug); });
        if (!any) debug_->owner.clear();
        // While held, detect host modal capture even without another input packet.
        if (any) deadline = std::min(deadline, Clock::now()+std::chrono::milliseconds(10));
        debug_->cv.wait_until(lock, deadline);
    }
}
std::string OrbisPadAdapter::DebugCommand(const std::vector<std::string>& a) {
    std::lock_guard lock(mutex_);
    auto reply = [&](std::string_view status) {
        return Json{{"schema",1},{"status",status},{"pid",debug_->pid},
            {"generation",debug_->generation},{"run_uuid",debug_->uuid},
            {"phase",debug_->phase},{"input_token",token_},{"focused",debug_->focused}};
    };
    auto error = [&](std::string_view status) { return reply(status).dump()+"\n"; };
    if (!a.empty() && a[0] == "trace") {
        auto trace_reply = [&](std::string_view status) {
            auto j = reply(status);
            j["trace_id"] = debug_->trace_id;
            j["game_id"] = debug_->trace_game_id;
            j["active"] = debug_->trace_active;
            j["oldest_seq"] = debug_->trace_next_seq > DebugState::kTraceCapacity
                                   ? debug_->trace_next_seq - DebugState::kTraceCapacity : 1;
            j["next_seq"] = debug_->trace_next_seq;
            j["dropped"] = debug_->trace_dropped;
            j["overflow"] = debug_->trace_overflow;
            j["clock"] = "CLOCK_MONOTONIC";
            j["clock_sample_ns"] = Ns();
            return j;
        };
        auto session_ok = [&](size_t off) {
            u64 pid{}, gen{};
            return a.size() > off + 2 && Uint(a[off], pid) && Uint(a[off + 1], gen) &&
                   pid == debug_->pid && gen == debug_->generation && a[off + 2] == debug_->uuid;
        };
        if (a.size() == 6 && a[1] == "start") {
            if (!session_ok(2) || !token_ || debug_->phase != 3) return error("not_running");
            if (debug_->trace_active) return error("trace_active");
            debug_->trace_active = true;
            debug_->trace_game_id = a[5] == "-" ? std::string{} : a[5];
            debug_->trace_id = "padtrace-" + std::to_string(debug_->generation) + "-" +
                               std::to_string(Ns());
            debug_->trace_next_seq = 1; debug_->trace_dropped = 0; debug_->trace_overflow = false;
            debug_->trace_merged_seq.fill(0);
            for (auto& v : debug_->trace_previous) v = {};
            for (int port = 0; port < kMaxPadPorts; ++port) TracePublishLocked(port);
            auto j = trace_reply("started");
            return j.dump() + "\n";
        }
        if (a.size() == 8 && a[1] == "read") {
            if (!session_ok(2)) return error("wrong_session");
            if (debug_->trace_id.empty() || a[5] != debug_->trace_id) return error("trace_inactive");
            u64 after{}, limit{};
            if (!Uint(a[6], after) || !Uint(a[7], limit) || limit == 0 || limit > 256)
                return error("invalid_arguments");
            auto j = trace_reply("ok");
            const u64 oldest = j["oldest_seq"].get<u64>();
            if (after + 1 < oldest) {
                j["status"] = "gap"; j["missed_from"] = after + 1;
                j["events"] = Json::array();
                return j.dump() + "\n";
            }
            auto events = Json::array();
            std::string jsonl;
            const u64 end = std::min(debug_->trace_next_seq, after + 1 + limit);
            for (u64 seq = after + 1; seq < end; ++seq) {
                auto event = TraceEntryJson(debug_->trace_ring[(seq - 1) % DebugState::kTraceCapacity]);
                event["seq"] = seq;
                event["pid"] = debug_->pid; event["generation"] = debug_->generation;
                event["run_uuid"] = debug_->uuid; event["game_id"] = debug_->trace_game_id;
                jsonl += event.dump(); jsonl.push_back('\n');
                events.push_back(std::move(event));
            }
            j["events"] = std::move(events); j["jsonl"] = std::move(jsonl);
            return j.dump() + "\n";
        }
        if (a.size() == 6 && a[1] == "stop") {
            if (!session_ok(2) || !debug_->trace_active || a[5] != debug_->trace_id)
                return error("wrong_session");
            TraceEntry e; e.kind = TraceKind::TraceStopped; e.source = TraceSource::Merged;
            e.reason = "trace_stopped"; TraceAppendLocked(std::move(e));
            debug_->trace_active = false;
            auto j = trace_reply("stopped");
            return j.dump() + "\n";
        }
        return error("invalid_arguments");
    }
    if (a.size()==1 && a[0]=="capabilities") {
        auto j=reply("ok"); auto buttons=Json::object();
        for (size_t i=0;i<Names.size();++i) buttons[Names[i]]=Bits[i];
        j["buttons"]=buttons; j["button_mask"]=ButtonMask; j["ports"]={0,1,2,3};
        j["stick_range"]={-1,1}; j["stick_y_positive"]="down";
        j["trigger_range"]={0,1}; j["touch_range"]={0,1}; j["touch_points"]=1;
        j["hold_ms_range"]={1,2000}; j["id_policy"]="strictly_increasing_per_owner_per_session";
        j["owner_limit"]=64; j["receipt_limit"]=256;
        j["unsupported"]={"intercepted_input","multi_touch","motion_sensors","system_ps_ui","system_share_ui"};
        j["system_button_semantics"]="ps/share publish existing guest Pad bits only";
        j["merge"]="buttons OR; strongest axis; physical then overlay win equal ties; debug touch wins while down";
        return j.dump()+"\n";
    }
    if (a.size()==1 && a[0]=="status") {
        auto j=reply("ok"); j["owner"]=debug_->owner; j["ports"]=Json::array();
        j["ui_captured"]=ImGui::Core::IsGamepadInputCaptured();
        for (int i=0;i<kMaxPadPorts;++i) {
            const auto& p=ports_[i];
            Json v{{"port",i},{"connected",p.data.connected},{"buttons",u32(p.data.buttons)},
                {"left_x",p.data.leftStick.x},{"left_y",p.data.leftStick.y},
                {"right_x",p.data.rightStick.x},{"right_y",p.data.rightStick.y},
                {"left_trigger",p.data.analogButtons.l2},{"right_trigger",p.data.analogButtons.r2},
                {"timestamp_us",p.data.timestamp},{"debug_active",bool(p.debug)}};
            if(p.debug_receipt) v["receipt"]=p.debug_receipt->JsonValue();
            j["ports"].push_back(v);
        }
        return j.dump()+"\n";
    }
    const bool state = !a.empty() && a[0]=="state";
    const bool release = !a.empty() && a[0]=="release_all";
    const bool query = !a.empty() && a[0]=="status";
    if ((!state && !release && !query) || a.size() != (state ? 18 : 6)) return error("invalid_arguments");
    u64 pid{},gen{},id{},port{},ms{},buttons{},touch{};
    if (!Uint(a[1],pid) || !Uint(a[2],gen) || !gen || !Owner(a[4]) || !Uint(a[5],id) || !id)
        return error("invalid_arguments");
    if (pid!=debug_->pid || gen!=debug_->generation || a[3]!=debug_->uuid)
        return error("wrong_session");
    std::string request;
    for(const auto& s:a) { request+=s; request+=' '; }
    for(const auto& r:debug_->receipts) {
        if(r->owner!=a[4] || r->id!=id) continue;
        TraceCommandLocked(TraceKind::CommandRequested, r->port, id, "duplicate");
        if(!query && r->request!=request) return error("id_conflict");
        auto j=reply(query ? "ok" : "dispatched");
        j.update(r->JsonValue()); j["duplicate"]=!query;
        return j.dump()+"\n";
    }
    if(query) return error("receipt_unavailable");
    if (!token_ || debug_->phase!=3) return error("not_running");
    if (const auto it=debug_->high_water.find(a[4]); it!=debug_->high_water.end() && id<=it->second)
        return error("stale_action_id");
    if (!debug_->high_water.contains(a[4]) && debug_->high_water.size()>=64) return error("owner_limit");
    if (!debug_->owner.empty() && debug_->owner!=a[4]) return error("busy");
    PadSnapshot value;
    if(state) {
        if(!Uint(a[6],port) || port>=kMaxPadPorts || !Uint(a[7],ms) || !ms || ms>2000 ||
           !Uint(a[8],buttons,true) || !Uint(a[15],touch) || touch>1 ||
           !Float(a[9],value.left_x,-1,1) || !Float(a[10],value.left_y,-1,1) ||
           !Float(a[11],value.right_x,-1,1) || !Float(a[12],value.right_y,-1,1) ||
           !Float(a[13],value.left_trigger,0,1) || !Float(a[14],value.right_trigger,0,1) ||
           !Float(a[16],value.touch_x,0,1) || !Float(a[17],value.touch_y,0,1)) return error("invalid_arguments");
        if(buttons & ~ButtonMask) return error("unsupported_buttons");
        if (!debug_->focused) return error("not_focused");
        if(ImGui::Core::IsGamepadInputCaptured()) return error("ui_captured");
        value.buttons=buttons; value.touch_down=touch;
    }
    TraceCommandLocked(TraceKind::CommandRequested, state ? int(port) : -1, id, "accepted");
    // Allocate the receipt before publishing; only accepted IDs enter the dedup ledger.
    auto receipt=std::make_shared<DebugReceipt>();
    receipt->owner=a[4]; receipt->id=id; receipt->request=request; receipt->port=state?int(port):-1;
    receipt->snapshot=value;
    debug_->high_water.try_emplace(a[4],0);
    debug_->receipts.push_back(receipt);
    if(state) {
        auto& p=ports_[port];
        if(!p.debug) {
            DeviceCapabilities caps;
            for(int i=0;i<6;++i) caps.axes.push_back({Axis(i),i<4?-1.f:0.f,1.f,0.f});
            const auto epoch=hub_.RegisterDevice(token_,Source::OnScreenOverlay,kMaxPadPorts+port,"debugbus",caps);
            if(!epoch) { debug_->receipts.pop_back(); return error("provider_rejected"); }
            p.debug=DeviceIdentity{Source::OnScreenOverlay,s64(kMaxPadPorts+port),epoch,"debugbus"};
        }
        InputPacket packet; packet.session_token=token_; packet.device=*p.debug; packet.sequence=++p.debug_sequence;
        for(size_t i=0;i<Bits.size();++i) packet.events.push_back({.button=Button(i),.pressed=bool(buttons&Bits[i])});
        const std::array<float,6> axes{value.left_x,value.left_y,value.right_x,value.right_y,value.left_trigger,value.right_trigger};
        for(int i=0;i<6;++i) packet.events.push_back({.kind=InputEvent::Kind::AxisValue,.axis=Axis(i),.raw_value=axes[i]});
        if(hub_.Submit(packet)!=InputResult::Ok) {
            debug_->receipts.pop_back(); ReleaseDebugLocked("provider_rejected",true); return error("provider_rejected");
        }
        if(p.debug_receipt) { p.debug_receipt->state="released"; p.debug_receipt->release_reason="superseded"; }
        p.debug_touch=value; p.debug_receipt=receipt; debug_->owner=a[4];
        receipt->deadline_ns=Ns()+ms*1'000'000; Publish(port);
        receipt->published_timestamp_us=p.data.timestamp;
    } else {
        ReleaseDebugLocked("release_all",false);
        receipt->state="released"; receipt->release_reason="release_all";
    }
    debug_->high_water[a[4]]=id;
    if(debug_->receipts.size()>256) debug_->receipts.pop_front();
    debug_->cv.notify_all();
    auto j=reply("dispatched"); j.update(receipt->JsonValue()); j["duplicate"]=false;
    return j.dump()+"\n";
}
} // namespace Core::HostRuntime
