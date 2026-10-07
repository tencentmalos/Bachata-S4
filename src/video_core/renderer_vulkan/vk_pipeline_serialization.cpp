// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <tuple>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "common/serdes.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 39u; // 16-bit inline 1/(2*pi) is half 0x3118
#ifdef ARCH_X86_64
static constexpr u32 ShaderMetaVersion = 23u; // dynamic image slot masks
#else
static constexpr u32 ShaderMetaVersion = 24u; // dynamic image slot masks
#endif
static constexpr u32 PipelineKeyVersion = 7u; // indirect draw base vertex/instance parameters
} // namespace Serialization

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar, PreloadJob& job) {
    job.compute = true;
    job.compute_key.Deserialize(ar);
    job.compute_data.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", job.compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};
    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }
    job.infos = infos;
    job.modules = modules;
    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
    sdata.Write(interpolation_gs);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    sdata.Read(interpolation_gs);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar, PreloadJob& job) {
    job.graphics_key.Deserialize(ar);
    job.graphics_data.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = job.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }
    job.infos = infos;
    job.modules = modules;
    job.fetch_shader = fetch_shader;
    return true;
}

namespace {

// Driver pipeline creation of a preload runs on this many threads. Qualcomm's proprietary
// driver gets one, as in citron; on Android a few cores stay free for the rest of startup.
u32 PreloadThreads(const Instance& instance, std::size_t jobs) {
    if (instance.GetDriverID() == vk::DriverId::eQualcommProprietary) {
        return 1;
    }
    const u32 cores = std::max(1u, std::thread::hardware_concurrency());
#ifdef __ANDROID__
    const u32 threads = std::clamp(cores > 3 ? cores - 3 : 1u, 1u, 4u);
#else
    const u32 threads = std::clamp(cores > 2 ? cores - 2 : 1u, 1u, 8u);
#endif
    return u32(std::min<std::size_t>(threads, std::max<std::size_t>(jobs / 8, 1)));
}

} // namespace

