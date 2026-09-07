#include <spatial/debugbus/DebugCommandRegistry.h>
#ifdef __ANDROID__
#include <spatial/debugbus/DumpsysBridge.h>
#endif

// This is an integration fixture, not an emulator command endpoint. Mutating
// commands in the real host must enqueue work onto the guest's owner thread.
extern "C" int Shadps4FoundationSmoke() {
    spatial::debugbus::DebugCommandRegistry registry;
    registry.Register("status", "Read validation state", [](const auto& args) {
        return args.empty() ? "ready\n" : "invalid arguments\n";
    });
    if (registry.Handle("status") != "ready\n" ||
        registry.Handle("status extra") != "invalid arguments\n" ||
        registry.Handle("help").find("status") == std::string::npos) {
        return 1;
    }
#ifdef __ANDROID__
    // No concurrent callers in this fixture. The app must drain in-flight
    // requests before destroying a registry: unbinding alone is insufficient.
    spatial::debugbus::SetDumpsysRegistry(&registry);
    const auto reply = spatial::debugbus::HandleDumpsysRequest("status", {});
    spatial::debugbus::SetDumpsysRegistry(nullptr);
    if (reply != "ready\n") {
        return 2;
    }
#endif
    return 0;
}
