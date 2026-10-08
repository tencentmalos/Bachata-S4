// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include "common/logging/log.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_videodec.h"
#include "core/libraries/videodec/videodec2_impl.h"
#include "core/libraries/videodec/videodec_error.h"

namespace Core::HostRuntime {
using namespace GuestCpu;
using namespace Libraries::Videodec2;
namespace {
struct Failure {
    s32 code;
};
void Need(bool ok, s32 code = ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER) {
    if (!ok)
        throw Failure{code};
}
constexpr u64 MaxBuffer = 64 << 20;
} // namespace
bool IsVideodec2Nid(std::string_view nid) {
    return std::ranges::find(Videodec2Nids, nid) != std::end(Videodec2Nids);
}
struct GuestVideodec2::Impl {
    GuestAddressSpace& space;
    std::mutex mutex;
    std::map<u64, std::unique_ptr<VdecDecoder>> decoders;
    std::set<u64> queues;
    u64 next_id{1};
    explicit Impl(GuestAddressSpace& space) : space(space) {}
    template <class T>
    T Read(u64 address) {
        T value{};
        Need(address && bool(space.ReadData(GuestAddress{address},
                                            std::as_writable_bytes(std::span{&value, 1}))));
        return value;
    }
    template <class T>
    T Record(u64 address) {
        Need(Read<u64>(address) == sizeof(T), ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE);
        return Read<T>(address);
    }
    PinnedSpan Output(u64 address, u64 size) {
        Need(address && size);
        auto pin = space.AcquireDataSpan({GuestAddress{address}, size}, true);
        Need(bool(pin));
        return std::move(pin).Value();
    }
    template <class T>
    void Put(PinnedSpan& pin, const T& value) {
        std::memcpy(pin.WritableBytes().data(), &value, sizeof(value));
    }
    OrbisVideodec2DecoderConfigInfo Config(u64 address) {
        auto cfg = Record<OrbisVideodec2DecoderConfigInfo>(address);
        Need(cfg.codec_type == OrbisVideodec2CodecType::Avc ||
                 cfg.codec_type == OrbisVideodec2CodecType::Hevc,
             ORBIS_VIDEODEC2_ERROR_CODEC_TYPE);
        Need(cfg.max_frame_width >= 0 && cfg.max_frame_width <= 8192 && cfg.max_frame_height >= 0 &&
                 cfg.max_frame_height <= 8192 && !cfg.extra_config_info,
             ORBIS_VIDEODEC2_ERROR_CONFIG_INFO);
        // These are scheduling/workspace tokens on PS4; FFmpeg never uses them.
        cfg.compute_queue = nullptr;
        return cfg;
    }
    s32 Invoke(std::string_view nid, const std::array<u64, 6>& a) {
        std::lock_guard lock(mutex);
        if (nid == "RnDibcGCPKw") {
            auto info = Record<OrbisVideodec2ComputeMemoryInfo>(a[0]);
            auto pin = Output(a[0], sizeof(info));
            const auto result = sceVideodec2QueryComputeMemoryInfo(&info);
            Put(pin, info);
            return result;
        }
        if (nid == "eD+X2SmxUt4") {
            auto cfg = Record<OrbisVideodec2ComputeConfigInfo>(a[0]);
            auto mem = Record<OrbisVideodec2ComputeMemoryInfo>(a[1]);
            auto pin = Output(a[2], sizeof(u64));
            OrbisVideodec2ComputeQueue queue{};
            const auto result = sceVideodec2AllocateComputeQueue(&cfg, &mem, &queue);
            if (!result) {
                const auto token = reinterpret_cast<u64>(queue);
                queues.insert(token);
                Put(pin, token);
            }
            return result;
        }
        if (nid == "UvtA3FAiF4Y") {
            Need(queues.erase(a[0]) != 0, ORBIS_VIDEODEC2_ERROR_COMPUTE_QUEUE);
            return ORBIS_OK;
        }
        if (nid == "qqMCwlULR+E" || nid == "CNNRoRYd8XI") {
            auto cfg = Config(a[0]);
            auto mem = Record<OrbisVideodec2DecoderMemoryInfo>(a[1]);
            if (nid == "qqMCwlULR+E") {
                auto pin = Output(a[1], sizeof(mem));
                const auto result = sceVideodec2QueryDecoderMemoryInfo(&cfg, &mem);
                Put(pin, mem);
                return result;
            }
            auto pin = Output(a[2], sizeof(u64));
            Need(decoders.size() < 16, ORBIS_VIDEODEC2_ERROR_MEMORY_SIZE);
            // Workspace addresses are not dereferenced by the software decoder.
            mem.cpu_memory = mem.gpu_memory = mem.cpu_gpu_memory = nullptr;
            auto decoder = std::make_unique<VdecDecoder>(cfg, mem);
            const u64 handle = next_id++;
            decoders.emplace(handle, std::move(decoder));
            Put(pin, handle);
            LOG_INFO(Lib_Vdec2, "Guest decoder={} codec={} dimensions={}x{}", handle,
                     u32(cfg.codec_type), cfg.max_frame_width, cfg.max_frame_height);
            return ORBIS_OK;
        }
        if (nid == "NtXRa3dRzU0" || nid == "kjrLbcyhEiw" || nid == "7M+1UFqWOAI") {
            const auto size = Read<u64>(a[0]);
            Need(size == 0x30 || size == sizeof(OrbisVideodec2OutputInfo),
                 ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE);
            OrbisVideodec2OutputInfo out{};
            Need(bool(space.ReadData(GuestAddress{a[0]},
                                     std::as_writable_bytes(std::span{&out, 1}).first(size))));
            if (!out.picture_count || !a[1])
                return ORBIS_OK;
            Need(out.picture_count == 1, ORBIS_VIDEODEC2_ERROR_OUTPUT_INFO);
            const u64 max_info = out.codec_type == OrbisVideodec2CodecType::Avc
                                     ? sizeof(OrbisVideodec2AvcPictureInfo)
                                     : sizeof(OrbisVideodec2HevcPictureInfo);
            const u64 requested = Read<u64>(a[1]);
            Need(requested >= 8 && requested <= max_info, ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE);
            const u64 buffer = reinterpret_cast<u64>(out.frame_buffer);
            Need(out.frame_buffer_size < MaxBuffer && buffer <= UINT64_MAX - out.frame_buffer_size);
            std::vector<std::byte> info(requested);
            Need(bool(space.ReadData(GuestAddress{buffer + out.frame_buffer_size}, info)));
            auto pin = Output(a[1], requested);
            std::memcpy(pin.WritableBytes().data() + 8, info.data() + 8, requested - 8);
            return ORBIS_OK;
        }
        auto it = decoders.find(a[0]);
        Need(it != decoders.end(), ORBIS_VIDEODEC2_ERROR_DECODER_INSTANCE);
        if (nid == "jwImxXRGSKA") {
            decoders.erase(it);
            return ORBIS_OK;
        }
        if (nid == "wJXikG6QFN8")
            return it->second->Reset();
        Need(nid == "852F5+q6+iM" || nid == "l1hXwscLuCY", ORBIS_VIDEODEC2_ERROR_API_FAIL);
        const bool decode = nid == "852F5+q6+iM";
        const u64 fb_address = a[decode ? 2 : 1], out_address = a[decode ? 3 : 2];
        auto fb = Record<OrbisVideodec2FrameBuffer>(fb_address);
        OrbisVideodec2OutputInfo out{};
        out.this_size = Read<u64>(out_address);
        Need(out.this_size == 0x30 || out.this_size == sizeof(out),
             ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE);
        const u64 guest_buffer = reinterpret_cast<u64>(fb.frame_buffer);
        Need(guest_buffer != 0, ORBIS_VIDEODEC2_ERROR_FRAME_BUFFER_POINTER);
        Need(fb.frame_buffer_size >= sizeof(OrbisVideodec2HevcPictureInfo) &&
                 fb.frame_buffer_size <= MaxBuffer,
             ORBIS_VIDEODEC2_ERROR_FRAME_BUFFER_SIZE);
        OrbisVideodec2InputData input{};
        std::vector<u8> au;
        if (decode) {
            input = Record<OrbisVideodec2InputData>(a[1]);
            Need(input.au_size && input.au_size <= MaxBuffer,
                 ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_SIZE);
            Need(input.au_data != nullptr, ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_POINTER);
            const GuestAddressSpace::DataRequest request{
                {GuestAddress{reinterpret_cast<u64>(input.au_data)}, input.au_size},
                GuestPermission::Read,
                {},
                true};
            auto pin = space.AcquireDataBatch(std::span{&request, 1});
            Need(bool(pin), ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_POINTER);
            au.resize(input.au_size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            std::memcpy(au.data(), pin.Value()[0].Bytes().data(), input.au_size);
            input.au_data = au.data();
        }
        // Admit all outputs before consuming a packet or changing codec state.
        const GuestAddressSpace::DataRequest requests[]{
            {{GuestAddress{fb_address}, sizeof(fb)}, GuestPermission::Write},
            {{GuestAddress{out_address}, out.this_size}, GuestPermission::Write},
            {{GuestAddress{guest_buffer}, fb.frame_buffer_size}, GuestPermission::Write, {}, true}};
        auto pins = space.AcquireDataBatch(requests);
        Need(bool(pins));
        fb.frame_buffer = pins.Value()[2].WritableBytes().data();
        const auto result =
            decode ? it->second->Decode(input, fb, out) : it->second->Flush(fb, out);
        fb.frame_buffer = reinterpret_cast<void*>(guest_buffer);
        if (out.is_valid)
            out.frame_buffer = reinterpret_cast<void*>(guest_buffer);
        Put(pins.Value()[0], fb);
        std::memcpy(pins.Value()[1].WritableBytes().data(), &out, out.this_size);
        return result;
    }
};
GuestVideodec2::GuestVideodec2(GuestAddressSpace& space) : impl(std::make_unique<Impl>(space)) {}
GuestVideodec2::~GuestVideodec2() = default;
u64 GuestVideodec2::Dispatch(std::string_view nid, const std::array<u64, 6>& args) {
    try {
        return u32(impl->Invoke(nid, args));
    } catch (const Failure& f) {
        LOG_WARNING(Lib_Vdec2, "Guest Videodec2 {} rejected: {:#x}", nid, u32(f.code));
        return u32(f.code);
    } catch (const std::bad_alloc&) {
        return u32(ORBIS_VIDEODEC2_ERROR_MEMORY_SIZE);
    }
}
} // namespace Core::HostRuntime
