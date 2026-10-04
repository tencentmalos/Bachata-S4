// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <sstream>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/replay/gpu_replay_player.h"

namespace VideoCore::Replay {

namespace {

Player* draw_hash_player{};

struct Area {
    VmaRecord vma{};
    std::vector<PhysRecord> phys;
    std::string name;
};

bool ReadAreas(PayloadReader& reader, std::vector<Area>& areas) {
    CountHeader count{};
    if (!reader.Get(count)) {
        return false;
    }
    areas.resize(count.count);
    for (auto& area : areas) {
        if (!reader.Get(area.vma)) {
            return false;
        }
        area.phys.resize(area.vma.phys_count);
        for (auto& run : area.phys) {
            if (!reader.Get(run)) {
                return false;
            }
        }
        const auto name = reader.View(area.vma.name_bytes);
        if (name.size() != area.vma.name_bytes) {
            return false;
        }
        area.name.assign(name.begin(), name.end());
        reader.Align(8);
    }
    return true;
}

/// Writes guest memory the way a CPU write would leave it, without touching page protection:
/// through the backing view where the area has physical backing, else through the address.
void WriteGuest(VAddr address, const u8* data, u64 size) {
    auto& memory = *Core::Memory::Instance();
    if (!memory.TryWriteBacking(reinterpret_cast<void*>(address), data, size)) {
        std::memcpy(reinterpret_cast<void*>(address), data, size);
    }
}

} // namespace

bool Player::Open(const std::filesystem::path& path, std::string& error) {
    if (!reader.Open(path)) {
        error = reader.Error();
        return false;
    }
    header = reader.Header();
    RecordHeader record{};
    std::vector<u8> payload;
    if (!reader.Next(record, payload) || record.type != static_cast<u32>(RecordType::Info)) {
        error = reader.Error().empty() ? "the trace does not start with its description"
                                       : reader.Error();
        return false;
    }
    std::istringstream lines{std::string(payload.begin(), payload.end())};
    for (std::string line; std::getline(lines, line);) {
        const auto equals = line.find('=');
        if (equals != std::string::npos) {
            info.emplace(line.substr(0, equals), line.substr(equals + 1));
        }
    }
    return true;
}

std::string Player::InfoValue(std::string_view key, std::string_view fallback) const {
    const auto it = info.find(key);
    return it != info.end() ? it->second : std::string{fallback};
}

bool Player::ApplyAreas(PayloadReader& payload, bool keep_contents, std::string& error) {
    std::vector<Area> areas;
    if (!ReadAreas(payload, areas)) {
        error = "damaged area list";
        return false;
    }
    auto& memory = *Core::Memory::Instance();
    // Direct memory first, at the recorded physical addresses. Aliased areas share runs: the
    // first one allocates, the rest map what is already allocated.
    for (const auto& area : areas) {
        const auto type = static_cast<Core::VMAType>(area.vma.type);
        if (type != Core::VMAType::Direct && type != Core::VMAType::Pooled) {
            continue;
        }
        for (const auto& run : area.phys) {
            memory.Allocate(run.base, run.base + run.size, run.size, 4_KB, run.memory_type);
        }
    }
    for (const auto& area : areas) {
        const auto type = static_cast<Core::VMAType>(area.vma.type);
        const auto prot = static_cast<Core::MemoryProt>(area.vma.prot);
        if (area.name == DriverObjectsName) {
            // GnmDriver placed its objects there when it started.
            continue;
        }
        if (keep_contents) {
            // A protection change: the area stays, with its contents.
            const s32 result = memory.Protect(area.vma.base, area.vma.size, prot);
            if (result != 0) {
                error = fmt::format("cannot protect {:#x}+{:#x}: {:#x}", area.vma.base,
                                    area.vma.size, static_cast<u32>(result));
                return false;
            }
            continue;
        }
        void* out{};
        if (type == Core::VMAType::Direct || type == Core::VMAType::Pooled) {
            // Pool commits are mapped as plain direct memory: the GPU sees the same pages.
            for (const auto& run : area.phys) {
                const s32 result = memory.MapMemory(
                    &out, area.vma.base + run.offset, run.size, prot, Core::MemoryMapFlags::Fixed,
                    Core::VMAType::Direct, area.name, false, run.base);
                if (result != 0) {
                    error = fmt::format("cannot map {:#x}+{:#x} at physical {:#x}: {:#x}",
                                        area.vma.base + run.offset, run.size, run.base,
                                        static_cast<u32>(result));
                    return false;
                }
            }
        } else if (type == Core::VMAType::Flexible) {
            const s32 result =
                memory.MapMemory(&out, area.vma.base, area.vma.size, prot,
                                 Core::MemoryMapFlags::Fixed, Core::VMAType::Flexible, area.name);
            if (result != 0) {
                error = fmt::format("cannot map flexible {:#x}+{:#x}: {:#x}", area.vma.base,
                                    area.vma.size, static_cast<u32>(result));
                return false;
            }
        } else {
            error = fmt::format("cannot replay an area of type {} at {:#x}", area.vma.type,
                                area.vma.base);
            return false;
        }
    }
    return true;
}

bool Player::WritePages(std::span<const u8> payload, bool initial, std::string& error) {
    PayloadReader reader{payload};
    MemoryPagesHeader pages{};
    if (!reader.Get(pages)) {
        error = "damaged page record";
        return false;
    }
    const auto data = reader.View(u64{pages.data_count} * PageSize);
    std::vector<u64> data_va(pages.data_count);
    std::vector<u64> zero_va(pages.zero_count);
    if (data.size() != u64{pages.data_count} * PageSize ||
        !reader.GetBytes(data_va.data(), data_va.size() * sizeof(u64)) ||
        !reader.GetBytes(zero_va.data(), zero_va.size() * sizeof(u64))) {
        error = "damaged page record";
        return false;
    }
    // Consecutive pages are written, and reported to the caches, as one range.
    const auto for_each_run = [](const std::vector<u64>& addresses, auto&& func) {
        for (size_t i = 0; i < addresses.size();) {
            size_t j = i + 1;
            while (j < addresses.size() && addresses[j] == addresses[j - 1] + PageSize) {
                ++j;
            }
            func(i, addresses[i], (j - i) * PageSize);
            i = j;
        }
    };
    for_each_run(data_va, [&](size_t first, VAddr address, u64 size) {
        WriteGuest(address, data.data() + first * PageSize, size);
        if (!initial) {
            rasterizer->InvalidateMemory(address, size);
        }
    });
    static const std::vector<u8> zeros(256 * PageSize);
    for_each_run(zero_va, [&](size_t, VAddr address, u64 size) {
        for (u64 offset = 0; offset < size; offset += zeros.size()) {
            WriteGuest(address + offset, zeros.data(), std::min<u64>(zeros.size(), size - offset));
        }
        if (!initial) {
            rasterizer->InvalidateMemory(address, size);
        }
    });
    if (!initial) {
        std::scoped_lock lock{mutex};
        ++delta_records;
        delta_pages += pages.data_count + pages.zero_count;
    }
    return true;
}

bool Player::ApplyMapping(std::span<const u8> payload, std::string& error) {
    PayloadReader reader{payload};
    MappingHeader change{};
    if (!reader.Get(change)) {
        error = "damaged mapping record";
        return false;
    }
    if (!change.protect_only) {
        // The listed areas replace whatever the range held; their pages follow.
        auto& memory = *Core::Memory::Instance();
        for (const auto& mapping : memory.SnapshotMappings(change.base, change.base + change.size)) {
            if (mapping.name == DriverObjectsName) {
                continue;
            }
            memory.UnmapMemory(mapping.base, mapping.size);
        }
    }
    {
        std::scoped_lock lock{mutex};
        ++mappings;
    }
    return ApplyAreas(reader, change.protect_only != 0, error);
}

bool Player::RestoreInitialState(AmdGpu::Liverpool& liverpool_, Vulkan::Rasterizer& rasterizer_,
                                 std::string& error) {
    liverpool = &liverpool_;
    rasterizer = &rasterizer_;
    RecordHeader record{};
    std::vector<u8> payload;
    std::vector<u8> liverpool_state;
    u64 pages_written = 0;
    while (reader.Next(record, payload)) {
        switch (static_cast<RecordType>(record.type)) {
        case RecordType::Vmas: {
            PayloadReader areas{payload};
            if (!ApplyAreas(areas, false, error)) {
                return false;
            }
            break;
        }
        case RecordType::MemoryPages:
            if (!WritePages(payload, true, error)) {
                return false;
            }
            ++pages_written;
            break;
        case RecordType::Liverpool:
            liverpool_state = std::move(payload);
            payload = {};
            break;
        case RecordType::AscQueues:
            if (!liverpool->LoadReplayState(liverpool_state, payload, error)) {
                return false;
            }
            break;
        case RecordType::VideoOut: {
            VideoOutState state{};
            std::memcpy(&state, payload.data(), std::min(sizeof(state), payload.size()));
            Libraries::VideoOut::RestoreReplayState(state);
            break;
        }
        case RecordType::Gds:
            rasterizer->GetBufferCache().WriteGds(payload);
            break;
        case RecordType::GnmDriver:
        case RecordType::Info:
            break;
        case RecordType::BeginStream:
            LOG_INFO(Render, "GPU replay: initial state restored ({} page records)", pages_written);
            return true;
        default:
            error = fmt::format("unexpected record {} in the initial state", record.type);
            return false;
        }
    }
    error = reader.Error().empty() ? "the trace ends before its event stream" : reader.Error();
    return false;
}

const Player::Event* Player::Peek() {
    if (pending) {
        return &*pending;
    }
    if (ended) {
        return nullptr;
    }
    RecordHeader record{};
    std::vector<u8> payload;
    std::string error;
    while (reader.Next(record, payload)) {
        const auto type = static_cast<RecordType>(record.type);
        switch (type) {
        case RecordType::MemoryPages:
            if (!WritePages(payload, false, error)) {
                Fail(error);
                return nullptr;
            }
            continue;
        case RecordType::Mapping:
            if (!ApplyMapping(payload, error)) {
                Fail(error);
                return nullptr;
            }
            continue;
        case RecordType::EopFlipArmed: {
            EopFlipRecord flip{};
            std::memcpy(&flip, payload.data(), std::min(sizeof(flip), payload.size()));
            Libraries::VideoOut::ReplayArmEopFlip(flip.port, flip.buffer, flip.flip_arg);
            continue;
        }
        case RecordType::Flip: {
            std::scoped_lock lock{mutex};
            ++recorded_flips;
            continue;
        }
        case RecordType::Info:
            continue;
        case RecordType::Submit:
        case RecordType::Resume:
        case RecordType::WaitPoll:
        case RecordType::Command:
        case RecordType::BurstEnd:
        case RecordType::End: {
            pending = Event{type, std::move(payload)};
            std::scoped_lock lock{mutex};
            ++events;
            return &*pending;
        }
        default:
            Fail(fmt::format("unexpected record {} in the event stream", record.type));
            return nullptr;
        }
    }
    Fail(reader.Error().empty() ? "the trace ends without an End record" : reader.Error());
    return nullptr;
}

void Player::Pop() {
    if (pending && pending->type == RecordType::End) {
        ended = true;
    }
    pending.reset();
}

bool Player::Poll(u32 queue, WaitKind kind, u64 address, bool satisfied) {
    const Event* event = Peek();
    if (!event) {
        return satisfied;
    }
    if (event->type != RecordType::WaitPoll) {
        Fail(fmt::format("queue {} evaluated a wait at {:#x} where the capture recorded record {}",
                         queue, address, static_cast<u32>(event->type)));
        return satisfied;
    }
    const auto record = event->As<WaitPollRecord>();
    Pop();
    if (record.queue != queue || record.kind != static_cast<u32>(kind) ||
        record.address != address) {
        Fail(fmt::format("queue {} evaluated a wait at {:#x} where the capture recorded queue {} "
                         "at {:#x}",
                         queue, address, record.queue, record.address));
        return satisfied;
    }
    std::scoped_lock lock{mutex};
    ++polls;
    if (record.satisfied && !satisfied) {
        // The memory the wait reads differs from the capture's.
        if (divergences++ == 0) {
            first_divergence = fmt::format("event {}: queue {} wait at {:#x} unsatisfied", events,
                                           queue, address);
            LOG_WARNING(Render, "GPU replay: {}", first_divergence);
        }
    } else if (!record.satisfied && satisfied) {
        // Expected: the capture copies memory after it evaluated the wait, so a write that
        // landed in between already satisfies the replay's evaluation.
        ++forced_waits;
    }
    return record.satisfied != 0;
}

void AfterDrawSlow(const char* kind, u64 hash0, u64 hash1) {
    if (draw_hash_player) {
        draw_hash_player->AfterDraw(kind, hash0, hash1);
    }
}

void Player::EnableImageHashes(const std::filesystem::path& path, u64 draw_event) {
    image_hashes.open(path, std::ios::trunc);
    draw_hash_event = draw_event;
}

void Player::BeforeEvent(const Event& event) {
    {
        std::scoped_lock lock{mutex};
        current_event = events;
    }
    if (image_hashes.is_open() && current_event == draw_hash_event) {
        draw_index = 0;
        draw_hash_player = this;
        Detail::draw_hashes = true;
    }
}

void Player::AfterDraw(const char* kind, u64 hash0, u64 hash1) {
    const u32 index = draw_index++;
    for (const auto& image : rasterizer->GetTextureCache().HashWrittenImages(hashed_epochs)) {
        image_hashes << fmt::format(
            "{} {} {} {:016x} {:016x} image {} {:#x} {}x{}x{} levels {} layers {} {} {}\n",
            current_event, kind, index, hash0, hash1, image.uid, image.address,
            image.extent.width, image.extent.height, image.extent.depth, image.levels,
            image.layers, vk::to_string(image.format),
            image.skipped ? image.skipped : fmt::format("{:016x}", image.hash));
    }
}

void Player::AfterEvent(const Event& event) {
    if (Detail::draw_hashes) {
        Detail::draw_hashes = false;
        draw_hash_player = nullptr;
    }
    if (!image_hashes.is_open()) {
        return;
    }
    std::string what;
    switch (event.type) {
    case RecordType::Resume: {
        const auto record = event.As<ResumeRecord>();
        what = fmt::format("resume q{} s{}", record.queue, record.submission);
        break;
    }
    case RecordType::BurstEnd:
        what = "burst_end";
        break;
    case RecordType::Command:
        what = "command";
        break;
    default:
        return;
    }
    const u64 index = current_event;
    for (const auto& image : rasterizer->GetTextureCache().HashWrittenImages(hashed_epochs)) {
        image_hashes << fmt::format("{} {} image {} {:#x} {}x{}x{} levels {} layers {} {} {}\n",
                                    index, what, image.uid, image.address, image.extent.width,
                                    image.extent.height, image.extent.depth, image.levels,
                                    image.layers, vk::to_string(image.format),
                                    image.skipped ? image.skipped
                                                  : fmt::format("{:016x}", image.hash));
    }
    image_hashes.flush();
}

void Player::RunCommand(const CommandRecord& command) {
    {
        std::scoped_lock lock{mutex};
        ++commands;
    }
    switch (static_cast<CommandKind>(command.kind)) {
    case CommandKind::CpuFlip:
        Libraries::VideoOut::ReplayCpuFlip(command.port, command.buffer, command.flip_arg);
        break;
    case CommandKind::Readback:
        rasterizer->GetBufferCache().ReadMemory(command.address, command.size, command.flag != 0);
        break;
    default:
        Fail(fmt::format("unknown command {}", command.kind));
        break;
    }
}

void Player::Fail(std::string reason) {
    {
        std::scoped_lock lock{mutex};
        if (failure.empty()) {
            failure = reason;
        }
    }
    LOG_ERROR(Render, "GPU replay stopped: {}", reason);
    ended = true;
    pending.reset();
}

void Player::Finish() {
    LOG_INFO(Render, "GPU replay finished:\n{}", Summary());
    done.store(true, std::memory_order_release);
    if (on_finish) {
        on_finish();
    }
}

std::string Player::Summary() const {
    std::scoped_lock lock{mutex};
    std::ostringstream out;
    out << "result=" << (failure.empty() ? "complete" : "failed") << "\n";
    if (!failure.empty()) {
        out << "failure=" << failure << "\n";
    }
    out << "events=" << events << "\n"
        << "wait_polls=" << polls << "\n"
        << "forced_waits=" << forced_waits << "\n"
        << "divergences=" << divergences << "\n";
    if (!first_divergence.empty()) {
        out << "first_divergence=" << first_divergence << "\n";
    }
    out << "recorded_flips=" << recorded_flips << "\n"
        << "delta_records=" << delta_records << "\n"
        << "delta_pages=" << delta_pages << "\n"
        << "mapping_changes=" << mappings << "\n"
        << "commands=" << commands << "\n";
    return out.str();
}

} // namespace VideoCore::Replay