u32 PipelineCache::BuildPreloaded(std::vector<PreloadJob>& jobs) {
    // With compile workers the driver objects are left to them (in priority order, see WarmUp);
    // only layouts and create state are made here.
    const bool defer = compiler->Threads() > 0;
    const auto build = [&](PreloadJob& job) {
        try {
            if (job.compute) {
                job.compute_pipeline = std::make_unique<ComputePipeline>(
                    instance, scheduler, desc_heap, profile, driver_cache->Handle(),
                    job.compute_key, *job.infos[0], job.modules[0], job.compute_data, true,
                    defer);
            } else {
                job.graphics = std::make_unique<GraphicsPipeline>(
                    instance, scheduler, desc_heap, profile, job.graphics_key,
                    driver_cache->Handle(), job.infos, runtime_infos, job.fetch_shader,
                    job.modules, job.graphics_data, true, defer);
            }
        } catch (const std::exception& e) {
            // The pipeline is simply built again when a draw needs it.
            LOG_WARNING(Render_Vulkan, "Preloading a cached pipeline failed: {}", e.what());
        }
    };
    const u32 threads = PreloadThreads(instance, jobs.size());
    if (threads <= 1) {
        std::ranges::for_each(jobs, build);
        return threads;
    }
    std::atomic<std::size_t> next{};
    std::vector<std::jthread> workers;
    for (u32 i = 0; i < threads; ++i) {
        workers.emplace_back([&] {
            Common::SetCurrentThreadName("shadPS4:PipelinePreload");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
            for (std::size_t index; (index = next.fetch_add(1)) < jobs.size();) {
                build(jobs[index]);
            }
        });
    }
    return threads;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto info = std::make_unique<Shader::Info>();
    Shader::StageSpecialization spec{};
    spec.info = info.get();
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, *info, fetch_shader, spec, perm_idx))
        return false;
    // A malformed cache must not allocate an unbounded sparse module vector.
    if (perm_idx > 4095)
        return false;
    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", info->pgm_hash, perm_idx), spv);
    if (spv.empty())
        return false;

    auto [it_pgm, new_program] = program_cache.try_emplace(info->pgm_hash);
    if (new_program)
        it_pgm.value() = std::make_unique<Program>();
    auto& program = *it_pgm.value();
    if (perm_idx < program.modules.size() && program.modules[perm_idx].info) {
        // Never replace a loaded Info: earlier preload jobs/pipelines retain it.
        const auto& existing = program.modules[perm_idx];
        if (!(existing.spec == spec))
            return false;
        infos[stage] = existing.info.get();
        modules[stage] = existing.module;
        return true;
    }
    const auto module = CompileSPV(spv, instance.GetDevice());
    program.InsertPermut(module, std::move(info), std::move(spec), perm_idx);
    infos[stage] = program.modules[perm_idx].info.get();
    modules[stage] = module;
    return true;
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    auto& storage = Storage::DataBase::Instance();
    storage.Open();
    if (!storage.IsOpened()) {
        return;
    }

    // The cache is only valid for the shader profile it was built with (device features,
    // render scale, MSAA policy) and for the current serialization formats. Anything else
    // replaces it, rather than turning caching off for the session or leaving blobs every later
    // preload has to read and reject.
    const std::array<u32, 4> versions = {Serialization::ShaderBinaryVersion,
                                         Serialization::ShaderMetaVersion,
                                         Serialization::PipelineKeyVersion,
                                         Storage::BlobFormatVersion};
    const auto start_fresh = [&] {
        storage.FinishPreload();
        std::vector<u8> data(sizeof(profile) + sizeof(versions));
        std::memcpy(data.data(), &profile, sizeof(profile));
        std::memcpy(data.data() + sizeof(profile), versions.data(), sizeof(versions));
        storage.Save(Storage::BlobType::ShaderProfile, "profile", std::move(data));
    };

    std::vector<u8> profile_data{};
    const auto profile_result =
        storage.Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_result == Storage::LoadResult::Missing) {
        start_fresh();
        return;
    }
    Shader::Profile cached_profile{};
    std::array<u32, 4> cached_versions{};
    const bool readable = profile_data.size() == sizeof(profile) + sizeof(versions);
    if (readable) {
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
        std::memcpy(cached_versions.data(), profile_data.data() + sizeof(profile),
                    sizeof(cached_versions));
    }
    if (!readable || cached_profile != profile || cached_versions != versions) {
        LOG_WARNING(Render, "Pipeline cache was built for a different shader profile or format, "
                            "rebuilding it");
        storage.Reset();
        start_fresh();
        return;
    }

    const auto preload_start = std::chrono::steady_clock::now();

    u32 num_pipelines{};
    u32 num_total_pipelines{};

    // Shaders are read and their modules created in order (programs and permutation indices
    // are shared state); only driver pipeline creation runs in parallel.
    std::vector<PreloadJob> jobs;
    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;

            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);

            PreloadJob job{};
            const bool result =
                is_compute ? LoadComputePipeline(ar, job) : LoadGraphicsPipeline(ar, job);
            // A pipeline whose later stage failed must not leave its earlier stages behind for
            // the next one.
            infos.fill(nullptr);
            modules.fill(nullptr);
            fetch_shader.reset();
            if (result) {
                jobs.push_back(std::move(job));
            }
        });

    const u32 preload_threads = BuildPreloaded(jobs);

    // Order: pipelines recent sessions used, by how many sessions ago, then by when in the
    // session they were first needed; pipelines without a record keep their file order last.
    LoadUsage();
    struct Ordered {
        const Pipeline* pipeline;
        UsageRecord usage;
        bool known;
    };
    std::vector<Ordered> order;
    order.reserve(jobs.size());
    PipelineStats::PreloadTiers tiers{};
    for (auto& job : jobs) {
        Pipeline* pipeline{};
        u64 hash{};
        if (job.compute_pipeline) {
            const auto [it, is_new] = compute_pipelines.try_emplace(job.compute_key);
            if (!is_new) {
                continue;
            }
            it.value() = std::move(job.compute_pipeline);
            pipeline = it.value().get();
            hash = job.compute_key.value;
        } else if (job.graphics) {
            const auto [it, is_new] = graphics_pipelines.try_emplace(job.graphics_key);
            if (!is_new) {
                continue;
            }
            it.value() = std::move(job.graphics);
            pipeline = it.value().get();
            hash = std::hash<GraphicsPipelineKey>{}(job.graphics_key);
        } else {
            continue;
        }
        ++num_pipelines;
        if (pipeline->Ready()) {
            NotePipeline(*pipeline, hash, true); // Built in its constructor (no workers).
            continue;
        }
        pipeline->SetBuildObserver(this, hash, true);
        const auto record = usage.find(UsageKey(pipeline->IsCompute(), hash));
        const bool known = record != usage.end();
        tiers.usage_entries += known ? 1 : 0;
        order.push_back({pipeline, known ? record->second : UsageRecord{}, known});
    }
    std::ranges::stable_sort(order, [](const Ordered& a, const Ordered& b) {
        if (a.known != b.known) {
            return a.known;
        }
        return std::tie(a.usage.idle_sessions, a.usage.first_ms, a.usage.rank) <
               std::tie(b.usage.idle_sessions, b.usage.first_ms, b.usage.rank);
    });
    tiers.construct_ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - preload_start)
                                 .count());

    // Build from the front for at most the wait budget, with every worker and this thread; the
    // rest is left to the workers (one at a time) and to first users, who build a pipeline
    // themselves when it is still queued.
    std::vector<const Pipeline*> backlog;
    backlog.reserve(order.size());
    for (const auto& entry : order) {
        backlog.push_back(entry.pipeline);
    }
    compiler->SetBacklogConcurrency(compiler->Threads());
    compiler->SubmitBacklog(backlog);
    const auto wait_start = std::chrono::steady_clock::now();
    const auto budget = std::chrono::milliseconds{EmulatorSettings.GetPipelinePreloadWaitMs()};
    for (const Pipeline* pipeline : backlog) {
        if (std::chrono::steady_clock::now() - wait_start >= budget) {
            break;
        }
        pipeline->WaitHandle(false);
    }
    compiler->SetBacklogConcurrency(1);
    tiers.wait_ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - wait_start)
                            .count());
    tiers.waited = u32(std::ranges::count_if(backlog, [](const Pipeline* p) { return p->Ready(); }));
    tiers.background = u32(backlog.size()) - tiers.waited;
    PipelineStats::RecordPreloadTiers(tiers);

    const auto preload_ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - preload_start)
                                    .count());
    PipelineStats::RecordPreload(num_total_pipelines, preload_threads, preload_ns);
    LOG_INFO(Render,
             "Preloaded {} of {} cached pipelines in {:.1f} ms on {} threads: {} built before "
             "start, {} left to the compile workers ({} ordered by earlier use)",
             num_pipelines, num_total_pipelines, double(preload_ns) / 1e6, preload_threads,
             tiers.waited, tiers.background, tiers.usage_entries);
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

