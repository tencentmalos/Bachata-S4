// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bitset>
#include <mutex>
#include <unordered_map>
#include <xxhash.h>
#include <fmt/format.h>

#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/pm4_opcodes.h"
#include "video_core/amdgpu/pm4_stats.h"
#include "video_core/amdgpu/regs.h"

namespace AmdGpu::Pm4Stats {
namespace {

constexpr u32 RegWindow = 0x400;  // context and SH register windows are 1024 dwords each
constexpr u32 MaxIbDepth = 4;
constexpr u32 MaxFrameDcbs = 256; // per-DCB rows kept for the detailed frames

struct DcbRow {
    u32 call{}, index{}, dwords{}, draws{}, dispatches{}, ib_calls{};
    bool ccb{}, clear_state_before_draw{};
    u32 ctx_before_first_draw{}, sh_before_first_draw{}, ctx_total{};
};

struct Walk {
    DcbRow row;
    std::bitset<RegWindow> ctx_pre, sh_pre, ctx_all;
    bool seen_draw{};
    bool truncated{};
};

void WalkPackets(Walk& w, std::span<const u32> cb, u32 depth) {
    while (!cb.empty()) {
        const auto* header = reinterpret_cast<const PM4Header*>(cb.data());
        u32 packet_dw = 1;
        if (header->type == 3) {
            const u32 count = header->type3.NumWords();
            packet_dw = count + 1;
            if (packet_dw > cb.size()) {
                w.truncated = true;
                return;
            }
            const auto* body = cb.data() + 1;
            const PM4ItOpcode opcode = header->type3.opcode;
            switch (opcode) {
            case PM4ItOpcode::ClearState:
                if (!w.seen_draw)
                    w.row.clear_state_before_draw = true;
                break;
            case PM4ItOpcode::SetContextReg:
            case PM4ItOpcode::SetShReg: {
                const bool ctx = opcode == PM4ItOpcode::SetContextReg;
                const u32 first = body[0] & 0xffff;
                for (u32 i = 0; i + 1 < count && first + i < RegWindow; ++i) {
                    if (ctx) {
                        w.ctx_all.set(first + i);
                        if (!w.seen_draw)
                            w.ctx_pre.set(first + i);
                    } else if (!w.seen_draw) {
                        w.sh_pre.set(first + i);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndex2:
            case PM4ItOpcode::DrawIndexOffset2:
            case PM4ItOpcode::DrawIndexAuto:
            case PM4ItOpcode::DrawIndirect:
            case PM4ItOpcode::DrawIndirectMulti:
            case PM4ItOpcode::DrawIndexIndirect:
            case PM4ItOpcode::DrawIndexIndirectMulti:
            case PM4ItOpcode::DrawIndexIndirectCountMulti:
                ++w.row.draws;
                w.seen_draw = true;
                break;
            case PM4ItOpcode::DispatchDirect:
            case PM4ItOpcode::DispatchIndirect:
                ++w.row.dispatches;
                w.seen_draw = true;
                break;
            case PM4ItOpcode::IndirectBuffer: {
                ++w.row.ib_calls;
                const auto* ib = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                if (depth < MaxIbDepth && ib->ib_size != 0)
                    WalkPackets(w, {ib->Address<const u32>(), ib->ib_size}, depth + 1);
                break;
            }
            default:
                break;
            }
        }
        w.row.dwords += packet_dw;
        cb = cb.subspan(packet_dw);
    }
}

struct State {
    std::mutex mutex;
    u32 frames_left{};       // detailed frames still to print
    u32 frames{};            // frames observed while armed
    u32 calls{}, dcbs{}, draws{}, dispatches{}, ib_calls{}, truncated{};
    u32 dcbs_with_clear_state{};
    u64 dwords{};
    u32 frame_calls{};
    std::vector<DcbRow> frame_rows;
    std::bitset<RegWindow> frame_ctx_union;
    std::string report;
} g;

// Stream-copy census, touched only by the PM4 owner thread while armed.
struct StreamEntry {
    u64 epoch{};
    u64 hash{};
};
struct StreamState {
    std::mutex mutex;
    std::unordered_map<u64, StreamEntry> last; // key: address ^ (size << 48)
    u64 first_epoch{}, last_epoch{};
    u64 copies{}, bytes{};
    u64 same_frame_same{}, same_frame_same_bytes{};   // reusable within the frame
    u64 cross_frame_same{}, cross_frame_same_bytes{}; // first use this frame, unchanged since last
    u64 changed{}, changed_bytes{}, first_seen{};
} s_stream;

struct HleState {
    std::mutex mutex;
    u64 first_epoch{}, last_epoch{}, prev_epoch{};
    u64 dispatches{}, regions{}, bytes{};
    u64 size_hist[5]{}; // <=64, <=256, <=4K, <=64K, larger
    u64 adjacent{};      // region i+1 continues region i in both src and dst
    u64 sorted_adjacent{}; // same after sorting by src
    u64 chained{};       // dispatch whose src/dst bases equal the previous dispatch's
    u64 prev_src{}, prev_dst{};
    u64 max_regions{};
} s_hle;

void FinishFrame() {
    ++g.frames;
    if (g.frames_left) {
        --g.frames_left;
        const u32 union_ctx = static_cast<u32>(g.frame_ctx_union.count());
        g.report += fmt::format("frame {} calls={} dcbs={} ctx_regs_union={}\n", g.frames,
                                g.frame_calls, g.frame_rows.size(), union_ctx);
        for (const auto& r : g.frame_rows) {
            g.report += fmt::format(
                "  call={} dcb={} dwords={} draws={} dispatches={} ib={} ccb={} clear_state={} "
                "ctx_before_first_draw={} sh_before_first_draw={} ctx_total={}\n",
                r.call, r.index, r.dwords, r.draws, r.dispatches, r.ib_calls, r.ccb ? 1 : 0,
                r.clear_state_before_draw ? 1 : 0, r.ctx_before_first_draw, r.sh_before_first_draw,
                r.ctx_total);
        }
    }
    g.frame_calls = 0;
    g.frame_rows.clear();
    g.frame_ctx_union.reset();
}

} // namespace

void NoteDcb(u32 index_in_call, std::span<const u32> dcb, bool has_ccb) {
    if (!armed.load(std::memory_order_relaxed))
        return;
    Walk w;
    WalkPackets(w, dcb, 0);
    w.row.index = index_in_call;
    w.row.ccb = has_ccb;
    w.row.ctx_before_first_draw = static_cast<u32>(w.ctx_pre.count());
    w.row.sh_before_first_draw = static_cast<u32>(w.sh_pre.count());
    w.row.ctx_total = static_cast<u32>(w.ctx_all.count());

    std::scoped_lock lk{g.mutex};
    w.row.call = g.frame_calls;
    ++g.dcbs;
    g.draws += w.row.draws;
    g.dispatches += w.row.dispatches;
    g.ib_calls += w.row.ib_calls;
    g.dwords += w.row.dwords;
    g.truncated += w.truncated ? 1 : 0;
    g.dcbs_with_clear_state += w.row.clear_state_before_draw ? 1 : 0;
    g.frame_ctx_union |= w.ctx_all;
    if (g.frame_rows.size() < MaxFrameDcbs)
        g.frame_rows.push_back(w.row);
}

void NoteStreamCopy(u64 epoch, u64 address, u32 size, const void* data) {
    if (!armed.load(std::memory_order_relaxed))
        return;
    const u64 hash = XXH3_64bits(data, size);
    std::scoped_lock lk{s_stream.mutex};
    auto& st = s_stream;
    if (st.copies == 0)
        st.first_epoch = epoch;
    st.last_epoch = epoch;
    ++st.copies;
    st.bytes += size;
    auto [it, inserted] = st.last.try_emplace(address ^ (u64(size) << 48), StreamEntry{epoch, hash});
    if (inserted) {
        ++st.first_seen;
        return;
    }
    if (it->second.hash != hash) {
        ++st.changed;
        st.changed_bytes += size;
    } else if (it->second.epoch == epoch) {
        ++st.same_frame_same;
        st.same_frame_same_bytes += size;
    } else {
        ++st.cross_frame_same;
        st.cross_frame_same_bytes += size;
    }
    it->second = {epoch, hash};
}

void NoteHleCopy(u64 epoch, u64 src_base, u64 dst_base, std::span<const HleRegion> regions) {
    if (!armed.load(std::memory_order_relaxed))
        return;
    std::scoped_lock lk{s_hle.mutex};
    auto& h = s_hle;
    if (h.dispatches == 0)
        h.first_epoch = epoch;
    h.last_epoch = epoch;
    ++h.dispatches;
    h.regions += regions.size();
    h.max_regions = std::max<u64>(h.max_regions, regions.size());
    if (h.prev_epoch == epoch && h.prev_src == src_base && h.prev_dst == dst_base)
        ++h.chained;
    h.prev_epoch = epoch;
    h.prev_src = src_base;
    h.prev_dst = dst_base;
    for (size_t i = 0; i < regions.size(); ++i) {
        const u64 sz = regions[i].size;
        h.bytes += sz;
        ++h.size_hist[sz <= 64 ? 0 : sz <= 256 ? 1 : sz <= 4096 ? 2 : sz <= 65536 ? 3 : 4];
        if (i + 1 < regions.size() && regions[i].src + sz == regions[i + 1].src &&
            regions[i].dst + sz == regions[i + 1].dst)
            ++h.adjacent;
    }
    std::vector<HleRegion> sorted(regions.begin(), regions.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.src < b.src; });
    for (size_t i = 0; i + 1 < sorted.size(); ++i)
        if (sorted[i].src + sorted[i].size == sorted[i + 1].src &&
            sorted[i].dst + sorted[i].size == sorted[i + 1].dst)
            ++h.sorted_adjacent;
}

void EndSubmitCall() {
    if (!armed.load(std::memory_order_relaxed))
        return;
    std::scoped_lock lk{g.mutex};
    ++g.calls;
    ++g.frame_calls;
}

void NoteFlip() {
    if (!armed.load(std::memory_order_relaxed))
        return;
    std::scoped_lock lk{g.mutex};
    FinishFrame();
}

std::string Command(const std::vector<std::string>& args) {
    const std::string cmd = args.empty() ? "status" : args[0];
    std::scoped_lock lk{g.mutex};
    if (cmd == "start") {
        g.frames = g.calls = g.dcbs = g.draws = g.dispatches = g.ib_calls = g.truncated = 0;
        g.dcbs_with_clear_state = g.frame_calls = 0;
        g.dwords = 0;
        g.frame_rows.clear();
        g.frame_ctx_union.reset();
        g.report.clear();
        {
            std::scoped_lock hlk{s_hle.mutex};
            s_hle.first_epoch = s_hle.last_epoch = s_hle.prev_epoch = 0;
            s_hle.dispatches = s_hle.regions = s_hle.bytes = 0;
            for (auto& v : s_hle.size_hist)
                v = 0;
            s_hle.adjacent = s_hle.sorted_adjacent = s_hle.chained = 0;
            s_hle.prev_src = s_hle.prev_dst = s_hle.max_regions = 0;
        }
        {
            std::scoped_lock slk{s_stream.mutex};
            s_stream.last.clear();
            s_stream.first_epoch = s_stream.last_epoch = s_stream.copies = s_stream.bytes = 0;
            s_stream.same_frame_same = s_stream.same_frame_same_bytes = 0;
            s_stream.cross_frame_same = s_stream.cross_frame_same_bytes = 0;
            s_stream.changed = s_stream.changed_bytes = s_stream.first_seen = 0;
        }
        g.frames_left = args.size() > 1 ? static_cast<u32>(std::stoul(args[1])) : 3;
        armed.store(true, std::memory_order_relaxed);
        return "pm4_stats armed\n";
    }
    if (cmd == "hle_merge" && args.size() > 1) {
        hle_merge_off.store(args[1] == "off", std::memory_order_relaxed);
        return fmt::format("hle_merge {}\n", args[1] == "off" ? "off" : "on");
    }
    if (cmd == "stop") {
        armed.store(false, std::memory_order_relaxed);
        return "pm4_stats stopped\n";
    }
    if (cmd != "status")
        return "usage: pm4_stats start [detailed_frames] | status | stop | hle_merge on|off\n";
    const double f = std::max(1u, g.frames);
    std::string out = fmt::format(
        "armed={} frames={} per_frame: calls={:.2f} dcbs={:.2f} draws={:.1f} dispatches={:.1f} "
        "ib_calls={:.1f} kdwords={:.1f} dcbs_with_clear_state={:.2f} truncated={}\n",
        armed.load() ? 1 : 0, g.frames, g.calls / f, g.dcbs / f, g.draws / f, g.dispatches / f,
        g.ib_calls / f, g.dwords / f / 1000.0, g.dcbs_with_clear_state / f, g.truncated);
    {
        std::scoped_lock slk{s_stream.mutex};
        const auto& st = s_stream;
        const double sf = std::max<u64>(1, st.last_epoch - st.first_epoch);
        out += fmt::format(
            "stream_copies per_frame(flips={}): copies={:.1f} kbytes={:.1f} keys={} "
            "same_frame_unchanged={:.1f} ({:.1f} KB) cross_frame_unchanged={:.1f} ({:.1f} KB) "
            "changed={:.1f} ({:.1f} KB) first_seen_total={}\n",
            st.last_epoch - st.first_epoch, st.copies / sf, st.bytes / sf / 1024.0, st.last.size(),
            st.same_frame_same / sf, st.same_frame_same_bytes / sf / 1024.0, st.cross_frame_same / sf,
            st.cross_frame_same_bytes / sf / 1024.0, st.changed / sf, st.changed_bytes / sf / 1024.0,
            st.first_seen);
    }
    {
        std::scoped_lock hlk{s_hle.mutex};
        const auto& h = s_hle;
        const double hf = std::max<u64>(1, h.last_epoch - h.first_epoch);
        out += fmt::format(
            "hle_copies per_frame: dispatches={:.1f} regions={:.1f} kbytes={:.1f} max_regions={} "
            "sizes(<=64/<=256/<=4K/<=64K/more)={:.1f}/{:.1f}/{:.1f}/{:.1f}/{:.1f} "
            "adjacent={:.1f} sorted_adjacent={:.1f} same_base_consecutive={:.1f}\n",
            h.dispatches / hf, h.regions / hf, h.bytes / hf / 1024.0, h.max_regions,
            h.size_hist[0] / hf, h.size_hist[1] / hf, h.size_hist[2] / hf, h.size_hist[3] / hf,
            h.size_hist[4] / hf, h.adjacent / hf, h.sorted_adjacent / hf, h.chained / hf);
    }
    return out + g.report;
}

} // namespace AmdGpu::Pm4Stats
