// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <barrier>
#include <cstdio>
#include <future>
#include <thread>
#include "core/host_runtime/guest_module_lifecycle.h"
using Core::HostRuntime::GuestModuleLifecycle;
using namespace std::chrono_literals;
static unsigned checks{}, failures{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__); } } while (0)
int main() {
    using namespace Core::HostRuntime;
    CHECK(GuestModuleNameKey("EOSSDK-PS4-Shipping.debug_prx") ==
          GuestModuleNameKey("prx/eossdk-ps4-shipping.prx"));
    CHECK(GuestModuleAliasIdentity("EOSSDK-PS4-Shipping.debug_prx", "EOSSDK-PS4-Shipping"));
    CHECK(!GuestModuleAliasIdentity("EOSSDK-PS4-Shipping.debug_prx", "EOSSDK-PS4-Shipping-other"));
    CHECK(GuestModuleNameKey("foo.sprx") != GuestModuleNameKey("foo.prx"));
    CHECK(GuestModuleNeedsLoadArguments("Media/Plugins/SaveData.prx"));
    CHECK(!GuestModuleNeedsLoadArguments("sce_module/libc.prx"));
    CHECK(!GuestModuleNeedsLoadArguments("prx/eossdk-ps4-shipping.prx"));
    // Real failure shape: IL2CPP DT_NEEDED links a Unity plug-in, but supplies
    // no Unity descriptor. Keep the relocation graph separate from DT_INIT.
    auto plan = PlanGuestModuleInitialization({{0, {1}}, {1, {2, 3}}, {2, {3, 4}}},
                                              {0}, {2});
    CHECK(plan.order == std::vector<u32>({3, 1}));
    CHECK(!plan.startup.contains(2) && !plan.startup.contains(4));
    GuestModuleLifecycle unity;
    for (u32 i = 0; i <= 4; ++i) unity.Add(i, plan.dependencies[i], i == 0);
    std::vector<u32> unity_calls;
    auto unity_init = [&](u32 id, u64 bytes, u64 argp) {
        unity_calls.push_back(id);
        if (id == 2) { CHECK(bytes == 16); CHECK(argp == 0x20001000); }
        else { CHECK(bytes == 0); CHECK(argp == 0); }
        return 0;
    };
    for (auto id : plan.order) CHECK(!unity.Start(id, 1, 0, 0, {}, unity_init).error);
    CHECK(unity_calls == std::vector<u32>({3, 1}));
    CHECK(unity.Start(2, 1, 16, 0x20001000, {}, unity_init).initialized);
    CHECK(unity_calls == std::vector<u32>({3, 1, 4, 2}));
    CHECK(!unity.Start(2, 1, 0, 0, {}, unity_init).initialized);
    using Core::HostRuntime::IsGuestPluginFile;
    CHECK(IsGuestPluginFile("/app0/Media/Plugins/Plugin.prx", true));
    CHECK(IsGuestPluginFile("/app0/Media/Plugins/Plugin.sprx", true));
    CHECK(!IsGuestPluginFile("/app0/Media/Plugins/._Plugin.prx", true));
    CHECK(!IsGuestPluginFile("/app0/Media/Plugins/Plugin.prx", false));
    CHECK(!IsGuestPluginFile("/app0/Media/Plugins/Plugin.txt", true));
    GuestModuleLifecycle modules;
    modules.Add(0, {}, true);
    modules.Add(1, {0});
    modules.Add(2, {1});
    modules.Add(3, {1});
    CHECK(modules.Visible(0) && !modules.Visible(2));
    std::vector<u32> calls;
    GuestModuleLifecycle::Initialize init = [&](u32 id, u64 bytes, u64 argp) {
        calls.push_back(id);
        CHECK(modules.Visible(id));
        if (id == 2) {
            CHECK(bytes == 52 && argp == 0x212345678ULL);
            const auto nested = modules.Start(2, 10, 0, 0, {}, init);
            CHECK(!nested.error && !nested.initialized);
        } else CHECK(bytes == 0 && argp == 0);
        return 0;
    };
    auto result = modules.Start(2, 10, 52, 0x212345678ULL, {}, init);
    CHECK(!result.error && result.initialized && result.value == 0);
    CHECK(calls == std::vector<u32>({1, 2}));
    result = modules.Start(2, 11, 0, 0, {}, init);
    CHECK(!result.error && !result.initialized); // caller must not overwrite pRes
    result = modules.Start(3, 11, 0, 0, {}, init);
    CHECK(result.initialized && calls == std::vector<u32>({1, 2, 3}));
    CHECK(modules.Start(99, 11, 0, 0, {}, init).error == ORBIS_KERNEL_ERROR_ENOENT);
    modules.Add(4, {5}); modules.Add(5, {4});
    CHECK(!modules.Start(4, 12, 0, 0, {}, init).error);
    CHECK(calls == std::vector<u32>({1, 2, 3, 5, 4}));
    modules.Add(6, {});
    CHECK(modules.Start(6, 1, 0, 0, {}, [](auto...) { return -42; }).value == -42);
    CHECK(!modules.Start(6, 2, 0, 0, {}, init).initialized);
    modules.Add(7, {});
    try { modules.Start(7, 1, 0, 0, {}, [](auto...) -> s32 { throw 1; }); CHECK(false); }
    catch (int) { CHECK(true); }
    CHECK(!modules.Visible(7));
    CHECK(modules.Start(7, 2, 0, 0, {}, init).error == ORBIS_KERNEL_ERROR_ESTART);
    // Independent modules can initialize concurrently; same-module callers wait
    // for completion, and a cancelled waiter does not poison the initializer.
    for (unsigned round = 0; round < 32; ++round) {
        GuestModuleLifecycle concurrent;
        concurrent.Add(0, {}); concurrent.Add(1, {});
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        std::atomic<unsigned> count{};
        auto starter = std::async(std::launch::async, [&] {
            return concurrent.Start(0, 1, 52, 123, {}, [&](u32, u64 size, u64 pointer) {
                ++count; entered.set_value(); gate.wait();
                return size == 52 && pointer == 123 ? 0 : -1;
            });
        });
        entered.get_future().wait();
        CHECK(concurrent.Start(1, 2, 0, 0, {}, [](auto...) { return 0; }).initialized);
        std::stop_source stop;
        auto waiter = std::async(std::launch::async, [&] {
            return concurrent.Start(0, 3, 0, 0, stop.get_token(), [&](auto...) { ++count; return 0; });
        });
        stop.request_stop();
        CHECK(waiter.wait_for(2s) == std::future_status::ready);
        CHECK(waiter.get().error == ORBIS_KERNEL_ERROR_EINTR);
        auto repeat = std::async(std::launch::async, [&] {
            return concurrent.Start(0, 4, 0, 0, {}, [&](auto...) { ++count; return 0; });
        });
        CHECK(repeat.wait_for(2ms) == std::future_status::timeout);
        release.set_value();
        CHECK(starter.get().initialized);
        CHECK(!repeat.get().initialized && count == 1);
    }
    // Cross-owner callbacks forming a cycle fail instead of deadlocking.
    GuestModuleLifecycle cyclic;
    cyclic.Add(0, {}); cyclic.Add(1, {});
    std::barrier barrier(2);
    auto cycle = [&](u32 id, u64 owner) {
        return cyclic.Start(id, owner, 0, 0, {}, [&](auto...) {
            barrier.arrive_and_wait();
            return cyclic.Start(1 - id, owner, 0, 0, {}, [](auto...) { return 0; }).error;
        });
    };
    auto a = std::async(std::launch::async, [&] { return cycle(0, 1); });
    auto b = std::async(std::launch::async, [&] { return cycle(1, 2); });
    auto ar = a.get(), br = b.get();
    CHECK(ar.value == ORBIS_KERNEL_ERROR_EDEADLK || br.value == ORBIS_KERNEL_ERROR_EDEADLK);
    std::printf("guest_module_lifecycle_tests: %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
