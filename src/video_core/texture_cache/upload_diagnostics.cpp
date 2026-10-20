// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <array>
#include <charconv>
#include <functional>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "video_core/buffer_cache/stream_reuse.h"
#include "core/address_space.h"
#include "core/memory.h"
#include "video_core/buffer_cache/gpu_byte_keeper.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/upload_diagnostics.h"

namespace VideoCore::UploadDiagnostics {

namespace {
// Streamed copy reuse switch and why windows ended: by reason, and the packets that end most.
std::string StreamReuseLine() {
    namespace R = VideoCore::StreamReuse;
    constexpr auto o = std::memory_order_relaxed;
    std::string out = fmt::format(
        "stream_reuse={} windows={} ends packet/yield/submission/commands/flip={}/{}/{}/{}/{}",
        R::enabled.load(o) ? "on" : "off", R::window.load(o), R::ends[0].load(o),
        R::ends[1].load(o), R::ends[2].load(o), R::ends[3].load(o), R::ends[4].load(o));
    std::array<std::pair<u64, u32>, 256> packets{};
    for (u32 i = 0; i < packets.size(); ++i) {
        packets[i] = {R::packet_ends[i].load(o), i};
    }
    std::partial_sort(packets.begin(), packets.begin() + 6, packets.end(), std::greater<>{});
    out += " top_packets=";
    for (u32 i = 0; i < 6 && packets[i].first; ++i) {
        out += fmt::format("{}{:#x}:{}", i ? "," : "", packets[i].second, packets[i].first);
    }
    return out + "\n";
}

std::string TargetMemoLine() {
    constexpr auto o = std::memory_order_relaxed;
    static constexpr std::array<const char*, 3> Modes{"off", "on", "verify"};
    return fmt::format("target_memo={} targets hits/misses={}/{} begin hits/misses={}/{} "
                       "verify_mismatches={}\n",
                       Modes[std::min<u32>(target_memo_mode.load(o), 2)], target_memo_hits.load(o),
                       target_memo_misses.load(o), begin_memo_hits.load(o),
                       begin_memo_misses.load(o), memo_mismatches.load(o));
}

std::string KeyMemoLine() {
    constexpr auto o = std::memory_order_relaxed;
    static constexpr std::array<const char*, 3> Modes{"off", "on", "verify"};
    return fmt::format("key_memo={} hits/misses={}/{} second_pass_hits={} pipeline_reuse={} "
                       "verify_mismatches={}\n",
                       Modes[std::min<u32>(key_memo_mode.load(o), 2)], key_memo_hits.load(o),
                       key_memo_misses.load(o), key_memo_final_hits.load(o),
                       key_memo_pipeline_reuse.load(o), key_memo_mismatches.load(o));
}
} // namespace

namespace {

constexpr u32 MaxMips = 16;
constexpr u32 DefaultLogLines = 256;
constexpr u32 MaxLogLines = 20000;
constexpr size_t SourceCount = static_cast<size_t>(DirtySource::Count);

// Outcome of one upload compared with the same image's previous upload.
enum Outcome : u8 { Unchanged, Changed, Partial, First, Unhashed, OutcomeCount };

constexpr std::array<const char*, SourceCount> SourceNames{
    "none", "cpu_fault", "host_write", "cpu_page_shared", "gpu_storage_write", "gpu_copy"};
constexpr std::array<const char*, OutcomeCount> OutcomeNames{
    "unchanged", "changed", "partial", "first", "gpu_resident"};

struct KeyHash {
    size_t operator()(const ImageKey& key) const {
        return std::hash<u64>{}(key.address * 0x9e3779b97f4a7c15ull ^ key.size ^
                                (u64(key.format) << 40));
    }
};

struct Record {
    u32 width{}, height{}, depth{}, layers{}, levels{}, tile_mode{};
    bool tiled{}, scaled{};
    DirtySource pending_source{DirtySource::None};
    VAddr pending_write{};
    u64 pending_write_size{};
    u64 pending_writer{};
    u64 last_writer{};
    std::array<u64, MaxMips> last_hash{};
    u32 hashed_mask{};
    u64 uploads{};
    std::array<u64, OutcomeCount> outcomes{};
    std::array<u64, SourceCount> sources{};
    u32 first_frame{}, last_frame{};
};

std::mutex mutex;
std::unordered_map<ImageKey, Record, KeyHash> records;
std::array<std::array<u64, OutcomeCount>, SourceCount> matrix{};
u64 total_uploads{};
u64 dirty_notes{};
u32 first_frame{std::numeric_limits<u32>::max()}, last_frame{};
std::unordered_map<u64, u64> writers; // shader program hash -> uploads it caused
std::atomic<u32> log_budget{0};
std::atomic<u32> dispatch_budget{0};
u64 sequence{};
u64 dispatch_sequence{};
std::unordered_map<u64, u64> image_dispatches; // shader program hash -> image-writing dispatches

constexpr size_t CreatedCount = static_cast<size_t>(Created::Count);
constexpr std::array<const char*, CreatedCount> CreatedNames{
    "fresh", "expand", "expand_twin", "twin_reused", "revived", "depth_copy"};
constexpr std::array<const char*, static_cast<size_t>(WriteKind::Count)> WriteKindNames{
    "-", "storage", "rt", "depth"};

// GPU-written images freed with their range, newest last (NoteFreedGpuImage).
struct Freed {
    VAddr address{};
    u64 size{};
    u32 format{};
    WriteKind kind{};
    u64 writer{};
    u32 frame{};
    bool kept{};
};
constexpr size_t FreedRingSize = 64;
std::array<Freed, FreedRingSize> freed_ring{};
size_t freed_next{};
// "usage created prior" -> uploads, and the writer of the prior GPU content -> uploads.
std::unordered_map<std::string, u64> causes;
std::unordered_map<u64, u64> prior_writers;

// Usage bits of UploadEvent::usage as letters: Texture Storage Rendertarget Depth.
std::string UsageName(u8 usage) {
    std::string out;
    if (usage & 1) out += 'T';
    if (usage & 2) out += 'S';
    if (usage & 4) out += 'R';
    if (usage & 8) out += 'D';
    return out.empty() ? "none" : out;
}

// The newest freed GPU image overlapping [address, address + size), or nullptr.
const Freed* FindFreed(VAddr address, u64 size) {
    for (size_t i = 0; i < FreedRingSize; ++i) {
        const auto& f = freed_ring[(freed_next + FreedRingSize - 1 - i) % FreedRingSize];
        if (f.size && f.address < address + size && address < f.address + f.size) return &f;
    }
    return nullptr;
}

constexpr size_t FillOutcomeCount = static_cast<size_t>(FillOutcome::Count);
constexpr std::array<const char*, FillOutcomeCount> FillOutcomeNames{
    "cleared", "no_image", "partial", "pattern", "format", "gpu_resident", "disabled"};
std::array<std::atomic<u64>, FillOutcomeCount> fill_outcomes{};
std::atomic<u64> fill_images{0};
std::atomic<u64> fill_bytes{0};

std::string FormatName(u32 format) {
    return vk::to_string(static_cast<vk::Format>(format));
}

std::string Summary() {
    std::scoped_lock lock{mutex};
    std::string out;
    const u32 frames = total_uploads ? last_frame - first_frame + 1 : 0;
    const auto uploaded = std::count_if(records.begin(), records.end(),
                                        [](const auto& entry) { return entry.second.uploads != 0; });
    out += fmt::format("armed={} uploads={} images={} frames={} uploads_per_frame={:.2f} dirty_notes={} log_left={}\n",
                       armed.load() ? 1 : 0, total_uploads, uploaded, frames,
                       frames ? double(total_uploads) / frames : 0.0, dirty_notes, log_budget.load());
    out += fmt::format("gpu_flag={}\n", region_gpu_flag.load() ? "on" : "off");
    out += fmt::format("watch_stream={} cycles={} checked={} copied={} promoted={} demoted={} "
                       "busy={}\n",
                       stream_pages.load() ? "on" : "off", stream_promote_cycles.load(),
                       stream_page_counters.checked.load(), stream_page_counters.copied.load(),
                       stream_page_counters.promoted.load(), stream_page_counters.demoted.load(),
                       stream_page_counters.busy.load());
    out += fmt::format("watch_coalesce={} watch_predict={} watch_content={} hashed_pages={} "
                       "rewritten_pages={}\n",
                       Core::gpu_watch_per_page.load() ? "off" : "on",
                       predict_write_faults.load() ? "on" : "off",
                       predict_from_contents.load() ? "on" : "off",
                       content_prediction_counters.hashed_pages.load(),
                       content_prediction_counters.rewritten_pages.load());
    out += fmt::format("write_faults cold={} cold_known={} after_dirty={} run_start={} "
                       "run_start_window={} watch_gaps={} watch_decay={} unchanged_pages={} "
                       "kept_pages={} watch_cross={} cross_released={}\n",
                       content_prediction_counters.fault_cold.load(),
                       content_prediction_counters.fault_cold_known.load(),
                       content_prediction_counters.fault_after_dirty.load(),
                       content_prediction_counters.fault_run_start.load(),
                       content_prediction_counters.fault_run_start_window.load(),
                       predict_across_gaps.load() ? "on" : "off",
                       decay_unchanged.load() ? "on" : "off",
                       content_prediction_counters.unchanged_pages.load(),
                       content_prediction_counters.kept_pages.load(),
                       predict_cross_region.load() ? "on" : "off",
                       content_prediction_counters.cross_released.load());
    {
        const auto& watch = Core::gpu_watch_counters;
        out += fmt::format("gpu_watch release_calls={} release_pages={} predicted_pages={} "
                           "watch_calls={} watch_pages={} syscalls={}\n",
                           watch.release_calls.load(), watch.release_pages.load(),
                           watch.predicted_pages.load(), watch.watch_calls.load(),
                           watch.watch_pages.load(), watch.syscalls.load());
        out += fmt::format("fault_textures={} texture_invalidates={} texture_skipped={} "
                           "texture_ns={}\n",
                           skip_unwatched_fault_textures.load() ? "skip" : "always",
                           watch.texture_invalidates.load(),
                           watch.texture_invalidates_skipped.load(),
                           watch.texture_invalidate_ns.load());
        out += fmt::format("texture_binds cache={} fast={} hits={} misses={} fast_hits={}\n",
                           texture_bind_cache.load() ? "on" : "off",
                           texture_bind_fast.load() ? "on" : "off", texture_bind_hits.load(),
                           texture_bind_misses.load(), texture_bind_fast_hits.load());
        out += fmt::format("pool_undefined={} marked={} unlearned={} watched={} learned={} "
                           "skips={} skip_bytes={} cpu_writes={} miss_defined={} "
                           "miss_untracked={} miss_gpu_buffer={} sampled_undefined={} "
                           "unused={}\n",
                           pool_undefined.load() ? "on" : "off", pool_undefined_marked.load(),
                           pool_undefined_unlearned.load(), pool_undefined_watched.load(),
                           pool_undefined_learned.load(), pool_undefined_skips.load(),
                           pool_undefined_skip_bytes.load(), pool_undefined_cpu_writes.load(),
                           pool_undefined_misses[0].load(), pool_undefined_misses[1].load(),
                           pool_undefined_misses[2].load(), pool_undefined_misses[3].load(),
                           pool_undefined_unused.load());
    }
    out += fmt::format("ignore_storage_dirty={} ignored_storage_dirty={}\n",
                       ignore_storage_dirty.load() ? 1 : 0, ignored_storage_dirty.load());
    std::vector<std::pair<u64, u64>> by_writer(writers.begin(), writers.end());
    std::sort(by_writer.begin(), by_writer.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    out += fmt::format("raw_sync: off={} syncs={} bytes={} stale_images_skipped={}\n",
                       raw_sync_off.load() ? 1 : 0, raw_syncs.load(), raw_sync_bytes.load(),
                       raw_sync_stale.load());
    out += fmt::format("texel_sync: current_contents_skipped={}\n", texel_sync_skips.load());
    out += fmt::format("depth_copy: mode={} direct={} buffered={}\n",
                       depth_copy_buffer.load() ? "buffer" : "maint8", depth_copy_direct.load(),
                       depth_copy_buffered.load());
    out += fmt::format("dma_bounds: enabled={} bounded_calls={} full_calls={} ranges_skipped={}\n",
                       dma_bounds.load() ? 1 : 0, dma_bounded_calls.load(), dma_full_calls.load(),
                       dma_ranges_skipped.load());
    out += fmt::format("raw_copy: off={} image_copies={} bytes={} dispatched={}\n",
                       raw_copy_off.load() ? 1 : 0, raw_copies.load(), raw_copy_bytes.load(),
                       raw_copy_fallbacks.load());
    out += fmt::format("htile_copy: off={} copies_to_htile={} depth_clears={} rejected={} "
                       "gpu_source={}\n",
                       htile_copy_off.load() ? 1 : 0, htile_copies.load(), htile_copy_clears.load(),
                       htile_copy_rejected.load(), htile_copy_gpu_source.load());
    out += fmt::format("format_list: mode={} listed_images={} moved_to_any_format={}\n",
                       format_list.load(), format_list_images.load(), format_list_moves.load());
    out += fmt::format("compute_fill: off={} images_cleared={} bytes_cleared={}",
                       fill_clear_off.load() ? 1 : 0, fill_images.load(), fill_bytes.load());
    for (size_t i = 0; i < FillOutcomeCount; ++i)
        out += fmt::format(" {}={}", FillOutcomeNames[i], fill_outcomes[i].load());
    out += '\n';
    std::vector<std::pair<u64, u64>> by_dispatch(image_dispatches.begin(), image_dispatches.end());
    std::sort(by_dispatch.begin(), by_dispatch.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    // Dispatches binding a written buffer that starts at a cached image; storage image writes
    // are not counted here (see the upload causes and the writers of freed GPU content).
    out += fmt::format("buffer writes at image bases: dispatches={} dispatch_log_left={} (pgm_hash dispatches):\n",
                       dispatch_sequence, dispatch_budget.load());
    for (size_t i = 0; i < std::min<size_t>(by_dispatch.size(), 24); ++i)
        out += fmt::format("  {:#018x} {}\n", by_dispatch[i].first, by_dispatch[i].second);
    out += "uploads by writing shader (pgm_hash uploads):\n";
    for (size_t i = 0; i < std::min<size_t>(by_writer.size(), 24); ++i)
        out += fmt::format("  {:#018x} {}\n", by_writer[i].first, by_writer[i].second);
    out += fmt::format("expand_twin={} expansions={} upload_skips={} twin_reuses={} stale_sources={}\n",
                       expand_twin.load() ? "on" : "off", expansions.load(),
                       expand_upload_skips.load(), expand_twin_reuses.load(),
                       expand_stale_sources.load());
    out += fmt::format("adopt_mips={} adopted={}\n", adopt_subresources.load() ? "on" : "off",
                       adopted_subresources.load());
    out += fmt::format("tiny_writeback={} writebacks={} per_frame_skips={}\n",
                       tiny_writeback.load() ? "on" : "off", tiny_writebacks.load(),
                       tiny_writeback_skips.load());
    out += fmt::format("tiny_linear_writeback={} writebacks={}\n",
                       tiny_linear_writeback.load() ? "on" : "off",
                       tiny_linear_writebacks.load());
    out += fmt::format("unused_small_depth={} dropped_draws={}\n",
                       unused_small_depth.load() ? "drop" : "keep",
                       unused_small_depth_draws.load());
    out += fmt::format("stream_submit={} early_submits={}\n", stream_submit_draws.load(),
                       stream_submits.load());
    out += StreamReuseLine();
    out += TargetMemoLine();
    out += KeyMemoLine();
    static constexpr std::array<const char*, 3> AsyncModes{"off", "tiny", "all"};
    out += fmt::format("async_writeback={} deferred fences={} submits={}\n",
                       AsyncModes[static_cast<u32>(async_writeback.load())],
                       async_writeback_fences.load(), async_writeback_submits.load());
    out += fmt::format("writebacks sync={} sync_ms={} async={} sync fences={} submits={}\n",
                       sync_writebacks.load(), sync_writeback_ns.load() / 1000000,
                       async_writebacks.load(), sync_writeback_fences.load(),
                       sync_writeback_submits.load());
    out += "writeback waits";
    for (u32 reader = 0; reader < static_cast<u32>(WritebackReader::Count); ++reader) {
        out += fmt::format(" {}={}", WritebackReaderName(static_cast<WritebackReader>(reader)),
                           writeback_waits[reader].load());
    }
    out += fmt::format(" wait_ms={}\n", writeback_wait_ns.load() / 1000000);
    out += fmt::format("writeback kept_foreign={} kept_bytes={} unsnapshotted={}\n",
                       writeback_kept.load(), writeback_kept_bytes.load(),
                       writeback_unsnapshotted.load());
    out += fmt::format("page_watchers underflows={} overflows={}\n",
                       PageManager::WatcherUnderflows(), PageManager::WatcherOverflows());
    out += fmt::format("scheduler finishes={} wait_ms={}\n", scheduler_finishes.load(),
                       scheduler_wait_ns.load() / 1000000);
    out += fmt::format("corner_views={} images={} copies={} misses={} evictions={}\n",
                       corner_views.load() ? "on" : "off", corner_images.load(),
                       corner_copies.load(), corner_misses.load(), corner_evictions.load());
    out += fmt::format("pool_revive={} kept={} revived={} revived_bytes={} released={} stale={}\n",
                       pool_revive.load() ? "on" : "off", pool_revive_kept.load(),
                       pool_revive_revived.load(), pool_revive_bytes.load(),
                       pool_revive_released.load(), pool_revive_stale.load());
    // Usage at the refresh (T texture, S storage, R render target, D depth), how the image was
    // made, and what the GPU had written over the range before it was freed.
    std::vector<std::pair<std::string, u64>> by_cause(causes.begin(), causes.end());
    std::sort(by_cause.begin(), by_cause.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    out += "upload causes (usage created prior_gpu_content uploads):\n";
    for (size_t i = 0; i < std::min<size_t>(by_cause.size(), 24); ++i)
        out += fmt::format("  {} {}\n", by_cause[i].first, by_cause[i].second);
    std::vector<std::pair<u64, u64>> by_prior(prior_writers.begin(), prior_writers.end());
    std::sort(by_prior.begin(), by_prior.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    out += "uploads over freed GPU content, by its writer (pgm_hash uploads):\n";
    for (size_t i = 0; i < std::min<size_t>(by_prior.size(), 24); ++i)
        out += fmt::format("  {:#018x} {}\n", by_prior[i].first, by_prior[i].second);
    out += "outcome_by_source (unchanged changed partial first gpu_resident):\n";
    for (size_t s = 0; s < SourceCount; ++s) {
        u64 row = 0;
        for (const u64 v : matrix[s]) row += v;
        if (!row) continue;
        out += fmt::format("  {:<18} {} {} {} {} {}\n", SourceNames[s], matrix[s][Unchanged],
                           matrix[s][Changed], matrix[s][Partial], matrix[s][First], matrix[s][Unhashed]);
    }
    std::vector<std::pair<const ImageKey*, const Record*>> sorted;
    sorted.reserve(records.size());
    for (const auto& [key, record] : records)
        if (record.uploads) sorted.emplace_back(&key, &record);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second->uploads > b.second->uploads; });
    out += "top images (addr size format WxHxD layers mips tile scaled | uploads unchanged changed partial first gpu_resident | dominant source last_writer):\n";
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 48); ++i) {
        const auto& [key, r] = sorted[i];
        const auto dominant = std::max_element(r->sources.begin(), r->sources.end()) - r->sources.begin();
        out += fmt::format("  {:#x} {} {} {}x{}x{} L{} M{} tile{}{} {} | {} {} {} {} {} {} | {} {:#x}\n",
                           key->address, key->size, FormatName(key->format), r->width, r->height,
                           r->depth, r->layers, r->levels, r->tile_mode, r->tiled ? "" : "(linear)",
                           r->scaled ? "scaled" : "native", r->uploads, r->outcomes[Unchanged],
                           r->outcomes[Changed], r->outcomes[Partial], r->outcomes[First],
                           r->outcomes[Unhashed], SourceNames[dominant], r->last_writer);
    }
    return out;
}

void Reset(u32 lines) {
    std::scoped_lock lock{mutex};
    records.clear();
    matrix = {};
    writers.clear();
    total_uploads = 0;
    dirty_notes = 0;
    first_frame = std::numeric_limits<u32>::max();
    last_frame = 0;
    sequence = 0;
    dispatch_sequence = 0;
    image_dispatches.clear();
    freed_ring = {};
    freed_next = 0;
    causes.clear();
    prior_writers.clear();
    log_budget.store(lines);
    dispatch_budget.store(lines);
}

} // namespace

void NoteDirty(const ImageKey& key, DirtySource source, VAddr write_address, u64 write_size,
               u64 writer) {
    std::scoped_lock lock{mutex};
    auto& record = records[key];
    record.pending_source = source;
    record.pending_write = write_address;
    record.pending_write_size = write_size;
    record.pending_writer = writer;
    ++dirty_notes;
}

void NoteUpload(const UploadEvent& event) {
    std::scoped_lock lock{mutex};
    auto& r = records[event.key];
    if (!r.uploads) {
        r.width = event.width, r.height = event.height, r.depth = event.depth;
        r.layers = event.layers, r.levels = event.levels, r.tile_mode = event.tile_mode;
        r.tiled = event.tiled, r.scaled = event.scaled;
        r.first_frame = event.frame;
    }
    r.last_frame = event.frame;
    first_frame = std::min(first_frame, event.frame);
    last_frame = std::max(last_frame, event.frame);

    u32 compared = 0, same = 0;
    if (!event.gpu_resident) {
        for (size_t i = 0; i < event.mips.size() && i < event.hashes.size(); ++i) {
            const u32 mip = event.mips[i];
            if (mip >= MaxMips) continue;
            if (r.hashed_mask & (1u << mip)) {
                ++compared;
                same += r.last_hash[mip] == event.hashes[i];
            }
            r.last_hash[mip] = event.hashes[i];
            r.hashed_mask |= 1u << mip;
        }
    }
    const Outcome outcome = event.gpu_resident ? Unhashed
                            : compared == 0    ? First
                            : same == compared ? Unchanged
                            : same == 0        ? Changed
                                               : Partial;
    const auto source = static_cast<size_t>(r.pending_source);
    ++r.uploads;
    ++r.outcomes[outcome];
    ++r.sources[source];
    ++matrix[source][outcome];
    if (r.pending_writer) {
        ++writers[r.pending_writer];
        r.last_writer = r.pending_writer;
    }
    ++total_uploads;
    ++sequence;
    const Freed* prior = FindFreed(event.key.address, event.key.size);
    const std::string prior_text =
        prior ? fmt::format("{}:{}:{}{}", WriteKindNames[static_cast<size_t>(prior->kind)],
                            FormatName(prior->format), event.frame - prior->frame,
                            prior->kept ? ":kept" : "")
              : std::string{"none"};
    const char* created = event.created < CreatedCount ? CreatedNames[event.created] : "unknown";
    ++causes[fmt::format("{} {} {}", UsageName(event.usage), created,
                         prior ? WriteKindNames[static_cast<size_t>(prior->kind)] : "none")];
    if (prior && prior->writer) ++prior_writers[prior->writer];

    u32 budget = log_budget.load(std::memory_order_relaxed);
    while (budget && !log_budget.compare_exchange_weak(budget, budget - 1)) {}
    if (budget) {
        const s64 offset = s64(r.pending_write) - s64(event.key.address);
        LOG_INFO(Render_Vulkan,
                 "Upload diag #{} frame={} {:#x}+{:#x} {} {}x{}x{} L{} M{}/{} tile={} {} flags={}{}{}{} "
                 "source={} write={:+#x}+{} writer={:#x} gpu_resident={} outcome={} mips_same={}/{} "
                 "image_uploads={} usage={} created={} action={:#x}{} prior={} prior_writer={:#x}",
                 sequence, event.frame, event.key.address, event.key.size, FormatName(event.key.format),
                 event.width, event.height, event.depth, event.layers, event.mips.size(), event.levels,
                 event.tile_mode, event.scaled ? "scaled" : "native", event.cpu_dirty ? "C" : "",
                 event.maybe_cpu_dirty ? "M" : "", event.gpu_dirty ? "G" : "",
                 event.gpu_modified ? "R" : "", SourceNames[source],
                 r.pending_source == DirtySource::None ? 0 : offset, r.pending_write_size,
                 r.pending_writer,
                 event.gpu_resident ? 1 : 0, OutcomeNames[outcome], same, compared, r.uploads,
                 UsageName(event.usage), created, event.action,
                 event.action_dispatch ? "(cs)" : "(draw)", prior_text,
                 prior ? prior->writer : 0);
    }
    r.pending_source = DirtySource::None;
    r.pending_write = 0;
    r.pending_write_size = 0;
    r.pending_writer = 0;
}

void NoteFreedGpuImage(VAddr address, u64 size, u32 format, WriteKind kind, u64 writer,
                       u32 frame, bool kept) {
    std::scoped_lock lock{mutex};
    freed_ring[freed_next] = {address, size, format, kind, writer, frame, kept};
    freed_next = (freed_next + 1) % FreedRingSize;
}

void NoteUndefinedMiss(UndefinedMiss miss, VAddr address, u64 size, u32 format, bool uploaded) {
    pool_undefined_misses[static_cast<size_t>(miss)].fetch_add(1, std::memory_order_relaxed);
    static std::atomic<u32> logged{};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
        static constexpr std::array<const char*, 4> names{"defined", "untracked", "gpu_buffer",
                                                          "sampled"};
        LOG_INFO(Render_Vulkan, "pool_undefined miss {} {:#x}+{:#x} format {} uploaded {}",
                 names[static_cast<size_t>(miss)], address, size, format, uploaded);
    }
}