namespace {

constexpr u32 UsageMagic = 0x53555053; // "SPUS"
constexpr u32 UsageFormat = 1;
constexpr u32 UsageEntrySize = 8 + 1 + 4 * 4;
constexpr u32 UsageMaxIdleSessions = 32;

void PutUsageLe(std::vector<u8>& out, u64 value, u32 bytes) {
    for (u32 i = 0; i < bytes; ++i) {
        out.push_back(u8(value >> (8 * i)));
    }
}

u64 GetUsageLe(const u8* in, u32 bytes) {
    u64 value = 0;
    for (u32 i = 0; i < bytes; ++i) {
        value |= u64(in[i]) << (8 * i);
    }
    return value;
}

} // namespace

void PipelineCache::LoadUsage() {
    usage.clear();
    std::vector<u8> data;
    if (Storage::DataBase::Instance().Load(Storage::BlobType::Usage, "usage", data) !=
            Storage::LoadResult::Ok ||
        data.size() < 12 || GetUsageLe(data.data(), 4) != UsageMagic ||
        GetUsageLe(data.data() + 4, 4) != UsageFormat) {
        return;
    }
    const u64 count = GetUsageLe(data.data() + 8, 4);
    if (count > (data.size() - 12) / UsageEntrySize) {
        return;
    }
    const u8* entry = data.data() + 12;
    for (u64 i = 0; i < count; ++i, entry += UsageEntrySize) {
        const u64 hash = GetUsageLe(entry, 8);
        const bool compute = entry[8] != 0;
        UsageRecord record{
            .rank = u32(GetUsageLe(entry + 9, 4)),
            .first_ms = u32(GetUsageLe(entry + 13, 4)),
            .idle_sessions = u32(GetUsageLe(entry + 17, 4)),
            .compile_us = u32(GetUsageLe(entry + 21, 4)),
        };
        usage[UsageKey(compute, hash)] = record;
    }
}

