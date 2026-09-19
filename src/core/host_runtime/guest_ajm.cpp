// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <thread>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_ajm.h"
#include "core/libraries/ajm/ajm_aac.h"
#include "core/libraries/ajm/ajm_at9.h"
#include "core/libraries/ajm/ajm_error.h"
#include "core/libraries/ajm/ajm_instance_statistics.h"
#include "core/libraries/ajm/ajm_mp3.h"

namespace Core::HostRuntime {
using namespace GuestCpu;
using namespace Libraries::Ajm;
namespace {
constexpr size_t MaxBuffer = 16 << 20, MaxResident = 64 << 20, MaxBatch = 1 << 20;
struct Failure {
    int error;
};
void Need(bool ok, int error = ORBIS_AJM_ERROR_MALFORMED_BATCH) {
    if (!ok)
        throw Failure{error};
}
struct Chunk {
    u32 header, size;
    u64 address;
};
struct BatchError {
    s32 error{};
    u32 pad{};
    u64 job{};
    u32 offset{}, pad2{};
    u64 ra{};
};
static_assert(sizeof(BatchError) == sizeof(AjmBatchError));
struct Cursor {
    std::span<const u8> bytes;
    template <class T>
    T Take() {
        Need(bytes.size() >= sizeof(T));
        T result;
        std::memcpy(&result, bytes.data(), sizeof(T));
        bytes = bytes.subspan(sizeof(T));
        return result;
    }
};
} // namespace
bool IsAjmNid(std::string_view nid) {
    return std::ranges::find(AjmNids, nid) != std::end(AjmNids);
}
struct GuestAjm::Impl {
    GuestAddressSpace& space;

