// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <fmt/format.h>

#include "common/path_util.h"
#include "core/diagnostics/trace_identity.h"
#include "video_core/amdgpu/pm4_trace.h"
#include "video_core/amdgpu/regs.h"

#if defined(__ANDROID__) || defined(__linux__)
#include <unistd.h>
#endif

namespace AmdGpu::Pm4Trace {
namespace {

// Record types. PM4-layer records go to .gpu.pm4.trace, action/host records to
// .gcmdtrace.ps4; Flip records go to both so each file carries its frame bounds.
enum RecordType : u16 {
    kSubmit = 1,
    kIbBegin = 2,
    kIbEnd = 3,
    kPacket = 4,
    kFlip = 5,
    kShader = 6,
    kAction = 16,
    kHost = 17,
};
constexpr u32 kFormatVersion = 1;
constexpr char kMagic[8] = {'P', 'S', '4', 'G', 'T', 'R', 'C', '1'};

// Record header: type, flags, payload bytes, sequence, CLOCK_MONOTONIC ns.
struct RecordHeader {
    u16 type;
    u16 flags;
    u32 size;
    u64 seq;
    u64 time_ns;
};
static_assert(sizeof(RecordHeader) == 24);

struct SubmitRecord {
    u64 submission;
    u32 queue, dcb_dwords;
    u64 dcb_va;
    u32 ccb_dwords, thread;
    u64 ccb_va;
};
struct IbBeginRecord {
    u64 ib, submission, va;
    u32 dwords, queue, kind, reserved;
};
struct PacketRecord {
    u64 ib, submission, va;
    u32 queue, dword_offset;
};
struct FlipRecord {
    u64 flip;
    u32 thread, reserved;
};
struct ShaderRecord {
    u64 hash, base;
    u32 stage, dwords;
};
struct ActionRecord {
    u64 action, submission, packet_va, ib;
    u32 queue, kind;
    u32 p0, p1, p2, p3;
    u64 p4;
    u32 stages, targets, images, buffers;
};
struct HostRecord {
    u64 action;
    u32 kind, text_bytes;
    u64 a, b, c, d;
};

// On-disk layout; tools/ps4-gpu-trace/ps4_gpu_trace.py mirrors these sizes.
static_assert(sizeof(SubmitRecord) == 40 && sizeof(IbBeginRecord) == 40);
static_assert(sizeof(PacketRecord) == 32 && sizeof(FlipRecord) == 16);
static_assert(sizeof(ShaderRecord) == 24);
static_assert(sizeof(ActionRecord) == 80 && sizeof(HostRecord) == 48);
static_assert(sizeof(ActionStage) == 24 && sizeof(ActionTarget) == 40);
static_assert(sizeof(ActionImage) == 48 && sizeof(ActionBuffer) == 24);

enum class State { Idle, Armed, Capturing, Writing, Ready, Cancelled, Failed };
const char* StateName(State s) {
    switch (s) {
    case State::Idle: return "idle";
    case State::Armed: return "armed";
    case State::Capturing: return "capturing";
    case State::Writing: return "writing";
    case State::Ready: return "ready";
    case State::Cancelled: return "cancelled";
    case State::Failed: return "failed";
    }
    return "unknown";
}

u64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
u32 ThreadId() {
#if defined(__ANDROID__) || defined(__linux__)
    return static_cast<u32>(gettid());
#else
    return 0;
#endif
}

struct Capture {
    std::mutex mutex;
    State state{State::Idle};
    std::string uuid, failure, partial;
    Identity identity;
    u32 frames{}, delay{}, skipped{}, captured_frames{};
    u64 max_bytes{}, armed_ns{}, start_ns{}, end_ns{}, first_flip{}, last_flip{};
    u64 seq{};
    std::vector<u8> buffer;
    std::array<u64, 18> counts{};
    std::unordered_set<u64> shaders;
    // Consumer-side context of the packet being processed (single PM4 consumer).
    u64 cur_submission{}, cur_ib{}, cur_va{};
    u32 cur_queue{};
    // Results.
    std::string pm4_path, gcmd_path;
    u64 pm4_bytes{}, gcmd_bytes{}, pm4_fnv{}, gcmd_fnv{};
    std::thread writer;
} capture;

std::atomic<u64> ib_counter{0};
std::atomic<u64> action_counter{0};
std::atomic<u64> current_action{0};

constexpr u64 kArmTimeoutNs = 30'000'000'000ull;
constexpr u64 kCaptureTimeoutNs = 20'000'000'000ull;

void Finish(std::string partial);

// Caller holds capture.mutex and has checked state == Capturing.
void AppendLocked(u16 type, const void* a, size_t a_bytes, const void* b = nullptr,
                  size_t b_bytes = 0, const void* c = nullptr, size_t c_bytes = 0) {
    const size_t payload = a_bytes + b_bytes + c_bytes;
    const size_t need = sizeof(RecordHeader) + payload;
    if (capture.buffer.size() + need > capture.max_bytes) {
        Finish("byte budget");
        return;
    }
    const u64 now = NowNs();
    if (now - capture.start_ns > kCaptureTimeoutNs) {
        Finish("time budget");
        return;
    }
    const RecordHeader header{type, 0, static_cast<u32>(payload), capture.seq++, now};
    const size_t at = capture.buffer.size();
    capture.buffer.resize(at + need);
    u8* out = capture.buffer.data() + at;
    std::memcpy(out, &header, sizeof(header));
    out += sizeof(header);
    if (a_bytes) std::memcpy(out, a, a_bytes), out += a_bytes;
    if (b_bytes) std::memcpy(out, b, b_bytes), out += b_bytes;
    if (c_bytes) std::memcpy(out, c, c_bytes);
    ++capture.counts[std::min<size_t>(type, capture.counts.size() - 1)];
}

// ---- register map written into the trace header ----
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif
#define REG(field) {#field, offsetof(Regs, field) / 4, sizeof(Regs::field) / 4, sizeof(Regs::field) / 4}
#define REG_ARRAY(field, n) {#field, offsetof(Regs, field) / 4, sizeof(Regs::field) / 4, sizeof(Regs::field) / 4 / (n)}
struct RegEntry {
    const char* name;
    size_t offset, words, element_words;
};
const RegEntry kRegs[] = {
    REG(ps_program), REG(vs_program), REG(gs_program), REG(es_program), REG(hs_program),
    REG(ls_program), REG(cs_program), REG(depth_render_control), REG(depth_view),
    REG(depth_render_override), REG(depth_htile_data_base), REG(depth_bounds_min),
    REG(depth_bounds_max), REG(stencil_clear), REG(depth_clear), REG(screen_scissor),
    REG(depth_buffer), REG(ta_bc_base), REG(window_offset), REG(window_scissor),
    REG(color_target_mask), REG(color_shader_mask), REG(generic_scissor),
    REG_ARRAY(viewport_scissors, NUM_VIEWPORTS), REG_ARRAY(viewport_depths, NUM_VIEWPORTS),
    REG(index_offset), REG(primitive_restart_index), REG(blend_constants),
    REG(stencil_control), REG(stencil_ref_front), REG(stencil_ref_back),
    REG_ARRAY(viewports, NUM_VIEWPORTS), REG_ARRAY(clip_user_data, NUM_CLIP_PLANES),
    REG_ARRAY(ps_inputs, 32), REG(vs_output_config), REG(ps_input_ena), REG(ps_input_addr),
    REG(shader_pos_format), REG(z_export_format), REG(color_export_format),
    REG_ARRAY(blend_control, NUM_COLOR_BUFFERS), REG(index_base_address), REG(draw_initiator),
    REG(depth_control), REG(color_control), REG(depth_shader_control), REG(clipper_control),
    REG(polygon_control), REG(viewport_control), REG(vs_output_control), REG(line_control),
    REG(hs_clamp), REG(vgt_gs_mode), REG(vgt_gs_onchip_control), REG(mode_control),
    REG(vgt_gsvs_ring_offset_1), REG(vgt_gsvs_ring_offset_2), REG(vgt_gsvs_ring_offset_3),
    REG(vgt_gs_out_prim_type), REG(index_size), REG(max_index_size), REG(index_buffer_type),
    REG(enable_primitive_id), REG(enable_primitive_restart), REG(vgt_instance_step_rate_0),
    REG(vgt_instance_step_rate_1), REG(vgt_esgs_ring_itemsize), REG(vgt_gsvs_ring_itemsize),
    REG(stage_enable), REG(ls_hs_config), REG_ARRAY(vgt_gs_vert_itemsize, 4),
    REG(tess_config), REG(poly_offset), REG(vgt_gs_instance_cnt), REG(vgt_strmout_config),
    REG(vgt_strmout_buffer_config), REG(aa_config), REG_ARRAY(color_buffers, NUM_COLOR_BUFFERS),
    REG(cp_strmout_cntl), REG(vgt_esgs_ring_size), REG(vgt_gsvs_ring_size),
    REG(primitive_type), REG(num_indices), REG(num_instances), REG(vgt_tf_memory_base),
};
#undef REG
#undef REG_ARRAY
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

std::string HeaderJson(std::string_view layer) {
    using Core::Diagnostics::JsonEscape;
    std::string j = "{";
    j += fmt::format("\"schema\":\"{}\",\"format_version\":{},", JsonEscape(layer), kFormatVersion);
    j += fmt::format("\"capture_uuid\":\"{}\",\"run_uuid\":\"{}\",\"pid\":\"{}\","
                     "\"session_generation\":\"{}\",\"driver\":\"{}\",",
                     capture.uuid, JsonEscape(capture.identity.run_uuid), capture.identity.pid,
                     capture.identity.generation, JsonEscape(capture.identity.driver));
    j += fmt::format("\"clock_domain\":\"CLOCK_MONOTONIC\",\"clock_units\":\"ns\","
                     "\"frames_requested\":{},\"frames_captured\":{},\"delay\":{},"
                     "\"max_bytes\":\"{}\",\"first_flip\":\"{}\",\"last_flip\":\"{}\","
                     "\"start_ns\":\"{}\",\"end_ns\":\"{}\",\"partial\":\"{}\",",
                     capture.frames, capture.captured_frames, capture.delay, capture.max_bytes,
                     capture.first_flip, capture.last_flip, capture.start_ns, capture.end_ns,
                     JsonEscape(capture.partial));
    j += "\"register_bases\":{\"config\":8192,\"sh\":11264,\"context\":40960,\"uconfig\":49152},";
    j += "\"registers\":[";
    bool first = true;
    for (const auto& r : kRegs) {
        j += fmt::format("{}[\"{}\",{},{},{}]", first ? "" : ",", r.name, r.offset, r.words,
                         r.element_words);
        first = false;
    }
    j += "]}";
    return j;
}

u64 Fnv1a(const u8* data, size_t size, u64 h = 1469598103934665603ull) {
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Worker: split the record stream into the two layer files. Runs without the lock.
void WriteFiles(std::vector<u8> buffer, std::string pm4_header, std::string gcmd_header,
                std::filesystem::path dir, std::string uuid) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto pm4_path = dir / (uuid + ".gpu.pm4.trace");
    const auto gcmd_path = dir / (uuid + ".gcmdtrace.ps4");
    std::string failure;
    u64 sizes[2]{}, fnv[2]{};
    {
        std::ofstream pm4(pm4_path, std::ios::binary | std::ios::trunc);
        std::ofstream gcmd(gcmd_path, std::ios::binary | std::ios::trunc);
        if (!pm4 || !gcmd) {
            failure = "cannot create output files in " + dir.string();
        } else {
            u64 h[2] = {1469598103934665603ull, 1469598103934665603ull};
            const auto put = [&](int which, std::ofstream& f, const void* p, size_t n) {
                f.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
                h[which] = Fnv1a(static_cast<const u8*>(p), n, h[which]);
                sizes[which] += n;
            };
            const auto write_header = [&](int which, std::ofstream& f, const std::string& json) {
                const u32 layer = which == 0 ? 1u : 2u;
                const u32 json_bytes = static_cast<u32>(json.size());
                put(which, f, kMagic, sizeof(kMagic));
                put(which, f, &kFormatVersion, sizeof(u32));
                put(which, f, &layer, sizeof(u32));
                put(which, f, &json_bytes, sizeof(u32));
                put(which, f, json.data(), json.size());
            };
            write_header(0, pm4, pm4_header);
            write_header(1, gcmd, gcmd_header);
            size_t at = 0;
            while (at + sizeof(RecordHeader) <= buffer.size()) {
                RecordHeader rh;
                std::memcpy(&rh, buffer.data() + at, sizeof(rh));
                const size_t n = sizeof(rh) + rh.size;
                if (at + n > buffer.size())
                    break;
                const u8* rec = buffer.data() + at;
                if (rh.type == kFlip) {
                    put(0, pm4, rec, n);
                    put(1, gcmd, rec, n);
                } else if (rh.type >= kAction) {
                    put(1, gcmd, rec, n);
                } else {
                    put(0, pm4, rec, n);
                }
                at += n;
            }
            fnv[0] = h[0];
            fnv[1] = h[1];
            pm4.flush();
            gcmd.flush();
            if (!pm4 || !gcmd)
                failure = "write failed";
        }
    }
    std::scoped_lock lk{capture.mutex};
    if (capture.uuid != uuid || capture.state != State::Writing)
        return; // cancelled or re-armed meanwhile
    if (!failure.empty()) {
        capture.state = State::Failed;
        capture.failure = failure;
        return;
    }
    capture.pm4_path = pm4_path.string();
    capture.gcmd_path = gcmd_path.string();
    capture.pm4_bytes = sizes[0];
    capture.gcmd_bytes = sizes[1];
    capture.pm4_fnv = fnv[0];
    capture.gcmd_fnv = fnv[1];
    capture.state = State::Ready;
}

// Caller holds the lock; state is Capturing.
void Finish(std::string partial) {
    capturing.store(false, std::memory_order_relaxed);
    capture.partial = std::move(partial);
    capture.end_ns = NowNs();
    capture.state = State::Writing;
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::CapturesDir) / "gpu-trace";
    auto pm4_header = HeaderJson("ps4.pm4.command");
    auto gcmd_header = HeaderJson("ps4.guest-command");
    if (capture.writer.joinable())
        capture.writer.detach(); // a previous writer finishes on its own and checks the uuid
    capture.writer = std::thread{WriteFiles, std::move(capture.buffer), std::move(pm4_header),
                                 std::move(gcmd_header), dir, capture.uuid};
    capture.buffer = {};
}

std::string Status(u64 now) {
    std::string out;
    if (capture.state == State::Armed && now - capture.armed_ns > kArmTimeoutNs) {
        capture.state = State::Failed;
        capture.failure = "no guest flip within 30 s of arming";
    }
    out += fmt::format("state: {}\n", StateName(capture.state));
    if (capture.uuid.empty())
        return out;
    out += fmt::format("capture_uuid: {}\n", capture.uuid);
    out += fmt::format("frames: {}/{}  delay: {}/{}\n", capture.captured_frames, capture.frames,
                       capture.skipped, capture.delay);
    out += fmt::format("flips: {}..{}\n", capture.first_flip, capture.last_flip);
    const u64 bytes = capture.state == State::Capturing ? capture.buffer.size() : 0;
    out += fmt::format(
        "records: submit={} ib={} packet={} flip={} shader={} action={} host={} seq={}\n",
        capture.counts[kSubmit], capture.counts[kIbBegin], capture.counts[kPacket],
        capture.counts[kFlip], capture.counts[kShader], capture.counts[kAction],
        capture.counts[kHost], capture.seq);
    if (capture.state == State::Capturing)
        out += fmt::format("buffer_bytes: {} of {}\n", bytes, capture.max_bytes);
    if (!capture.partial.empty())
        out += fmt::format("ended_by: {}\n", capture.partial);
    if (!capture.failure.empty())
        out += fmt::format("failure: {}\n", capture.failure);
    if (capture.state == State::Ready) {
        out += fmt::format("pm4_file: {} bytes={} fnv1a64={:016x}\n", capture.pm4_path,
                           capture.pm4_bytes, capture.pm4_fnv);
        out += fmt::format("gcmd_file: {} bytes={} fnv1a64={:016x}\n", capture.gcmd_path,
                           capture.gcmd_bytes, capture.gcmd_fnv);
    }
    if (capture.start_ns)
        out += fmt::format("capture_ms: {:.1f}\n",
                           ((capture.end_ns ? capture.end_ns : now) - capture.start_ns) / 1e6);
    return out;
}

bool ParseU64(const std::string& s, u64& out) {
    if (s.empty())
        return false;
    u64 v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + static_cast<u64>(c - '0');
        if (v > (1ull << 40))
            return false;
    }
    out = v;
    return true;
}

} // namespace

