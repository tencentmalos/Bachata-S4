// JNI surface for the shadps4-app in-process FEX session.
//
// A stateful variant of the fex-validation app's validation_jni.cpp: instead of building and tearing
// down a whole guest session per call, it holds one process-global session (address space + context
// + owner thread) and exposes a small start/stop/identity surface that FexSessionService drives from
// the Kotlin side. It uses the *public* guest_cpu API and the same guest_cpu_fex backend the CLI and
// validation app use — no external process, no BACHATA/1 socket, no Vortek/X.
//
// This is the "FEX smoke session" of the A0 plan: it proves the CPU backend is alive inside a normal
// app process. It runs a bounded x86-64 decrement loop so a Run stays in the JIT long enough for an
// async Cancel (Stop) to demonstrably interrupt it; it does NOT run a real PS4 game (the Android
// host is not yet native — see docs).
//
// Guest routine (hand-assembled, position-independent), rdi = iteration count:
//     loop:  sub rdi, 1            ; 48 83 ef 01
//            jne loop              ; 75 fa
//            movabs r15, <gate>    ; 49 bf <imm64>
//            jmp r15               ; 41 ff e7   (-> StopReason::Returned)
#include <jni.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <android/log.h>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/context.h"
#include "core/guest_cpu/api/execution.h"
#include "core/guest_cpu/api/memory.h"
#include "core/guest_cpu/api/registers.h"
#include "core/guest_cpu/api/result.h"
#include "core/guest_cpu/api/status.h"

using namespace Core::GuestCpu;

namespace {

constexpr std::uint64_t kReservationSize = std::uint64_t{1} << 28;
constexpr std::uint64_t kMappingSize = 0x4000;
constexpr std::uint64_t kCodeOffset = 0x10000;
constexpr std::uint64_t kStackOffset = 0x20000;

constexpr const char* kTag = "FexSession";

void LogI(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, kTag, fmt, ap);
    va_end(ap);
}

// Assemble the bounded decrement loop; the caller patches the return-gate address.
std::vector<std::uint8_t> BuildLoopRoutine(std::uint64_t gate_address) {
    std::vector<std::uint8_t> code;
    auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        for (auto b : bytes) code.push_back(b);
    };
    emit({0x48, 0x83, 0xef, 0x01});          // sub rdi, 1
    emit({0x75, 0xfa});                       // jne -6 (back to sub)
    emit({0x49, 0xbf});                       // movabs r15, imm64
    for (int i = 0; i < 8; ++i)
        code.push_back(static_cast<std::uint8_t>((gate_address >> (8 * i)) & 0xff));
    emit({0x41, 0xff, 0xe7});                 // jmp r15
    return code;
}

std::string ReadBuildId() {
    std::string result;
    dl_iterate_phdr(
        [](dl_phdr_info* info, std::size_t, void* data) -> int {
            auto* out = static_cast<std::string*>(data);
            const char* name = info->dlpi_name ? info->dlpi_name : "";
            if (std::strstr(name, "shadps4_fex_session") == nullptr)
                return 0;
            for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_NOTE)
                    continue;
                const auto* base =
                    reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr);
                std::size_t left = ph.p_memsz;
                std::size_t off = 0;
                while (off + 12 <= left) {
                    const auto namesz = *reinterpret_cast<const std::uint32_t*>(base + off);
                    const auto descsz = *reinterpret_cast<const std::uint32_t*>(base + off + 4);
                    const auto type = *reinterpret_cast<const std::uint32_t*>(base + off + 8);
                    if (type == NT_GNU_BUILD_ID) {
                        const unsigned char* d = base + off + 12 + ((namesz + 3) & ~3u);
                        char buf[3];
                        for (std::uint32_t b = 0; b < descsz; ++b) {
                            std::snprintf(buf, sizeof(buf), "%02x", d[b]);
                            *out += buf;
                        }
                        return 1;
                    }
                    off += 12 + ((namesz + 3) & ~3u) + ((descsz + 3) & ~3u);
                    if (off >= left) break;
                }
            }
            return 0;
        },
        &result);
    return result;
}

