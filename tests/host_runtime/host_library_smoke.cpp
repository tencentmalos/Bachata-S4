// SPDX-License-Identifier: GPL-2.0-or-later
// Link this to the complete host DSO on Android; on the build host the portable
// frontend/control sources can be linked directly. No game, GPU or APK acceptance.
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include "common/poll_timeout.h"
#include "core/host_runtime/application_control.h"
#include "frontend/window.h"
#ifdef __ANDROID__
#include <cstring>
#include <fstream>
#include <new>
#include <sys/ucontext.h>
#include "common/path_util.h"
#include "core/libraries/kernel/threads/exception.h"
#include "frontend/android_window.h"
namespace Libraries::SystemService {
int sceSystemServiceLoadExec(const char*, const char**);
}
#endif

static unsigned checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL line %d: %s\n", __LINE__, #x);                                       \
        }                                                                                          \
    } while (0)

struct TestWindow final : Frontend::Window {
    explicit TestWindow(std::atomic<unsigned>& destroyed_) : destroyed{destroyed_} {}
    ~TestWindow() override {
        ++destroyed;
    }
    s32 GetWidth() const override {
        return 1280;
    }
    s32 GetHeight() const override {
        return 720;
    }
    Frontend::WindowSystemInfo GetWindowInfo() const override {
        return {};
    }
    bool RequestKeyboard() override {
        ++keyboard;
        return true;
    }
    void ReleaseKeyboard() override {
        --keyboard;
    }
    std::atomic<unsigned>& destroyed;
    int keyboard{};
};

struct CallLatch {
    bool entered{}, released{};
    std::mutex mutex;
    std::condition_variable cv;
};

struct TestControl final : Core::HostRuntime::ApplicationControl {
    Core::HostRuntime::LoadExecResult RequestLoadExec(
        const Core::HostRuntime::LoadExecRequest& r) override {
        seen = r;
        if (fail)
            throw std::runtime_error("injected frontend failure");
        if (wait) {
            const auto gate = latch;
            std::unique_lock lock{gate->mutex};
            gate->entered = true;
            gate->cv.notify_all();
            gate->cv.wait(lock, [&] { return gate->released; });
        }
        return Core::HostRuntime::LoadExecResult::Accepted;
    }
    bool fail{}, wait{};
    std::shared_ptr<CallLatch> latch = std::make_shared<CallLatch>();
    Core::HostRuntime::LoadExecRequest seen;
};

