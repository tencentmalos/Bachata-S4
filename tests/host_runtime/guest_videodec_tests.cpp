// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <fstream>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_videodec.h"
#include "core/libraries/videodec/videodec2.h"
#include "core/libraries/videodec/videodec.h"
#include "core/libraries/videodec/videodec_error.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
using namespace Libraries::Videodec2;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<u8> h264{std::istreambuf_iterator<char>(in), {}};
    if (h264.empty() || h264.size() > 4096)
        return 2;
    AddressSpaceConfig config;
    config.reservation_size = 1 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    const auto rw = GuestPermission::Read | GuestPermission::Write;
    for (u64 i = 0; i < 16; ++i)
        CHECK(space->Map({GuestAddress{base + i * 4096}, 4096}, rw));
    auto put = [&](u64 offset, const auto& value) {
        CHECK(space->WriteData(GuestAddress{base + offset}, std::as_bytes(std::span{&value, 1})));
    };
    auto get = [&]<class T>(u64 offset) {
        T value{};
        CHECK(space->ReadData(GuestAddress{base + offset},
                              std::as_writable_bytes(std::span{&value, 1})));
        return value;
    };
    GuestVideodec2 bridge(*space);
    auto call = [&](std::string_view nid, std::array<u64, 6> a) {
        return u32(bridge.Dispatch(nid, a));
    };
    CHECK(call("RnDibcGCPKw", {}) == u32(ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER));
    OrbisVideodec2DecoderConfigInfo cfg{};
    cfg.this_size = sizeof(cfg);
    cfg.codec_type = OrbisVideodec2CodecType::Avc;
    cfg.max_frame_width = 64;
    cfg.max_frame_height = 32;
    OrbisVideodec2DecoderMemoryInfo mem{};
    mem.this_size = sizeof(mem);
    put(0x100, cfg);
    put(0x200, mem);
    CHECK(call("qqMCwlULR+E", {base + 0x100, base + 0x200}) == 0);
    mem = get.template operator()<OrbisVideodec2DecoderMemoryInfo>(0x200);
    CHECK(mem.max_frame_buffer_size >= 64 * 32 * 3 / 2 + 0x78);
    CHECK(call("CNNRoRYd8XI", {base + 0x100, base + 0x200, base + 0x300}) == 0);
    const u64 decoder = get.template operator()<u64>(0x300);
    CHECK(decoder && decoder < base);
    CHECK(call("wJXikG6QFN8", {base}) == u32(ORBIS_VIDEODEC2_ERROR_DECODER_INSTANCE));
    // The AU crosses two distinct guest mappings. FFmpeg must receive a padded copy.
    const u64 au = base + 0x1ff8;
    for (size_t i = 0; i < h264.size(); ++i)
        put(0x1ff8 + i, h264[i]);
    OrbisVideodec2InputData input{};
    input.this_size = sizeof(input);
    input.au_data = reinterpret_cast<void*>(au);
    input.au_size = h264.size();
    input.pts_data = 1234;
    input.dts_data = 1234;
    input.attached_data = 0x12345678;
    OrbisVideodec2FrameBuffer fb{};
    fb.this_size = sizeof(fb);
    fb.frame_buffer = reinterpret_cast<void*>(base + 0x4000);
    fb.frame_buffer_size = 8192;
    OrbisVideodec2OutputInfo out{};
    out.this_size = 0x30;
    put(0x500, input);
    put(0x600, fb);
    put(0x700, out);
    put(0x730, u64{0xfeedfacecafebeef});
    // Rejected output admission must not consume the packet.
    CHECK(space->Protect({GuestAddress{base + 0x4000}, 4096}, GuestPermission::Read));
    CHECK(call("852F5+q6+iM", {decoder, base + 0x500, base + 0x600, base + 0x700}) != 0);
    CHECK(space->Protect({GuestAddress{base + 0x4000}, 4096}, rw));
    CHECK(call("852F5+q6+iM", {decoder, base + 0x500, base + 0x600, base + 0x700}) == 0);
    out = get.template operator()<OrbisVideodec2OutputInfo>(0x700);
    fb = get.template operator()<OrbisVideodec2FrameBuffer>(0x600);
    CHECK(out.is_valid && fb.is_accepted && out.picture_count == 1);
    CHECK(out.frame_width == 64 && out.frame_height == 32 && out.frame_pitch == 64);
    CHECK(out.frame_buffer == reinterpret_cast<void*>(base + 0x4000));
    CHECK(out.frame_buffer_size == 3072);
    CHECK(get.template operator()<u64>(0x730) == 0xfeedfacecafebeef);
    CHECK(get.template operator()<u8>(0x4000) == 235);
    CHECK(get.template operator()<u8>(0x4800) == 128);
    OrbisVideodec2AvcPictureInfo picture{};
    picture.this_size = sizeof(picture);
    put(0x800, picture);
    CHECK(call("kjrLbcyhEiw", {base + 0x700, base + 0x800, 0}) == 0);
    picture = get.template operator()<OrbisVideodec2AvcPictureInfo>(0x800);
    CHECK(picture.this_size == sizeof(picture) && picture.is_valid && picture.pts_data == 1234);
    CHECK(call("wJXikG6QFN8", {decoder}) == 0);
    fb.frame_buffer_size = 256;
    put(0x600, fb);
    CHECK(call("852F5+q6+iM", {decoder, base + 0x500, base + 0x600, base + 0x700}) ==
          u32(ORBIS_VIDEODEC2_ERROR_FRAME_BUFFER_SIZE));
    CHECK(call("jwImxXRGSKA", {decoder}) == 0);
    CHECK(call("wJXikG6QFN8", {decoder}) == u32(ORBIS_VIDEODEC2_ERROR_DECODER_INSTANCE));
    CHECK(call("jwImxXRGSKA", {decoder}) == u32(ORBIS_VIDEODEC2_ERROR_DECODER_INSTANCE));
    // Original Videodec API used by early SDK titles. Same AU, checked guest
    // control record, no native decoder pointer, and no packet consumed on EFAULT.
    {
        using namespace Libraries::Videodec;
        GuestVideodec legacy(*space), foreign(*space);
        auto invoke = [&](std::string_view nid, std::array<u64,6> a) {
            CHECK(IsVideodecNid(nid));
            return u32(legacy.Dispatch(nid,a));
        };
        OrbisVideodecConfigInfo config{};
        config.thisSize=sizeof(config); config.maxFrameWidth=64; config.maxFrameHeight=32;
        OrbisVideodecResourceInfo resource{}; resource.thisSize=sizeof(resource);
        // GoW III requests 1920x1088 AVC with -1 DPB frames. Desktop treats
        // non-positive DPB counts as automatic (eight reference surfaces).
        auto automatic = config;
        automatic.profile = 100;
        automatic.maxLevel = 42;
        automatic.maxFrameWidth = 1920;
        automatic.maxFrameHeight = 1088;
        automatic.maxDpbFrameCount = -1;
        put(0x100, automatic); put(0x200, resource);
        CHECK(invoke("leCAscipfFY", {base+0x100, base+0x200}) == 0);
        const auto automatic_resource = get.template operator()<OrbisVideodecResourceInfo>(0x200);
        automatic.maxDpbFrameCount = 8;
        put(0x100, automatic); put(0x200, resource);
        CHECK(invoke("leCAscipfFY", {base+0x100, base+0x200}) == 0);
        const auto explicit_resource = get.template operator()<OrbisVideodecResourceInfo>(0x200);
        CHECK(automatic_resource.cpuGpuMemorySize == explicit_resource.cpuGpuMemorySize);
        CHECK(automatic_resource.maxFrameBufferSize == explicit_resource.maxFrameBufferSize);
        // Keep bounds checking for values outside the supported sentinel/range.
        automatic.maxDpbFrameCount = -2;
        put(0x100, automatic);
        CHECK(invoke("leCAscipfFY", {base+0x100, base+0x200}) == u32(ORBIS_VIDEODEC_ERROR_CONFIG_INFO));
        // Exercise automatic DPB through create, real decode, reset and delete.
        config.maxDpbFrameCount = -1;
        put(0x100,config); put(0x200,resource);
        CHECK(invoke("leCAscipfFY",{base+0x100,base+0x200})==0);
        resource=get.template operator()<OrbisVideodecResourceInfo>(0x200);
        CHECK(resource.maxFrameBufferSize>=3072 && resource.frameBufferAlignment==256);
        CHECK(invoke("qkgRiwHyheU",{base+0x100,base+0x200,base+0x300})==0);
        const auto ctrl=get.template operator()<OrbisVideodecCtrl>(0x300);
        CHECK(ctrl.thisSize==sizeof(ctrl) && ctrl.version==1 && reinterpret_cast<u64>(ctrl.handle)<base);
        CHECK(u32(foreign.Dispatch("f8AgDv-1X8A",{base+0x300}))==u32(ORBIS_VIDEODEC_ERROR_HANDLE));
        OrbisVideodecInputData input{};
        input.thisSize=sizeof(input); input.pAuData=reinterpret_cast<void*>(au);
        input.auSize=h264.size(); input.ptsData=input.dtsData=4321; input.attachedData=0x87654321;
        OrbisVideodecFrameBuffer fb{sizeof(OrbisVideodecFrameBuffer),reinterpret_cast<void*>(base+0x4000),8192};
        OrbisVideodecPictureInfo pic{}; pic.thisSize=sizeof(pic);
        put(0x500,input); put(0x600,fb); put(0x700,pic);
        put(0x700+sizeof(pic),u64{0xfeedfacecafebeef});
        CHECK(space->Protect({{base+0x4000},4096},GuestPermission::Read));
        CHECK(invoke("q0W5GJMovMs",{base+0x300,base+0x500,base+0x600,base+0x700})!=0);
        CHECK(space->Protect({{base+0x4000},4096},rw));
        CHECK(invoke("q0W5GJMovMs",{base+0x300,base+0x500,base+0x600,base+0x700})==0);
        pic=get.template operator()<OrbisVideodecPictureInfo>(0x700);
        CHECK(pic.isValid && pic.frameWidth==64 && pic.frameHeight==32 && pic.framePitch==64);
        CHECK(pic.ptsData==4321 && pic.attachedData==0x87654321);
        CHECK(get.template operator()<u8>(0x4000)==235 && get.template operator()<u8>(0x4800)==128);
        CHECK(get.template operator()<u64>(0x700+sizeof(pic))==0xfeedfacecafebeef);
        CHECK(invoke("jeigLlKdp5I",{base+0x300,base+0x600,base+0x700})==0);
        CHECK(invoke("f8AgDv-1X8A",{base+0x300})==0);
        fb.frameBufferSize=256; put(0x600,fb);
        CHECK(invoke("q0W5GJMovMs",{base+0x300,base+0x500,base+0x600,base+0x700})==u32(ORBIS_VIDEODEC_ERROR_FRAME_BUFFER_SIZE));
        CHECK(invoke("U0kpGF1cl90",{base+0x300})==0);
        CHECK(invoke("U0kpGF1cl90",{base+0x300})==u32(ORBIS_VIDEODEC_ERROR_HANDLE));
    }
    std::printf("guest_videodec_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