// One process-global session. The owner thread runs Create/Run/Destroy; Stop comes from any thread
// via RequestInterrupt(Cancel) + WaitStopped, which the API explicitly allows off-owner.
struct Session {
    std::mutex mutex;
    std::thread owner;
    std::atomic<bool> running{false};

    std::unique_ptr<GuestAddressSpace> space;
    std::unique_ptr<CpuContext> context;
    ThreadHandle thread{};

    std::string last_error;
    std::atomic<int> last_stop_reason{-1};  // StopReason as int, -1 = none yet
};

Session g_session;

void OwnerRun(std::uint64_t iterations) {
    auto fail = [&](const std::string& e) {
        std::lock_guard<std::mutex> lk(g_session.mutex);
        g_session.last_error = e;
        LogI("session error: %s", e.c_str());
    };

    AddressSpaceConfig cfg{};
    cfg.reservation_size = kReservationSize;
    cfg.max_address = QueryBackendCapabilities().max_guest_address;
    auto space_r = GuestAddressSpace::Create(cfg);
    if (!space_r) { fail("address space: " + Describe(space_r.GetError())); g_session.running = false; return; }
    auto space = std::move(space_r).Value();

    const std::uint64_t base = space->ReservationBase().value;
    const std::uint64_t code_base = base + kCodeOffset;
    const std::uint64_t stack_top = base + kStackOffset + kMappingSize - 16;

    if (auto m = space->Map(GuestRange{GuestAddress{code_base}, kMappingSize},
                            GuestPermission::Read | GuestPermission::Write); !m) {
        fail("map code: " + Describe(m.GetError())); g_session.running = false; return;
    }
    if (auto m = space->Map(GuestRange{GuestAddress{stack_top - kMappingSize + 16}, kMappingSize},
                            GuestPermission::Read | GuestPermission::Write); !m) {
        fail("map stack: " + Describe(m.GetError())); g_session.running = false; return;
    }

    auto ctx_r = CreateContext(CpuConfig{}, *space);
    if (!ctx_r) { fail("create context: " + Describe(ctx_r.GetError())); g_session.running = false; return; }
    auto context = std::move(ctx_r).Value();
    const std::uint64_t gate = context->Capabilities().return_gate_address;
    if (gate == 0) { fail("no return gate"); g_session.running = false; return; }

    auto code = BuildLoopRoutine(gate);
    if (auto w = space->Write(GuestAddress{code_base},
                             {reinterpret_cast<const std::byte*>(code.data()), code.size()}); !w) {
        fail("write code: " + Describe(w.GetError())); g_session.running = false; return;
    }
    if (auto p = space->Protect(GuestRange{GuestAddress{code_base}, kMappingSize},
                               GuestPermission::Read | GuestPermission::Execute); !p) {
        fail("protect code: " + Describe(p.GetError())); g_session.running = false; return;
    }

    ThreadInit init{};
    init.entry_rip = GuestCodeAddress{code_base};
    init.initial_rsp = GuestAddress{stack_top};
    init.guest_tid = 1;
    init.initial_state.fields = RegisterValidity::Gpr;
    init.initial_state.gpr_mask = (1u << Index(Gpr::Rdi));
    init.initial_state.values.Set(Gpr::Rdi, iterations);

    auto th_r = context->CreateThread(init);
    if (!th_r) { fail("create thread: " + Describe(th_r.GetError())); g_session.running = false; return; }

    {
        std::lock_guard<std::mutex> lk(g_session.mutex);
        g_session.space = std::move(space);
        g_session.context = std::move(context);
        g_session.thread = th_r.Value();
    }

    LogI("session running: %llu iterations, gate=0x%llx",
         static_cast<unsigned long long>(iterations), static_cast<unsigned long long>(gate));

    auto run = g_session.context->Run(g_session.thread, RunOptions{});
    if (run) {
        g_session.last_stop_reason = static_cast<int>(run.Value().primary_reason);
        LogI("session stopped: reason=%d", static_cast<int>(run.Value().primary_reason));
    } else {
        fail("run: " + Describe(run.GetError()));
    }

    (void)g_session.context->DestroyThread(g_session.thread);
    {
        std::lock_guard<std::mutex> lk(g_session.mutex);
        g_session.context.reset();
        g_session.space.reset();
        g_session.thread = ThreadHandle{};
    }
    g_session.running = false;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeIdentity(JNIEnv* env, jclass) {
    char msg[256];
    std::snprintf(msg, sizeof(msg), "page_size=%ld pid=%d uid=%d build_id=%s",
                  sysconf(_SC_PAGESIZE), static_cast<int>(getpid()), static_cast<int>(getuid()),
                  ReadBuildId().c_str());
    return env->NewStringUTF(msg);
}

// Starts the smoke session on a dedicated owner thread. Returns true if a new session was started,
// false if one is already running. `iterations` bounds the loop (large => stays live long enough to
// be interrupted by stop; still finite so a natural Returned end is reachable).
extern "C" JNIEXPORT jboolean JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeStart(
        JNIEnv* env, jclass, jlong iterations) {
    // Route FEXCore diagnostics to logcat (tag FexCore) — Android discards stderr.
    ::setenv("GUEST_CPU_DEBUG", "1", 0);
    bool expected = false;
    if (!g_session.running.compare_exchange_strong(expected, true))
        return JNI_FALSE;  // already running
    {
        std::lock_guard<std::mutex> lk(g_session.mutex);
        g_session.last_error.clear();
        g_session.last_stop_reason = -1;
        if (g_session.owner.joinable()) g_session.owner.join();
    }
    const std::uint64_t iters = iterations > 0 ? static_cast<std::uint64_t>(iterations)
                                               : (std::uint64_t{1} << 32);
    g_session.owner = std::thread(OwnerRun, iters);
    return JNI_TRUE;
}

// Async stop: RequestInterrupt(Cancel) + WaitStopped from this (non-owner) thread, then let the
// owner thread's Run return Cancelled and tear the session down.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeRequestStop(
        JNIEnv* env, jclass, jlong timeout_ms) {
    CpuContext* ctx = nullptr;
    ThreadHandle th{};
    {
        std::lock_guard<std::mutex> lk(g_session.mutex);
        ctx = g_session.context.get();
        th = g_session.thread;
    }
    if (ctx == nullptr || !th.IsValid())
        return JNI_FALSE;

    auto ticket = ctx->RequestInterrupt(th, InterruptReason::Cancel);
    if (!ticket) {
        std::lock_guard<std::mutex> lk(g_session.mutex);
        g_session.last_error = "request stop: " + Describe(ticket.GetError());
        return JNI_FALSE;
    }
    const std::uint64_t timeout_ns =
        (timeout_ms > 0 ? static_cast<std::uint64_t>(timeout_ms) : 1000ull) * 1'000'000ull;
    auto receipt = ctx->WaitStopped(ticket.Value(), timeout_ns);
    if (!receipt) {
        std::lock_guard<std::mutex> lk(g_session.mutex);
        g_session.last_error = "wait stopped: " + Describe(receipt.GetError());
        return JNI_FALSE;
    }
    // Owner thread observes Cancelled and tears down; join it so the session is fully idle.
    if (g_session.owner.joinable()) g_session.owner.join();
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeIsRunning(JNIEnv*, jclass) {
    return g_session.running.load() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeLastStopReason(JNIEnv*, jclass) {
    return static_cast<jint>(g_session.last_stop_reason.load());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_shadps4_android_runtime_session_NativeFexSession_nativeLastError(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_session.mutex);
    if (g_session.last_error.empty())
        return nullptr;
    return env->NewStringUTF(g_session.last_error.c_str());
}
