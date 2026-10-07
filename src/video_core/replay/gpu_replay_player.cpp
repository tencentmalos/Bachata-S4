// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <condition_variable>
#include <thread>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/replay/gpu_replay_player.h"
#include "shader_recompiler/srt_gate_diag.h"

namespace VideoCore::Replay {

namespace {

Player* draw_hash_player{};

/// CPU time of the calling thread in ns (0 where not measured).
u64 ThreadCpuNow() {
#if defined(__linux__) || defined(__APPLE__)
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0) {
        return static_cast<u64>(time.tv_sec) * 1'000'000'000ULL + static_cast<u64>(time.tv_nsec);
    }
#endif
    return 0;
}

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

Player::~Player() {
    for (auto& thread : prefetch_threads) {
        thread.request_stop();
    }
    prefetch_cv.notify_all();
    prefetch_threads.clear();
}

void Player::StartPrefetch() {
    // The event stream is read and decompressed ahead of the command processor, which then
    // only applies records. Decompressing on the command processor took about 60% of its time
    // (Bloodborne on the AYN Thor), so replay timings did not show the command processor.
    constexpr u64 MaxAheadBytes = 256_MB;
    prefetch_threads.emplace_back([this](std::stop_token stop) {
        Common::SetCurrentThreadName("shadPS4:ReplayRead");
        while (true) {
            {
                std::unique_lock lock{prefetch_mutex};
                prefetch_cv.wait(lock, stop, [&] { return prefetch_bytes < MaxAheadBytes; });
                if (stop.stop_requested()) {
                    return;
                }
            }
            auto item = std::make_shared<Prefetched>();
            if (!reader.NextStored(item->header, item->data)) {
                std::scoped_lock lock{prefetch_mutex};
                prefetch_end = true;
                prefetch_error = reader.Error();
                prefetch_cv.notify_all();
                return;
            }
            std::scoped_lock lock{prefetch_mutex};
            prefetch_bytes += item->header.raw_bytes + item->header.stored_bytes;
            prefetch_order.push_back(item);
            prefetch_work.push_back(std::move(item));
            prefetch_cv.notify_all();
        }
    });
    const u32 workers = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 3u);
    for (u32 i = 0; i < workers; ++i) {
        prefetch_threads.emplace_back([this](std::stop_token stop) {
            Common::SetCurrentThreadName("shadPS4:ReplayZstd");
            while (true) {
                std::shared_ptr<Prefetched> item;
                {
                    std::unique_lock lock{prefetch_mutex};
                    prefetch_cv.wait(lock, stop, [&] { return !prefetch_work.empty(); });
                    if (prefetch_work.empty()) {
                        return;
                    }
                    item = std::move(prefetch_work.front());
                    prefetch_work.pop_front();
                }
                std::vector<u8> payload;
                const bool ok = TraceReader::Decompress(item->header, item->data, payload);
                std::scoped_lock lock{prefetch_mutex};
                item->payload = std::move(payload);
                item->data = {};
                item->ok = ok;
                item->ready = true;
                prefetch_cv.notify_all();
            }
        });
    }
}

