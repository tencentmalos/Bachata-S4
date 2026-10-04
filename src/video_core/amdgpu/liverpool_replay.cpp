// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fmt/format.h>

#include "video_core/amdgpu/liverpool.h"
#include "video_core/replay/gpu_replay_file.h"

namespace AmdGpu {

void Liverpool::SaveReplayState(VideoCore::Replay::PayloadBuilder& state,
                                VideoCore::Replay::PayloadBuilder& ring_queues) {
    using namespace VideoCore::Replay;
    LiverpoolStateHeader header{};
    header.reg_bytes = sizeof(regs.reg_array);
    header.queue_count = NumTotalQueues;
    header.compute_state_bytes = sizeof(ComputeProgram);
    header.constants_bytes = static_cast<u32>(cblock.constants_heap.size());
    header.indirect_args_addr = indirect_args_addr;
    header.pixel_counter = pixel_counter;
    header.num_counter_pairs = num_counter_pairs;
    header.num_mapped_queues = num_mapped_queues.load();
    header.ce_count = cblock.ce_count;
    header.de_count = cblock.de_count;
    header.ce_compare_count = cblock.ce_compare_count;
    header.cb_extent_count = static_cast<u32>(last_cb_extent.size());
    header.flip_epoch = flip_epoch;
    state.Put(header);
    state.PutBytes(regs.reg_array.data(), sizeof(regs.reg_array));
    for (const auto& queue : mapped_queues) {
        state.Put(queue.cs_state);
    }
    state.PutBytes(cblock.constants_heap.data(), cblock.constants_heap.size());
    state.PutSpan(std::span<const CbDbExtent>{last_cb_extent});
    state.Put(last_db_extent);

    CountHeader count{};
    for (auto it = asc_queues.begin(); it != asc_queues.end(); ++it) {
        ++count.count;
    }
    ring_queues.Put(count);
    for (auto it = asc_queues.begin(); it != asc_queues.end(); ++it) {
        const AscQueueInfo& queue = *it;
        AscQueueRecord record{};
        record.slot = it.Id().index;
        record.pipe_id = queue.pipe_id;
        record.map_addr = queue.map_addr;
        record.read_addr = reinterpret_cast<u64>(queue.read_addr);
        record.ring_size_dw = queue.ring_size_dw;
        record.tmp_dwords = queue.tmp_dwords;
        ring_queues.Put(record);
        ring_queues.PutSpan(std::span<const u32>{queue.tmp_packet.data(), queue.tmp_dwords});
        ring_queues.Pad(8);
    }
}

bool Liverpool::LoadReplayState(std::span<const u8> state_bytes,
                                std::span<const u8> ring_bytes, std::string& error) {
    using namespace VideoCore::Replay;
    PayloadReader state{state_bytes};
    LiverpoolStateHeader header{};
    if (!state.Get(header) || header.reg_bytes != sizeof(regs.reg_array) ||
        header.queue_count != NumTotalQueues ||
        header.compute_state_bytes != sizeof(ComputeProgram) ||
        header.constants_bytes != cblock.constants_heap.size() ||
        header.cb_extent_count != last_cb_extent.size()) {
        error = "the command processor state does not match this build";
        return false;
    }
    bool ok = state.GetBytes(regs.reg_array.data(), sizeof(regs.reg_array));
    for (auto& queue : mapped_queues) {
        ok = ok && state.Get(queue.cs_state);
    }
    ok = ok && state.GetBytes(cblock.constants_heap.data(), cblock.constants_heap.size());
    for (auto& extent : last_cb_extent) {
        ok = ok && state.Get(extent);
    }
    ok = ok && state.Get(last_db_extent);
    if (!ok) {
        error = "truncated command processor state";
        return false;
    }
    indirect_args_addr = header.indirect_args_addr;
    pixel_counter = header.pixel_counter;
    num_counter_pairs = header.num_counter_pairs;
    num_mapped_queues = std::clamp<u32>(header.num_mapped_queues, 1, NumTotalQueues);
    cblock.ce_count = header.ce_count;
    cblock.de_count = header.de_count;
    cblock.ce_compare_count = header.ce_compare_count;
    flip_epoch = header.flip_epoch;

    // Compute rings go back to their slots: a submission names its ring by slot.
    PayloadReader rings{ring_bytes};
    CountHeader count{};
    if (!rings.Get(count)) {
        error = "truncated ring queue state";
        return false;
    }
    std::vector<Common::SlotId> fillers;
    for (u32 i = 0; i < count.count; ++i) {
        AscQueueRecord record{};
        if (!rings.Get(record)) {
            error = "truncated ring queue state";
            return false;
        }
        AscQueueInfo info{};
        info.map_addr = record.map_addr;
        info.read_addr = reinterpret_cast<u32*>(record.read_addr);
        info.ring_size_dw = record.ring_size_dw;
        info.pipe_id = record.pipe_id;
        info.tmp_dwords = record.tmp_dwords;
        if (record.tmp_dwords > info.tmp_packet.size() ||
            !rings.GetBytes(info.tmp_packet.data(), record.tmp_dwords * sizeof(u32))) {
            error = "bad ring queue record";
            return false;
        }
        rings.Align(8);
        while (true) {
            const auto id = asc_queues.insert(info);
            if (id.index == record.slot) {
                break;
            }
            fillers.push_back(id);
            if (fillers.size() > NumTotalQueues * 2) {
                error = fmt::format("cannot place ring queue slot {}", record.slot);
                return false;
            }
        }
    }
    for (const auto id : fillers) {
        asc_queues.erase(id);
    }
    return true;
}

} // namespace AmdGpu