u64 NextIb() noexcept {
    return ib_counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

void NoteSubmit(u32 queue, u64 submission, u64 dcb_va, u32 dcb_dwords, u64 ccb_va,
                u32 ccb_dwords) {
    std::scoped_lock lk{capture.mutex};
    if (capture.state != State::Capturing)
        return;
    const SubmitRecord r{submission, queue, dcb_dwords, dcb_va, ccb_dwords, ThreadId(), ccb_va};
    AppendLocked(kSubmit, &r, sizeof(r));
}

void NoteIbBegin(u64 ib, u64 submission, u32 queue, IbKind kind, u64 va, u32 dwords) {
    std::scoped_lock lk{capture.mutex};
    if (capture.state != State::Capturing)
        return;
    const IbBeginRecord r{ib, submission, va, dwords, queue, static_cast<u32>(kind), 0};
    AppendLocked(kIbBegin, &r, sizeof(r));
}

void NoteIbEnd(u64 ib) {
    std::scoped_lock lk{capture.mutex};
    if (capture.state != State::Capturing)
        return;
    AppendLocked(kIbEnd, &ib, sizeof(ib));
}

void NotePacket(u64 ib, u64 submission, u32 queue, u32 dword_offset, u64 va,
                std::span<const u32> dwords) {
    std::scoped_lock lk{capture.mutex};
    capture.cur_submission = submission;
    capture.cur_ib = ib;
    capture.cur_va = va;
    capture.cur_queue = queue;
    if (capture.state != State::Capturing)
        return;
    const PacketRecord r{ib, submission, va, queue, dword_offset};
    AppendLocked(kPacket, &r, sizeof(r), dwords.data(), dwords.size_bytes());
}

void NoteFlip(u64 flip) {
    std::scoped_lock lk{capture.mutex};
    const FlipRecord r{flip, ThreadId(), 0};
    if (capture.state == State::Armed) {
        if (capture.skipped < capture.delay) {
            ++capture.skipped;
            return;
        }
        capture.state = State::Capturing;
        capture.start_ns = NowNs();
        capture.first_flip = capture.last_flip = flip;
        capturing.store(true, std::memory_order_relaxed);
        AppendLocked(kFlip, &r, sizeof(r));
        return;
    }
    if (capture.state != State::Capturing)
        return;
    capture.last_flip = flip;
    AppendLocked(kFlip, &r, sizeof(r));
    if (capture.state == State::Capturing && ++capture.captured_frames >= capture.frames)
        Finish("");
}

bool WantShader(u64 hash) {
    std::scoped_lock lk{capture.mutex};
    return capture.state == State::Capturing && !capture.shaders.contains(hash);
}

void NoteShader(u64 hash, u64 base, u32 stage, std::span<const u32> code) {
    std::scoped_lock lk{capture.mutex};
    if (capture.state != State::Capturing || !capture.shaders.insert(hash).second)
        return;
    code = code.first(std::min<size_t>(code.size(), 64 * 1024 / 4));
    const ShaderRecord r{hash, base, stage, static_cast<u32>(code.size())};
    AppendLocked(kShader, &r, sizeof(r), code.data(), code.size_bytes());
}

u64 BeginAction() {
    const u64 id = action_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    current_action.store(id, std::memory_order_relaxed);
    return id;
}

void NoteAction(const Action& action) {
    std::scoped_lock lk{capture.mutex};
    if (capture.state != State::Capturing)
        return;
    const ActionRecord r{
        current_action.load(std::memory_order_relaxed),
        capture.cur_submission,
        capture.cur_va,
        capture.cur_ib,
        capture.cur_queue,
        static_cast<u32>(action.kind),
        action.p0,
        action.p1,
        action.p2,
        action.p3,
        action.p4,
        static_cast<u32>(action.stages.size()),
        static_cast<u32>(action.targets.size()),
        static_cast<u32>(action.images.size()),
        static_cast<u32>(action.buffers.size()),
    };
    // Assemble the variable part contiguously, then one record.
    static std::vector<u8> tail;
    tail.clear();
    const auto add = [&](const auto& v) {
        const size_t at = tail.size();
        tail.resize(at + v.size() * sizeof(v[0]));
        if (!v.empty())
            std::memcpy(tail.data() + at, v.data(), v.size() * sizeof(v[0]));
    };
    add(action.stages);
    add(action.targets);
    add(action.images);
    add(action.buffers);
    AppendLocked(kAction, &r, sizeof(r), tail.data(), tail.size());
}

void NoteHost(HostEvent kind, u64 a, u64 b, u64 c, u64 d, std::string_view text) {
    std::scoped_lock lk{capture.mutex};
    if (capture.state != State::Capturing)
        return;
    const u32 text_bytes = static_cast<u32>(std::min<size_t>(text.size(), 4096));
    const HostRecord r{current_action.load(std::memory_order_relaxed),
                       static_cast<u32>(kind),
                       text_bytes,
                       a,
                       b,
                       c,
                       d};
    AppendLocked(kHost, &r, sizeof(r), text.data(), text_bytes);
}

std::string Command(const std::vector<std::string>& args, const Identity& identity, u64 now_ns) {
    const std::string sub = args.empty() ? "status" : args[0];
    std::scoped_lock lk{capture.mutex};
    if (sub == "status")
        return Status(now_ns);
    if (sub == "cancel") {
        if (capture.state == State::Armed || capture.state == State::Capturing ||
            capture.state == State::Writing) {
            capturing.store(false, std::memory_order_relaxed);
            capture.buffer = {};
            capture.state = State::Cancelled;
        }
        return Status(now_ns);
    }
    if (sub == "save") {
        if (capture.state == State::Capturing)
            Finish("save requested");
        else if (capture.state == State::Armed)
            return "status: nothing_captured_yet\n" + Status(now_ns);
        return Status(now_ns);
    }
    if (sub != "arm")
        return "usage: status | arm [frames=2] [delay=0] [max_mib=64] | save | cancel\n";
    if (capture.state == State::Armed || capture.state == State::Capturing ||
        capture.state == State::Writing)
        return "status: busy\n" + Status(now_ns);
    u64 frames = 2, delay = 0, max_mib = 64;
    if ((args.size() > 1 && !ParseU64(args[1], frames)) ||
        (args.size() > 2 && !ParseU64(args[2], delay)) ||
        (args.size() > 3 && !ParseU64(args[3], max_mib)) || args.size() > 4 || frames < 1 ||
        frames > 16 || delay > 600 || max_mib < 1 || max_mib > 512)
        return "status: invalid_arguments (frames 1-16, delay 0-600, max_mib 1-512)\n";
    if (capture.writer.joinable())
        capture.writer.detach();
    capture.uuid = Core::Diagnostics::MakeRunUuid();
    capture.identity = identity;
    capture.frames = static_cast<u32>(frames);
    capture.delay = static_cast<u32>(delay);
    capture.skipped = capture.captured_frames = 0;
    capture.max_bytes = max_mib << 20;
    capture.armed_ns = now_ns;
    capture.start_ns = capture.end_ns = capture.first_flip = capture.last_flip = 0;
    capture.seq = 0;
    capture.counts = {};
    capture.shaders.clear();
    capture.partial.clear();
    capture.failure.clear();
    capture.pm4_path.clear();
    capture.gcmd_path.clear();
    capture.buffer.clear();
    capture.buffer.reserve(std::min<u64>(capture.max_bytes, 16ull << 20));
    capture.state = State::Armed;
    return Status(now_ns);
}

} // namespace AmdGpu::Pm4Trace