bool Player::NextRecord(RecordHeader& record, std::vector<u8>& payload) {
    std::unique_lock lock{prefetch_mutex};
    prefetch_cv.wait(lock, [&] {
        return (!prefetch_order.empty() && prefetch_order.front()->ready) ||
               (prefetch_order.empty() && prefetch_end);
    });
    if (prefetch_order.empty()) {
        return false;
    }
    auto item = std::move(prefetch_order.front());
    prefetch_order.pop_front();
    prefetch_bytes -= item->header.raw_bytes + item->header.stored_bytes;
    prefetch_cv.notify_all();
    if (!item->ok) {
        prefetch_error = "corrupt compressed record";
        return false;
    }
    record = item->header;
    payload = std::move(item->payload);
    return true;
}

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
        if (!memory.CanReplayAt(area.vma.base, area.vma.size)) {
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
            prefilled += boost::icl::interval<VAddr>::right_open(area.vma.base,
                                                                  area.vma.base + area.vma.size);
            continue;
        }
        if (!keep_contents && !memory.CanReplayAt(area.vma.base, area.vma.size)) {
            LOG_WARNING(Render, "GPU replay: cannot place {} {:#x}+{:#x} (type {}) here; skipped",
                        area.name, area.vma.base, area.vma.size, area.vma.type);
            std::scoped_lock lock{mutex};
            skipped += boost::icl::interval<VAddr>::right_open(area.vma.base,
                                                                area.vma.base + area.vma.size);
            skipped_bytes += area.vma.size;
            continue;
        }
        if (keep_contents && boost::icl::intersects(
                                 skipped, boost::icl::interval<VAddr>::right_open(
                                              area.vma.base, area.vma.base + area.vma.size))) {
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
    // The caches learn about the writes as they would from the guest CPU: the pages a cache
    // watches fault (the buffer cache counts the write cycle and predicts, the texture cache
    // invalidates its images); a write to a page no cache watches goes unnoticed, as in a live
    // run (buffer pages already CPU modified, streaming pages within a generation).
    const auto note_writes = [&](VAddr address, u64 size) {
        if (Shader::SrtGateDiag::enabled.load(std::memory_order_relaxed)) {
            for (u64 offset = 0; offset < size; offset += PageSize) {
                Shader::SrtGateDiag::NoteWrite(address + offset);
            }
        }
        const auto& page_manager = rasterizer->GetPageManager();
        for (u64 offset = 0; offset < size;) {
            u64 end = offset;
            while (end < size && page_manager.HasWriteWatchers(address + end, PageSize)) {
                end += PageSize;
            }
            if (end != offset) {
                rasterizer->InvalidateMemoryFromWriteFault(address + offset, end - offset);
                offset = end;
            } else {
                offset += PageSize;
            }
        }
    };
    const auto is_skipped = [&](VAddr address, u64 size) {
        if (skipped.empty() ||
            !boost::icl::intersects(skipped,
                                    boost::icl::interval<VAddr>::right_open(address, address + size))) {
            return false;
        }
        std::scoped_lock lock{mutex};
        skipped_pages += size / PageSize;
        return true;
    };
    for_each_run(data_va, [&](size_t first, VAddr address, u64 size) {
        if (is_skipped(address, size)) {
            return;
        }
        WriteGuest(address, data.data() + first * PageSize, size);
        if (!initial) {
            note_writes(address, size);
        }
    });
    static const std::vector<u8> zeros(256 * PageSize);
    for_each_run(zero_va, [&](size_t, VAddr address, u64 size) {
        if (is_skipped(address, size)) {
            return;
        }
        if (initial && !boost::icl::intersects(prefilled, boost::icl::interval<VAddr>::right_open(
                                                              address, address + size))) {
            // Freshly mapped memory reads as zero already. Writing the zeros would allocate
            // every page of them (Bloodborne: 1.8 GiB of the Android memfd backing).
            return;
        }
        for (u64 offset = 0; offset < size; offset += zeros.size()) {
            WriteGuest(address + offset, zeros.data(), std::min<u64>(zeros.size(), size - offset));
        }
        if (!initial) {
            note_writes(address, size);
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
        case RecordType::ScalePlans:
            rasterizer->GetTextureCache().LoadScalePlans(payload);
            break;
        case RecordType::GnmDriver:
        case RecordType::Info:
            break;
        case RecordType::BeginStream:
            // Diagnostic: seed the scale decisions saved from another run (PLANS_OUT).
            if (const char* path = std::getenv("SHADPS4_GPU_REPLAY_PLANS_IN")) {
                std::ifstream in{path, std::ios::binary};
                const std::vector<u8> plans{std::istreambuf_iterator<char>{in}, {}};
                rasterizer->GetTextureCache().LoadScalePlans(plans);
            }
            LOG_INFO(Render, "GPU replay: initial state restored ({} page records)", pages_written);
            StartPrefetch();
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
    while (NextRecord(record, payload)) {
        const auto type = static_cast<RecordType>(record.type);
        switch (type) {
        case RecordType::MemoryPages: {
            const u64 begin = ThreadCpuNow();
            const bool ok = WritePages(payload, false, error);
            apply_cpu_ns += ThreadCpuNow() - begin;
            if (!ok) {
                Fail(error);
                return nullptr;
            }
            continue;
        }
        case RecordType::Mapping: {
            const u64 begin = ThreadCpuNow();
            const bool ok = ApplyMapping(payload, error);
            apply_cpu_ns += ThreadCpuNow() - begin;
            if (!ok) {
                Fail(error);
                return nullptr;
            }
            continue;
        }
        case RecordType::EopFlipArmed: {
            EopFlipRecord flip{};
            std::memcpy(&flip, payload.data(), std::min(sizeof(flip), payload.size()));
            Libraries::VideoOut::ReplayArmEopFlip(flip.port, flip.buffer, flip.flip_arg);
            continue;
        }
        case RecordType::Flip: {
            u64 flip;
            {
                std::scoped_lock lock{mutex};
                flip = ++recorded_flips;
                Shader::SrtGateDiag::NoteFlip();
                frame_cpu_ns.push_back(ThreadCpuNow());
                frame_apply_ns.push_back(apply_cpu_ns);
            }
            if (on_flip) {
                const u64 begin = ThreadCpuNow();
                on_flip(flip);
                apply_cpu_ns += ThreadCpuNow() - begin;
            }
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
    std::string end_error;
    {
        std::scoped_lock lock{prefetch_mutex};
        end_error = prefetch_error;
    }
    Fail(end_error.empty() ? "the trace ends without an End record" : end_error);
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
    std::string dump_prefix;
    if (const char* dump_dir = std::getenv("SHADPS4_GPU_REPLAY_DUMP")) {
        std::filesystem::create_directories(dump_dir);
        dump_prefix = (std::filesystem::path{dump_dir} /
                       fmt::format("e{:04}_d{:04}_{}_{:08x}", current_event, index, kind,
                                   static_cast<u32>(hash1 ? hash1 : hash0)))
                          .string();
    }
    for (const auto& image :
         rasterizer->GetTextureCache().HashWrittenImages(hashed_epochs, dump_prefix)) {
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
    // Diagnostic: SHADPS4_GPU_REPLAY_DUMP=<dir> [SHADPS4_GPU_REPLAY_DUMP_EVENTS=<first>-<last>]
    // writes level 0 of every image the events write, next to the image_hashes.txt lines.
    std::string dump_prefix;
    static const char* dump_dir = std::getenv("SHADPS4_GPU_REPLAY_DUMP");
    if (dump_dir) {
        static const auto range = [] {
            u64 first = 0, last = ~u64{0};
            if (const char* events = std::getenv("SHADPS4_GPU_REPLAY_DUMP_EVENTS")) {
                std::sscanf(events, "%llu-%llu", reinterpret_cast<unsigned long long*>(&first),
                            reinterpret_cast<unsigned long long*>(&last));
            }
            return std::pair{first, last};
        }();
        if (index >= range.first && index <= range.second) {
            std::filesystem::create_directories(dump_dir);
            dump_prefix = (std::filesystem::path{dump_dir} / fmt::format("e{:04}", index)).string();
        }
    }
    for (const auto& image :
         rasterizer->GetTextureCache().HashWrittenImages(hashed_epochs, dump_prefix)) {
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
    if (const char* path = std::getenv("SHADPS4_GPU_REPLAY_PLANS_OUT"); path && rasterizer) {
        const auto plans = rasterizer->GetTextureCache().SaveScalePlans();
        std::ofstream{path, std::ios::binary}.write(reinterpret_cast<const char*>(plans.data()),
                                                     static_cast<std::streamsize>(plans.size()));
    }
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
        << "commands=" << commands << "\n"
        << "processor_wall_ms=" << processor_wall_ns / 1'000'000.0 << "\n"
        << "processor_cpu_ms=" << processor_cpu_ns / 1'000'000.0 << "\n";
    // Command processor CPU per recorded frame, from the flip records. The first frames carry
    // shader translation and pipeline creation; the steady figure leaves out the first quarter.
    // command_ cpu leaves out applying the recorded guest writes (memory pages and mapping
    // changes), which the game's own threads do in a live run.
    if (frame_cpu_ns.size() >= 4 && frame_cpu_ns.back() != 0) {
        std::vector<double> frames;
        std::vector<double> commands;
        for (size_t i = 1; i < frame_cpu_ns.size(); ++i) {
            const double total = (frame_cpu_ns[i] - frame_cpu_ns[i - 1]) / 1'000'000.0;
            const double apply = (frame_apply_ns[i] - frame_apply_ns[i - 1]) / 1'000'000.0;
            frames.push_back(total);
            commands.push_back(total - apply);
        }
        {
            const size_t first = commands.size() / 4;
            std::vector<double> steady(commands.begin() + first, commands.end());
            double sum = 0;
            for (const double value : steady) {
                sum += value;
            }
            std::sort(steady.begin(), steady.end());
            out << "steady_command_cpu_ms mean=" << sum / steady.size()
                << " median=" << steady[steady.size() / 2] << "\n";
        }
        const size_t first = frames.size() / 4;
        std::vector<double> steady(frames.begin() + first, frames.end());
        double sum = 0;
        for (const double value : steady) {
            sum += value;
        }
        std::sort(steady.begin(), steady.end());
        out << "steady_frame_cpu_ms mean=" << sum / steady.size()
            << " median=" << steady[steady.size() / 2] << " frames=" << steady.size() << "\n";
        out << "frame_cpu_ms=";
        for (const double value : frames) {
            out << fmt::format("{:.1f} ", value);
        }
        out << "\n";
        out << "command_cpu_ms=";
        for (const double value : commands) {
            out << fmt::format("{:.1f} ", value);
        }
        out << "\n";
    }
    if (skipped_bytes) {
        out << "skipped_area_bytes=" << skipped_bytes << "\n"
            << "skipped_pages=" << skipped_pages << "\n";
    }
    // Streaming pages (SHADPS4_WATCH_STREAM=1) change how uploads copy, not what they copy.
    if (const auto& stream = VideoCore::stream_page_counters;
        VideoCore::stream_pages.load() || stream.promoted.load() != 0) {
        out << "stream_pages cycles=" << VideoCore::stream_promote_cycles.load()
            << " checked=" << stream.checked.load() << " copied=" << stream.copied.load()
            << " promoted=" << stream.promoted.load() << " demoted=" << stream.demoted.load()
            << " busy=" << stream.busy.load() << "\n";
    }
    return out.str();
}

} // namespace VideoCore::Replay
