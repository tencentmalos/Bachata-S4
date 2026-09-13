// SPDX-License-Identifier: GPL-2.0-or-later
// Auxiliary CLI exercising the SAME production backend linked by the ordinary
// APK.
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/host_runtime/session_backend_fex.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <thread>

using namespace Core::HostRuntime;
int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 3 || argc > 4) {
    std::fprintf(stderr, "usage: production_runtime_runner USER_DIR ELF "
                         "[return|cancel|fault|reject]\n");
    return 2;
  }
  const std::string mode = argc == 4 ? argv[3] : "return";
  if (mode != "return" && mode != "cancel" && mode != "fault" &&
      mode != "reject")
    return 2;
  try {
    Common::FS::InitializeAndroidUserPaths(std::filesystem::path(argv[1]));
    Common::Log::Setup("production-runtime.log");
    StopTicket stale{};
    for (unsigned round = 1; round <= 3; ++round) {
      FexSessionBackend backend;
      SessionParams params;
      params.content_id = "production-fixture";
      params.executable_path = argv[2];
      auto prepared = backend.Prepare(params);
      if (!prepared) {
        std::printf("ROUND %u PREPARE_REJECT %s\n", round,
                    Core::GuestCpu::Describe(prepared.GetError()).c_str());
        if (mode == "reject")
          continue;
        return 1;
      }
      auto runtime = prepared.Value();
      if (mode == "reject" || backend.WaitStopped(*runtime, {}, 1) ||
          (stale.valid && backend.WaitStopped(*runtime, stale, 1))) {
        std::printf("TICKET_OR_REJECTION_FAIL\n");
        return 1;
      }
      std::optional<Core::GuestCpu::Result<StopTicket>> requested;
      std::jthread stopper;
      if (mode == "cancel") {
        stopper = std::jthread([&] {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          requested = backend.RequestCancel(*runtime);
        });
      }
      const auto report = backend.Run(*runtime);
      if (stopper.joinable())
        stopper.join();
      std::printf("ROUND %u OUTCOME %s DETAIL %s\n", round,
                  ToString(report.outcome), report.detail.c_str());
      bool pass =
          mode == "cancel" ? report.outcome == RunOutcome::Cancelled
          : mode == "fault"
              ? report.outcome == RunOutcome::Faulted &&
                    report.detail.find("operation=") != std::string::npos
              : report.outcome == RunOutcome::Returned &&
                    report.detail.starts_with("guest return=51966");
      if (!requested)
        requested = backend.RequestCancel(*runtime);
      if (!*requested)
        pass = false;
      else {
        stale = requested->Value();
        pass &= bool(backend.WaitStopped(*runtime, stale, 1'000'000'000));
      }
      backend.Destroy(*runtime);
      // A destroyed runtime must not accept its old stop receipt.
      pass &= !backend.WaitStopped(*runtime, stale, 1);
      runtime.reset();
      if (!pass)
        return 1;
    }
    Common::Log::Flush();
    std::printf("PRODUCTION_RUNTIME_PASS mode=%s rounds=3\n", mode.c_str());
    return 0;
  } catch (const std::exception &e) {
    std::printf("PRODUCTION_RUNTIME_FAIL %s\n", e.what());
    return 1;
  }
}