int main(int argc, char** argv) {
#ifdef __ANDROID__
    if (argc != 2) {
        std::fprintf(stderr, "usage: host_library_smoke <absolute-user-data-directory>\n");
        return 2;
    }
    using namespace Common::FS;
    bool uninitialized = false;
    try {
        (void)GetUserPath(PathType::UserDir);
    } catch (const std::logic_error&) {
        uninitialized = true;
    }
    CHECK(uninitialized);
    bool bad_path = false;
    try {
        InitializeAndroidUserPaths("relative/path");
    } catch (const std::invalid_argument&) {
        bad_path = true;
    }
    CHECK(bad_path);
    const std::filesystem::path root{argv[1]};
    // A genuine filesystem failure must not publish a partial layout or latch
    // initialization permanently. The subsequent valid request must recover.
    const auto blocker = root.parent_path() / "not-a-directory";
    {
        std::ofstream file(blocker);
        file << "blocked";
    }
    bool io_failed = false;
    try {
        InitializeAndroidUserPaths(blocker / "child");
    } catch (const std::filesystem::filesystem_error&) {
        io_failed = true;
    }
    CHECK(io_failed);
    InitializeAndroidUserPaths(root);
    const auto* stable = &GetUserPath(PathType::UserDir);
    CHECK(*stable == root);
    CHECK(std::filesystem::is_directory(GetUserPath(PathType::LogDir)));
    InitializeAndroidUserPaths(root);
    CHECK(&GetUserPath(PathType::UserDir) == stable);
    bool changed = false;
    try {
        InitializeAndroidUserPaths(root / "new");
    } catch (const std::logic_error&) {
        changed = true;
    }
    CHECK(changed);
    CHECK(GetUserPath(PathType::UserDir) == root);
#endif
    using namespace Core::HostRuntime;
    CHECK(!Frontend::AcquireWindow());
    CHECK(!Frontend::BindWindow(nullptr));
    CHECK(!Frontend::UnbindWindow(nullptr));
    std::atomic<unsigned> destroyed{};
    auto first = std::make_shared<TestWindow>(destroyed);
    auto next = std::make_shared<TestWindow>(destroyed);
    CHECK(Frontend::BindWindow(first));
    CHECK(!Frontend::BindWindow(next));
    auto held = Frontend::AcquireWindow();
    CHECK(held->GetWidth() == 1280 && held->GetHeight() == 720);
    CHECK(held->GetSDLWindow() == nullptr);
    CHECK(held->RequestKeyboard());
    held->ReleaseKeyboard();
    CHECK(first->keyboard == 0);
    CHECK(Frontend::UnbindWindow(first));
    CHECK(Frontend::BindWindow(next));
    CHECK(!Frontend::UnbindWindow(first));
    CHECK(Frontend::AcquireWindow() == next);
    first.reset();
    CHECK(destroyed == 0); // In-flight reader still owns the retired window.
    held.reset();
    CHECK(destroyed == 1);
    CHECK(Frontend::UnbindWindow(next));
    next.reset();
    CHECK(destroyed == 2);

    LoadExecRequest request{.guest_path = "/app0/next.self", .args = {"next", "hello"}};
    CHECK(RequestLoadExec(request) == LoadExecResult::Unavailable);
    CHECK(RequestLoadExec({}) == LoadExecResult::InvalidRequest);
    CHECK(!BindApplicationControl(nullptr));
    CHECK(!UnbindApplicationControl(nullptr));
    auto control = std::make_shared<TestControl>();
    CHECK(BindApplicationControl(control));
    CHECK(!BindApplicationControl(control));
    CHECK(RequestLoadExec(request) == LoadExecResult::Accepted);
    CHECK(control->seen.guest_path == request.guest_path && control->seen.args == request.args);
    control->fail = true;
    CHECK(RequestLoadExec(request) == LoadExecResult::Failed);
    control->fail = false;
    control->wait = true;
    auto latch = control->latch;
    LoadExecResult result = LoadExecResult::Failed;
    std::thread caller([&] { result = RequestLoadExec(request); });
    {
        std::unique_lock lock{latch->mutex};
        CHECK(latch->cv.wait_for(lock, std::chrono::seconds(5), [&] { return latch->entered; }));
    }
    CHECK(UnbindApplicationControl(control));
    auto replacement = std::make_shared<TestControl>();
    CHECK(BindApplicationControl(replacement));
    CHECK(!UnbindApplicationControl(control));
    // The call owns its old generation's controller, even after withdrawal.
    std::weak_ptr<TestControl> weak = control;
    control.reset();
    CHECK(!weak.expired()); // Dispatch retains the old controller while its call runs.
    {
        std::scoped_lock lock{latch->mutex};
        latch->released = true;
    }
    latch->cv.notify_all();
    caller.join();
    CHECK(result == LoadExecResult::Accepted);
    CHECK(weak.expired());
    CHECK(UnbindApplicationControl(replacement));

    CHECK(Common::PollTimeoutMilliseconds(-1) == -1);
    CHECK(Common::PollTimeoutMilliseconds(INT_MIN) == INT_MIN);
    CHECK(Common::PollTimeoutMilliseconds(0) == 0);
    CHECK(Common::PollTimeoutMilliseconds(1) == 1);
    CHECK(Common::PollTimeoutMilliseconds(999) == 1);
    CHECK(Common::PollTimeoutMilliseconds(1000) == 1);
    CHECK(Common::PollTimeoutMilliseconds(1001) == 2);
    CHECK(Common::PollTimeoutMilliseconds(INT_MAX) == 2147484);
#ifdef __ANDROID__
    // Call the production HLE export: absence of a bound session is a failure,
    // never a no-op success or a process exit.
    CHECK(Libraries::SystemService::sceSystemServiceLoadExec("/app0/next.self", nullptr) != 0);
    CHECK(Libraries::SystemService::sceSystemServiceLoadExec(nullptr, nullptr) != 0);
    CHECK(BindApplicationControl(replacement));
    const char* guest_argv[] = {"hello", nullptr};
    CHECK(Libraries::SystemService::sceSystemServiceLoadExec("/app0/next.self", guest_argv) == 0);
    CHECK(replacement->seen.args == std::vector<std::string>{"hello"});
    CHECK(UnbindApplicationControl(replacement));
    using Libraries::Kernel::Ucontext;
    alignas(Ucontext) unsigned char storage[sizeof(Ucontext)];
    std::memset(storage, 0xa5, sizeof(storage));
    siginfo_t info{};
    ucontext_t host{};
    host.uc_mcontext.pc = 0x11223344;
    auto* context = new (storage) Ucontext(&info, &host);
    CHECK(context->uc_mcontext.mc_rip == 0 && context->uc_mcontext.mc_rsp == 0);
    CHECK(!context->HasGuestContext());
    CHECK(!context->SyncHostFromGuest());
    CHECK(host.uc_mcontext.pc == 0x11223344);
    context->~Ucontext();
    bool rejected = false;
    try {
        Frontend::AndroidWindow invalid(nullptr, 1);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
#endif
    std::printf("host_library_smoke: %u checks / %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
