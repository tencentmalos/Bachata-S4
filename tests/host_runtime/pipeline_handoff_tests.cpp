// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
#include "core/diagnostics/diagnostics_hub.h"
#include "core/diagnostics/pipeline_handoff.h"

using namespace Core::Diagnostics;
int main() {
    unsigned checks{};
    const auto check = [&](bool value) {
        ++checks;
        if (!value) {
            std::cerr << "FAIL check " << checks << '\n';
            std::exit(1);
        }
    };
    DiagnosticsSnapshot snapshot;
    snapshot.generation = 42;
    snapshot.pid = 1;
    snapshot.run_uuid = "test";
    check(Handoff::Control({"start"}, snapshot).find("no_session") != std::string::npos);
    snapshot.has_session = true;
    for (auto duration : {"-1", "0", "99", "10001", "100x", "18446744073709551616"})
        check(Handoff::Control({"start", duration}, snapshot).find("invalid_duration") !=
              std::string::npos);
    check(Handoff::Control({"start", "100"}, snapshot).find("recording") != std::string::npos);
    check(Handoff::Control({"start"}, snapshot).find("already_recording") != std::string::npos);
    check(Handoff::Control({"dump"}, snapshot).find("still_recording") != std::string::npos);
    Handoff::Event(41, "foreign", 1, 1);
    {
        Handoff::Scope stale{"stale", 42};
        Handoff::Control({"stop"}, snapshot);
        check(Handoff::Control({"dump"}, snapshot).find("\"complete\":false") != std::string::npos);
        Handoff::Control({"start", "100"}, snapshot);
    } // A scope from the previous recording must not finish in the new capture.
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i)
        threads.emplace_back([i] {
            for (unsigned n = 0; n < 10; ++n) {
                Handoff::Scope root{"work", 42, i, n};
                Handoff::Scope wait{"wait", 42, i, n, true};
            }
        });
    for (auto& thread : threads)
        thread.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    const auto dump = Handoff::Control({"dump"}, snapshot);
    check(dump.find("\"complete\":true") != std::string::npos);
    check(dump.find("stale") == std::string::npos);
    check(dump.find("foreign") == std::string::npos);
    check(Handoff::Control({"status"}, snapshot).find("events=160") != std::string::npos);
    Handoff::Control({"start", "10000"}, snapshot);
    Handoff::EndGeneration(41);
    check(Handoff::Enabled(42));
    Handoff::EndGeneration(42);
    check(!Handoff::Enabled(42));
    check(Handoff::Control({"dump"}, snapshot).find("session_ended") != std::string::npos);
    int evaluated{};
    SHAD_HANDOFF(42, "disabled", ++evaluated, 1);
    check(evaluated == 0);
    Handoff::Control({"start", "10000"}, snapshot);
    for (unsigned i = 0; i < 100001; ++i)
        Handoff::Event(42, "capacity", 1, i);
    check(Handoff::Control({"status"}, snapshot).find("reason=capacity") != std::string::npos);
    check(Handoff::Control({"dump"}, snapshot).find("\"complete\":false") != std::string::npos);
    std::cout << checks << " checks, 0 failures\n";
}