void PipelineCache::SaveUsage() {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }
    // Loaded records describe earlier sessions: one more session has passed for each of them.
    auto merged = usage;
    for (auto& [key, record] : merged) {
        ++record.idle_sessions;
    }
    const auto note = [&](const Pipeline& pipeline, u64 hash) {
        const bool used = pipeline.FirstUseRank() != 0;
        const bool built = pipeline.Ready();
        if (!used && !built) {
            return;
        }
        const auto [it, inserted] = merged.try_emplace(UsageKey(pipeline.IsCompute(), hash));
        auto& record = it->second;
        if (used) {
            record.rank = pipeline.FirstUseRank();
            record.first_ms = pipeline.FirstUseMs();
            record.idle_sessions = 0;
        } else if (inserted) {
            record.idle_sessions = 1; // Built (preloaded) but not needed this session.
        }
        if (built && pipeline.Creation().ns != 0) {
            record.compile_us = u32(std::min<u64>(pipeline.Creation().ns / 1000, ~0U));
        }
    };
    for (const auto& [key, pipeline] : graphics_pipelines) {
        if (pipeline)
            note(*pipeline, std::hash<GraphicsPipelineKey>{}(key));
    }
    for (const auto& [key, pipeline] : compute_pipelines) {
        if (pipeline)
            note(*pipeline, key.value);
    }

    // The map key folds the kind into the hash; recover both for the file.
    std::vector<u8> out;
    out.reserve(12 + merged.size() * UsageEntrySize);
    PutUsageLe(out, UsageMagic, 4);
    PutUsageLe(out, UsageFormat, 4);
    PutUsageLe(out, 0, 4); // count, patched below
    u32 count{};
    const auto emit = [&](bool compute, u64 hash, const UsageRecord& record) {
        if (record.idle_sessions > UsageMaxIdleSessions) {
            return;
        }
        PutUsageLe(out, hash, 8);
        out.push_back(compute ? 1 : 0);
        PutUsageLe(out, record.rank, 4);
        PutUsageLe(out, record.first_ms, 4);
        PutUsageLe(out, record.idle_sessions, 4);
        PutUsageLe(out, record.compile_us, 4);
        ++count;
    };
    for (const auto& [key, pipeline] : graphics_pipelines) {
        const u64 hash = std::hash<GraphicsPipelineKey>{}(key);
        if (const auto it = merged.find(UsageKey(false, hash)); it != merged.end()) {
            emit(false, hash, it->second);
            merged.erase(it);
        }
    }
    for (const auto& [key, pipeline] : compute_pipelines) {
        if (const auto it = merged.find(UsageKey(true, key.value)); it != merged.end()) {
            emit(true, key.value, it->second);
            merged.erase(it);
        }
    }
    // Records of pipelines not loaded this session (e.g. a failed preload) cannot tell their
    // kind apart from the folded key any more; they are dropped rather than guessed.
    for (u32 i = 0; i < 4; ++i) {
        out[8 + i] = u8(count >> (8 * i));
    }
    Storage::DataBase::Instance().Save(Storage::BlobType::Usage, "usage", std::move(out));
    usage_checkpoint_rank = PipelineStats::UsedPipelines();
    usage_checkpoint_time = std::chrono::steady_clock::now();
    PipelineStats::RecordUsageSaved(count);
}

void PipelineCache::MaybeCheckpointUsage() {
    // A killed process (common on Android) never reaches Sync: store the first uses once a
    // minute while new ones appear. Checked every few thousand draws to keep the draw path cheap.
    if (++usage_checkpoint_calls % 4096 != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - usage_checkpoint_time < std::chrono::seconds{60}) {
        return;
    }
    const u32 rank = PipelineStats::UsedPipelines();
    if (rank == usage_checkpoint_rank) {
        usage_checkpoint_time = now;
        return;
    }
    SaveUsage();
}

void PipelineCache::Sync() {
    SaveUsage();
    // Flushes every queued blob to disk; later pipelines of this session are not cached.
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(uses_lane_id);
    info.Write(uses_group_ballot);
    info.Write(loads.flags.data(), sizeof(loads.flags));
    info.Write(stores.flags.data(), sizeof(stores.flags));
    info.Write(fs_interpolation.data(), sizeof(fs_interpolation));
    info.Write(translation_failed);
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(uses_lane_id);
    info.Read(uses_group_ballot);
    info.Read(loads.flags.data(), sizeof(loads.flags));
    info.Read(stores.flags.data(), sizeof(stores.flags));
    info.Read(fs_interpolation.data(), sizeof(fs_interpolation));
    info.Read(translation_failed);
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

#ifndef ARCH_X86_64
    srt.Write(flattened_bufsize_dw);
    srt.Write(portable.expressions);
    srt.Write(portable.commands);
#else
    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
#endif
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

#ifndef ARCH_X86_64
    if (ar.RemainingBytes() < sizeof(flattened_bufsize_dw) + sizeof(size_t))
        return false;
    srt.Read(flattened_bufsize_dw);
    PortableSrt plan;
    size_t count{};
    srt.Read(count);
    if (count > PortableSrt::MaxEntries || count > ar.RemainingBytes() / sizeof(PortableSrt::Expr))
        return false;
    plan.expressions.resize(count);
    for (auto& expr : plan.expressions)
        srt.Read(expr);
    if (ar.RemainingBytes() < sizeof(count))
        return false;
    srt.Read(count);
    if (count > PortableSrt::MaxEntries ||
        count > ar.RemainingBytes() / sizeof(PortableSrt::Command))
        return false;
    plan.commands.resize(count);
    for (auto& command : plan.commands)
        srt.Read(command);
    if (!plan.Validate(flattened_bufsize_dw))
        return false;
    portable = std::move(plan);
    walker_func = nullptr;
    walker_func_size = 0;
#else
    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }
#endif

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());
    for (const u32 mask : dynamic_image_masks) {
        spec.Write(mask);
    }

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);
    for (u32& mask : dynamic_image_masks) {
        spec.Read(mask);
    }

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
