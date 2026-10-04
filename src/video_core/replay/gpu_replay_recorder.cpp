// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "core/emulator_settings.h"
#include "core/libraries/fiber/fiber.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/threads/thread_state.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "core/platform.h"
#include "core/tls.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/replay/gpu_replay_file.h"
#include "video_core/replay/gpu_replay_recorder.h"

namespace VideoCore::Replay {

void NoteGuestStackSlow(VAddr base, u64 size) {
    Recorder::Instance().NoteGuestStack(base, size);
}

void NoteMappingChangeSlow(VAddr base, u64 size, bool protect_only) {
    Recorder::Instance().NoteMappingChange(base, size, protect_only);
}

void NoteEopFlipArmedSlow(s32 handle, s32 index, s64 flip_arg) {
    Recorder::Instance().NoteEopFlipArmed(handle, index, flip_arg);
}

namespace {

using Clock = std::chrono::steady_clock;
using Mapping = Core::MemoryManager::MappingSnapshot;
using Interval = boost::icl::interval_set<VAddr>::interval_type;

constexpr u32 MaxFrames = 600;
// A flip armed at submission time must have fired before the snapshot; give up after this many
// frame ends that still had one pending.
constexpr u32 MaxDeferrals = 600;
// Data pages per MemoryPages record (16 MiB), and zero pages listed per record.
constexpr u64 PagesPerRecord = 4096;
constexpr u64 ZeroPagesPerRecord = PagesPerRecord * 16;
constexpr VAddr MaxGpuAddress = 0x10000000000ULL;

u64 ElapsedMs(Clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

bool HasBacking(Core::VMAType type) {
    return type == Core::VMAType::Direct || type == Core::VMAType::Flexible ||
           type == Core::VMAType::Pooled;
}

/// The areas a guest GPU command can reach and the trace therefore holds. Stacks are excluded
/// by type; stacks the guest placed in its own memory are excluded by address (NoteGuestStack).
bool IsRecorded(const Mapping& mapping) {
    if (mapping.base + mapping.size > MaxGpuAddress) {
        return false;
    }
    return HasBacking(mapping.type) || mapping.name == DriverObjectsName;
}

/// Recorded areas whose CPU writes the recorder follows: the guest can write them, and they are
/// not executable (the host protection is set without execute permission).
bool IsTracked(const Mapping& mapping) {
    return IsRecorded(mapping) && True(mapping.prot & Core::MemoryProt::CpuWrite) &&
           !True(mapping.prot & Core::MemoryProt::CpuExec);
}

Interval PageInterval(VAddr base, u64 size) {
    return Interval::right_open(Common::AlignDown(base, PageSize),
                                Common::AlignUp(base + size, PageSize));
}

bool ValidName(std::string_view name) {
    return !name.empty() && name.size() <= 64 &&
           std::ranges::all_of(name, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.';
           }) &&
           name.find("..") == std::string_view::npos;
}

std::vector<u8> AsBytes(const auto& value) {
    std::vector<u8> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

void PutAreas(PayloadBuilder& payload, std::span<const Mapping* const> areas) {
    payload.Put(CountHeader{static_cast<u32>(areas.size()), 0});
    for (const Mapping* mapping : areas) {
        payload.Put(VmaRecord{mapping->base, mapping->size, static_cast<u32>(mapping->type),
                              static_cast<u32>(mapping->prot),
                              static_cast<u32>(mapping->phys.size()),
                              static_cast<u32>(mapping->name.size())});
        for (const auto& [offset, area] : mapping->phys) {
            payload.Put(PhysRecord{offset, area.base, area.size, area.memory_type,
                                   static_cast<u32>(area.dma_type)});
        }
        payload.PutBytes(mapping->name.data(), mapping->name.size());
        payload.Pad(8);
    }
}

/// Collects pages into MemoryPages records. Pages are read straight into the record payload;
/// pages that read as zero are dropped from it again and listed by address only.
class PageBatch {
public:
    explicit PageBatch(TraceWriter& writer_, bool initial_) : writer{writer_}, initial{initial_} {}

    void AddRange(VAddr base, u64 size) {
        auto& memory = *Core::Memory::Instance();
        u64 pages = size / PageSize;
        while (pages) {
            if (!begun) {
                payload.Extend(sizeof(MemoryPagesHeader));
                begun = true;
            }
            const u64 count = std::min(pages, PagesPerRecord - data_va.size());
            const size_t start = payload.Size();
            u8* out = payload.Extend(count * PageSize);
            memory.ReadForReplay(base, out, count * PageSize);
            u64 kept = 0;
            for (u64 i = 0; i < count; ++i) {
                const u8* page = out + i * PageSize;
                const VAddr address = base + i * PageSize;
                if (std::memcmp(page, zero_page.data(), PageSize) == 0) {
                    zero_va.push_back(address);
                    continue;
                }
                if (kept != i) {
                    std::memcpy(out + kept * PageSize, page, PageSize);
                }
                data_va.push_back(address);
                ++kept;
            }
            payload.Shrink(start + kept * PageSize);
            base += count * PageSize;
            pages -= count;
            if (data_va.size() >= PagesPerRecord || zero_va.size() >= ZeroPagesPerRecord) {
                Flush();
            }
        }
    }

    void Flush() {
        if (data_va.empty() && zero_va.empty()) {
            return;
        }
        const MemoryPagesHeader header{static_cast<u32>(data_va.size()),
                                       static_cast<u32>(zero_va.size()), initial ? 1u : 0u, 0};
        std::memcpy(payload.Data(), &header, sizeof(header));
        payload.PutSpan(std::span<const u64>{data_va});
        payload.PutSpan(std::span<const u64>{zero_va});
        data_total += data_va.size();
        zero_total += zero_va.size();
        ++records;
        data_va.clear();
        zero_va.clear();
        begun = false;
        writer.Write(RecordType::MemoryPages, payload.Take());
    }

    u64 DataPages() const {
        return data_total;
    }
    u64 ZeroPages() const {
        return zero_total;
    }
    u64 Records() const {
        return records;
    }

private:
    inline static const std::array<u8, PageSize> zero_page{};
    TraceWriter& writer;
    bool initial;
    bool begun{};
    PayloadBuilder payload;
    std::vector<u64> data_va;
    std::vector<u64> zero_va;
    u64 data_total{};
    u64 zero_total{};
    u64 records{};
};

/// Stacks of live guest threads that run on memory the game provided: thread stacks set with
/// pthread_attr_setstack and the stacks of the fibers the threads currently run.
std::vector<Interval> GuestStacks() {
    std::vector<Interval> stacks;
    auto* threads = Libraries::Kernel::ThrState::Instance();
    std::scoped_lock lock{threads->thread_list_lock};
    for (const Libraries::Kernel::Pthread* thread : threads->threads) {
        const auto& attr = thread->attr;
        if (True(attr.flags & Libraries::Kernel::PthreadAttrFlags::StackUser) &&
            attr.stackaddr_attr && attr.stacksize_attr) {
            stacks.push_back(PageInterval(reinterpret_cast<VAddr>(attr.stackaddr_attr),
                                          attr.stacksize_attr));
        }
        const Core::Tcb* tcb = thread->tcb;
        if (tcb && tcb->tcb_fiber && tcb->tcb_fiber->current_fiber) {
            const auto* fiber = tcb->tcb_fiber->current_fiber;
            if (fiber->addr_context && fiber->size_context) {
                stacks.push_back(PageInterval(reinterpret_cast<VAddr>(fiber->addr_context),
                                              fiber->size_context));
            }
        }
    }
    return stacks;
}

std::string InfoText(std::string_view name, u32 frames) {
    std::ostringstream info;
    info << "program=shadPS4\n"
         << "build=" << Common::g_scm_desc << "\n"
         << "branch=" << Common::g_scm_branch << "\n"
         << "title_id=" << Common::ElfInfo::Instance().GameSerial() << "\n"
         << "name=" << name << "\n"
         << "frames_requested=" << frames << "\n"
         << "internal_scale_percent=" << EmulatorSettings.GetInternalScalePercent() << "\n"
         << "readbacks_mode=" << EmulatorSettings.GetReadbacksMode() << "\n"
         << "readback_linear_images=" << EmulatorSettings.IsReadbackLinearImagesEnabled() << "\n"
         << "copy_gpu_buffers=" << EmulatorSettings.IsCopyGpuBuffers() << "\n"
         << "pipeline_compile_mode=" << EmulatorSettings.GetPipelineCompileMode() << "\n";
    return info.str();
}

} // namespace

Recorder& Recorder::Instance() {
    static Recorder recorder;
    return recorder;
}

std::string Recorder::Arm(u32 frames, std::string trace_name) {
    if (frames == 0 || frames > MaxFrames) {
        return fmt::format("error: frames must be 1-{}", MaxFrames);
    }
    // The write tracking relies on the classic memory backend's fault path; the Android guest
    // runtime routes guest faults through FEX instead.
    if (Core::Memory::Instance()->GetAddressSpace().IsGuestBackend()) {
        return "error: GPU replay capture needs the classic memory backend";
    }
    const State current = state.load();
    if (current == State::Armed || current == State::Capturing) {
        return "error: a capture is already armed or running";
    }
    if (trace_name.empty()) {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        trace_name = fmt::format("{}_{:04}{:02}{:02}_{:02}{:02}{:02}",
                                 Common::ElfInfo::Instance().GameSerial(), local.tm_year + 1900,
                                 local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                                 local.tm_sec);
    }
    if (!ValidName(trace_name)) {
        return "error: the name may only use letters, digits, '_', '-' and '.'";
    }
    const auto trace_path = Common::FS::GetUserPath(Common::FS::PathType::CapturesDir) /
                            "gpu_replay" / (trace_name + ".sgpurply");
    std::error_code ec;
    if (std::filesystem::exists(trace_path, ec)) {
        return "error: " + trace_path.string() + " exists";
    }
    {
        std::scoped_lock lock{mutex};
        name = std::move(trace_name);
        path = trace_path;
        reason.clear();
        frames_requested = frames;
        deferrals = 0;
        writeback_ms = snapshot_ms = 0;
        vma_count = data_pages = zero_pages = raw_bytes = stored_bytes = 0;
    }
    for (auto& count : event_counts) {
        count = 0;
    }
    events = frames_seen = flushes = flush_ns = delta_pages = delta_zero_pages = whole_bytes =
        mapping_changes = excluded_stacks = stream_begin_ns = stream_ns = 0;
    cancel_requested = false;
    state.store(State::Armed, std::memory_order_release);
    return fmt::format("armed: {} frames -> {}\n", frames, trace_path.string());
}

std::string Recorder::StatsText() {
    static constexpr std::array<std::pair<RecordType, const char*>, 10> Named{{
        {RecordType::MemoryPages, "memory_pages"},
        {RecordType::Mapping, "mapping"},
        {RecordType::Submit, "submit"},
        {RecordType::EopFlipArmed, "eop_flip_armed"},
        {RecordType::Resume, "resume"},
        {RecordType::WaitPoll, "wait_poll"},
        {RecordType::Command, "command"},
        {RecordType::BurstEnd, "burst_end"},
        {RecordType::Flip, "flip"},
        {RecordType::End, "end"},
    }};
    u64 stream = stream_ns.load();
    if (!stream && stream_begin_ns.load()) {
        stream = Clock::now().time_since_epoch().count() - stream_begin_ns.load();
    }
    std::ostringstream out;
    out << "events=" << events.load() << "\n"
        << "frames=" << frames_seen.load() << "\n"
        << "stream_ms=" << stream / 1000000 << "\n";
    for (const auto& [type, label] : Named) {
        out << "records_" << label << "=" << event_counts[static_cast<u32>(type) & 31].load()
            << "\n";
    }
    out << "flushes=" << flushes.load() << "\n"
        << "flush_ms=" << flush_ns.load() / 1000000 << "\n"
        << "write_faults=" << VideoCore::PageManager::RecorderWriteFaults() - fault_base << "\n"
        << "delta_pages=" << delta_pages.load() << "\n"
        << "delta_zero_pages=" << delta_zero_pages.load() << "\n"
        << "whole_copy_bytes=" << whole_bytes.load() << "\n"
        << "mapping_changes=" << mapping_changes.load() << "\n"
        << "excluded_stacks=" << excluded_stacks.load() << "\n";
    return out.str();
}

std::string Recorder::Status() {
    static constexpr std::array StateNames{"idle", "armed", "capturing", "finished", "failed"};
    const State current = state.load();
    std::ostringstream out;
    {
        std::scoped_lock lock{mutex};
        out << "state: " << StateNames[static_cast<u32>(current)] << "\n";
        if (name.empty()) {
            return out.str();
        }
        out << "name: " << name << "\n"
            << "path: " << path.string() << "\n"
            << "frames_requested: " << frames_requested << "\n"
            << "deferrals: " << deferrals << "\n"
            << "writeback_ms: " << writeback_ms << "\n"
            << "snapshot_ms: " << snapshot_ms << "\n"
            << "vmas: " << vma_count << "\n"
            << "data_pages: " << data_pages << "\n"
            << "zero_pages: " << zero_pages << "\n"
            << "raw_bytes: " << raw_bytes << "\n"
            << "stored_bytes: " << stored_bytes << "\n";
        if (!reason.empty()) {
            out << "reason: " << reason << "\n";
        }
    }
    if (current != State::Armed) {
        std::istringstream stats{StatsText()};
        for (std::string line; std::getline(stats, line);) {
            const auto equals = line.find('=');
            out << line.substr(0, equals) << ": " << line.substr(equals + 1) << "\n";
        }
    }
    return out.str();
}

std::string Recorder::Cancel() {
    const State current = state.load();
    if (current != State::Armed && current != State::Capturing) {
        return "error: no capture is armed or running";
    }
    cancel_requested = true;
    return "cancel requested\n";
}

bool Recorder::CheckCancel() {
    if (!cancel_requested.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    cancel_requested = false;
    Stop(false, "cancelled");
    return true;
}

void Recorder::Emit(RecordType type, std::vector<u8> payload) {
    event_counts[static_cast<u32>(type) & 31].fetch_add(1, std::memory_order_relaxed);
    events.fetch_add(1, std::memory_order_relaxed);
    writer->Write(type, std::move(payload));
}

template <typename T>
void Recorder::EmitRecord(RecordType type, const T& record) {
    Emit(type, AsBytes(record));
}

void Recorder::BurstEnd(AmdGpu::Liverpool& liverpool_, Vulkan::Rasterizer* rasterizer_,
                        bool submit_done) {
    const State current = state.load();
    if (current == State::Capturing) {
        if (CheckCancel()) {
            return;
        }
        Flush();
        EmitRecord(RecordType::BurstEnd, BurstEndRecord{submit_done ? 1u : 0u, 0});
        if (end_pending) {
            const auto stats = StatsText();
            Emit(RecordType::Info, std::vector<u8>(stats.begin(), stats.end()));
            EmitRecord(RecordType::End, EndRecord{events.load(), frames_seen.load()});
            Stop(true, "finished");
        }
        return;
    }
    if (current != State::Armed) {
        return;
    }
    if (cancel_requested.exchange(false)) {
        Stop(false, "cancelled");
        return;
    }
    if (!submit_done) {
        return;
    }
    if (Platform::IrqC::Instance()->PendingOnce(Platform::InterruptId::GfxFlip) != 0) {
        u32 count{};
        {
            std::scoped_lock lock{mutex};
            count = ++deferrals;
        }
        if (count >= MaxDeferrals) {
            Stop(false, "a flip stayed armed at every frame end");
        }
        return;
    }
    std::string error;
    if (!Start(liverpool_, rasterizer_, error)) {
        Stop(false, error);
        return;
    }
    state.store(State::Capturing, std::memory_order_release);
}

bool Recorder::Start(AmdGpu::Liverpool& liverpool_, Vulkan::Rasterizer* rasterizer_,
                     std::string& error) {
    if (!rasterizer_) {
        error = "no rasterizer";
        return false;
    }
    if (!Libraries::GnmDriver::DriverLabels()) {
        error = "the flip labels and embedded shaders are in host memory";
        return false;
    }
    liverpool = &liverpool_;
    rasterizer = rasterizer_;
    const auto begin = Clock::now();
    // Results the GPU produced exist only in host caches while readbacks are off; a replay
    // starts from empty caches and must find them in guest memory.
    rasterizer->Finish();
    rasterizer->GetBufferCache().WriteBackGpuModified();
    rasterizer->GetTextureCache().WriteBackGpuModified();
    const auto gds = rasterizer->GetBufferCache().ReadGds();
    {
        std::scoped_lock lock{mutex};
        writeback_ms = ElapsedMs(begin);
    }

    std::filesystem::path trace_path;
    std::string trace_name;
    u32 frames{};
    {
        std::scoped_lock lock{mutex};
        trace_path = path;
        trace_name = name;
        frames = frames_requested;
    }
    std::error_code ec;
    std::filesystem::create_directories(trace_path.parent_path(), ec);
    if (std::filesystem::exists(trace_path, ec)) {
        error = trace_path.string() + " exists";
        return false;
    }
    FileHeader header{};
    header.magic = FileMagic;
    header.version = FormatVersion;
    header.header_bytes = sizeof(FileHeader);
    const auto serial = Common::ElfInfo::Instance().GameSerial();
    std::memcpy(header.title_id.data(), serial.data(),
                std::min(serial.size(), header.title_id.size()));
    header.sdk_version = Common::ElfInfo::Instance().CompiledSdkVer();
    header.neo_mode = Libraries::Kernel::sceKernelIsNeoMode() ? 1 : 0;
    header.page_bits = PageBits;
    header.internal_scale_eighths =
        static_cast<u32>(EmulatorSettings.GetInternalScalePercent() * 8.f / 100.f + 0.5f);
    header.created_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    header.extra_dmem_mb = EmulatorSettings.GetExtraDmemInMBytes();
    header.extra_fmem_mb = EmulatorSettings.GetExtraFmemInMBytes();
    header.direct_memory_size = Core::Memory::Instance()->GetTotalDirectSize();
    header.flexible_memory_size = Core::Memory::Instance()->GetTotalFlexibleSize();
    writer = std::make_unique<TraceWriter>();
    if (!writer->Open(trace_path, header)) {
        writer.reset();
        error = "cannot create " + trace_path.string();
        return false;
    }
    const auto info = InfoText(trace_name, frames);
    writer->Write(RecordType::Info, std::vector<u8>(info.begin(), info.end()));

    const auto snapshot_begin = Clock::now();
    {
        std::scoped_lock lock{pending_mutex};
        pending_mappings.clear();
        pending_flips.clear();
    }
    end_pending = false;
    fault_base = seen_faults = VideoCore::PageManager::RecorderWriteFaults();
    // From here on, other threads report stacks, mapping changes and armed flips. A change the
    // mapping list below already shows is applied again at the first boundary, which is
    // harmless.
    VideoCore::PageManager::SetRecorderActive(true);
    tracking = true;
    Detail::capture_hooks = true;
    WriteInitialMemory();

    PayloadBuilder liverpool_state;
    PayloadBuilder ring_queues;
    liverpool->SaveReplayState(liverpool_state, ring_queues);
    writer->Write(RecordType::Liverpool, liverpool_state.Take());
    writer->Write(RecordType::AscQueues, ring_queues.Take());
    GnmDriverState gnm{};
    Libraries::GnmDriver::SaveReplayState(gnm);
    writer->Write(RecordType::GnmDriver, AsBytes(gnm));
    VideoOutState video_out{};
    Libraries::VideoOut::SaveReplayState(video_out);
    writer->Write(RecordType::VideoOut, AsBytes(video_out));
    writer->Write(RecordType::Gds, gds);
    writer->Write(RecordType::BeginStream, {});
    liverpool->SetReplayCapture(true);
    stream_begin_ns = Clock::now().time_since_epoch().count();
    {
        std::scoped_lock lock{mutex};
        snapshot_ms = ElapsedMs(snapshot_begin);
    }
    LOG_INFO(Render, "GPU replay: capture started, initial state written to {}",
             trace_path.string());
    return true;
}

void Recorder::WriteInitialMemory() {
    auto& memory = *Core::Memory::Instance();
    const auto mappings = memory.SnapshotMappings();
    std::vector<const Mapping*> areas;
    for (const auto& mapping : mappings) {
        if (IsRecorded(mapping)) {
            areas.push_back(&mapping);
        }
    }

    // Protect before copying: a write from now on faults and lands in the first delta.
    const auto& page_manager = rasterizer->GetPageManager();
    {
        std::scoped_lock track{track_mutex};
        recorded.clear();
        tracked.clear();
        excluded.clear();
        for (const auto& stack : GuestStacks()) {
            excluded += stack;
        }
        excluded_stacks = boost::icl::interval_count(excluded);
        for (const Mapping* mapping : areas) {
            const auto interval = PageInterval(mapping->base, mapping->size);
            recorded += interval;
            if (IsTracked(*mapping)) {
                tracked += interval;
            }
        }
        tracked -= excluded;
        for (const auto& interval : tracked) {
            page_manager.RecordWrites(interval.lower(), interval.upper() - interval.lower());
        }
    }

    PayloadBuilder payload;
    PutAreas(payload, areas);
    writer->Write(RecordType::Vmas, payload.Take());

    PageBatch batch{*writer, true};
    for (const Mapping* mapping : areas) {
        batch.AddRange(mapping->base, mapping->size);
    }
    batch.Flush();
    std::scoped_lock lock{mutex};
    vma_count = areas.size();
    data_pages = batch.DataPages();
    zero_pages = batch.ZeroPages();
}

void Recorder::Flush() {
    const auto begin = Clock::now();
    std::vector<PendingMapping> mappings;
    std::vector<EopFlipRecord> flips;
    {
        std::scoped_lock lock{pending_mutex};
        mappings.swap(pending_mappings);
        flips.swap(pending_flips);
    }
    // A fault clears its page's bit before it counts, and the faulting write retries only after
    // that: an unchanged count means no page was written since the previous boundary.
    const u64 faults = VideoCore::PageManager::RecorderWriteFaults();
    if (!mappings.empty() || faults != seen_faults) {
        seen_faults = faults;
        const auto& page_manager = rasterizer->GetPageManager();
        IntervalSet copy;
        {
            std::scoped_lock track{track_mutex};
            IntervalSet whole;
            for (const auto& change : mappings) {
                ApplyMappingChange(change, whole);
            }
            dirty.clear();
            for (const auto& interval : tracked) {
                page_manager.CollectRecorderDirty(interval.lower(),
                                                  interval.upper() - interval.lower(), dirty);
            }
            // Protect again before copying: a write from now on faults again and goes into
            // the next delta. The scan returns pages in ascending order.
            for (size_t i = 0; i < dirty.size();) {
                size_t j = i + 1;
                while (j < dirty.size() && dirty[j] == dirty[j - 1] + 1) {
                    ++j;
                }
                const VAddr run_begin = dirty[i] << PageBits;
                const VAddr run_end = (dirty[j - 1] + 1) << PageBits;
                page_manager.RecordWrites(run_begin, run_end - run_begin);
                copy += Interval::right_open(run_begin, run_end);
                i = j;
            }
            for (const auto& interval : whole) {
                whole_bytes.fetch_add(interval.upper() - interval.lower(),
                                      std::memory_order_relaxed);
            }
            copy += whole;
        }
        if (!copy.empty()) {
            PageBatch batch{*writer, false};
            for (const auto& interval : copy) {
                batch.AddRange(interval.lower(), interval.upper() - interval.lower());
            }
            batch.Flush();
            delta_pages.fetch_add(batch.DataPages(), std::memory_order_relaxed);
            delta_zero_pages.fetch_add(batch.ZeroPages(), std::memory_order_relaxed);
            event_counts[static_cast<u32>(RecordType::MemoryPages)].fetch_add(
                batch.Records(), std::memory_order_relaxed);
        }
    }
    for (const auto& flip : flips) {
        EmitRecord(RecordType::EopFlipArmed, flip);
    }
    flushes.fetch_add(1, std::memory_order_relaxed);
    flush_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count(),
        std::memory_order_relaxed);
}

void Recorder::ApplyMappingChange(const PendingMapping& change, IntervalSet& whole) {
    auto& memory = *Core::Memory::Instance();
    const auto& page_manager = rasterizer->GetPageManager();
    const auto range = PageInterval(change.base, change.size);
    mapping_changes.fetch_add(1, std::memory_order_relaxed);

    // The guest set the protection of the range itself: drop the recorder's bits there without
    // changing it.
    for (const auto& interval : tracked & range) {
        page_manager.StopRecordingWrites(interval.lower(), interval.upper() - interval.lower(),
                                         false);
    }
    tracked -= range;
    recorded -= range;

    const auto mappings = memory.SnapshotMappings(range.lower(), range.upper());
    std::vector<const Mapping*> areas;
    for (const auto& mapping : mappings) {
        if (IsRecorded(mapping)) {
            areas.push_back(&mapping);
        }
    }
    PayloadBuilder payload;
    payload.Put(MappingHeader{range.lower(), range.upper() - range.lower(),
                              change.protect_only ? 1u : 0u, 0});
    PutAreas(payload, areas);
    Emit(RecordType::Mapping, payload.Take());

    for (const Mapping* mapping : areas) {
        const auto interval = PageInterval(mapping->base, mapping->size);
        recorded += interval;
        whole += interval;
        if (IsTracked(*mapping)) {
            IntervalSet part{interval};
            part -= excluded;
            for (const auto& piece : part) {
                tracked += piece;
                page_manager.RecordWrites(piece.lower(), piece.upper() - piece.lower());
            }
        }
    }

#ifdef _WIN32
    if (!change.protect_only) {
        // Splitting a mapped placeholder remaps the rest of it with the protection it was
        // mapped with (AddressSpace SplitRegion), which drops the recorder's protection without
        // a fault. Protect the neighbouring areas again and copy them whole.
        for (const VAddr neighbour : {range.lower() - 1, range.upper()}) {
            const auto area = memory.SnapshotMappingAt(neighbour);
            if (!area || !IsRecorded(*area)) {
                continue;
            }
            const auto interval = PageInterval(area->base, area->size);
            whole += interval;
            for (const auto& piece : tracked & interval) {
                const u64 size = piece.upper() - piece.lower();
                page_manager.StopRecordingWrites(piece.lower(), size, false);
                page_manager.RecordWrites(piece.lower(), size);
            }
        }
    }
#endif
}

void Recorder::OnResume(u32 queue, u64 submission, const SubmitRecord* first_submit) {
    if (state.load(std::memory_order_relaxed) != State::Capturing || CheckCancel()) {
        return;
    }
    Flush();
    if (first_submit) {
        EmitRecord(RecordType::Submit, *first_submit);
    }
    EmitRecord(RecordType::Resume, ResumeRecord{queue, 0, submission});
}

void Recorder::OnWaitPoll(u32 queue, WaitKind kind, u64 address, bool satisfied) {
    if (state.load(std::memory_order_relaxed) != State::Capturing) {
        return;
    }
    Flush();
    EmitRecord(RecordType::WaitPoll,
               WaitPollRecord{queue, static_cast<u32>(kind), address, satisfied ? 1u : 0u, 0});
}

void Recorder::OnCommands() {
    if (state.load(std::memory_order_relaxed) != State::Capturing || CheckCancel()) {
        return;
    }
    Flush();
}

void Recorder::OnCommand(const CommandRecord& command) {
    if (state.load(std::memory_order_relaxed) != State::Capturing) {
        return;
    }
    EmitRecord(RecordType::Command, command);
}

void Recorder::OnFlip(s32 index, u64 address, bool is_eop) {
    if (state.load(std::memory_order_relaxed) != State::Capturing) {
        return;
    }
    const u64 frame = frames_seen.fetch_add(1, std::memory_order_relaxed);
    EmitRecord(RecordType::Flip, FlipRecord{frame, address, index, is_eop ? 1u : 0u});
    u32 frames{};
    {
        std::scoped_lock lock{mutex};
        frames = frames_requested;
    }
    if (frame + 1 >= frames) {
        end_pending = true;
    }
}

void Recorder::NoteGuestStack(VAddr base, u64 size) {
    if (!size) {
        return;
    }
    const auto range = PageInterval(base, size);
    std::scoped_lock track{track_mutex};
    if (boost::icl::contains(excluded, range)) {
        return;
    }
    excluded += range;
    excluded_stacks.fetch_add(1, std::memory_order_relaxed);
    if (!tracking || !rasterizer) {
        return;
    }
    const auto& page_manager = rasterizer->GetPageManager();
    for (const auto& interval : tracked & range) {
        page_manager.StopRecordingWrites(interval.lower(), interval.upper() - interval.lower(),
                                         true);
    }
    tracked -= range;
}

void Recorder::NoteMappingChange(VAddr base, u64 size, bool protect_only) {
    if (!size || base >= MaxGpuAddress) {
        return;
    }
    std::scoped_lock lock{pending_mutex};
    pending_mappings.push_back({base, size, protect_only});
}

void Recorder::NoteEopFlipArmed(s32 handle, s32 index, s64 flip_arg) {
    std::scoped_lock lock{pending_mutex};
    pending_flips.push_back({handle, index, flip_arg});
}

void Recorder::Stop(bool ok, std::string stop_reason) {
    Detail::capture_hooks = false;
    if (stream_begin_ns.load()) {
        stream_ns = Clock::now().time_since_epoch().count() - stream_begin_ns.load();
    }
    if (liverpool) {
        liverpool->SetReplayCapture(false);
    }
    if (tracking) {
        std::vector<PendingMapping> mappings;
        {
            std::scoped_lock lock{pending_mutex};
            mappings.swap(pending_mappings);
            pending_flips.clear();
        }
        const auto& page_manager = rasterizer->GetPageManager();
        std::scoped_lock track{track_mutex};
        // Ranges the guest protected meanwhile keep the protection it set.
        for (const auto& change : mappings) {
            const auto range = PageInterval(change.base, change.size);
            for (const auto& interval : tracked & range) {
                page_manager.StopRecordingWrites(interval.lower(),
                                                 interval.upper() - interval.lower(), false);
            }
            tracked -= range;
        }
        for (const auto& interval : tracked) {
            page_manager.StopRecordingWrites(interval.lower(), interval.upper() - interval.lower(),
                                             true);
        }
        tracked.clear();
        recorded.clear();
        excluded.clear();
        tracking = false;
        VideoCore::PageManager::SetRecorderActive(false);
    }
    bool closed = true;
    u64 raw = 0;
    u64 stored = 0;
    if (writer) {
        closed = writer->Close();
        raw = writer->RawBytes();
        stored = writer->StoredBytes();
        writer.reset();
    }
    {
        std::scoped_lock lock{mutex};
        raw_bytes = raw;
        stored_bytes = stored;
        reason = closed ? std::move(stop_reason) : "trace write failed";
    }
    const bool success = ok && closed;
    state.store(success ? State::Finished : State::Failed, std::memory_order_release);
    LOG_INFO(Render, "GPU replay: capture {} ({})", success ? "finished" : "failed", Status());
}

} // namespace VideoCore::Replay
