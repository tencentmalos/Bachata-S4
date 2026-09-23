// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <array>
#include <thread>
#include <future>
#include "core/host_runtime/guest_live_streaming.h"
#include "core/host_runtime/guest_remote_services.h"
using namespace Core::HostRuntime;
static unsigned checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__); } } while (0)
int main() {
    // Fixed firmware cold-path errors; never infer success from nonfatal binding.
    for (u64 size : {0ULL, 1ULL, 0x3fffULL, 0x4001ULL, 0x100004000ULL, ~0ULL})
        CHECK(DispatchLiveStreamingUnavailable("kvYEw2lBndk", {size}) == 0x80a00002);
    for (int retry = 0; retry < 3; ++retry) {
        CHECK(DispatchLiveStreamingUnavailable("kvYEw2lBndk", {0x4000}) == 0x80a00007);
        // Cold getters/setters must not read or write these poison pointers.
        // A failed init cannot leave a handle or change error ordering.
        for (auto nid : LiveStreamingUnavailableNids) {
            CHECK(AdmitsLiveStreamingUnavailable(nid, "#libSceGameLiveStreaming#1#libSceGameLiveStreaming#Function"));
            CHECK(!AdmitsLiveStreamingUnavailable(nid, "#libkernel#1#libkernel#Function"));
            CHECK(!AdmitsLiveStreamingUnavailable(nid, "#libSceGameLiveStreaming#2#libSceGameLiveStreaming#Function"));
            if (nid == "kvYEw2lBndk") continue;
            CHECK(DispatchLiveStreamingUnavailable(nid, {1, ~0ULL, 0, ~0ULL, 1, 1}) == 0x80a00004);
        }
        CHECK(DispatchLiveStreamingUnavailable("9yK6Fk8mKOQ", {}) == 0x80a00004);
    }
    std::array<unsigned char, 128> out; out.fill(0xa5);
    CHECK(DispatchLiveStreamingUnavailable("lK8dLBNp9OE", {reinterpret_cast<u64>(out.data())}) == 0x80a00004);
    for (auto b : out) CHECK(b == 0xa5);
    CHECK(!IsLiveStreamingUnavailableNid("unknown"));
    // No process-global initialization leaks between concurrent sessions.
    std::array<std::future<bool>, 4> jobs;
    for (auto& job : jobs) job = std::async(std::launch::async, [] {
        for (int n = 0; n < 32; ++n)
            if (DispatchLiveStreamingUnavailable("kvYEw2lBndk", {0x4000}) != 0x80a00007 ||
                DispatchLiveStreamingUnavailable("dWM80AX39o4", {1}) != 0x80a00004)
                return false;
        return true;
    });
    for (auto& job : jobs) CHECK(job.get());
    CHECK(!FindRemoteService("unknown"));
    for (const auto& entry : RemoteServiceEntries) {
        CHECK(AdmitsRemoteService(entry.nid, entry.suffix));
        CHECK(!AdmitsRemoteService(entry.nid, "#libkernel#1#libkernel#Function"));
        CHECK(!AdmitsRemoteService(entry.nid, entry.suffix == SharePlaySuffix ? RemoteplaySuffix : SharePlaySuffix));
        for (int retry = 0; retry < 3; ++retry) {
            if (entry.initialize) {
                CHECK(DispatchRemoteService(entry, {}) == 0x8002004e);
                CHECK(DispatchRemoteService(entry, {0, ~0ULL}) == 0x8002004e);
                CHECK(DispatchRemoteService(entry, {1, 0x17ff}) == (entry.error_base | 1));
                CHECK(DispatchRemoteService(entry, {1, 0x1800}) == 0x8002004e);
                CHECK(DispatchRemoteService(entry, {~0ULL, 0x100001800ULL}) == 0x8002004e);
            } else {
                CHECK(DispatchRemoteService(entry, {1, ~0ULL}) == (entry.error_base | 4));
                CHECK(DispatchRemoteService(entry, {reinterpret_cast<u64>(out.data()), reinterpret_cast<u64>(out.data())}) == (entry.error_base | 4));
            }
        }
    }
    for (auto b : out) CHECK(b == 0xa5);
    std::printf("guest_broadcast_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