void NoteFill(FillOutcome outcome, u32 images_cleared, u64 bytes) {
    fill_outcomes[static_cast<size_t>(outcome)].fetch_add(1, std::memory_order_relaxed);
    if (images_cleared) {
        fill_images.fetch_add(images_cleared, std::memory_order_relaxed);
        fill_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
}

void NoteDispatch(u64 pgm_hash, u32 dim_x, u32 dim_y, u32 dim_z, bool indirect,
                  std::span<const DispatchBinding> bindings) {
    u64 seq;
    {
        std::scoped_lock lock{mutex};
        ++image_dispatches[pgm_hash];
        seq = ++dispatch_sequence;
    }
    u32 budget = dispatch_budget.load(std::memory_order_relaxed);
    while (budget && !dispatch_budget.compare_exchange_weak(budget, budget - 1)) {}
    if (!budget) return;
    std::string text;
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i];
        text += fmt::format(" | b{} {:#x}+{:#x} stride={} records={} fmt={}/{} {}{}", i, b.address,
                            b.size, b.stride, b.num_records, b.data_format, b.num_format,
                            b.written ? "W" : "R", b.formatted ? "F" : "");
        if (b.gpu_resident)
            text += " head=gpu_resident";
        else if (b.head_valid)
            text += fmt::format(" head={:08x} {:08x} {:08x} {:08x}", b.head[0], b.head[1], b.head[2],
                                b.head[3]);
        else
            text += " head=unreadable";
        if (!b.images.empty()) text += " images=" + b.images;
    }
    LOG_INFO(Render_Vulkan, "Dispatch diag #{} pgm={:#x} dims={}x{}x{}{}{}", seq, pgm_hash, dim_x,
             dim_y, dim_z, indirect ? " indirect" : "", text);
}

