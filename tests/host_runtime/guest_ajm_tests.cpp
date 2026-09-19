// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <future>
#include <thread>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_ajm.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/ajm/ajm_error.h"
using namespace Core::GuestCpu;
using namespace Core::HostRuntime;
using namespace Libraries::Ajm;
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
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    const auto rw = GuestPermission::Read | GuestPermission::Write;
    CHECK(space->Map({GuestAddress{base}, 0x3000}, rw));
    CHECK(space->Map({GuestAddress{base + 0x3000}, 0x1000}, rw));
    CHECK(space->Map({GuestAddress{base + 0x4000}, 0xfc000}, rw));
    GuestAjm ajm(*space);
    auto call = [&](std::string_view nid, std::array<u64, 10> a = {}) {
        return ajm.Dispatch(nid, a);
    };
    auto read = [&]<class T>(u64 address) {
        T value{};
        CHECK(space->Read(GuestAddress{address}, std::as_writable_bytes(std::span{&value, 1})));
        return value;
    };
    auto write = [&](u64 address, const auto& value) {
        CHECK(space->Write(GuestAddress{address}, std::as_bytes(std::span{&value, 1})));
    };
    CHECK(call("dl+4eHSzUu4", {1, base}) == u32(ORBIS_AJM_ERROR_INVALID_PARAMETER));
    CHECK(call("dl+4eHSzUu4", {0, 1}) == u32(ORBIS_AJM_ERROR_INVALID_ADDRESS));
    CHECK(call("dl+4eHSzUu4", {0, base}) == 0);
    const u32 ctx = read.operator()<u32>(base);
    CHECK(call("Q3dyFuwGn64", {ctx + 1, 0, 0}) == u32(ORBIS_AJM_ERROR_INVALID_CONTEXT));
    CHECK(call("Q3dyFuwGn64", {ctx, 3, 0}) == u32(ORBIS_AJM_ERROR_CODEC_NOT_SUPPORTED));
    for (u32 codec = 0; codec < 3; ++codec)
        CHECK(call("Q3dyFuwGn64", {ctx, codec, 0}) == 0);
    CHECK(call("Q3dyFuwGn64", {ctx, 0, 0}) == u32(ORBIS_AJM_ERROR_CODEC_ALREADY_REGISTERED));
    CHECK(call("AxoDrINp4J8", {ctx, 0, 0, base}) == u32(ORBIS_AJM_ERROR_WRONG_REVISION_FLAG));
    CHECK(call("AxoDrINp4J8", {ctx, 0, 1 | (7 << 7), base}) ==
          u32(ORBIS_AJM_ERROR_FLAG_NOT_SUPPORTED));
    CHECK(call("AxoDrINp4J8", {ctx, 2, 1 | (1 << 7), base}) ==
          u32(ORBIS_AJM_ERROR_FLAG_NOT_SUPPORTED));
    CHECK(call("AxoDrINp4J8", {ctx, 0, 1, 1}) == u32(ORBIS_AJM_ERROR_INVALID_ADDRESS));
    CHECK(call("AxoDrINp4J8", {ctx, 0, 1, base + 4}) == 0);
    const u32 instance = read.operator()<u32>(base + 4);
    CHECK(call("diXjQNiMu-s", {instance}) == 0);
    const u64 batch = base + 0x1000, output = base + 0x3000, error = base + 0x100, idout = base + 8;
    const u64 end = call("dmDybN--Fn8", {batch, instance, 1, 0, 0, output, 8, 0});
    CHECK(end == batch + 48);
    auto start = [&](u32 bytes = 48) {
        return call("fFFkk0xfGWs", {ctx, batch, bytes, 0, error, idout});
    };
    auto wait = [&](u32 id, u32 timeout = 1000) {
        return call("-qLsfDAywIY", {ctx, id, timeout, error});
    };
    CHECK(start() == 0);
    auto id = read.operator()<u32>(idout);
    CHECK(wait(id) == 0);
    CHECK(read.operator()<AjmSidebandResult>(output).result == 0);
    CHECK(wait(id) == u32(ORBIS_AJM_ERROR_INVALID_BATCH));
    CHECK(call("RbLbuKv8zho", {ctx, instance | 0x4000}) == u32(ORBIS_AJM_ERROR_INVALID_INSTANCE));
    // Close only this output range while its submitting owner retains the old
    // bytes. The worker must wait without partial pins; no external VM gate.
    auto delayed = [&](GuestRange held, GuestRange changed, bool replace, auto action) {
        auto retained = space->AcquireDataSpan(held, true);
        CHECK(retained);
        std::stop_source stop;
        auto edit = std::async(std::launch::async, [&] {
            return space->UpdateDataMapping(GuestAddressSpace::VmOperation::Map, changed, rw, -1, 0,
                                            stop.get_token());
        });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!space->Counts().retiring_ranges && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        if (!space->Counts().retiring_ranges)
            std::_Exit(3);
        CHECK(start() == 0);
        id = read.operator()<u32>(idout);
        action();
        if (!replace)
            stop.request_stop();
        retained = MakeError(ErrorCategory::InvalidArgument, "test", "release");
        const auto result = edit.get();
        CHECK(replace ? bool(result) : !result);
    };
    delayed({GuestAddress{output}, 0x1000}, {GuestAddress{output}, 0x1000}, false, [&] {
        CHECK(call("RbLbuKv8zho", {ctx, instance}) == u32(ORBIS_AJM_ERROR_BUSY));
        CHECK(call("NVDXiUesSbA", {ctx + 1, id}) == u32(ORBIS_AJM_ERROR_INVALID_CONTEXT));
        CHECK(call("NVDXiUesSbA", {ctx, id}) == 0);
    });
    CHECK(wait(id) == u32(ORBIS_AJM_ERROR_CANCELLED));
    delayed({GuestAddress{output}, 0x1000}, {GuestAddress{output}, 0x1000}, true, [] {});
    write(output, u64(0xdeadbeefdeadbeef));
    CHECK(wait(id) == u32(ORBIS_AJM_ERROR_INVALID_ADDRESS));
    CHECK(read.operator()<u64>(output) == 0xdeadbeefdeadbeef);
    delayed({GuestAddress{output}, 0x1000}, {GuestAddress{output}, 0x1000}, false, [&] {
        CHECK(space->MapFreshData({GuestAddress{base + 0x200000}, 0x1000}, rw));
    });
    CHECK(wait(id) == 0);
    CHECK(call("dmDybN--Fn8", {batch, instance, 1, 0, 0, base + 0x5000, 0x3000, 0}) == batch + 48);
    delayed({GuestAddress{base + 0x5000}, 0x3000}, {GuestAddress{base + 0x6000}, 0x1000}, true,
            [] {});
    write(base + 0x6000, u64(0xabcabc));
    CHECK(wait(id) == u32(ORBIS_AJM_ERROR_INVALID_ADDRESS));
    CHECK(read.operator()<u64>(base + 0x6000) == 0xabcabc);
    CHECK(call("dmDybN--Fn8", {batch, instance, 1, 0, 0, output, 8, 0}) == batch + 48);
    // Failed writable outputs do not consume batch ids or mutate output bytes.
    CHECK(call("dmDybN--Fn8", {batch, instance, 1, 0, 0, 1, 8, 0}) == 0);
    CHECK(start(47) == u32(ORBIS_AJM_ERROR_MALFORMED_BATCH));
    const auto original = read.operator()<std::array<u64, 6>>(batch);
    auto malformed = original;
    malformed[0] = 0xffffffff00000000ull;
    write(batch, malformed);
    CHECK(start() == u32(ORBIS_AJM_ERROR_MALFORMED_BATCH));
    write(batch, original);
    // Short sideband, duplicate flags and unsupported chunk rejected before decoder.
    write(batch + 36, u32(7));
    CHECK(start() == u32(ORBIS_AJM_ERROR_MALFORMED_BATCH));
    write(batch, original);
    write(batch + 8, u32(63));
    CHECK(start() == u32(ORBIS_AJM_ERROR_INVALID_OPCODE));
    write(batch, original);
    write(batch + 40, u64(1));
    CHECK(start() == u32(ORBIS_AJM_ERROR_INVALID_ADDRESS));
    write(batch, original);
    CHECK(call("stlghnic3Jc", {batch, 1, UINT64_MAX, base}) == 0);
    write(base + 0x200, u64(0x123456789abcdef0));
    CHECK(call("stlghnic3Jc", {batch + 0x100, base + 0x200, 8, base + 0x208}) == batch + 0x110);
    CHECK(read.operator()<u64>(base + 0x208) == batch + 0x108);
    CHECK(read.operator()<u64>(batch + 0x108) == 0x123456789abcdef0);
    // Malformed MP3 cannot divide by zero or read OFL outside the provided span.
    write(base + 0x200, u32(0xffffffff));
    CHECK(call("eDFeTyi+G3Y", {base + 0x200, 4, 1, base + 0x400}) ==
          u32(ORBIS_AJM_ERROR_INVALID_PARAMETER));
    // Known MPEG1 layer3/128kbps/44.1kHz header, header-only parsing is valid.
    write(base + 0x200, u32(0x6490fbff));
    CHECK(call("eDFeTyi+G3Y", {base + 0x200, 4, 1, base + 0x400}) == 0);
    CHECK(read.operator()<u64>(base + 0x400) == 417);
    // The statistics worker follows existing desktop estimates, bounds its array.
    write(base + 0x200, AjmSidebandStatisticsEngineParameters{4, {}});
    CHECK(call("dmDybN--Fn8", {batch, AJM_INSTANCE_STATISTICS, 1 | (1ull << 31), base + 0x200, 16,
                               output, 24, 0}) == batch + 48);
    CHECK(start() == u32(ORBIS_AJM_ERROR_MALFORMED_BATCH));
    write(base + 0x200, AjmSidebandStatisticsEngineParameters{3, {}});
    CHECK(start() == 0);
    id = read.operator()<u32>(idout);
    CHECK(wait(id) == 0);
    // Actual codec execution: generated sine-wave MP3, split PCM destinations,
    // stream counters and nonzero decoded samples; not a mocked decoder.
    if (argc != 2) {
        std::printf("FAIL: generated MP3 fixture is required\n");
        ++failures;
    } else {
        std::ifstream file(argv[1], std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
        CHECK(!bytes.empty() && bytes.size() < 0x10000);
        CHECK(space->Write(GuestAddress{base + 0x10000}, std::as_bytes(std::span{bytes})));
        std::array<AjmBuffer, 1> in{{{reinterpret_cast<u8*>(base + 0x10000), bytes.size()}}};
        std::array<AjmBuffer, 2> out{{{reinterpret_cast<u8*>(base + 0x20000), 32768},
                                      {reinterpret_cast<u8*>(base + 0x30000), 32768}}};
        write(base + 0x600, in);
        write(base + 0x700, out);
        const u64 flags = 1 | (1 << 12) | (1ull << 47); // multiple frames + stream
        const u64 finish = call("7jdAXK+2fMo", {batch, instance, flags, base + 0x600, 1,
                                                base + 0x700, 2, output, 32, 0});
        CHECK(finish == batch + 80);
        CHECK(start(80) == 0);
        id = read.operator()<u32>(idout);
        CHECK(wait(id) == 0);
        const auto stream = read.operator()<AjmSidebandStream>(output + 8);
        CHECK(stream.input_consumed > 0 && stream.output_written > 0 &&
              stream.total_decoded_samples > 0);
        std::vector<s16> pcm(16384);
        CHECK(space->Read(GuestAddress{base + 0x20000}, std::as_writable_bytes(std::span{pcm})));
        CHECK(std::any_of(pcm.begin(), pcm.end(), [](s16 sample) { return sample != 0; }));
        std::printf("AJM_MP3 input=%d output=%d samples=%llu\n", stream.input_consumed,
                    stream.output_written, (unsigned long long)stream.total_decoded_samples);
    }
    // Invalid AT9 initialization is a batch error, with no partial sideband write.
    CHECK(call("AxoDrINp4J8", {ctx, 1, 1, base + 4}) == 0);
    const u32 at9 = read.operator()<u32>(base + 4);
    write(base + 0x200, u64(0));
    write(output, u64(0xabcdef));
    CHECK(call("dmDybN--Fn8", {batch, at9, 1 | (1 << 14), base + 0x200, 8, output, 8, 0}) ==
          batch + 48);
    CHECK(start() == 0);
    id = read.operator()<u32>(idout);
    CHECK(wait(id) == u32(ORBIS_AJM_ERROR_INVALID_PARAMETER));
    CHECK(read.operator()<u64>(output) == 0xabcdef);
    CHECK(call("RbLbuKv8zho", {ctx, at9}) == 0);
    CHECK(call("AxoDrINp4J8", {ctx, 2, 1, base + 4}) == 0);
    const u32 aac = read.operator()<u32>(base + 4);
    CHECK(call("dmDybN--Fn8", {batch, aac, 1 | (1ull << 46), 0, 0, output, 32, 0}) == batch + 48);
    CHECK(start() == 0);
    id = read.operator()<u32>(idout);
    CHECK(wait(id) == 0);
    CHECK(read.operator()<AjmSidebandFormat>(output + 8).num_channels == 0);
    CHECK(call("RbLbuKv8zho", {ctx, aac}) == 0);
    CHECK(call("RbLbuKv8zho", {ctx, instance}) == 0);
    CHECK(call("RbLbuKv8zho", {ctx, instance}) == u32(ORBIS_AJM_ERROR_INVALID_INSTANCE));
    ajm.RequestStop();
    CHECK(call("dl+4eHSzUu4", {0, base}) == u32(ORBIS_AJM_ERROR_CANCELLED));
    std::printf("GUEST_AJM checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