    std::mutex mutex;
    std::condition_variable_any changed;
    bool stopping{};
    struct Instance {
        AjmInstance decoder;
        AjmCodecType codec;
        u32 sample_size;
        Instance(AjmCodecType codec, AjmInstanceFlags flags)
            : decoder(codec, flags), codec(codec),
              sample_size(GetPCMSize(AjmFormatEncoding(flags.format))) {}
    };
    struct Output {
        u64 address{};
        std::vector<u8> bytes;
        std::vector<GuestAddressSpace::MappingIdentity> identities;
    };
    struct Job {
        AjmJob job;
        std::shared_ptr<Instance> instance;
        std::deque<Output> outputs;
        u64 address{}, ra{};
        u32 offset{};
    };
    struct Batch {
        u32 id{}, context{};
        std::vector<std::unique_ptr<Job>> jobs;
        size_t bytes{};
        std::stop_source cancel;
        bool done{}, waiting{};
        BatchError error;
    };
    struct Context {
        std::array<bool, 3> codecs{};
        std::map<u32, std::shared_ptr<Instance>> instances;
        std::map<u64, u64> memory;
        u32 next_instance{1};
    };
    std::map<u32, Context> contexts;
    std::map<u32, std::shared_ptr<Batch>> batches;
    std::deque<std::shared_ptr<Batch>> queue;
    u32 next_context{1}, next_batch{1};
    size_t resident{};
    std::jthread worker;
    Impl(GuestAddressSpace& space)
        : space(space), worker([this](std::stop_token stop) { Work(stop); }) {}
    ~Impl() {
        Stop();
        worker.join();
    } // join before queues/decoders/VM owners disappear
    void Stop() {
        std::lock_guard lock(mutex);
        stopping = true;
        for (auto& [id, batch] : batches)
            batch->cancel.request_stop();
        worker.request_stop();
        changed.notify_all();
    }
    bool Valid(u64 address, size_t bytes, GuestPermission permission) {
        return bytes == 0 ||
               (address && bool(space.ValidateRange({GuestAddress{address}, bytes}, permission)));
    }
    std::vector<u8> Read(u64 address, size_t bytes) {
        Need(bytes <= MaxBuffer, ORBIS_AJM_ERROR_BUFFER_TOO_BIG);
        Need(Valid(address, bytes, GuestPermission::Read), ORBIS_AJM_ERROR_INVALID_ADDRESS);
        std::vector<u8> data(bytes);
        if (bytes)
            Need(bool(space.ReadData(GuestAddress{address},
                                     std::as_writable_bytes(std::span{data}))),
                 ORBIS_AJM_ERROR_INVALID_ADDRESS);
        return data;
    }
    void Put(u64 address, std::span<const u8> bytes) {
        if (bytes.empty())
            return;
        auto pin = space.AcquireDataSpan({GuestAddress{address}, bytes.size()}, true);
        Need(bool(pin), ORBIS_AJM_ERROR_INVALID_ADDRESS);
        std::memcpy(pin.Value().WritableBytes().data(), bytes.data(), bytes.size());
    }
    template <class T>
    void Put(u64 address, const T& value) {
        Put(address, std::span<const u8>(reinterpret_cast<const u8*>(&value), sizeof(T)));
    }
    Output& AddOutput(Job& job, Chunk chunk, size_t& total) {
        Need(chunk.size <= MaxBuffer && total <= MaxResident - chunk.size,
             ORBIS_AJM_ERROR_BUFFER_TOO_BIG);
        Need(Valid(chunk.address, chunk.size, GuestPermission::Write),
             ORBIS_AJM_ERROR_INVALID_ADDRESS);
        // Capture preserved bytes and mapping identity under the same lease.
        auto pin = space.AcquireDataSpan({GuestAddress{chunk.address}, chunk.size}, true);
        Need(bool(pin), ORBIS_AJM_ERROR_INVALID_ADDRESS);
        Output output{chunk.address, std::vector<u8>(chunk.size), {}};
        std::memcpy(output.bytes.data(), pin.Value().Bytes().data(), chunk.size);
        auto mapping = space.Query(GuestAddress{chunk.address});
        Need(bool(mapping), ORBIS_AJM_ERROR_INVALID_ADDRESS);
        output.identities.push_back(
            {chunk.address, chunk.address + chunk.size, mapping.Value().mapping_generation});
        total += chunk.size;
        job.outputs.push_back(std::move(output));
        return job.outputs.back();
    }
    void Parse(std::shared_ptr<Batch>& batch, Context& context, u64 address, u32 size) {
        Need(!(size & 7) && size <= MaxBatch);
        auto data = Read(address, size);
        Cursor input{data};
        batch->bytes = size;
        while (!input.bytes.empty()) {
            const size_t offset = size - input.bytes.size();
            batch->error = {.job = address + offset, .offset = u32(offset)};
            auto header = input.Take<std::array<u32, 2>>();
            Need(!(header[0] >> 26) && header[1] <= input.bytes.size() && !(header[1] & 7));
            Cursor parts{input.bytes.first(header[1])};
            input.bytes = input.bytes.subspan(header[1]);
            if ((header[0] & 63) == 7)
                continue;
            Need((header[0] & 63) == 0 && batch->jobs.size() < 1024);
            auto owned = std::make_unique<Job>();
            auto& out = *owned;
            out.address = address + offset;
            out.offset = offset;
            out.job.instance_id = (header[0] >> 6) & 0xfffff;
            const bool stats = out.job.instance_id == AJM_INSTANCE_STATISTICS;
            if (!stats) {
                auto it = context.instances.find(out.job.instance_id);
                Need(it != context.instances.end(), ORBIS_AJM_ERROR_INVALID_INSTANCE);
                out.instance = it->second;
            }
            bool have_flags{};
            std::optional<Chunk> control_in, control_out;
            while (!parts.bytes.empty()) {
                u32 word{};
                Need(parts.bytes.size() >= 8);
                std::memcpy(&word, parts.bytes.data(), 4);
                Need(!(word >> 26));
                const u32 ident = word & 63;
                if (ident == 3 || ident == 4) {
                    Need(!have_flags && (!stats || ident == 3));
                    have_flags = true;
                    auto flags = parts.Take<std::array<u32, 2>>();
                    out.job.flags.raw = (u64(word >> 6) << 32) | flags[1];
                    const u64 mask = stats        ? 0xC0018007ull
                                     : ident == 3 ? 0x60000000E7FFull
                                                  : 0xE00000001FFFull;
                    Need(!(out.job.flags.raw & ~mask), ORBIS_AJM_ERROR_FLAG_NOT_SUPPORTED);
                    continue;
                }
                auto chunk = parts.Take<Chunk>();
                Need((word >> 6) == 0);
                if (ident == 6) {
                    out.ra = chunk.address;
                    batch->error.ra = chunk.address;
                    continue;
                }
                if (ident == 2) {
                    Need(!control_in);
                    control_in = chunk;
                    continue;
                }
                if (ident == 18) {
                    Need(!control_out);
                    control_out = chunk;
                    continue;
                }
                Need(!stats);
                if (ident == 1) {
                    Need(chunk.size <= MaxBuffer && batch->bytes <= MaxResident - chunk.size,
                         ORBIS_AJM_ERROR_BUFFER_TOO_BIG);
                    auto bytes = Read(chunk.address, chunk.size);
                    batch->bytes += bytes.size();
                    out.job.input.buffer.insert(out.job.input.buffer.end(), bytes.begin(),
                                                bytes.end());
                } else if (ident == 17) {
                    Need(chunk.size % out.instance->sample_size == 0,
                         ORBIS_AJM_ERROR_INVALID_PARAMETER);
                    if (chunk.size)
                        out.job.output.buffers.emplace_back(
                            AddOutput(out, chunk, batch->bytes).bytes);
                } else
                    throw Failure{ORBIS_AJM_ERROR_INVALID_OPCODE};
            }
            Need(have_flags && control_out && control_out->size >= sizeof(AjmSidebandResult));
            auto control = control_in && control_in->size
                               ? Read(control_in->address, control_in->size)
                               : std::vector<u8>{};
            Need(batch->bytes <= MaxResident - control.size(), ORBIS_AJM_ERROR_BUFFER_TOO_BIG);
            batch->bytes += control.size();
            Cursor params{control};
            auto& sideband = AddOutput(out, *control_out, batch->bytes);
            size_t at{};
            auto field = [&]<class T>(T*& ptr, bool wanted, bool required = false) {
                if (!wanted || (!required && at == sideband.bytes.size()))
                    return;
                Need(sizeof(T) <= sideband.bytes.size() - at);
                ptr = reinterpret_cast<T*>(sideband.bytes.data() + at);
                *ptr = {};
                at += sizeof(T);
            };
            field(out.job.output.p_result, true, true);
            if (stats) {
                AjmStatisticsJobFlags flags{out.job.flags.raw};
                const bool engine = True(flags.statistics_flags & AjmStatisticsFlags::Engine);
                if (engine) {
                    out.job.input.statistics_engine_parameters =
                        params.Take<AjmSidebandStatisticsEngineParameters>();
                    Need(out.job.input.statistics_engine_parameters->interval_count <= 3);
                }
                field(out.job.output.p_engine, engine, true);
                field(out.job.output.p_engine_per_codec,
                      True(flags.statistics_flags & AjmStatisticsFlags::EnginePerCodec), true);
                field(out.job.output.p_memory,
                      True(flags.statistics_flags & AjmStatisticsFlags::Memory), true);
            } else {
                const auto flags = out.job.flags;
                if (True(flags.sideband_flags & AjmJobSidebandFlags::Format) &&
                    !params.bytes.empty())
                    out.job.input.format = params.Take<AjmSidebandFormat>();
                if (True(flags.sideband_flags & AjmJobSidebandFlags::GaplessDecode) &&
                    !params.bytes.empty())
                    out.job.input.gapless_decode = params.Take<AjmSidebandGaplessDecode>();
                if (True(flags.control_flags & AjmJobControlFlags::Resample))
                    out.job.input.resample_parameters =
                        params.Take<AjmSidebandResampleParameters>();
                if (True(flags.control_flags & AjmJobControlFlags::Initialize))
                    out.job.input.init_params = params.Take<AjmSidebandInitParameters>();
                field(out.job.output.p_stream,
                      True(flags.sideband_flags & AjmJobSidebandFlags::Stream));
                field(out.job.output.p_format,
                      True(flags.sideband_flags & AjmJobSidebandFlags::Format));
                field(out.job.output.p_gapless_decode,
                      True(flags.sideband_flags & AjmJobSidebandFlags::GaplessDecode));
                field(out.job.output.p_mframe,
                      True(flags.run_flags & AjmJobRunFlags::MultipleFrames));
                if (True(flags.run_flags & AjmJobRunFlags::GetCodecInfo) &&
                    at < sideband.bytes.size()) {
                    const size_t bytes = out.instance->codec == AjmCodecType::M4aacDec
                                             ? sizeof(AjmSidebandDecM4aacCodecInfo)
                                             : 16;
                    Need(bytes <= sideband.bytes.size() - at);
                    out.job.output.p_codec_info = sideband.bytes.data() + at;
                }
            }
            batch->jobs.push_back(std::move(owned));
        }
        batch->error = {};
    }
    void Publish(Batch& batch) {
        if (batch.cancel.stop_requested())
            throw Failure{ORBIS_AJM_ERROR_CANCELLED};
        std::vector<GuestAddressSpace::DataRequest> requests;
        for (auto& owned : batch.jobs)
            for (auto& output : owned->outputs)
                requests.push_back({{GuestAddress{output.address}, output.bytes.size()},
                                    GuestPermission::Write,
                                    output.identities});
        auto pinned = space.AcquireDataBatch(requests, batch.cancel.get_token());
        Need(bool(pinned), batch.cancel.stop_requested() ? ORBIS_AJM_ERROR_CANCELLED
                                                         : ORBIS_AJM_ERROR_INVALID_ADDRESS);
        auto& pins = pinned.Value();
        size_t i{};
        for (auto& owned : batch.jobs)
            for (auto& output : owned->outputs)
                std::memcpy(pins[i++].WritableBytes().data(), output.bytes.data(),
                            output.bytes.size());
    }
    void Work(std::stop_token stop) {
        for (;;) {
            std::shared_ptr<Batch> batch;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, stop, [&] { return !queue.empty(); });
                if (queue.empty())
                    return;
                batch = queue.front();
                queue.pop_front();
            }
            BatchError error{};
            try {
                for (auto& owned : batch->jobs) {
                    error = {.job = owned->address, .offset = owned->offset, .ra = owned->ra};
                    if (batch->cancel.stop_requested())
                        throw Failure{ORBIS_AJM_ERROR_CANCELLED};
                    if (owned->instance)
                        owned->instance->decoder.ExecuteJob(owned->job, batch->cancel.get_token());
                    else
                        AjmInstanceStatistics::Getinstance().ExecuteJob(
                            owned->job); // desktop compatibility estimates
                }
                Publish(*batch);
                error = {};
            } catch (Failure failure) {
                error.error = failure.error;
            } catch (const std::exception&) {
                error.error = ORBIS_AJM_ERROR_INVALID_PARAMETER;
            }
            {
                std::lock_guard lock(mutex);
                batch->error = error;
                batch->done = true;
            }
            changed.notify_all();
        }
    }
    u64 Wait(const std::array<u64, 10>& a, std::stop_token stop) {
        {

            if (a[3] && !Valid(a[3], sizeof(BatchError), GuestPermission::Write))
                return u32(ORBIS_AJM_ERROR_INVALID_ADDRESS);
        }
        std::unique_lock lock(mutex);
        if (!contexts.contains(u32(a[0])))
            return u32(ORBIS_AJM_ERROR_INVALID_CONTEXT);
        auto it = batches.find(u32(a[1]));
        if (it == batches.end() || it->second->context != u32(a[0]))
            return u32(ORBIS_AJM_ERROR_INVALID_BATCH);
        auto batch = it->second;
        if (batch->waiting)
            return u32(ORBIS_AJM_ERROR_BUSY);
        batch->waiting = true;
        const auto ready = [&] { return batch->done || stopping; };
        if (u32(a[2]) == UINT32_MAX)
            changed.wait(lock, stop, ready);
        else
            changed.wait_for(lock, stop, std::chrono::milliseconds(u32(a[2])), ready);
        if (stop.stop_requested() || stopping) {
            batch->waiting = false;
            return u32(ORBIS_AJM_ERROR_CANCELLED);
        }
        if (!batch->done) {
            batch->waiting = false;
            return u32(ORBIS_AJM_ERROR_IN_PROGRESS);
        }
        // The batch remains claimed by this waiter through its result copy.
        try {
            if (a[3])
                Put(a[3], batch->error);
        } catch (Failure f) {
            batch->waiting = false;
            return u32(f.error);
        }
        resident -= batch->bytes;
        batches.erase(batch->id);
        return u32(batch->error.error);
    }
    u64 Build(std::string_view nid, const std::array<u64, 10>& a) {
        auto ptr = [](u64 value) { return reinterpret_cast<void*>(value); };
        const bool control = nid == "dmDybN--Fn8", split = nid == "7jdAXK+2fMo",
                   inlined = nid == "stlghnic3Jc";
        Need(!(a[0] & 7), ORBIS_AJM_ERROR_INVALID_ADDRESS);
        if (inlined) {
            Need(a[2] <= MaxBatch - 8, ORBIS_AJM_ERROR_BUFFER_TOO_BIG);
            auto input = Read(a[1], a[2]);
            const size_t size = 8 + ((a[2] + 7) & ~7ull);
            Need(Valid(a[0], size, GuestPermission::Write) &&
                     Valid(a[3], 8, GuestPermission::Write),
                 ORBIS_AJM_ERROR_INVALID_ADDRESS);
            std::vector<u8> data(size);
            const void* inner{};
            BatchJobInlineBuffer(data.data(), input.data(), input.size(), &inner);
            const std::array<GuestAddressSpace::DataRequest, 2> requests{
                {{{GuestAddress{a[0]}, data.size()}, GuestPermission::Write},
                 {{GuestAddress{a[3]}, 8}, GuestPermission::Write}}};
            auto pinned = space.AcquireDataBatch(requests);
            Need(bool(pinned), ORBIS_AJM_ERROR_INVALID_ADDRESS);
            const u64 address = a[0] + 8;
            std::memcpy(pinned.Value()[0].WritableBytes().data(), data.data(), data.size());
            std::memcpy(pinned.Value()[1].WritableBytes().data(), &address, 8);
            return a[0] + size;
        }
        Need(a[1] <= 0xfffff, ORBIS_AJM_ERROR_INVALID_INSTANCE);
        std::vector<AjmBuffer> inputs, outputs;
        size_t size{};
        if (split) {
            Need(a[4] <= 256 && a[6] <= 256, ORBIS_AJM_ERROR_BUFFER_TOO_BIG);
            auto in = Read(a[3], a[4] * sizeof(AjmBuffer)),
                 out = Read(a[5], a[6] * sizeof(AjmBuffer));
            inputs.resize(a[4]);
            outputs.resize(a[6]);
            if (!in.empty())
                std::memcpy(inputs.data(), in.data(), in.size());
            if (!out.empty())
                std::memcpy(outputs.data(), out.data(), out.size());
            size = 32 + (a[9] ? 16 : 0) + 16 * (inputs.size() + outputs.size());
        } else
            size = (control ? 48 : 64) + (a[control ? 7 : 9] ? 16 : 0);
        auto valid_buffer = [&](u64 address, u64 bytes, bool output) {
            Need(bytes <= MaxBuffer, ORBIS_AJM_ERROR_BUFFER_TOO_BIG);
            Need(Valid(address, bytes, output ? GuestPermission::Write : GuestPermission::Read),
                 ORBIS_AJM_ERROR_INVALID_ADDRESS);
        };
        if (split) {
            for (auto b : inputs)
                valid_buffer(u64(b.p_address), b.size, false);
            for (auto b : outputs)
                valid_buffer(u64(b.p_address), b.size, true);
        } else {
            valid_buffer(a[3], a[4], false);
            valid_buffer(a[5], a[6], true);
        }
        if (!control)
            valid_buffer(a[7], a[8], true);
        Need(Valid(a[0], size, GuestPermission::Write), ORBIS_AJM_ERROR_INVALID_ADDRESS);
        std::vector<u8> data(size);
        if (control)
            BatchJobControlBufferRa(data.data(), a[1], a[2], ptr(a[3]), a[4], ptr(a[5]), a[6],
                                    ptr(a[7]));
        else if (split)
            BatchJobRunSplitBufferRa(data.data(), a[1], a[2], inputs.data(), inputs.size(),
                                     outputs.data(), outputs.size(), ptr(a[7]), a[8], ptr(a[9]));
        else
            BatchJobRunBufferRa(data.data(), a[1], a[2], ptr(a[3]), a[4], ptr(a[5]), a[6],
                                ptr(a[7]), a[8], ptr(a[9]));
        Put(a[0], std::span<const u8>(data));
        return a[0] + size;
    }
    u64 Dispatch(std::string_view nid, const std::array<u64, 10>& a, std::stop_token stop) {
        if (nid == "-qLsfDAywIY")
            return Wait(a, stop);
        const bool builder = nid == "dmDybN--Fn8" || nid == "stlghnic3Jc" || nid == "ElslOCpOIns" ||
                             nid == "7jdAXK+2fMo";

        std::lock_guard lock(mutex);
        if (stopping || stop.stop_requested())
            return builder ? 0 : u32(ORBIS_AJM_ERROR_CANCELLED);
        try {
            if (builder)
                return Build(nid, a);
            if (nid == "diXjQNiMu-s")
                return (u32(a[0]) >> 14) & 31;
            if (nid == "eDFeTyi+G3Y") {
                auto bytes = Read(a[0], u32(a[1]));
                Need(Valid(a[3], sizeof(AjmDecMp3ParseFrame), GuestPermission::Write),
                     ORBIS_AJM_ERROR_INVALID_ADDRESS);
                AjmDecMp3ParseFrame result{};
                const int status =
                    AjmMp3Decoder::ParseMp3Header(bytes.data(), bytes.size(), s32(a[2]), &result);
                if (!status)
                    Put(a[3], result);
                return u32(status);
            }
            if (nid == "dl+4eHSzUu4") {
                Need(!a[0], ORBIS_AJM_ERROR_INVALID_PARAMETER);
                Need(Valid(a[1], 4, GuestPermission::Write), ORBIS_AJM_ERROR_INVALID_ADDRESS);
                Need(contexts.size() < 16 && next_context < INT32_MAX,
                     ORBIS_AJM_ERROR_OUT_OF_RESOURCES);
                const u32 id = next_context;
                Put(a[1], id);
                contexts.emplace(id, Context{});
                ++next_context;
                return 0;
            }
            auto found = contexts.find(u32(a[0]));
            Need(found != contexts.end(), ORBIS_AJM_ERROR_INVALID_CONTEXT);
            auto& context = found->second;
            if (nid == "Q3dyFuwGn64") {
                Need(!a[2], ORBIS_AJM_ERROR_INVALID_PARAMETER);
                Need(u32(a[1]) < 3, ORBIS_AJM_ERROR_CODEC_NOT_SUPPORTED);
                Need(!context.codecs[u32(a[1])], ORBIS_AJM_ERROR_CODEC_ALREADY_REGISTERED);
                context.codecs[u32(a[1])] = true;
                return 0;
            }
            if (nid == "AxoDrINp4J8") {
                Need(u32(a[1]) < 3, ORBIS_AJM_ERROR_CODEC_NOT_SUPPORTED);
                Need(context.codecs[u32(a[1])], ORBIS_AJM_ERROR_CODEC_NOT_REGISTERED);
                AjmInstanceFlags flags{a[2]};
                Need(flags.version != 0, ORBIS_AJM_ERROR_WRONG_REVISION_FLAG);
                Need(!(flags.raw & 0xf0000000fffff800ull) && flags.channels <= 8 &&
                         flags.format <= 2,
                     ORBIS_AJM_ERROR_FLAG_NOT_SUPPORTED);
                Need(!(u32(a[1]) == 2 && flags.format == 1),
                     ORBIS_AJM_ERROR_FLAG_NOT_SUPPORTED); // desktop AAC S32 aborts
                const u64 codec_mask = u32(a[1]) == 2 ? 15 : 0x101;
                Need(!(flags.codec & ~codec_mask), ORBIS_AJM_ERROR_FLAG_NOT_SUPPORTED);
                Need(Valid(a[3], 4, GuestPermission::Write), ORBIS_AJM_ERROR_INVALID_ADDRESS);
                Need(context.next_instance <= 0x3fff && context.instances.size() < 4096,
                     ORBIS_AJM_ERROR_OUT_OF_RESOURCES);
                auto instance = std::make_shared<Instance>(AjmCodecType(u32(a[1])), flags);
                const u32 id = context.next_instance | (u32(a[1]) << 14);
                Put(a[3], id);
                context.instances.emplace(id, std::move(instance));
                ++context.next_instance;
                return 0;
            }
            if (nid == "RbLbuKv8zho") {
                auto it = context.instances.find(u32(a[1]));
                Need(it != context.instances.end(), ORBIS_AJM_ERROR_INVALID_INSTANCE);
                Need(it->second.use_count() == 1, ORBIS_AJM_ERROR_BUSY);
                context.instances.erase(it);
                return 0;
            }
            if (nid == "bkRHEYG6lEM") {
                Need(a[2] && a[2] <= MaxResident / 16384 && !(a[1] & 0x3fff),
                     ORBIS_AJM_ERROR_INVALID_PARAMETER);
                Need(Valid(a[1], a[2] * 16384, GuestPermission::Read | GuestPermission::Write),
                     ORBIS_AJM_ERROR_INVALID_ADDRESS);
                Need(!context.memory.contains(a[1]), ORBIS_AJM_ERROR_BUSY);
                context.memory.emplace(a[1], a[2] * 16384);
                return 0;
            }
            if (nid == "pIpGiaYkHkM")
                return context.memory.erase(a[1]) ? 0 : u32(ORBIS_AJM_ERROR_INVALID_ADDRESS);
            if (nid == "NVDXiUesSbA") {
                auto it = batches.find(u32(a[1]));
                Need(it != batches.end() && it->second->context == u32(a[0]),
                     ORBIS_AJM_ERROR_INVALID_BATCH);
                if (!it->second->done)
                    it->second->cancel.request_stop();
                return 0;
            }
            if (nid == "fFFkk0xfGWs") {
                Need(Valid(a[5], 4, GuestPermission::Write) &&
                         (!a[4] || Valid(a[4], sizeof(BatchError), GuestPermission::Write)),
                     ORBIS_AJM_ERROR_INVALID_ADDRESS);
                Need(batches.size() < 128 && next_batch < INT32_MAX,
                     ORBIS_AJM_ERROR_OUT_OF_RESOURCES);
                auto batch = std::make_shared<Batch>();
                batch->context = a[0];
                batch->id = next_batch;
                try {
                    Parse(batch, context, a[1], u32(a[2]));
                } catch (Failure f) {
                    batch->error.error = f.error;
                    if (a[4])
                        Put(a[4], batch->error);
                    throw;
                }
                Need(resident <= MaxResident - batch->bytes, ORBIS_AJM_ERROR_OUT_OF_MEMORY);
                // Prepare allocations before publishing the id. The worker cannot
                // observe a partially registered batch while mutex is held.
                batches.emplace(batch->id, batch);
                try {
                    queue.push_back(batch);
                    Put(a[5], batch->id);
                } catch (...) {
                    if (!queue.empty() && queue.back() == batch)
                        queue.pop_back();
                    batches.erase(batch->id);
                    throw;
                }
                ++next_batch;
                resident += batch->bytes;
                changed.notify_all();
                return 0;
            }
            return u32(ORBIS_AJM_ERROR_INVALID_PARAMETER);
        } catch (Failure f) {
            return builder ? 0 : u32(f.error);
        } catch (const std::bad_alloc&) {
            return builder ? 0 : u32(ORBIS_AJM_ERROR_OUT_OF_MEMORY);
        } catch (const std::exception&) {
            return builder ? 0 : u32(ORBIS_AJM_ERROR_INVALID_PARAMETER);
        }
    }
};
GuestAjm::GuestAjm(GuestAddressSpace& space) : impl(std::make_unique<Impl>(space)) {}
GuestAjm::~GuestAjm() = default;
u64 GuestAjm::Dispatch(std::string_view nid, const std::array<u64, 10>& args,
                       std::stop_token cancel) {
    return impl->Dispatch(nid, args, cancel);
}
void GuestAjm::RequestStop() {
    impl->Stop();
}
} // namespace Core::HostRuntime