std::string Command(const std::vector<std::string>& args) {
    const std::string sub = args.empty() ? "status" : args[0];
    if (sub == "start" && args.size() <= 2) {
        u32 lines = DefaultLogLines;
        if (args.size() == 2) {
            const auto& text = args[1];
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), lines);
            if (ec != std::errc{} || end != text.data() + text.size())
                return "status=bad_arguments usage: start [log_lines] | status | stop\n";
            lines = std::min(lines, MaxLogLines);
        }
        armed.store(false);
        Reset(lines);
        armed.store(true);
        return fmt::format("status=armed log_lines={}\n", lines);
    }
    if (sub == "stop" && args.size() == 1) {
        armed.store(false);
        return "status=stopped\n" + Summary();
    }
    if (sub == "status" && args.size() <= 1) return Summary();
    if (sub == "ignore_storage_dirty" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        ignore_storage_dirty.store(args[1] == "on");
        return fmt::format("ignore_storage_dirty={} (diagnostic A/B only; may show stale images)\n",
                           args[1]);
    }
    if (sub == "expand_twin" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        expand_twin.store(args[1] == "on");
        return fmt::format("expand_twin={} (ExpandImage {})\n", args[1],
                           args[1] == "on" ? "skips covered uploads, parks reinterpretations"
                                           : "uploads then copies, frees the source");
    }
    if (sub == "pool_revive" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        pool_revive.store(args[1] == "on");
        return fmt::format("pool_revive={} (GPU images of decommitted pool memory {})\n", args[1],
                           args[1] == "on" ? "come back at the next commit" : "are freed");
    }
    if (sub == "async_writeback" && args.size() == 2 &&
        (args[1] == "off" || args[1] == "tiny" || args[1] == "all")) {
        async_writeback.store(args[1] == "off"    ? AsyncWriteback::Off
                              : args[1] == "tiny" ? AsyncWriteback::Tiny
                                                  : AsyncWriteback::All);
        return fmt::format("async_writeback={} deferred fences={} submits={}\n", args[1],
                           async_writeback_fences.load(), async_writeback_submits.load());
    }
    if (sub == "writeback_log" && args.size() == 2) {
        u32 lines{};
        const auto [ptr, ec] =
            std::from_chars(args[1].data(), args[1].data() + args[1].size(), lines);
        if (ec != std::errc{} || ptr != args[1].data() + args[1].size() || lines > 4096) {
            return "status=bad_arguments writeback_log <0..4096>\n";
        }
        writeback_log_left.store(lines);
        writeback_wait_log_left.store(lines);
        return fmt::format("writeback_log={} (write-backs and waits for them)\n", lines);
    }
    if (sub == "corner_views" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        corner_views.store(args[1] == "on");
        return fmt::format("corner_views={} images={} copies={} misses={}\n", args[1],
                           corner_images.load(), corner_copies.load(), corner_misses.load());
    }
    if (sub == "stream_submit" && args.size() == 2) {
        try {
            stream_submit_draws.store(u32(std::stoul(args[1])));
        } catch (const std::exception&) {
            return "status=bad_arguments usage: stream_submit <draws, 0 = off>\n";
        }
        return fmt::format("stream_submit={} early_submits={}\n", stream_submit_draws.load(),
                           stream_submits.load());
    }
    if (sub == "stream_reuse" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        StreamReuse::enabled.store(args[1] == "on");
        return StreamReuseLine();
    }
    if (sub == "stream_reuse" && args.size() == 1) {
        return StreamReuseLine();
    }
    if (sub == "target_memo" && args.size() == 2 &&
        (args[1] == "on" || args[1] == "off" || args[1] == "verify")) {
        target_memo_mode.store(args[1] == "off" ? 0u : args[1] == "on" ? 1u : 2u);
        return TargetMemoLine();
    }
    if (sub == "target_memo" && args.size() == 1) {
        return TargetMemoLine();
    }
    if (sub == "key_memo" && args.size() == 2 &&
        (args[1] == "on" || args[1] == "off" || args[1] == "verify")) {
        key_memo_mode.store(args[1] == "off" ? 0u : args[1] == "on" ? 1u : 2u);
        return KeyMemoLine();
    }
    if (sub == "key_memo" && args.size() == 1) {
        return KeyMemoLine();
    }
    if (sub == "unused_small_depth" && args.size() == 2 &&
        (args[1] == "drop" || args[1] == "keep")) {
        unused_small_depth.store(args[1] == "drop");
        return fmt::format("unused_small_depth={} dropped_draws={}\n", args[1],
                           unused_small_depth_draws.load());
    }
    if (sub == "tiny_linear_writeback" && args.size() == 2 &&
        (args[1] == "on" || args[1] == "off")) {
        tiny_linear_writeback.store(args[1] == "on");
        return fmt::format("tiny_linear_writeback={} writebacks={}\n", args[1],
                           tiny_linear_writebacks.load());
    }
    if (sub == "tiny_writeback" && args.size() == 2 &&
        (args[1] == "on" || args[1] == "off" || args[1] == "dump")) {
        tiny_writeback_dump.store(args[1] == "dump");
        tiny_writeback.store(args[1] != "off");
        return fmt::format("tiny_writeback={} (render targets of <= 16 pixels written now and "
                           "then {}); writebacks={} per_frame_skips={}\n",
                           args[1], args[1] != "off" ? "reach guest memory at the next fence"
                                                     : "stay on the GPU",
                           tiny_writebacks.load(), tiny_writeback_skips.load());
    }
    if (sub == "adopt_mips" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        adopt_subresources.store(args[1] == "on");
        return fmt::format("adopt_mips={} (GPU-written images that are mips of a new image {}); "
                           "adopted={}\n",
                           args[1], args[1] == "on" ? "are copied into it" : "are left behind",
                           adopted_subresources.load());
    }
    if (sub == "image_stats" && args.size() == 2) {
        const u64 address = std::strtoull(args[1].c_str(), nullptr, 16);
        stats_address.store(address);
        return fmt::format("image_stats armed for the image at {:#x}\n", address);
    }
    if (sub == "cb_dump" && args.size() >= 2 && args.size() <= 4) {
        const u64 hash = std::strtoull(args[1].c_str(), nullptr, 16);
        const s32 count = args.size() >= 3 ? std::clamp(std::atoi(args[2].c_str()), 1, 64) : 4;
        const u32 bytes = args.size() == 4 ? std::clamp(std::atoi(args[3].c_str()), 4, 4096) : 64;
        cb_dump_left.store(0);
        cb_dump_hash.store(hash);
        cb_dump_bytes.store(bytes);
        cb_dump_left.store(count);
        return fmt::format("cb_dump armed for program {:#x}, {} binds\n", hash, count);
    }
    if (sub == "format_list" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        format_list.store(args[1] == "on" ? 1 : 0);
        return fmt::format("format_list={} (images created from now on)\n", args[1]);
    }
    if (sub == "fill_clear" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        fill_clear_off.store(args[1] == "off");
        return fmt::format("fill_clear={} (compute fill kernels {})\n", args[1],
                           args[1] == "on" ? "clear images directly" : "dispatch as before");
    }
    if (sub == "raw_sync" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        raw_sync_off.store(args[1] == "off");
        return fmt::format("raw_sync={} (raw buffer reads of GPU-written images {})\n", args[1],
                           args[1] == "on" ? "tile the image back first" : "read the buffer as is");
    }
    if (sub == "dma_bounds" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        dma_bounds.store(args[1] == "on");
        return fmt::format("dma_bounds={} (default on; off synchronizes all resident ranges)\n",
                           args[1]);
    }
    if (sub == "depth_copy" && args.size() == 2 && (args[1] == "maint8" || args[1] == "buffer")) {
        depth_copy_buffer.store(args[1] == "buffer");
        return fmt::format("depth_copy={} (depth<->color twins copied {})\n", args[1],
                           args[1] == "buffer" ? "through the scratch buffer"
                                               : "directly when maintenance8 is available");
    }
    if (sub == "htile_copy" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        htile_copy_off.store(args[1] == "off");
        return fmt::format("htile_copy={} (DMA copies of an HTILE template {})\n", args[1],
                           args[1] == "on" ? "clear the depth target" : "only copy bytes");
    }
    if (sub == "raw_copy" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        raw_copy_off.store(args[1] == "off");
        return fmt::format("raw_copy={} (raw dword copies between images {})\n", args[1],
                           args[1] == "on" ? "copy the images" : "dispatch as before");
    }
    if (sub == "watch_coalesce" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        Core::gpu_watch_per_page.store(args[1] == "off");
        return fmt::format("watch_coalesce={} (GPU write-watch mprotect {})\n", args[1],
                           args[1] == "on" ? "per run of pages" : "per 4 KiB page");
    }
    if (sub == "watch_predict" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        predict_write_faults.store(args[1] == "on");
        return fmt::format("watch_predict={} (write faults {})\n", args[1],
                           args[1] == "on" ? "also release pages rewritten last cycle"
                                           : "release only the faulting page");
    }
    if (sub == "watch_cross" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        predict_cross_region.store(args[1] == "on");
        return fmt::format("watch_cross={} (a release ahead reaching the end of its region {})\n",
                           args[1],
                           args[1] == "on" ? "goes on into the next region" : "stops there");
    }
    if (sub == "watch_decay" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        decay_unchanged.store(args[1] == "on");
        return fmt::format("watch_decay={} (released-ahead pages uploaded unchanged {})\n", args[1],
                           args[1] == "on" ? "lose one step of write confidence"
                                           : "keep their write confidence");
    }
    if (sub == "watch_gaps" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        predict_across_gaps.store(args[1] == "on");
        return fmt::format("watch_gaps={} (release ahead {})\n", args[1],
                           args[1] == "on" ? "every confident page left in the window"
                                           : "up to the first page without write confidence");
    }
    if (sub == "watch_content" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        predict_from_contents.store(args[1] == "on");
        return fmt::format("watch_content={} (released-ahead pages {})\n", args[1],
                           args[1] == "on" ? "keep their write confidence when rewritten"
                                           : "lose write confidence every cycle");
    }
    if (sub == "stream_host" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        stream_host.store(args[1] == "on");
        return fmt::format("stream_host={} (streamed data in {} memory)\n", args[1],
                           args[1] == "on" ? "host" : "device");
    }
    if (sub == "watch_stream" && (args.size() == 2 || args.size() == 3) &&
        (args[1] == "on" || args[1] == "off")) {
        if (args.size() == 3) {
            const u32 cycles = args[2] == "1" ? 1 : args[2] == "2" ? 2 : args[2] == "3" ? 3 : 0;
            if (cycles == 0) {
                return "usage: upload_diag watch_stream on|off [1|2|3]\n";
            }
            stream_promote_cycles.store(cycles);
        }
        stream_pages.store(args[1] == "on");
        return fmt::format("watch_stream={} cycles={} (pages the CPU rewrites every write cycle "
                           "{})\n",
                           args[1], stream_promote_cycles.load(),
                           args[1] == "on" ? "stay unwatched and are copied when they changed"
                                           : "are watched again at their next upload");
    }
    if (sub == "gpu_flag" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        region_gpu_flag.store(args[1] == "on");
        return fmt::format("gpu_flag={} (GPU-modified queries {})\n", args[1],
                           args[1] == "on" ? "skip regions without GPU-modified pages"
                                           : "lock and scan every region");
    }
    if (sub == "texture_bind_fast" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        texture_bind_fast.store(args[1] == "on");
        return fmt::format("texture_bind_fast={} (unchanged sampled bindings {}); fast_hits={}\n",
                           args[1], args[1] == "on" ? "reuse view and layout" : "take the full path",
                           texture_bind_fast_hits.load());
    }
    if (sub == "texture_bind_cache" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        texture_bind_cache.store(args[1] == "on");
        return fmt::format("texture_bind_cache={} (sampled-image lookups {}); hits={} misses={}\n",
                           args[1], args[1] == "on" ? "reused per T#" : "resolved every draw",
                           texture_bind_hits.load(), texture_bind_misses.load());
    }
    if (sub == "stream_dma" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        stream_dma.store(args[1] == "on");
        return fmt::format("stream_dma={} (streamed data {})\n", args[1],
                           args[1] == "on" ? "copied into VRAM on the transfer queue where available"
                                           : "as selected by stream_host");
    }
    if (sub == "pool_undefined" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        pool_undefined.store(args[1] == "on");
        return fmt::format("pool_undefined={} (images the GPU writes first, created in freshly "
                           "committed pool memory, {}); skips={} skip_bytes={}\n",
                           args[1], args[1] == "on" ? "skip the guest upload" : "upload it",
                           pool_undefined_skips.load(), pool_undefined_skip_bytes.load());
    }
    if (sub == "fault_textures" && args.size() == 2 &&
        (args[1] == "skip" || args[1] == "always")) {
        skip_unwatched_fault_textures.store(args[1] == "skip");
        return fmt::format("fault_textures={} (write faults on pages no image watches {})\n",
                           args[1], args[1] == "skip" ? "skip the texture cache"
                                                      : "still call the texture cache");
    }
    if (sub == "stream_lease" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        stream_lease.store(args[1] == "on");
        return fmt::format("stream_lease={} (streamed copies of a draw {})\n", args[1],
                           args[1] == "on" ? "share one memory read lock"
                                           : "lock the memory manager each");
    }
    if (sub == "stream_bounce" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        stream_bounce.store(args[1] == "on");
        return fmt::format("stream_bounce={} (streamed data {})\n", args[1],
                           args[1] == "on" ? "copied through a cached buffer"
                                           : "copied straight into the stream buffer");
    }
    if (sub == "stream_max" && args.size() == 2) {
        u32 bytes = 0;
        const auto [end, ec] =
            std::from_chars(args[1].data(), args[1].data() + args[1].size(), bytes);
        if (ec != std::errc{} || end != args[1].data() + args[1].size()) {
            return "status=bad_arguments usage: stream_max <bytes>\n";
        }
        stream_max.store(bytes);
        return fmt::format("stream_max={} (read-only buffers up to this size are copied into "
                           "the stream buffer)\n",
                           bytes);
    }
    if (sub == "read_cache" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        Core::MemoryManager::read_cache_enabled.store(args[1] == "on");
        return fmt::format("read_cache={} (streamed buffer copies {})\n", args[1],
                           args[1] == "on" ? "reuse recent mapping lookups"
                                           : "look up the mapping every time");
    }
    if (sub == "full_barriers" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        full_barriers.store(args[1] == "on");
        return fmt::format("full_barriers={} (full memory barrier after each pass, around each "
                           "dispatch)\n",
                           args[1]);
    }
    if (sub == "sharp_watch" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        sharp_watch.store(args[1] == "on");
        return fmt::format("sharp_watch={} (bound V#/T# bases outside guest memory logged)\n",
                           args[1]);
    }
    if (sub == "draw_pairs" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        draw_pairs.store(args[1] == "on");
        return fmt::format("draw_pairs={} (first use of each shader pair logged)\n", args[1]);
    }
    if (sub == "vb_watch" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        vb_watch.store(args[1] == "on");
        return fmt::format("vb_watch={} (vertex buffer V#s re-read after each draw)\n", args[1]);
    }
    if (sub == "frame_shape" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        frame_shape.store(args[1] == "on");
        return fmt::format("frame_shape={} (per-frame draw summaries in the log)\n", args[1]);
    }
    if (sub == "stream_barriers" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        stream_barriers.store(args[1] == "on");
        return fmt::format("stream_barriers={} (reads of streamed buffers {} the barrier tracker)\n",
                           args[1], args[1] == "on" ? "go through" : "skip");
    }
    if (sub == "keep_gpu" && args.size() == 2 && (args[1] == "on" || args[1] == "off")) {
        GpuByteKeeper::enabled.store(args[1] == "on");
        return fmt::format("keep_gpu={} (CPU writes beside GPU-written bytes {})\n", args[1],
                           args[1] == "on" ? "keep those bytes" : "re-upload the whole page");
    }
    return "status=bad_arguments usage: start [log_lines] | status | stop | "
           "ignore_storage_dirty on|off | fill_clear on|off | watch_coalesce on|off | "
           "watch_predict on|off | watch_content on|off | watch_gaps on|off | "
           "watch_decay on|off | watch_cross on|off | keep_gpu on|off | read_cache on|off | "
           "stream_barriers on|off | frame_shape on|off | vb_watch on|off | draw_pairs on|off | "
           "sharp_watch on|off | full_barriers on|off | "
           "stream_max <bytes> | stream_lease on|off | stream_bounce on|off | stream_host on|off | stream_dma on|off | "
           "texture_bind_cache on|off | texture_bind_fast on|off | gpu_flag on|off | watch_stream on|off [1|2|3] | "
           "fault_textures skip|always | pool_undefined on|off | expand_twin on|off | "
           "pool_revive on|off | tiny_writeback on|off|dump | tiny_linear_writeback on|off | "
           "async_writeback off|tiny|all | writeback_log <n> | corner_views on|off | "
           "unused_small_depth drop|keep | stream_submit <draws> | stream_reuse [on|off] | "
           "target_memo [on|off|verify] | key_memo [on|off|verify]\n";
}

} // namespace VideoCore::UploadDiagnostics
