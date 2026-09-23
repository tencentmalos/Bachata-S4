// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/host_runtime/guest_graphics_hle.h"
#include "core/host_runtime/guest_ime_dialog.h"
#include "core/host_runtime/guest_ime_keyboard.h"
#include "core/host_runtime/guest_error_dialog.h"
#include "core/host_runtime/guest_signin_dialog.h"
#include "core/host_runtime/guest_msg_dialog.h"
#include "core/host_runtime/guest_commerce_dialog.h"
#include "core/host_runtime/guest_ssl.h"
#include "core/host_runtime/guest_posix_network.h"
#include "core/host_runtime/guest_random.h"
#include <atomic>
#include <fstream>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <sys/mman.h>
#if defined(__linux__)
#include <sched.h>
#endif
#include <unistd.h>
#include "common/alignment.h"
#include "common/profiler.h"
#include "common/thread.h"
#include "common/elf_info.h"
#include "common/string_util.h"
#include "common/singleton.h"
#include "core/aerolib/aerolib.h"
#include "core/libraries/kernel/kernel.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/guest_cpu/hle/scope.h"
#include "core/guest_cpu/hle/veneer_allocator.h"
#include "core/host_runtime/guest_ajm.h"
#include "core/host_runtime/guest_aio.h"
#include "core/host_runtime/guest_app_content.h"
#include "core/host_runtime/guest_audio.h"
#include "core/host_runtime/guest_audio3d.h"
#include "core/host_runtime/guest_audio_input.h"
#include "core/host_runtime/guest_live_streaming.h"
#include "core/host_runtime/guest_remote_services.h"
#include "core/host_runtime/guest_avplayer.h"
#include "core/host_runtime/guest_clock.h"
#include "core/host_runtime/guest_camera.h"
#include "core/host_runtime/guest_reprojection.h"
#include "core/host_runtime/guest_hmd_diagnostics.h"
#include "core/libraries/kernel/threads/event_flag_state.h"
#include "core/host_runtime/guest_graphics.h"
#include "core/host_runtime/guest_kernel_semaphore.h"
#include "core/host_runtime/guest_libc_policy.h"
#include "core/host_runtime/guest_memory_hle.h"
#include "core/host_runtime/guest_direct_memory_hle.h"
#include "core/host_runtime/guest_page_states.h"
#include "core/host_runtime/guest_process_services.h"
#include "core/host_runtime/guest_backtrace.h"
#include "core/host_runtime/guest_kernel_time.h"
#include "core/libraries/sysmodule/sysmodule_internal.h"
#include "core/host_runtime/guest_video_event_hle.h"
#include "core/host_runtime/guest_mouse.h"
#include "core/host_runtime/guest_mutex.h"
#include "core/host_runtime/guest_module_lifecycle.h"
#include "core/host_runtime/guest_module_policy.h"
#include "core/host_runtime/guest_playgo.h"
#include "core/host_runtime/guest_matching2.h"
#include "core/host_runtime/guest_trophy.h"
#include "core/file_format/trophy_support.h"
#include "core/host_runtime/guest_sync_arena.h"
#include "core/host_runtime/guest_sync_metrics.h"
#include "core/host_runtime/guest_network.h"
#include "core/host_runtime/guest_http.h"
#include "core/host_runtime/guest_http2.h"
#include "core/host_runtime/guest_http2_compat.h"
#include <openssl/sha.h>
#include "core/host_runtime/guest_np.h"
#include "core/host_runtime/guest_np_score.h"
#include "core/host_runtime/guest_np_tus.h"
#include "core/host_runtime/guest_np_utility.h"
#include "core/host_runtime/guest_np_webapi.h"
#include "core/host_runtime/guest_user_callbacks.h"
#include "core/host_runtime/guest_pad.h"
#include "core/host_runtime/guest_platform.h"
#include "core/host_runtime/guest_system_service.h"
#include "core/host_runtime/guest_video_mode.h"
#include "core/host_runtime/guest_hmd_geometry.h"
#include "core/host_runtime/guest_rtc.h"
#include "core/host_runtime/guest_runtime.h"
#include "core/host_runtime/guest_patch.h"
#include "core/diagnostics/executable_export.h"
#include "core/host_runtime/guest_auto_tag.h"
#include "core/host_runtime/guest_rwlock.h"
#include "core/host_runtime/guest_rwlock_diagnostics.h"
#include "core/host_runtime/guest_sync_abi.h"
#if __has_include("guest_sync_payload.h")
// Generated from guest/runtime/sync by build-guest-payload at host build time.
#include "guest_sync_payload.h"
#define SHADPS4_HAS_GUEST_SYNC_PAYLOAD 1
#endif
#include "core/host_runtime/guest_save_dialog.h"
#include "core/host_runtime/guest_semaphore.h"
#include "core/host_runtime/guest_storage_hle.h"
#include "core/host_runtime/guest_sysmodule_hle.h"
#include "core/host_runtime/guest_thread_attributes.h"
#include "core/host_runtime/guest_vr_session.h"
#include "core/host_runtime/guest_vr_sensor.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_error.h"
#include "core/libraries/hmd/hmd_setup_dialog.h"
#include "core/libraries/mouse/mouse_error.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/camera/camera.h"
#include "core/libraries/camera/camera_error.h"
#include "core/libraries/move/move.h"
#include "core/libraries/move/move_error.h"
#include "core/libraries/videoout/videoout_error.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/libraries/vr_tracker/vr_tracker_error.h"
#include "core/libraries/libs.h"
#include "core/libraries/network/ssl2.h"
#include "core/libraries/sysmodule/sysmodule_error.h"
#include "core/libraries/sysmodule/sysmodule_internal.h"
#include "core/libraries/system/systemservice.h"
#include "core/libraries/system/systemservice_error.h"
#include "core/libraries/system/userservice.h"
#include "core/libraries/system/userservice_error.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/tls.h"
#include "core/user_settings.h"
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace Core::HostRuntime {
using namespace GuestCpu;
using namespace GuestCpu::Hle;
namespace {
class PatchControl final : public GuestPatch::Control {
public:
    std::mutex mutex;
    std::function<std::string(const std::vector<std::string>&)> command;
    std::string Command(const std::vector<std::string>& args) override {
        std::scoped_lock lock(mutex);
        return command ? command(args) : "status: session_retired\n";
    }
    void Retire() { std::scoped_lock lock(mutex); command={}; }
};
void Require(Status status) {
    if (!status)
        throw std::runtime_error(Describe(status.GetError()));
}
void PrepareStage(std::string_view stage) {
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "ProductionRuntime", "%.*s",
                        static_cast<int>(stage.size()), stage.data());
#else
    (void)stage;
#endif
}
template <class T>
T Require(Result<T> result) {
    if (!result)
        throw std::runtime_error(Describe(result.GetError()));
    return std::move(result).Value();
}
void RestoreOfflineHttp2Imports(Module& module, GuestAddressSpace& space,
                                const QuiescenceToken& token) {
    auto* m = &module;
    for (const auto& segment : m->elf.GetProgramHeader()) {
        if (segment.p_type != PT_LOAD || segment.p_vaddr != 0 ||
            segment.p_flags != (PF_READ | PF_EXEC) ||
            segment.p_filesz != Http2CompatTextSize || segment.p_memsz != Http2CompatTextSize)
            continue;
        std::string digest;
        {
            auto text = Require(space.AcquireDataSpan(
                {GuestAddress{m->GetBaseAddress()}, segment.p_filesz}, false));
            std::array<u8, SHA256_DIGEST_LENGTH> hash{};
            SHA256(reinterpret_cast<const u8*>(text.Bytes().data()), text.Bytes().size(), hash.data());
            constexpr char hex[] = "0123456789abcdef";
            for (auto byte : hash) { digest += hex[byte >> 4]; digest += hex[byte & 15]; }
        }
        if (digest != Http2CompatTextSha)
            continue;
        std::vector<Http2ImportBinding> bindings;
        for (const auto& expected : Http2CompatBindings) {
            m->ForEachRelocation([&](const elf_relocation* rel, u32, bool jump) {
                if (rel->rel_offset != expected.got)
                    return;
                const auto& d = m->dynamic_info;
                if (!jump || rel->GetType() != R_X86_64_JUMP_SLOT || rel->rel_addend != 0 ||
                    rel->GetSymbol() >= d.symbol_table_total_size / sizeof(elf_symbol))
                    throw std::runtime_error("HTTP2 content compatibility relocation mismatch");
                const auto& sym = d.symbol_table[rel->GetSymbol()];
                if (sym.st_name >= d.str_table_size || sym.GetType() != STT_FUN || sym.st_shndx != 0)
                    throw std::runtime_error("HTTP2 content compatibility symbol mismatch");
                const char* name = d.str_table + sym.st_name;
                const auto* end = static_cast<const char*>(std::memchr(name, 0, d.str_table_size - sym.st_name));
                if (!end)
                    throw std::runtime_error("HTTP2 content compatibility unterminated import");
                const std::string_view full{name, size_t(end - name)};
                const auto first = full.find('#'), second = full.find('#', first == full.npos ? full.size() : first + 1);
                if (first == full.npos || second == full.npos || full.substr(0, first) != expected.nid)
                    throw std::runtime_error("HTTP2 content compatibility NID mismatch");
                const auto* library = m->FindLibrary(full.substr(first + 1, second - first - 1));
                const auto* module = m->FindModule(full.substr(second + 1));
                if (!library || !module || library->name != "libSceHttp2" || library->version != 1 || module->name != "libSceHttp2")
                    throw std::runtime_error("HTTP2 content compatibility provider mismatch");
                bindings.push_back(expected);
            });
        }
        const GuestRange patch_range{GuestAddress{m->GetBaseAddress() + Http2CompatStart}, Http2CompatSize};
        std::array<u8, Http2CompatSize> code{};
        Require(space.ReadData(patch_range.base, std::as_writable_bytes(std::span{code})));
        RestoreHttp2ImportBytes(digest, code, bindings);
        // Prepare owns quiescence: use its publication token, never request
        // an ordinary writable pin that correctly excludes token writers.
        Require(space.PublishCode(token, patch_range, std::as_bytes(std::span{code})));
        LOG_WARNING(Core_Linker, "Content compatibility CUSA50828/01.08 text-sha256={}: restored {} HTTP2 import jumps to normal HLE; source file unchanged", digest, bindings.size());
    }
}
struct FunctionAdapter final : HleCallAdapter {
    std::string profile_name{"HLE.Unknown"};
    bool profile_poll{};
    bool profile_sync{}, profile_sync_mutex{};
    std::shared_ptr<SyncMetrics::Session> sync_metrics;
    SyncMetrics::Operation sync_operation{SyncMetrics::Operation::None};
    std::function<Status(HleCallFrame&)> call;
    explicit FunctionAdapter(std::function<Status(HleCallFrame&)> f) : call(std::move(f)) {}
    Status Invoke(HleCallFrame& frame) const override {
        Common::Profiler::Scope profile{profile_name.c_str()};
        bool sync_sample = profile_sync && Common::Profiler::Enabled();
        if (sync_sample && profile_sync_mutex) {
            // Independent physical-owner/operation streams, deliberately a
            // non-power-of-two stride. Locator samples are not frequency estimates.
            static thread_local std::map<u64, u32> samples;
            sync_sample = (samples[frame.operation]++ % 4093) == 0;
        }
        if (sync_sample) {
            if (const auto* scope = HleScope::Current()) {
                Common::Profiler::Counter("GuestSync.Thread", scope->Thread().id);
                Common::Profiler::Counter("GuestSync.ThreadGeneration", scope->Thread().generation);
                Common::Profiler::Counter("GuestSync.Invocation", scope->InvocationId());
            }
            Common::Profiler::Counter("GuestSync.Operation", frame.operation);
            Common::Profiler::Counter("GuestSync.Arg0", frame.registers.Get(Gpr::Rdi));
            Common::Profiler::Counter("GuestSync.Arg1", frame.registers.Get(Gpr::Rsi));
            Common::Profiler::Counter("GuestSync.Arg2", frame.registers.Get(Gpr::Rdx));
            Common::Profiler::Counter("GuestSync.Arg3", frame.registers.Get(Gpr::Rcx));
            u64 caller{};
            if (frame.space &&
                frame.space->ReadData(GuestAddress{frame.registers.Get(Gpr::Rsp)},
                                      std::as_writable_bytes(std::span{&caller, 1}))) {
                Common::Profiler::Counter("GuestSync.Caller", caller);
                auto fp = frame.registers.Get(Gpr::Rbp);
                for (const char* name : {"GuestSync.Parent0", "GuestSync.Parent1",
                                        "GuestSync.Parent2", "GuestSync.Parent3",
                                        "GuestSync.Parent4", "GuestSync.Parent5"}) {
                    std::array<u64, 2> record{};
                    if (!fp || (fp & 7) ||
                        !frame.space->ReadData(GuestAddress{fp},
                                               std::as_writable_bytes(std::span{record})))
                        break;
                    if (!frame.space->ValidateRange({GuestAddress{record[1]}, 1},
                                                    GuestPermission::Execute)) break;
                    Common::Profiler::Counter(name, record[1]);
                    if (record[0] <= fp || record[0] - fp > 16 * 1024 * 1024) break;
                    fp = record[0];
                }
            }
        }
        // Explicit clock/sleep diagnostics, not JIT probes. Sample a real HLE
        // boundary without stopping ART or guessing guest state from an async
        // native stack. Diagnostic reads never change the call's outcome.
        if (profile_poll && Common::Profiler::Enabled()) {
            static thread_local u32 samples{};
            if ((samples++ & 63) == 0) {
                u64 caller{};
                const auto rsp = frame.registers.Get(Gpr::Rsp);
                if (frame.space &&
                    frame.space->ReadData(GuestAddress{rsp},
                                          std::as_writable_bytes(std::span{&caller, 1}))) {
                    Common::Profiler::Counter("GuestPoll.Operation", frame.operation);
                    Common::Profiler::Counter("GuestPoll.Caller", caller);
                    Common::Profiler::Counter("GuestPoll.Arg0", frame.registers.Get(Gpr::Rdi));
                    Common::Profiler::Counter("GuestPoll.Rbp", frame.registers.Get(Gpr::Rbp));
                    Common::Profiler::Counter("GuestPoll.R14", frame.registers.Get(Gpr::R14));
                    auto fp = frame.registers.Get(Gpr::Rbp);
                    for (const auto* name : {"GuestPoll.Parent", "GuestPoll.Grandparent"}) {
                        std::array<u64, 2> record{};
                        if (!fp || !frame.space->ReadData(
                                       GuestAddress{fp}, std::as_writable_bytes(std::span{record})))
                            break;
                        Common::Profiler::Counter(name, record[1]);
                        if (record[0] <= fp || record[0] - fp > 16 * 1024 * 1024)
                            break;
                        fp = record[0];
                    }
                }
            }
        }
#if defined(__ANDROID__)
        static std::atomic<unsigned> traces{};
        // Once the startup sample is exhausted, avoid a shared atomic RMW on
        // every HLE call (and eventual wraparound re-enabling startup logging).
        const bool trace = traces.load(std::memory_order_relaxed) < 64 &&
                           traces.fetch_add(1, std::memory_order_relaxed) < 64;
        if (trace)
            __android_log_print(ANDROID_LOG_INFO, "ProductionHLE", "enter op=%llu rip=%llx",
                                (unsigned long long)frame.operation,
                                (unsigned long long)frame.registers.rip);
#endif
        std::optional<SyncMetrics::Call> sync_call;
        if (sync_metrics && sync_metrics->Enabled()) {
            const auto* owner = HleScope::Current();
            sync_call.emplace(sync_metrics.get(), sync_operation, frame.operation,
                frame.registers.Get(Gpr::Rdi), owner ? owner->Thread().id : 0,
                owner ? owner->Thread().generation : 0);
        }
        Status status;
        try {
            status = call(frame);
        } catch (const std::exception& e) {
            LOG_ERROR(Core, "Production HLE operation {} failed: {}", frame.operation, e.what());
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_ERROR, "ProductionHLE", "failed op=%llu: %s",
                                (unsigned long long)frame.operation, e.what());
#endif
            throw;
        }
        if (sync_call) sync_call->Finish(bool(status), frame.registers.Get(Gpr::Rax));
        if (sync_sample) Common::Profiler::Counter("GuestSync.Result", frame.registers.Get(Gpr::Rax));
#if defined(__ANDROID__)
        if (trace)
            __android_log_print(ANDROID_LOG_INFO, "ProductionHLE", "leave op=%llu rax=%llx",
                                (unsigned long long)frame.operation,
                                (unsigned long long)frame.registers.Get(Gpr::Rax));
#endif
        return status;
    }
    bool SignatureSupported() const noexcept override {
        return true;
    }
    std::string SignatureDescription() const override {
        return "explicit production boundary";
    }
};
// Module::Start has a legacy integer return type. Carry a stopped guest call
// through that synchronous host wrapper without converting Cancel/Fault to a
// generic host exception or continuing into the main executable.
struct GuestCallbackStop {
    GuestCallResult result;
};
thread_local void* active_runtime{};
thread_local std::uint64_t active_thread{};
GuestPermission Permission(u32 prot) {
    return static_cast<GuestPermission>(prot & 7);
}
} // namespace

struct GuestRuntime::Impl final : GuestMemoryBackend {
    CpuContext& cpu;
    GuestAddressSpace& space;
    HleCallRegistry& registry;
    std::unique_ptr<GuestPatch::Manager> guest_patch;
    std::shared_ptr<GuestAutoTag::Profile> auto_tag;
    std::shared_ptr<PatchControl> patch_control;
    std::vector<GuestRange> patch_reservations;
    GuestClock clock;
    std::unique_ptr<GuestStorage> storage;
    std::unique_ptr<GuestAio> aio;
    std::shared_ptr<GuestDescriptorIds> descriptor_ids = std::make_shared<GuestDescriptorIds>();
    std::unique_ptr<GuestNetwork> network;
    std::unique_ptr<GuestHttp2> http2;
    std::unique_ptr<GuestHttp> http;
    std::unique_ptr<GuestPlayGo> playgo;
    std::unique_ptr<GuestMatching2Offline> matching2;
    std::unique_ptr<GuestTrophy> trophy;
    std::unique_ptr<GuestNpOffline> np;
    std::unique_ptr<GuestNpUtility> np_utility;
    std::unique_ptr<GuestNpWebApiControl> np_webapi;
    std::unique_ptr<GuestUserCallbacks> user_callbacks;
    bool np_offline{};
    bool profile_sync{};
    std::shared_ptr<SyncMetrics::Session> sync_metrics;
    std::unique_ptr<GuestAjm> ajm;
    std::unique_ptr<GuestAvPlayer> avplayer;
    std::unique_ptr<GuestAudio> audio;
    std::unique_ptr<GuestAudio3d> audio3d;
    std::unique_ptr<GuestKernelSemaphore> kernel_semaphores;
    std::unique_ptr<GuestPad> pad;
    std::shared_ptr<GuestSaveDialog> save_dialog;
    std::unique_ptr<GuestImeDialog> ime_dialog;
    GuestImeKeyboard ime_keyboard;
    GuestErrorDialog error_dialog{EmulatorSettings.AreGuestDialogsSilent()};
    std::unique_ptr<GuestSigninDialog> signin_dialog;
    std::shared_ptr<GuestMsgDialog> msg_dialog;
    std::unique_ptr<GuestCommerceDialog> commerce_dialog;
    std::unique_ptr<GuestSslOffline> ssl;
    std::shared_ptr<Frontend::Window> graphics_window;
    std::shared_ptr<const Vulkan::Driver> graphics_driver;
    std::unique_ptr<GuestGraphics> graphics;
    // Android/session-owned PSVR lifecycle state.  It records the guest
    // lifecycle independently from the desktop HLE's process globals; no
    // provider or pose data is implied by this object.
    std::unique_ptr<GuestVrSession> vr_session;
    std::unique_ptr<GuestReprojection> reprojection;
    std::atomic<u64> reprojection_calls{};
    bool hmd_diagnostics{};
    std::atomic<u64> hmd_diagnostic_frames{};
    bool network_diagnostics{};
    std::atomic<u64> network_diagnostic_calls{};
    bool rwlock_diagnostics{};
    u64 rwlock_diagnostic_slot{};
    // CPU VM permissions and GPU watch permissions are separate. Signal-time
    // watch retirement must not take the VM transaction lock or retire FEX code.
    GuestPageStates gpu_pages;
    u64 video_labels{};
    std::mutex graphics_init_mutex;
    std::mutex graphics_mutex;

    GuestGraphics& Graphics() {
        std::scoped_lock creation(graphics_init_mutex);
        if (!graphics) {
            if (!graphics_window || !graphics_driver)
                throw std::runtime_error("VideoOut requires a session Surface and verified Turnip");
            if (cancelling)
                throw std::runtime_error("Graphics initialization cancelled");
            if (!video_labels) {
                video_labels = Allocate(0x4000, "VideoOutLabelsAndShaders");
                for (u32 i = 0; i < 3; ++i)
                    Require(
                        space.WriteData(GuestAddress{video_labels + (i + 1) * 4096},
                                        std::as_bytes(Libraries::GnmDriver::GetEmbeddedShader(i))));
            }
            auto created = std::make_unique<GuestGraphics>(
                graphics_window, graphics_driver,
                [this] { return clock.ticks.GetTimeUS(clock.origin); },
                [this] { return clock.ReadTsc(); },
                [this] { return platform->SplashVisible(); }, video_labels,
                [this] { (void)Cancel(); });
            std::scoped_lock lock(graphics_mutex);
            if (cancelling)
                created->RequestStop();
            graphics = std::move(created);
            sysmodules.Publish("libSceVideoOut", 0x10000004);
        }
        graphics->CheckHealth();
        return *graphics;
    }

    std::unique_ptr<GuestThreadAttributeDomain> thread_attributes;
    std::unique_ptr<GuestMutexDomain> mutex_domain;
    std::unique_ptr<GuestSyncArena> sync_arena;
    std::unique_ptr<GuestSyncArena> rwlock_arena;
    std::unique_ptr<GuestRwlockDomain> rwlock_domain;
    std::unique_ptr<GuestSemaphoreDomain> semaphore_domain;
    int backing_fd{-1};
    u8* backing{};
    static constexpr u64 BackingSize = 12ULL << 30;
    std::unique_ptr<MemoryManager> memory;
    std::unique_ptr<Common::Singleton<MemoryManager>::Binding> memory_binding;
    std::unique_ptr<Linker> linker;
    std::unique_ptr<Common::Singleton<Linker>::Binding> linker_binding;
    Common::ElfInfo elf_info;
    std::unique_ptr<Common::Singleton<Common::ElfInfo>::Binding> elf_binding;
    FileSys::MntPoints mounts;
    std::shared_ptr<const Diagnostics::ExecutableExport::Source> executable_export_source;
    std::unique_ptr<GuestAppContent> app_content;
    std::unique_ptr<Common::Singleton<FileSys::MntPoints>::Binding> mount_binding;
    std::map<std::string, u64> veneers;
    std::map<u64, std::string> operation_names;
    // App-shipped guest synchronization fast path (guest/runtime/sync). Installed
    // lazily on the first import resolution, before any guest thread exists;
    // `debug.shadps4.sync_fastpath=0` keeps every mutex entry on the HLE path.
    bool sync_fastpath_enabled{true}, sync_fastpath_installed{};
    u64 sync_fastpath_base{}, sync_fastpath_size{};
    std::string sync_fastpath_status{"not_installed"};
    std::atomic<unsigned> sync_arena_outside{};
    void InstallSyncFastPath();
    u64 stack_guard{}, progname_object{}, environ_object{}, heap_trace{};
    std::once_flag heap_trace_once;
    // Guest PCs copied from the libc heap table. Never install them in the
    // desktop Linker's native function-pointer HeapAPI.
    std::array<u64, sizeof(HeapAPI) / sizeof(u64)> guest_heap_api{};
    std::string program_name;
    std::map<std::string, std::function<Status(HleCallFrame&)>> handlers;
    std::vector<std::string> refused;
    // Every import gets an explicit migration status.  A desktop HLE adapter is
    // useful coverage, but it is not evidence that the Android host has a
    // device/provider implementation (many desktop functions are intentional
    // stubs or still need pointer/callback policy).
    std::map<std::string, std::string> hle_status;
    std::set<std::string> graphics_gnm_nids, graphics_video_nids;
    std::atomic<bool> cancelling{};
    mutable std::mutex threads_mutex;
    std::condition_variable threads_changed;
    struct SpecificValue {
        u64 sequence{}, value{};
    };
    struct SpecificKey {
        bool allocated{};
        u64 sequence{}, destructor{};
    };
    std::array<SpecificKey, 256> specific_keys{};
    // libkernel's libc destructor hook is per runtime; its value is a guest PC.
    std::atomic<u64> thread_dtors{};
    // Optional libc reporting callbacks. Desktop leaves these unused; retain
    // their full guest addresses without installing native function pointers.
    std::array<std::atomic<u64>, 2> thread_atexit_reports{};
    struct Owner {
        u64 id{}, stack{}, stack_size{}, tls{}, tls_size{}, tcb{}, dtv{}, handle_va{};
        GuestThreadAttributes attributes{};
        std::string name;
        int startup_error{};
        bool startup_done{};
        std::array<SpecificValue, 256> specific{};
        ThreadHandle handle{};
        std::thread worker;
        std::optional<GuestCallResult> result;
        std::optional<Error> error;
        bool started{}, finished{}, joined{}, detached{}, destructors_started{};
        bool cancel_enabled{true};
    };
    std::map<u64, std::shared_ptr<Owner>> owners;
    u64 next_id{1};
    using EventFlag = Libraries::Kernel::EventFlagState;
    std::mutex event_flags_mutex;
    std::map<u64, std::shared_ptr<EventFlag>> event_flags;
    u64 next_event_flag{1};
    std::optional<GuestCallResult> child_fault;
    std::optional<Error> child_error;
    bool prepared{}, has_system_libc{};
    std::vector<u32> init_order;
    GuestModuleLifecycle module_lifecycle;
    std::set<u32> deferred_modules;
    std::vector<std::string> hle_modules;
    std::mutex once_mutex;
    std::map<u64, u64> once_owners;
    std::unique_ptr<GuestPlatform> platform;
    std::unique_ptr<GuestCamera> camera;
    GuestSysmodules sysmodules{[](u32 id) -> std::optional<std::string> {
        const char* name{};
        if (!Libraries::SysModule::LookupSysmodule(id, &name, nullptr))
            return std::nullopt;
        return name;
    }};

    struct CodePublication {
        Impl& rt;
        std::unique_lock<std::recursive_mutex> mapping_owner;
        std::optional<QuiescenceToken> token;
        CodePublication* previous{};
        static CodePublication*& Current() {
            static thread_local CodePublication* current{};
            return current;
        }
        explicit CodePublication(Impl& runtime, bool mapping_already_owned = false) : rt(runtime) {
            previous = Current();
            if (previous && &previous->rt == &rt)
                return;
            if (rt.memory && !mapping_already_owned)
                mapping_owner = rt.memory->SerializeGuestCodePublication();
            Common::Profiler::Scope pause("VM.QuiesceAllOwners");
            token.emplace(Require(rt.cpu.QuiesceContext(2'000'000'000)));
            Current() = this;
        }
        ~CodePublication() {
            if (token) {
                token.reset();
                Current() = previous;
            }
        }
    };
    QuiescenceToken* CodeToken() {
        auto* publication = CodePublication::Current();
        return publication && &publication->rt == this ? &*publication->token : nullptr;
    }
    Impl(CpuContext& c, GuestAddressSpace& s, HleCallRegistry& r) : cpu(c), space(s), registry(r) {
        vr_session = std::make_unique<GuestVrSession>();
        reprojection = std::make_unique<GuestReprojection>(space, GuestReprojection::Backend{
            .validate_display = [this](s32 v, s32 l, s32 r) { return Graphics().ValidateVrDisplay(v, l, r); },
            .event_exists = [this](s64 queue, u32 id) {
                auto* q = Graphics().FindEqueue(queue);
                return q && q->EventExists(id, Libraries::Kernel::OrbisKernelEvent::Filter::User);
            },
            .trigger = [this](s64 queue, u32 id) {
                // The graphics owner keeps queue storage until VideoOut/GPU workers join.
                if (auto* q = graphics->FindEqueue(queue))
                    q->TriggerEvent(id, Libraries::Kernel::OrbisKernelEvent::Filter::User, nullptr);
            },
            .set_cadence = [this](std::function<void()> callback) {
                Graphics().VideoOut().SetVrCadence(std::move(callback));
            },
            .set_active = [this](bool active) { Graphics().VideoOut().SetVrActive(active); },
            .submit = [this](ReprojectionFrame frame, std::function<void(bool)> complete) {
                if (hmd_diagnostics && HmdDiagnostics::Sample(++hmd_diagnostic_frames)) {
                    try {
                        LOG_INFO(Lib_Hmd, "HMD_TRACE {}", HmdDiagnostics::Json{
                            {"context", cpu.ContextId()}, {"pid", getpid()}, {"phase", "submit"},
                            {"sequence", frame.sequence}, {"display_index", frame.display_index},
                            {"label", frame.completion_label}, {"eyes", HmdDiagnostics::Frame(frame)}}.dump());
                    } catch (...) {} // diagnostics cannot change the submission contract
                }
                return Graphics().SubmitVrFrame(space, frame, std::move(complete));
            }});
        if (space.ReservationBase().value != ReservationBegin ||
            space.ReservationSize() != ReservationEnd - ReservationBegin)
            throw std::invalid_argument("production guest reservation does not match VM geometry");
        backing_fd = memfd_create("shadps4-guest-backing", MFD_CLOEXEC);
        if (backing_fd < 0 || ftruncate(backing_fd, BackingSize)) {
            if (backing_fd >= 0)
                close(backing_fd);
            throw std::runtime_error("cannot allocate guest backing fd");
        }
        backing = static_cast<u8*>(
            mmap(nullptr, BackingSize, PROT_READ | PROT_WRITE, MAP_SHARED, backing_fd, 0));
        if (backing == MAP_FAILED) {
            int saved = errno;
            close(backing_fd);
            throw std::runtime_error("guest backing mmap errno=" + std::to_string(saved));
        }
        const char* stage = "bind metadata/mounts";
        try {
            CodePublication vm(*this);
            elf_info.InitializeGuestMetadata({}, 0);
            elf_binding = std::make_unique<Common::Singleton<Common::ElfInfo>::Binding>(elf_info);
            stage = "bind mount table";
            mount_binding =
                std::make_unique<Common::Singleton<FileSys::MntPoints>::Binding>(mounts);
            stage = "construct MemoryManager";
            memory = std::make_unique<MemoryManager>(this);
            memory_binding = std::make_unique<Common::Singleton<MemoryManager>::Binding>(*memory);
            stage = "construct Linker";
            linker = std::make_unique<Linker>(*memory);
            linker_binding = std::make_unique<Common::Singleton<Linker>::Binding>(*linker);
            // Do not run the desktop registration bootstrap here.  RegisterLib
            // functions are not declarations: several construct process-wide
            // desktop drivers/workers while they register their symbols.  The
            // Android session binds only the explicitly audited handlers below;
            // the desktop catalogue is kept in the migration documents until a
            // guest ABI bridge has been implemented for a particular family.
            memory->guest_call = [this](u64 entry, u64 a, u64 b, u64 c) {
                GuestCallArgs args;
                args.values = {a, b, c};
                args.count = 3;
                auto result = Require(Call(entry, args));
                if (result.reason != StopReason::Returned)
                    throw GuestCallbackStop{std::move(result)};
                return static_cast<s32>(result.return_value);
            };
            linker->guest_hle_resolver = [this](const Loader::SymbolRecord& symbol) {
                return Bind(symbol);
            };
            linker->guest_data_resolver = [this](const Loader::SymbolRecord& symbol) {
                if (symbol.name == "djxxOmW6-aw#libkernel#1#libkernel#Object" ||
                    symbol.name == "+2thxYZ4syk#libkernel#1#libkernel#Object") {
                    const bool name = symbol.name.starts_with("djxxOmW6-aw#");
                    auto& object = name ? progname_object : environ_object;
                    if (!object) {

                        const auto address =
                            Allocate(0x4000, name ? "GuestProgname" : "GuestEnviron");
                        const u64 payload = address + sizeof(u64);
                        // Guest pointer object plus owned string / null-terminated
                        // empty environment. Never expose the host's environment.
                        std::memcpy(reinterpret_cast<void*>(address), &payload, sizeof(payload));
                        if (name) {
                            if (program_name.size() + 1 > 0x4000 - sizeof(u64))
                                throw std::runtime_error("guest program name exceeds bound");
                            std::memcpy(reinterpret_cast<void*>(payload), program_name.c_str(),
                                        program_name.size() + 1);
                        }
                        object = address;
                    }
                    return object;
                }
                if (symbol.name != "f7uOxY9mM1U#libkernel#1#libkernel#Object") {
                    // A deliberately narrow guest-libc compatibility policy. The
                    // game libc and libSceLibcInternal are distinct libraries; do
                    // not alias arbitrary functions or other libraries by NID.
                    // Functions use the separately audited guest_libc_policy surface.
                    // Reuse real guest-owned stream objects and the libc's real
                    // 4-byte dependency tag instead of fabricated zero objects.
                    const auto suffix = "#libSceLibcInternal#1#libSceLibcInternal#Object";
                    const auto nid = symbol.name.substr(0, symbol.name.find('#'));
                    const auto* known = AeroLib::FindByNid(nid.c_str());
                    const bool standard_stream =
                        known && (std::string_view(known->name) == "_Stdin" ||
                                  std::string_view(known->name) == "_Stdout" ||
                                  std::string_view(known->name) == "_Stderr");
                    if (!has_system_libc && symbol.name.substr(nid.size()) == suffix &&
                        (standard_stream || nid == "ZT4ODD2Ts9o")) {
                        Loader::SymbolResolver lookup;
                        lookup.name = nid == "ZT4ODD2Ts9o" ? "P330P3dFF68" : nid;
                        lookup.library = "libc";
                        lookup.module = "libc";
                        lookup.library_version = 1;
                        lookup.type = Loader::SymbolType::Object;
                        const Loader::SymbolRecord* provider{};
                        for (u32 id = 0; auto* m = linker->GetModule(id); ++id) {
                            if (const auto* candidate = m->export_sym.FindSymbol(lookup)) {
                                if (provider)
                                    throw std::runtime_error(
                                        "ambiguous guest libc object provider");
                                provider = candidate;
                            }
                        }
                        if (provider)
                            return provider->virtual_address;
                    }
                    throw std::runtime_error("unimplemented guest data policy: " + symbol.name);
                }
                if (!stack_guard) {

                    const auto address = Allocate(0x4000, "GuestStackGuard");
                    u64 value{};
                    do {
                        arc4random_buf(&value, sizeof(value));
                        value &= ~u64{0xff};
                    } while (!value);
                    // Fresh RW allocation belongs to this live VM token. The
                    // ordinary Write API deliberately refuses token mutations.
                    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
                    if (memory->Protect(address, 0x4000, MemoryProt::CpuRead))
                        throw std::runtime_error("stack guard publication failed");
                    stack_guard = address;
                }
                return stack_guard;
            };
            linker->guest_tls_resolver = [this](u64 module, u64 offset) {
                return reinterpret_cast<void*>(TlsAddress(module, offset));
            };
            // The runtime owns TCB allocation. Desktop InitializeTLS and heap
            // callbacks must never allocate a second, host-backed guest TCB.
            linker->guest_tls_allocate = [](u64) -> void* {
                throw std::logic_error("production TLS is allocated with its guest owner");
            };
            linker->guest_tls_free = [](void*) {
                throw std::logic_error("production TLS is released with its guest owner");
            };
#if defined(__ANDROID__)
            char sync_property[PROP_VALUE_MAX]{};
            __system_property_get("debug.shadps4.profile_sync", sync_property);
            profile_sync = std::string_view(sync_property) == "1";
            char fastpath_property[PROP_VALUE_MAX]{};
            __system_property_get("debug.shadps4.sync_fastpath", fastpath_property);
            sync_fastpath_enabled = std::string_view(fastpath_property) != "0";
            char hmd_property[PROP_VALUE_MAX]{};
            __system_property_get("debug.shadps4.hmd_log", hmd_property);
            hmd_diagnostics = std::string_view(hmd_property) == "1";
            char network_property[PROP_VALUE_MAX]{};
            __system_property_get("debug.shadps4.network_log", network_property);
            network_diagnostics = std::string_view(network_property) == "1";
            char rwlock_property[PROP_VALUE_MAX]{}, rwlock_slot_property[PROP_VALUE_MAX]{};
            __system_property_get("debug.shadps4.rwlock_log", rwlock_property);
            __system_property_get("debug.shadps4.rwlock_slot", rwlock_slot_property);
            const auto rwlock_slot = GuestRwlockDiagnostics::ParseSlot(rwlock_slot_property);
            rwlock_diagnostics = std::string_view(rwlock_property) == "1" && rwlock_slot.has_value();
            rwlock_diagnostic_slot = rwlock_slot.value_or(0);
            if (std::string_view(rwlock_property) == "1" && !rwlock_slot)
                LOG_WARNING(Core, "Invalid rwlock_slot; rwlock diagnostics disabled");
#else
            const char* rwlock_slot_value = std::getenv("SHADPS4_RWLOCK_SLOT");
            const auto rwlock_slot = GuestRwlockDiagnostics::ParseSlot(
                rwlock_slot_value ? rwlock_slot_value : "");
            if (const char* value = std::getenv("SHADPS4_RWLOCK_LOG"))
                rwlock_diagnostics = std::string_view(value) == "1" && rwlock_slot.has_value();
            rwlock_diagnostic_slot = rwlock_slot.value_or(0);
            if (const char* value = std::getenv("SHADPS4_NETWORK_LOG"))
                network_diagnostics = std::string_view(value) == "1";
            if (const char* value = std::getenv("SHADPS4_HMD_LOG"))
                hmd_diagnostics = std::string_view(value) == "1";
            if (const char* value = std::getenv("SHADPS4_SYNC_FASTPATH"))
                sync_fastpath_enabled = std::string_view(value) != "0";
#endif
            sync_metrics = std::make_shared<SyncMetrics::Session>(cpu.ContextId());
            SyncMetrics::SetControl(sync_metrics);
#if defined(__ANDROID__)
            char metrics_property[PROP_VALUE_MAX]{};
            __system_property_get("debug.shadps4.hle_sync", metrics_property);
            if (std::string_view(metrics_property) == "1" || std::string_view(metrics_property) == "detail")
                sync_metrics->Enable(true, std::string_view(metrics_property) == "detail");
#endif
            ime_dialog = std::make_unique<GuestImeDialog>(space);
            // Construct the bridge before recording handler closures so every
            // bound import captures a session-owned object.
            InstallHandlers();
        } catch (const std::exception& e) {
            munmap(backing, BackingSize);
            close(backing_fd);
            backing = nullptr;
            backing_fd = -1;
            throw std::runtime_error(std::string(stage) + ": " + e.what());
        }
    }
    ~Impl() {
        Diagnostics::ExecutableExport::Retire(executable_export_source);
        executable_export_source.reset();
        if (patch_control) patch_control->Retire();
        (void)Cancel();
        user_callbacks.reset();
        avplayer.reset(); // join native decode and owned FEX callback workers before VM teardown
        for (auto& [id, o] : owners)
            if (o->worker.joinable())
                o->worker.join();
        audio3d.reset();
        audio.reset(); // joins audio workers before clock/VM teardown
        ajm.reset(); // joins decoder worker before VM/backing teardown
        np.reset(); // desktop NP resources/callback thunks retire after guest owners join
        np_webapi.reset();
        http.reset(); // HTTP waiters joined above; retire desktop control resources.
        playgo.reset();
        matching2.reset();
        trophy.reset();
        aio.reset(); // join pinned file I/O before closing descriptors and guest VM
        storage.reset();
        app_content.reset();
        graphics.reset();
        graphics_window.reset();
        graphics_driver.reset();
        patch_reservations.clear();
        guest_patch.reset();
        linker_binding.reset();
        linker.reset();
        memory_binding.reset();
        memory.reset();
        if (backing)
            munmap(backing, BackingSize);
        if (backing_fd >= 0)
            close(backing_fd);
    }
    u8* BackingBase() const override {
        return backing;
    }
    bool OwnsRange(VAddr address, u64 size) const override {
        return space.OwnsRange({GuestAddress{address}, size});
    }
    boost::icl::interval_set<VAddr> UsableRegions() const override {
        boost::icl::interval_set<VAddr> regions;
        for (auto range : space.OwnedRanges())
            regions.add(boost::icl::interval<VAddr>::right_open(range.base.value, range.End()));
        return regions;
    }
    std::unique_ptr<GuestMemoryBackend::MappingPreparation> PrepareMapping(
        std::span<const GuestMemoryBackend::MappingRange> ranges, bool executable) override {
        if (CodeToken() || ranges.empty())
            return {};
        struct DataPreparation final : GuestMemoryBackend::MappingPreparation {
            std::unique_ptr<GuestAddressSpace::DataRetirement> retirement;
        };
        struct CodePreparation final : GuestMemoryBackend::MappingPreparation {
            explicit CodePreparation(Impl& rt) : publication(rt, true) {}
            CodePublication publication;
        };
        std::vector<GuestRange> guest_ranges;
        guest_ranges.reserve(ranges.size());
        for (const auto& [address, size] : ranges)
            guest_ranges.push_back({GuestAddress{address}, size});
        const auto* scope = HleScope::Current();
        Common::Profiler::Scope preparation("VM.RangeRetirement");
        auto result = space.PrepareDataMapping(
            guest_ranges, executable, scope ? scope->CancellationToken() : std::stop_token{});
        if (!result && result.GetError().category == ErrorCategory::Unsupported)
            return std::make_unique<CodePreparation>(*this);
        auto prepared = std::make_unique<DataPreparation>();
        prepared->retirement = Require(std::move(result));
        return prepared;
    }
    void Update(GuestAddressSpace::VmOperation op, u64 address, u64 size,
                GuestPermission permission, int fd = -1, u64 offset = 0, bool shared = true) {
        for (const auto& reserved : patch_reservations) {
            if (address < reserved.End() && reserved.base.value < address + size)
                throw std::runtime_error("VM mutation overlaps resident guest patch; restart without the package");
        }

        gpu_pages.Ensure(address, size);
        const GuestRange range{GuestAddress{address}, size};
        if (auto* token = CodeToken()) {
            Common::Profiler::Scope update("VM.FullCodeRetirement");
            Require(space.UpdateVmUnderToken(*token, op, range, permission, fd, offset, shared));
        } else {
            Common::Profiler::Scope update("VM.DataMapping");
            const auto* scope = HleScope::Current();
            auto result =
                space.UpdateDataMapping(op, range, permission, fd, offset,
                                        scope ? scope->CancellationToken() : std::stop_token{}, shared);
            if (!result && result.GetError().category == ErrorCategory::Unsupported) {
                // Only an actual executable mapping reaches this path. MemoryManager
                // already owns its mapping transaction; no HLE work shares this scope.
                CodePublication publication(*this, true);
                Require(space.UpdateVmUnderToken(*CodeToken(), op, range, permission, fd, offset, shared));
            } else
                Require(std::move(result));
        }
        const u8 state =
            op == GuestAddressSpace::VmOperation::Unmap ? 0 : 0x80 | static_cast<u8>(permission);
        for (u64 page = address / 4096; page < (address + size) / 4096; ++page) {
            const auto watches =
                op == GuestAddressSpace::VmOperation::Protect ? gpu_pages.At(page).load() & 24 : 0;
            gpu_pages.At(page).store(state | watches, std::memory_order_release);
            if (watches && ::mprotect(reinterpret_cast<void*>(page * 4096), 4096,
                                      (state & 7) & ~(watches >> 3)) != 0)
                throw std::runtime_error("VM protection could not preserve GPU tracking");
        }
    }
    void* Map(VAddr address, u64 size, PAddr physical, bool executable) override {
        auto perm = GuestPermission::Read | GuestPermission::Write;
        if (executable)
            perm = perm | GuestPermission::Execute;
        if (physical != PAddr(-1) && (physical > BackingSize || size > BackingSize - physical))
            throw std::runtime_error("direct/flexible backing overflow");
        Update(GuestAddressSpace::VmOperation::Map, address, size, perm,
               physical == PAddr(-1) ? -1 : backing_fd, physical == PAddr(-1) ? 0 : physical);
        return reinterpret_cast<void*>(address);
    }
    void* MapFile(VAddr address, u64 size, u64 offset, u32 prot, uintptr_t fd, bool shared) override {
        Update(GuestAddressSpace::VmOperation::Map, address, size, Permission(prot),
               static_cast<int>(fd), fd == uintptr_t(-1) ? 0 : offset, shared);
        return reinterpret_cast<void*>(address);
    }
    void Unmap(VAddr address, u64 size) override {
        Update(GuestAddressSpace::VmOperation::Unmap, address, size, GuestPermission::None);
    }
    void Protect(VAddr address, u64 size, MemoryPermission permission) override {
        Update(GuestAddressSpace::VmOperation::Protect, address, size,
               Permission(static_cast<u32>(permission)));
    }
    bool IsGpuWatchFault(VAddr address, bool write) const override {
        if (address >= ReservationEnd)
            return false;
        const u8 state = gpu_pages.Load(address);
        const u8 access = write ? 2 : 1;
        return (state & 0x80) && (state & access) && (state & ((access << 3) | (access << 5)));
    }
    void ProtectGpu(VAddr address, u64 size, MemoryPermission permission) override {
        if (!space.OwnsRange({GuestAddress{address}, size}) || (address | size) % 4096)
            throw std::runtime_error("invalid GPU watch range");
        const u8 watch = (~static_cast<u8>(permission) & 3) << 3;
        for (u64 page = address / 4096; page < (address + size) / 4096; ++page) {
            const auto old = gpu_pages.At(page).load(std::memory_order_acquire);
            // Never let GPU tracking alter executable code or revive an unmapped page.
            if (!(old & 0x80) || (old & 4))
                throw std::runtime_error(fmt::format(
                    "GPU watch requires mapped non-executable memory: range={:#x}+{:#x} "
                    "page={:#x} state={:#x} permission={:#x}",
                    address, size, page * 4096, old, static_cast<u32>(permission)));
            // Publish before protection; retain a retired-watch bit until the
            // next VM mutation for faults already pending on another owner.
            const u8 retired = ((old & 24) & ~watch) << 2;
            gpu_pages.At(page).store((old & ~u8(24)) | watch | retired, std::memory_order_release);
            const int prot = (old & 3) & static_cast<u8>(permission);
            if (::mprotect(reinterpret_cast<void*>(page * 4096), 4096, prot) != 0)
                throw std::runtime_error("GPU watch mprotect failed");
        }
    }
    u64 Allocate(u64 size, std::string_view name, u64 base = GuestRuntime::ServiceAllocationBase,
                 VMAType type = VMAType::File) {
        void* address{};
        auto result = memory->MapMemory(&address, base, Common::AlignUp(size, 0x4000ULL),
                                        MemoryProt::CpuReadWrite, MemoryMapFlags::NoFlags,
                                        type, std::string(name));
        if (result)
            throw std::runtime_error("guest allocation failed: " + std::string(name));
        return reinterpret_cast<u64>(address);
    }
    template <class T>
    T Read(u64 address) {
        T result{};
        Require(
            space.ReadData(GuestAddress{address}, std::as_writable_bytes(std::span{&result, 1})));
        return result;
    }
    template <class T>
    void Write(u64 address, const T& value) {
        // Retain only this output mapping; no runtime-wide execution gate.

        Require(space.WriteData(GuestAddress{address}, std::as_bytes(std::span{&value, 1})));
    }
    std::string String(u64 address, size_t limit = 4096) {
        std::string out;
        for (size_t i = 0; i < limit; ++i) {
            auto ch = Read<char>(address + i);
            if (!ch)
                return out;
            out += ch;
        }
        throw std::runtime_error("unterminated guest string");
    }
    std::shared_ptr<Owner> NewOwner(GuestThreadAttributes attributes = {}) {
        auto o = std::make_shared<Owner>();
        o->attributes = attributes;
        o->detached = attributes.detached;
        {
            std::lock_guard lock(threads_mutex);
            if (cancelling)
                throw std::runtime_error("session is stopping");
            o->id = next_id++;
            o->name = fmt::format("Guest-{}",o->id);
        }
        {

            o->stack_size = attributes.size;
            if (attributes.stack) {
                Require(space.ValidateRange({GuestAddress{attributes.stack}, attributes.size},
                                            GuestPermission::Read | GuestPermission::Write));
                o->stack = attributes.stack;
                o->attributes.guard = 0; // Caller owns neighboring memory; never protect it.
            } else {
                const u64 guard = Common::AlignUp(std::max<u64>(0x4000, attributes.guard), 0x4000ULL);
                const u64 allocation = Allocate(o->stack_size + guard, "GuestStack",
                                                GuestRuntime::ServiceAllocationBase, VMAType::Stack);
                if (memory->Protect(allocation, guard, MemoryProt::NoAccess))
                    throw std::runtime_error("guest stack guard failed");
                o->stack = allocation + guard; // Always the first usable guest byte.
                o->attributes.guard = guard;
            }
            o->attributes.stack = o->stack;
            o->tls_size =
                Common::AlignUp(linker->StaticTlsSize() + sizeof(Tcb) +
                                    (linker->MaxTlsIndex() + 2) * sizeof(DtvEntry) + 0x100,
                                0x4000ULL);
            o->tls = Allocate(o->tls_size, "GuestTls");
            o->tcb = o->tls + linker->StaticTlsSize();
            o->dtv = Common::AlignUp(o->tcb + sizeof(Tcb), 16ULL);
            o->handle_va = o->tls + o->tls_size - 64;
            auto tls_pin =
                Require(space.AcquireDataSpan({GuestAddress{o->tls}, o->tls_size}, true));
            auto* tcb = reinterpret_cast<Tcb*>(o->tcb);
            *tcb = {};
            tcb->tcb_self = tcb;
            tcb->tcb_dtv = reinterpret_cast<DtvEntry*>(o->dtv);
            tcb->tcb_thread = reinterpret_cast<void*>(o->handle_va);
            tcb->tcb_dtv[0].counter = linker->GenerationCounter();
            tcb->tcb_dtv[1].counter = linker->MaxTlsIndex();
            for (u32 i = 0;; ++i) {
                auto* m = linker->GetModule(i);
                if (!m)
                    break;
                if (!m->tls.image_size)
                    continue;
                auto* dest = reinterpret_cast<u8*>(o->tcb - m->tls.offset);
                if (m->tls.init_image_size)
                    Require(space.ReadData(GuestAddress{m->tls.image_virtual_addr},
                                           std::span<std::byte>(reinterpret_cast<std::byte*>(dest),
                                                                m->tls.init_image_size)));
                tcb->tcb_dtv[m->tls.modid + 1].pointer = dest;
            }
        }
        {
            std::lock_guard lock(threads_mutex);
            if (cancelling)
                throw std::runtime_error("session is stopping");
            owners.emplace(o->id, o);
        }
        return o;
    }
    std::shared_ptr<Owner> Current() {
        // Hot path for every HLE call that needs its owner (mutex, semaphore,
        // rwlock, pthread_self, errno). A thread-local weak reference keyed by
        // runtime and active thread avoids taking the process-wide threads
        // mutex on each call; it never extends the owner's lifetime and falls
        // back to the registry whenever the owner was replaced or released.
        struct Cache {
            const void* runtime{};
            u64 thread{};
            std::weak_ptr<Owner> owner;
        };
        static thread_local Cache cache;
        if (cache.runtime == this && cache.thread == active_thread && active_thread) {
            if (auto owner = cache.owner.lock())
                return owner;
        }
        std::lock_guard lock(threads_mutex);
        auto it = owners.find(active_thread);
        if (active_runtime != this || it == owners.end())
            throw std::logic_error("no production guest owner");
        cache = {this, active_thread, it->second};
        return it->second;
    }
    Result<GuestCallResult> Call(u64 entry, const GuestCallArgs& args) {
        Common::Profiler::Scope profile{"Guest.Invoke"};
        if (auto* scope = HleScope::Current())
            return scope->InvokeGuest(GuestCodeAddress{entry}, args);
        return cpu.InvokeGuest(Current()->handle, GuestCodeAddress{entry}, args, {});
    }
    void Attach(const std::shared_ptr<Owner>& o, u64 entry) {
        { std::lock_guard lock(threads_mutex); Common::SetCurrentThreadName(o->name.c_str()); }
        active_runtime = this;
        active_thread = o->id;
        SetTcbBase(reinterpret_cast<void*>(o->tcb));
        ThreadInit init;
        init.entry_rip = GuestCodeAddress{entry};
        init.initial_rsp = GuestAddress{Common::AlignDown(o->stack + o->stack_size, 16ULL) - 16};
        init.guest_tid = o->id;
        init.initial_state.fields = RegisterValidity::SegmentBases;
        init.initial_state.values.fs_base = o->tcb;
        ThreadHandle handle;
        {

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            for (;;) {
                auto created = cpu.CreateThread(init);
                if (created) {
                    handle = std::move(created).Value();
                    break;
                }
                if (created.GetError().category != ErrorCategory::Busy ||
                    std::chrono::steady_clock::now() >= deadline)
                    Require(std::move(created));
                (void)space.WaitForQuiescenceRelease(100'000'000);
            }
        }
        {
            std::lock_guard lock(threads_mutex);
            o->handle = handle;
            o->started = true;
            if (cancelling)
                Require(cpu.RequestInterrupt(handle, InterruptReason::Cancel));
        }
        if (profile_sync) {
            Common::Profiler::Counter("GuestSync.OwnerHandle", o->handle_va);
            Common::Profiler::Counter("GuestSync.OwnerThread", handle.id);
            Common::Profiler::Counter("GuestSync.OwnerGeneration", handle.generation);
            Common::Profiler::Counter("GuestSync.OwnerEntry", entry);
        }
        threads_changed.notify_all();
    }
    void Finish(const std::shared_ptr<Owner>& o) {
        // Guest destructors run through the same callback gate, without the key
        // lock or a guest pin. Values are cleared before invocation; a destructor
        // may repopulate them, up to the Orbis four-pass bound.
        if (o->result && o->result->reason == StopReason::Returned && !cancelling &&
            !o->destructors_started) {
            o->destructors_started = true;
            bool stopped{};
            // Desktop pthread_exit calls the libc hook before key destructors,
            // while this owner's TLS is still installed. Never call an x86 PC
            // natively, or keep a registry/VM lock across the callback.
            if (const u64 entry = thread_dtors.load(std::memory_order_acquire)) {
                auto result = Call(entry, {});
                if (!result) {
                    o->error = result.GetError();
                    stopped = true;
                } else if (result.Value().reason != StopReason::Returned) {
                    o->result = result.Value();
                    stopped = true;
                }
                if (stopped) (void)Cancel();
            }
            for (unsigned pass = 0; pass < 4 && !stopped; ++pass) {
                for (size_t i = 0; i < specific_keys.size(); ++i) {
                    u64 value{}, destructor{};
                    {
                        std::lock_guard lock(threads_mutex);
                        auto& slot = o->specific[i];
                        const auto& key = specific_keys[i];
                        if (key.allocated && key.sequence == slot.sequence) {
                            value = slot.value;
                            destructor = key.destructor;
                        }
                        slot.value = 0;
                    }
                    if (!value || !destructor)
                        continue;
                    GuestCallArgs args;
                    args.count = 1;
                    args.values[0] = value;
                    auto result = Call(destructor, args);
                    if (!result) {
                        o->error = result.GetError();
                        stopped = true;
                    } else if (result.Value().reason != StopReason::Returned) {
                        o->result = result.Value();
                        stopped = true;
                    }
                    if (stopped) {
                        (void)Cancel();
                        break;
                    }
                }
            }
        }

        if (o->id != 1 && (o->error || (o->result &&
            o->result->reason != StopReason::Returned && o->result->reason != StopReason::Cancelled))) {
            {
                std::lock_guard lock(threads_mutex);
                if (!child_fault && !child_error) {
                    if (o->error) child_error = o->error;
                    else child_fault = o->result;
                }
            }
            // A failed background owner cannot leave the main guest blocked on
            // its semaphore/condition forever. Preserve its exact fault first.
            (void)Cancel();
        }
        if (o->handle.IsValid()) {
            // A finished owner has no Run lease. A concurrent code publisher may
            // transiently close retirement; retry without retaining another lock.

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            for (;;) {
                auto destroyed = cpu.DestroyThread(o->handle);
                if (destroyed)
                    break;
                if (destroyed.GetError().category != ErrorCategory::Busy ||
                    std::chrono::steady_clock::now() >= deadline)
                    Require(std::move(destroyed));
                (void)space.WaitForQuiescenceRelease(100'000'000);
            }
        }
        SetTcbBase(nullptr);
        active_runtime = nullptr;
        active_thread = 0;
        {
            std::lock_guard lock(threads_mutex);
            o->handle = {};
            o->finished = true;
        }
        threads_changed.notify_all();
    }
    GuestCallbackOwner CallbackOwner(std::string label) {
        struct State {
            std::shared_ptr<Owner> owner;
            u64 scratch{};
            std::atomic_bool closing{};
        };
        auto state = std::make_shared<State>();
        return {
            .begin =
                [this, state, label](u64 entry) {
                    auto o = NewOwner();
                    {
                        std::lock_guard lock(threads_mutex);
                        state->owner = o;
                    }
                    {

                        state->scratch =
                            Allocate(GuestAvPlayer::ScratchSize, label);
                    }
                    Attach(state->owner, entry);
                    return state->scratch;
                },
            .call = [this, state](u64 entry, std::span<const u64> values) -> u64 {
                GuestCallArgs args;
                args.count = values.size();
                std::copy(values.begin(), values.end(), args.values.begin());
                auto result = Call(entry, args);
                if (!result) {
                    state->owner->error = result.GetError();
                    (void)Cancel();
                    throw std::runtime_error("AvPlayer guest callback failed");
                }
                state->owner->result = result.Value();
                if (result.Value().reason != StopReason::Returned) {
                    if (!state->closing || result.Value().reason != StopReason::Cancelled)
                        (void)Cancel();
                    throw std::runtime_error("AvPlayer guest callback stopped");
                }
                return result.Value().return_value;
            },
            .cancel =
                [this, state] {
                    state->closing = true;
                    ThreadHandle handle;
                    {
                        std::lock_guard lock(threads_mutex);
                        if (state->owner)
                            handle = state->owner->handle;
                    }
                    if (handle.IsValid())
                        (void)cpu.RequestInterrupt(handle, InterruptReason::Cancel);
                },
            .end =
                [this, state] {
                    auto o = state->owner;
                    if (!o)
                        return;
                    if (state->closing && !o->error &&
                        (!o->result || o->result->reason == StopReason::Returned))
                        o->result = GuestCallResult{.reason = StopReason::Cancelled};
                    try {
                        Finish(o);
                        // During global Stop, mappings still belong to Session VM.
                        // Keep them until GPU/decoder/guest workers are joined; a
                        // stopped renderer cannot admit another per-owner drain.
                        if (!cancelling) {
                            if ((state->scratch &&
                                 memory->UnmapMemory(state->scratch, GuestAvPlayer::ScratchSize)) ||
                                memory->UnmapMemory(o->tls, o->tls_size) ||
                                memory->UnmapMemory(o->stack - o->attributes.guard,
                                                    o->stack_size + o->attributes.guard))
                                throw std::runtime_error("AvPlayer callback data release failed");
                        }
                        {
                            std::lock_guard lock(threads_mutex);
                            owners.erase(o->id);
                        }
                    } catch (const std::exception& e) {
                        {
                            std::lock_guard lock(threads_mutex);
                            o->error = MakeError(ErrorCategory::BackendFailure, "AvPlayer::Finish",
                                                 e.what());
                            if (!child_error && !child_fault)
                                child_error = o->error;
                            o->finished =
                                true; // retain failed owner resources for session teardown
                        }
                        SetTcbBase(nullptr);
                        active_runtime = nullptr;
                        active_thread = 0;
                        (void)Cancel();
                        threads_changed.notify_all();
                    }
                },
            .invalidate = [this](u64 at, size_t bytes) { memory->InvalidateMemory(at, bytes); },
            .fail = [this](std::string_view message) {
                { std::lock_guard lock(threads_mutex);
                  if (!child_error && !child_fault)
                      child_error = MakeError(ErrorCategory::BackendFailure, "GuestCallback", std::string(message)); }
                (void)Cancel();
            },
        };
    }
    std::optional<Result<GuestCallResult>> ChildFailure() {
        std::lock_guard lock(threads_mutex);
        if (child_error) return Result<GuestCallResult>(*child_error);
        if (child_fault) return Result<GuestCallResult>(*child_fault);
        return {};
    }
    Status Cancel() {
        std::vector<ThreadHandle> handles;
        {
            std::lock_guard lock(threads_mutex);
            cancelling = true;
            for (auto& [id, o] : owners)
                if (o->handle.IsValid())
                    handles.push_back(o->handle);
        }
        if (user_callbacks) user_callbacks->RequestStop();
        if (avplayer)
            avplayer->RequestStop();
        if (audio3d) audio3d->RequestStop();
        if (audio) audio->RequestStop();
        if (ajm) ajm->RequestStop();
        if (network) network->RequestStop();
        if (aio) aio->RequestStop();
        if (storage)
            storage->Cancel();
        if (save_dialog)
            save_dialog->Cancel();
        if (ime_dialog)
            ime_dialog->Cancel();
        ime_keyboard.Cancel();
        error_dialog.Cancel();
        if (signin_dialog)
            signin_dialog->Cancel();
        if (msg_dialog)
            msg_dialog->Cancel();
        if (commerce_dialog)
            commerce_dialog->Cancel();
        Status result = Ok();
        for (auto handle : handles) {
            auto s = cpu.RequestInterrupt(handle, InterruptReason::Cancel);
            if (!s && s.GetError().category != ErrorCategory::InvalidHandle && result)
                result = s.GetError();
        }
        {
            std::scoped_lock lock(graphics_mutex);
            if (graphics)
                graphics->RequestStop();
        }
        threads_changed.notify_all();
        return result;
    }
    u64 Bind(const Loader::SymbolRecord& symbol) {
        if (!sync_fastpath_installed) {
            InstallSyncFastPath();
            SyncMetrics::SetFastPathStatus(sync_fastpath_status);
        }
        if (auto it = veneers.find(symbol.name); it != veneers.end())
            return it->second;
        const auto nid = symbol.name.substr(0, symbol.name.find('#'));
        if (!has_system_libc && IsGuestLibcFunction(nid) && symbol.name.substr(nid.size()) ==
            "#libSceLibcInternal#1#libSceLibcInternal#Function") {
            Loader::SymbolResolver lookup;
            lookup.name = nid; lookup.library = "libc"; lookup.module = "libc";
            lookup.library_version = 1; lookup.type = Loader::SymbolType::Function;
            const Loader::SymbolRecord* provider{};
            Module* provider_module{};
            for (u32 id = 0; auto* module = linker->GetModule(id); ++id) {
                if (const auto* candidate = module->export_sym.FindSymbol(lookup)) {
                    if (provider) throw std::runtime_error("ambiguous guest libc provider");
                    provider = candidate;
                    provider_module = module;
                }
            }
            if (provider) {
                // Relocations run while module pages are RW. Check the defining
                // ELF's executable segment, not permissions finalized afterward.
                const auto info = provider_module->GetModuleInfo();
                bool executable{};
                for (u32 i = 0; i < info.num_segments; ++i) {
                    const auto& segment = info.segments[i];
                    executable |= (segment.prot & PF_EXEC) &&
                        provider->virtual_address >= segment.address &&
                        provider->virtual_address - segment.address < segment.size;
                }
                if (!executable || !space.OwnsRange({GuestAddress{provider->virtual_address}, 1}))
                    throw std::runtime_error("guest libc provider is outside executable ELF segments");
                veneers.emplace(symbol.name, provider->virtual_address);
                return provider->virtual_address; // x86 va_list remains entirely in guest libc.
            }
        }

        // A desktop registration is only a declaration of a native callback.
        // It does not prove the guest ABI, pointer ownership, callback/TLS
        // bridge, or Android lifecycle.  Never adopt it implicitly here.
        // Families awaiting a bridge remain named refused imports in the audit.
        // Only the sysmodule NIDs this runtime implements are admitted under the
        // libSceSysmodule library suffix; every other sysmodule function still
        // name-faults so a missing capability is never silently a success.
        static const std::set<std::string> sysmodule_functions{
            "g8cM39EUZ6o", "39iV5E1HoCk", "eR2bZFAAU0Q", "fMP5NHUOaMk",
            "ynFKQ5bfGks", "D8cuU4d72xM", "hHrGoGoNf+s", "4fU5yvOkVG4"};
        // Guest-facing libSceUserService startup family; same policy as above.
        static const std::set<std::string> userservice_functions{
            "j3YMu1MVNNo", "bwFjS+bX9mA", "CdWp0oHWGr0", "eNb53LQJmIM",
            "fPhymKNvK-A", "1xxcMiGu2fo", "yH17Q6NWtVg", "lUoqwTQu4Go",
            "wuI7c7UNk0A", "spW--yoLQ9o"};
        // Guest-facing libSceSystemService startup family; same policy as above.
        static const std::set<std::string> systemservice_functions{"fZo48un7LK4", "rPo6tV8D9bM",
                                                                   "656LMQSrg6U", "Vo5V8KAwCmk",
                                                                   DisplaySafeAreaNid};
        // Explicit checked GPU policies plus retail owner registration; unknown
        // GNM entry points still produce a named unsupported-import fault.
        auto gnmdriver_functions = graphics_gnm_nids;
        gnmdriver_functions.insert("ZFqKFl23aMc");
        std::set<std::string> videoout_functions{"Up36PTk687E", "6kPnj51T62Y", "8XGijEoThE0",
                                                 "i6-sR91Wt-4", "1FZBKy8HeNU", "d1AjT2uZJn0",
                                                 "kGVLc3htQE8", "pjkDsgxli6c"};
        videoout_functions.insert("N1bEoJ4SRw4");
        videoout_functions.insert("ktP9j1fN-zE");
        videoout_functions.insert(graphics_video_nids.begin(), graphics_video_nids.end());
        for (const auto getter : VideoEventGetterNids) videoout_functions.emplace(getter);
        // These are the guest-safe PSVR entry points implemented below.  The
        // set intentionally excludes multilayer/callback/opaque records whose
        // ABI is not recovered yet; those remain named refused imports.
        static const std::set<std::string> hmd_functions{
            "K4KnH0QkT2c", "s-J66ar9g50", "d2g5Ij7EUzo", "6biw1XHTSqQ",
            "thDt9upZlp8", "1pxQfif1rkE", "NPQwYFqi0bs", "z-RMILqP6tE", HmdEyeOffsetNid,
            "OuygGEWkins", "ZrV5YIqD09I", "z0KtN1vqF2E", "kLUAkN6a1e8",
            "TkcANcGM0s8", "IWybWbR-xvA", "vzMEkwBQciM", "mdyFbaJj66M",
            "7as0CjXW1B8", "knyIhlkpLgE", "E+dPfjeQLHI", "iGNNpDDjcwo",
            "LjdLRysHU6Y", "8gH1aLgty5I", "94+Ggm38KCg",
            "dntZTJ7meIU", "q3e8+nEguyE", "kcldQ7zLYQQ"};
        // The desktop tree already provides the setup-dialog lifecycle.  The
        // Android adapter below selects the virtual SBS headset while copying
        // guest records through GuestAddressSpace; desktop keeps no-provider
        // cancellation semantics.
        static const std::set<std::string> hmd_setup_dialog_functions{
            "nmHzU4Gh0xs", "6lVRHMV5LY0", "NNgiV4T+akU", "NB1Y2kA2jCY",
            "+z4OJmFreZc", "Ud7j3+RDIBg", "J9eBpW1udl4"};
        static const std::set<std::string> vr_tracker_functions{
            "K7yhYrsIBPc", "QkRl7pART9M", "sIh8GwcevaQ", "TVegDMLaBB8",
            "gkGuO9dd57M", "ARhgpXvwoR0", "76OBvrrQXUc", "XoeWzXlrnMw",
            "EUCaQtXXYNI", "E0P0sN-wy+4", "9fvHMUbsom4", "Q8skQqEwn5c",
            "IBv4P3q1pQ0", "zvyKP0Z3UvU", "5IFOAYv-62g", "VItTwN8DmS8"};
        // Camera and Move use checked, session-enabled SBS adapters. Library
        // suffix checks still gate every import; unrelated entry points refuse.
        static const std::set<std::string> move_functions{
            "IFQwtT2CeY0", "HzC60MfjJxU", "j1ITE-EoJmE", "tsZi60H4ypY",
            "GWXTyxs4QbE", "f2bcpK6kJfg", "T8KYHPs1JE8"};
        const bool save_nid = std::any_of(std::begin(StorageEntries), std::end(StorageEntries),
                                          [&](const auto& e) { return e.save && e.nid == nid; });
        const bool dialog_nid = GuestSaveDialog::IsSaveNid(nid);
        const bool common_nid = GuestSaveDialog::IsCommonNid(nid);
        const bool ime_dialog_nid = GuestImeDialog::IsNid(nid);
        const bool np_nid = IsNpOfflineNid(nid);
        const bool ssl_nid = GuestSslOffline::IsNid(nid);
        const bool posix_net_nid = IsPosixNetworkNid(nid);
        const bool random_nid = nid == "PI7jIZj4pcE";
        const bool coredump_nid = std::ranges::find(CoredumpUnavailableNids, nid) != CoredumpUnavailableNids.end();
        const bool kernel_nid =
            !coredump_nid && !IsAudioInputNid(nid) && !IsLiveStreamingUnavailableNid(nid) && !FindRemoteService(nid) &&
            !random_nid && !posix_net_nid && !np_nid && !IsNpScoreOfflineNid(nid) && !IsNpTusOfflineNid(nid) && !IsNpUtilityNid(nid) && !IsNpWebApiControlNid(nid) && !ssl_nid && !IsMatching2Nid(nid) && !IsTrophyNid(nid) && !IsPlayGoNid(nid) && !IsHttpNid(nid) && !IsHttp2Nid(nid) && !IsAvPlayerNid(nid) && !IsAudioNid(nid) && !IsAudio3dNid(nid) && !IsAjmNid(nid) &&
            !IsPadNid(nid) && !IsMouseNid(nid) && !IsNetNid(nid) && !IsNetCtlNid(nid) && !IsAppContentNid(nid) &&
            !IsRtcNid(nid) && !IsDiscMapNid(nid) && !dialog_nid && !common_nid && !save_nid &&
            !videoout_functions.contains(nid) && !sysmodule_functions.contains(nid) &&
            !userservice_functions.contains(nid) && !systemservice_functions.contains(nid) &&
            !gnmdriver_functions.contains(nid) && !hmd_functions.contains(nid) &&
            !hmd_setup_dialog_functions.contains(nid) &&
            !vr_tracker_functions.contains(nid) && !IsCameraNid(nid) &&
            !move_functions.contains(nid) && !GuestImeKeyboard::IsNid(nid) && !GuestErrorDialog::IsNid(nid) && !ime_dialog_nid && !GuestSigninDialog::IsNid(nid) &&
            !GuestMsgDialog::IsNid(nid) && !GuestCommerceDialog::IsNid(nid) && nid != "NWtTN10cJzE";
        std::shared_ptr<HleCallAdapter> adapter;
        if (auto it = handlers.find(nid);
            it != handlers.end() &&
            ((avplayer && IsAvPlayerNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceAvPlayer#1#libSceAvPlayer#Function") ||
             (IsAudioInputNid(nid) && symbol.name.substr(nid.size()) == "#libSceAudioIn#1#libSceAudioIn#Function") ||
             AdmitsLiveStreamingUnavailable(nid, symbol.name.substr(nid.size())) ||
             AdmitsRemoteService(nid, symbol.name.substr(nid.size())) ||
             (audio && IsAudioNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceAudioOut#1#libSceAudioOut#Function") ||
             (audio3d && IsAudio3dNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceAudio3d#1#libSceAudio3d#Function") ||
             (ajm && IsAjmNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceAjm#1#libSceAjm#Function") ||
             (pad && IsPadNid(nid) &&
              symbol.name.substr(nid.size()) == "#libScePad#1#libScePad#Function") ||
             (IsMouseNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceMouse#1#libSceMouse#Function") ||
             (np && AdmitsNpOffline(nid, symbol.name.substr(nid.size()), np_offline)) ||
             AdmitsNpScoreOffline(nid, symbol.name.substr(nid.size()), np_offline) ||
             AdmitsNpTusOffline(nid, symbol.name.substr(nid.size()), np_offline) ||
             (np_webapi && np_offline && IsNpWebApiControlNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceNpWebApi#1#libSceNpWebApi#Function") ||
             (np_utility && np_offline && IsNpUtilityNid(nid) &&
              (symbol.name.substr(nid.size()) == "#libSceNpUtility#1#libSceNpUtility#Function" ||
               (nid == "r9BgI0PfJZg" && symbol.name.substr(nid.size()) == "#libSceNpUtilityCompat#1#libSceNpUtility#Function"))) ||
             (random_nid && symbol.name.substr(nid.size()) == "#libSceRandom#1#libSceRandom#Function") ||
             (network && posix_net_nid && (symbol.name.substr(nid.size()) == "#libScePosix#1#libkernel#Function" ||
                                          symbol.name.substr(nid.size()) == "#libkernel#1#libkernel#Function")) ||
             (ssl && ssl_nid && symbol.name.substr(nid.size()) == "#libSceSsl#1#libSceSsl#Function") ||
             (http && AdmitsHttp(nid, symbol.name.substr(nid.size()), true)) ||
             (trophy && IsTrophyNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceNpTrophy#1#libSceNpTrophy#Function") ||
             (matching2 && np_offline && IsMatching2Nid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceNpMatching2#1#libSceNpMatching2#Function") ||
             (playgo && IsPlayGoNid(nid) &&
              symbol.name.substr(nid.size()) == "#libScePlayGo#1#libScePlayGo#Function") ||
             (http2 && AdmitsHttp2(nid, symbol.name.substr(nid.size()), true)) ||
             (network && IsNetNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceNet#1#libSceNet#Function") ||
             (network && IsNetCtlNid(nid) &&
              (symbol.name.substr(nid.size()) == "#libSceNetCtl#1#libSceNetCtl#Function" ||
               ((nid == "u5oqtlIP+Fw" || nid == "wIsKy+TfeLs" || nid == "2oUqKR5odGc") &&
                symbol.name.substr(nid.size()) == "#libSceNetCtlForNpToolkit#1#libSceNetCtl#Function"))) ||
             (app_content && IsAppContentNid(nid) &&
              symbol.name.substr(nid.size()) ==
                  "#libSceAppContent#1#libSceAppContentUtil#Function") ||
             (IsRtcNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceRtc#1#libSceRtc#Function") ||
             (IsDiscMapNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceDiscMap#1#libSceDiscMap#Function") ||
             (coredump_nid && symbol.name.substr(nid.size()) == "#libSceCoredump#1#libkernel#Function") ||
             (nid == "f7KBOafysXo" && symbol.name.substr(nid.size()) == "#libkernel_psmkit#1#libkernel#Function") ||
             // IsProspero also has this desktop-registered platform export.
             // Keep the alias scoped to its NID, library version and module.
             (nid == "mpxAdqW7dKY" &&
              symbol.name.substr(nid.size()) == "#libkernel_cpumode_platform#1#libkernel#Function") ||
             (kernel_nid &&
              (symbol.name.substr(nid.size()) == "#libkernel#1#libkernel#Function" ||
               symbol.name.substr(nid.size()) == "#libScePosix#1#libkernel#Function")) ||
             ((nid == "4R6-OvI2cEA" || nid == "WDszmSbWuDk" || nid == "F6e0kwo4cnk" ||
               nid == "LJDwdSNTnDg") &&
              symbol.name.substr(nid.size()) == "#libkernel#1#libkernel#Function") ||
             ((nid == "WkwEd3N7w0Y" || nid == "Qhv5ARAoOEc" || nid == "il03nluKfMk") &&
              symbol.name.substr(nid.size()) == "#libkernel_unity#1#libkernel#Function") ||
             (nid == "Qs0wWulgl7U" &&
              symbol.name.substr(nid.size()) == "#libSceMouse#1#libSceMouse#Function") ||
             ((nid == SHAD_SYNC_NID_WAIT || nid == SHAD_SYNC_NID_WAKE) &&
              symbol.name.substr(nid.size()) == SHAD_SYNC_LIBRARY_SUFFIX) ||
             (save_dialog && dialog_nid &&
              symbol.name.substr(nid.size()) ==
                  "#libSceSaveDataDialog#1#libSceSaveDataDialog#Function") ||
             (save_dialog && common_nid &&
              symbol.name.substr(nid.size()) ==
                  "#libSceCommonDialog#1#libSceCommonDialog#Function") ||
             (save_nid &&
              symbol.name.substr(nid.size()) == "#libSceSaveData#1#libSceSaveData#Function") ||
             (nid == "QwOO7vegnV8" &&
              symbol.name.substr(nid.size()) == "#libSceSaveData#1#libSceSaveData#Function") ||
             (GuestImeKeyboard::Admits(nid, symbol.name.substr(nid.size()))) ||
             (GuestErrorDialog::IsNid(nid) && symbol.name.substr(nid.size()) == "#libSceErrorDialog#1#libSceErrorDialog#Function") ||
             (ime_dialog && ime_dialog_nid &&
              symbol.name.substr(nid.size()) == "#libSceImeDialog#1#libSceImeDialog#Function") ||
             (signin_dialog && GuestSigninDialog::IsNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceSigninDialog#1#libSceSigninDialog#Function") ||
             (msg_dialog && GuestMsgDialog::IsNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceMsgDialog#1#libSceMsgDialog#Function") ||
             (commerce_dialog && GuestCommerceDialog::IsNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceNpCommerce#1#libSceNpCommerce#Function") ||
             (symbol.name.substr(nid.size()) == "#libSceSysmodule#1#libSceSysmodule#Function" &&
              sysmodule_functions.contains(nid)) ||
             ((symbol.name.substr(nid.size()) == "#libSceUserService#1#libSceUserService#Function" ||
               ((nid == "wuI7c7UNk0A" || nid == "spW--yoLQ9o") &&
                symbol.name.substr(nid.size()) == "#libSceUserServiceForNpToolkit#1#libSceUserService#Function")) &&
              userservice_functions.contains(nid)) ||
             (symbol.name.substr(nid.size()) ==
                  "#libSceSystemService#1#libSceSystemService#Function" &&
              systemservice_functions.contains(nid)) ||
             (symbol.name.substr(nid.size()) == "#libSceGnmDriver#1#libSceGnmDriver#Function" &&
              gnmdriver_functions.contains(nid)) ||
             (graphics_window &&
              symbol.name.substr(nid.size()) == "#libSceVideoOut#1#libSceVideoOut#Function" &&
              videoout_functions.contains(nid)) ||
             (hmd_functions.contains(nid) &&
              (symbol.name.substr(nid.size()) == "#libSceHmd#1#libSceHmd#Function" ||
               (nid == "8gH1aLgty5I" && symbol.name.substr(nid.size()) ==
                    "#libsceHmdReprojectionMultilayer#1#libSceHmd#Function"))) ||
             (hmd_setup_dialog_functions.contains(nid) &&
              symbol.name.substr(nid.size()) ==
                  "#libSceHmdSetupDialog#1#libSceHmdSetupDialog#Function") ||
             (vr_tracker_functions.contains(nid) &&
              symbol.name.substr(nid.size()) == "#libSceVrTracker#1#libSceVrTracker#Function") ||
             (IsCameraNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceCamera#1#libSceCamera#Function") ||
             (move_functions.contains(nid) &&
              symbol.name.substr(nid.size()) == "#libSceMove#1#libSceMove#Function") ||
             symbol.name == "NWtTN10cJzE#libSceLibcInternalExt#1#libSceLibcInternal#Function"))
        {
            if (it != handlers.end()) {
                adapter = std::make_shared<FunctionAdapter>(it->second);
                hle_status[symbol.name] =
                    hmd_functions.contains(nid) || vr_tracker_functions.contains(nid) ||
                        hmd_setup_dialog_functions.contains(nid) || IsCameraNid(nid) ||
                        move_functions.contains(nid)
                        ? "android_bridge_guest_vr_sbs_virtual"
                        : IsMouseNid(nid)
                            ? "android_bridge_guest_device_no_provider"
                            : IsAudioInputNid(nid) ? "android_bridge_audio_input_no_provider"
                            : FindRemoteService(nid) ? "android_bridge_remote_service_no_provider"
                            : IsLiveStreamingUnavailableNid(nid) ? "android_bridge_broadcast_no_provider"
                            : coredump_nid ? "android_bridge_coredump_no_provider"
                            : nid == "VjBtg5Btl94" ? "android_bridge_fsst_no_provider"
                            : MemoryServiceStatus(nid);
            }
        }
        if (!adapter) {
            // No unspecialized raw pointers, host stubs, or scalar functions with
            // hidden desktop/thread state are callable merely by having a descriptor.
            refused.push_back(symbol.name);
            if (!hle_status.contains(symbol.name))
                hle_status[symbol.name] = "unsupported_import";
            adapter = std::make_shared<FunctionAdapter>([name = symbol.name](HleCallFrame&) {
                return MakeError(ErrorCategory::Unsupported, "ProductionHle",
                                 "unimplemented import policy: " + name);
            });
        }
        if (const auto* entry = AeroLib::FindByNid(nid.c_str()))
            static_cast<FunctionAdapter&>(*adapter).profile_name = std::string("HLE.") + entry->name;
        else
            static_cast<FunctionAdapter&>(*adapter).profile_name = "HLE." + nid;
        static_cast<FunctionAdapter&>(*adapter).profile_poll =
            nid == "QBi7HCK03hw" || nid == "lLMT9vJAck0" || nid == "1jfXLRVzisc";
        auto& function = static_cast<FunctionAdapter&>(*adapter);
        function.sync_operation = SyncMetrics::Classify(function.profile_name);
        if (function.sync_operation != SyncMetrics::Operation::None) function.sync_metrics = sync_metrics;
        function.profile_sync_mutex = function.profile_name.find("MutexLock") != std::string::npos ||
            function.profile_name.find("MutexUnlock") != std::string::npos ||
            function.profile_name.find("mutex_lock") != std::string::npos ||
            function.profile_name.find("mutex_unlock") != std::string::npos;
        function.profile_sync = profile_sync && (function.profile_sync_mutex ||
            function.profile_name == "HLE.sceNetEpollWait" ||
            function.profile_name.find("PthreadCond") != std::string::npos ||
            function.profile_name.find("pthread_cond") != std::string::npos ||
            function.profile_name.find("Usleep") != std::string::npos ||
            function.profile_name.find("usleep") != std::string::npos);
        const auto op = Require(registry.Adopt(std::move(adapter), symbol.name));
        operation_names.emplace(op, symbol.name);
#if defined(__ANDROID__)
        // A bounded startup map lets the guest RSP census identify a blocked
        // HLE boundary without exposing mutable runtime state to the debugger.
        // Keep this diagnostic local to Android startup; it has no dispatch
        // or ABI effect and naturally expires after the import table is built.
        static std::atomic<unsigned> operation_map_traces{};
        if (operation_map_traces.fetch_add(1, std::memory_order_relaxed) < 2048) {
            const auto status_it = hle_status.find(symbol.name);
            const auto status = status_it == hle_status.end() ? "unknown" : status_it->second;
            __android_log_print(ANDROID_LOG_INFO, "ProductionHLEMap",
                                "op=%llu status=%s symbol=%s",
                                (unsigned long long)op, status.c_str(), symbol.name.c_str());
        }
#endif
        CodePublication vm(*this);
        const u64 address = Allocate(0x4000, "HleVeneer", 0x1800000000ULL);
        // Same ABI as HleVeneerAllocator; page tail traps using operation zero.
        const auto bytes = HleVeneerAllocator::Encode(0);
        for (size_t offset = 0; offset < 0x4000; offset += bytes.size())
            std::memcpy(reinterpret_cast<void*>(address + offset), bytes.data(), bytes.size());
        const auto entry = HleVeneerAllocator::Encode(op);
        std::memcpy(reinterpret_cast<void*>(address), entry.data(), entry.size());
        if (memory->Protect(address, 0x4000, MemoryProt::CpuRead | MemoryProt::CpuExec))
            throw std::runtime_error("veneer publication failed");
        veneers.emplace(symbol.name, address);
        return address;
    }
    GuestModuleLifecycle::Result StartModule(u32 id, u64 bytes, u64 argp) {
        auto* scope = HleScope::Current();
        return module_lifecycle.Start(id, Current()->id, bytes, argp,
            scope ? scope->CancellationToken() : std::stop_token{},
            [this](u32 module_id, u64 size, u64 arguments) {
                auto* module = linker->GetModule(module_id);
                void* param{};
                if (deferred_modules.contains(module_id)) {
                    param = module->GetProcParam<void*>();
                    if (param) {
                        const auto address = reinterpret_cast<u64>(param);
                        Require(space.ValidateRange({GuestAddress{address}, 0x18}, GuestPermission::Read));
                        if (Read<u64>(address) < 0x18)
                            throw std::runtime_error("invalid dynamic module parameters");
                    }
                }
                const auto result = module->Start(size, reinterpret_cast<const void*>(arguments), param);
                if (!result)
                    sysmodules.Publish(std::filesystem::path(module->name).stem().string(),
                                       static_cast<s32>(module_id));
#if defined(__ANDROID__)
                __android_log_print(ANDROID_LOG_INFO, "ProductionModule",
                    "DT_INIT path=%s handle=%u bytes=%llu argp=%llx result=%d deferred=%d",
                    module->name.c_str(), module_id, (unsigned long long)size,
                    (unsigned long long)arguments, result, deferred_modules.contains(module_id));
#endif
                return result;
            });
    }
    u64 TlsAddress(u64 module, u64 offset) {
        if (module == 0 || module > linker->MaxTlsIndex())
            throw std::runtime_error("invalid TLS module");
        Module* target{};
        for (u32 i = 0;; ++i) {
            auto* m = linker->GetModule(i);
            if (!m)
                break;
            if (m->tls.modid == module) {
                target = m;
                break;
            }
        }
        if (!target || offset >= target->tls.image_size)
            throw std::runtime_error("TLS offset out of range");
        const auto address = reinterpret_cast<u64>(
            Read<DtvEntry>(Current()->dtv + (module + 1) * sizeof(DtvEntry)).pointer);
        if (!address)
            throw std::runtime_error("TLS image is not installed on this guest owner");
        Require(space.ValidateRange({GuestAddress{address + offset}, 1}, GuestPermission::Read));
        return address + offset;
    }
    u64 ErrnoAddress() {
        return Current()->handle_va + sizeof(u64);
    }
    u64 PosixFailure(int error) {
        Write(ErrnoAddress(), static_cast<s32>(error));
        return UINT64_MAX;
    }
    u64 SysmoduleLoad(u32 id) {
        // Activity enables SBS after runtime construction. Publish the complete
        // checked provider family lazily, before both public/internal load paths.
        sysmodules.PublishSbsVrProviders(GuestVrSensor::Instance().Read().enabled);
        const s32 result = sysmodules.Load(id);
        const char* name{};
        (void)Libraries::SysModule::LookupSysmodule(id, &name, nullptr);
        LOG_INFO(Lib_SysModule, "session sysmodule id={:#x} name={} result={:#x}", id,
                 name ? name : "unknown", static_cast<u32>(result));
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "ProductionModule",
                            "SysmoduleLoad id=%#x name=%s result=%#x", id,
                            name ? name : "unknown", static_cast<u32>(result));
#endif
        return static_cast<u32>(result);
    }
    u64 SysmoduleUnload(u32 id) {
        return static_cast<u32>(sysmodules.Unload(id));
    }
    u64 SysmoduleIsLoaded(u32 id) {
        return static_cast<u32>(sysmodules.Handle(id));
    }
    u64 SysmoduleGetHandle(u32 id, u64 handle_ptr) {
        if (handle_ptr &&
            !space.ValidateRange({GuestAddress{handle_ptr}, sizeof(s32)}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_KERNEL_ERROR_EFAULT);
        s32 handle{};
        const s32 result = sysmodules.Handle(id, &handle);
        if (!result && handle_ptr)
            Write(handle_ptr, handle);
        return static_cast<u32>(result);
    }
    void InstallHandlers();
};

// Publishes the app-shipped x86-64 synchronization payload and routes the
// mutex/self imports of every title to it. The payload's slow paths are the
// ordinary HLE veneers of the same NIDs, so lazy initialization, destroyed
// handles, unmapped slots and every error stay on the checked host path.
void GuestRuntime::Impl::InstallSyncFastPath() {
    sync_fastpath_installed = true;
    if (!sync_fastpath_enabled) {
        sync_fastpath_status = "disabled_by_property";
        return;
    }
#if !defined(SHADPS4_HAS_GUEST_SYNC_PAYLOAD)
    sync_fastpath_status = "unavailable_in_build";
    return;
#else
    using namespace GuestSyncPayload;
    const auto symbol = [](std::string_view name) {
        for (const auto& s : Symbols)
            if (name == s.name) return s;
        throw std::runtime_error("guest sync payload lacks symbol " + std::string(name));
    };
    const auto table = symbol(SHAD_SYNC_IMPORT_TABLE);
    if (table.size != ShadSyncImportCount * sizeof(u64) || table.offset % 8)
        throw std::runtime_error("guest sync payload import table mismatch");
    // Import slots: the two runtime primitives plus the POSIX HLE fallbacks.
    // Bind() records ordinary veneers for these names; the guest-facing entries
    // are replaced below, after the fallbacks were captured.
    static constexpr const char* kPosixLock = "7H0iTOciTLo#libScePosix#1#libkernel#Function";
    static constexpr const char* kPosixTrylock = "K-jXhbt2gn4#libScePosix#1#libkernel#Function";
    static constexpr const char* kPosixUnlock = "2Z+PpY6CaJg#libScePosix#1#libkernel#Function";
    std::array<u64, ShadSyncImportCount> imports{};
    imports[ShadSyncImportWait] = Bind({.name = SHAD_SYNC_NID_WAIT SHAD_SYNC_LIBRARY_SUFFIX});
    imports[ShadSyncImportWake] = Bind({.name = SHAD_SYNC_NID_WAKE SHAD_SYNC_LIBRARY_SUFFIX});
    imports[ShadSyncImportMutexLock] = Bind({.name = kPosixLock});
    imports[ShadSyncImportMutexTrylock] = Bind({.name = kPosixTrylock});
    imports[ShadSyncImportMutexUnlock] = Bind({.name = kPosixUnlock});
    CodePublication vm(*this);
    sync_fastpath_size = Common::AlignUp(u64{sizeof(Image)}, 0x4000ULL);
    sync_fastpath_base = Allocate(sync_fastpath_size, "GuestSyncFastPath", 0x1800000000ULL);
    std::memset(reinterpret_cast<void*>(sync_fastpath_base), 0xcc, sync_fastpath_size); // int3
    std::memcpy(reinterpret_cast<void*>(sync_fastpath_base), Image, sizeof(Image));
    for (unsigned i = 0; i < ShadSyncImportCount; ++i) {
        u64 slot{};
        std::memcpy(&slot, reinterpret_cast<void*>(sync_fastpath_base + table.offset + 8 * i), 8);
        if (slot) throw std::runtime_error("guest sync payload import slot is not empty");
        std::memcpy(reinterpret_cast<void*>(sync_fastpath_base + table.offset + 8 * i), &imports[i], 8);
    }
    if (memory->Protect(sync_fastpath_base, sync_fastpath_size, MemoryProt::CpuRead | MemoryProt::CpuExec))
        throw std::runtime_error("guest sync payload publication failed");
    // Game-visible entries: both library spellings each NID is imported under.
    struct Route { const char* nid; const char* export_name; };
    static constexpr Route routes[] = {
        {"7H0iTOciTLo", SHAD_SYNC_EXPORT_POSIX_LOCK},    {"K-jXhbt2gn4", SHAD_SYNC_EXPORT_POSIX_TRYLOCK},
        {"2Z+PpY6CaJg", SHAD_SYNC_EXPORT_POSIX_UNLOCK},  {"EotR8a3ASf4", SHAD_SYNC_EXPORT_SELF},
        {"9UK1vLZQft4", SHAD_SYNC_EXPORT_SCE_LOCK},      {"upoVrzMHFeE", SHAD_SYNC_EXPORT_SCE_TRYLOCK},
        {"tn3VlD0hG60", SHAD_SYNC_EXPORT_SCE_UNLOCK},    {"aI+OeCz8xrQ", SHAD_SYNC_EXPORT_SELF},
    };
    for (const auto& route : routes) {
        const auto entry = sync_fastpath_base + symbol(route.export_name).offset;
        for (const char* suffix : {"#libScePosix#1#libkernel#Function", "#libkernel#1#libkernel#Function"}) {
            const auto name = std::string(route.nid) + suffix;
            veneers.insert_or_assign(name, entry);
            hle_status[name] = "guest_fastpath";
        }
    }
    sync_fastpath_status = "installed";
    LOG_INFO(Core_Linker, "Guest sync fast path installed: base={:#x} bytes={} image_sha256={}",
             sync_fastpath_base, sizeof(Image), ImageSha256);
#endif
}

void GuestRuntime::Impl::InstallHandlers() {
    auto bind = [&](std::initializer_list<const char*> nids,
                    std::function<u64(const std::array<u64, 6>&)> fn) {
        for (auto nid : nids)
            handlers[nid] = [fn](HleCallFrame& frame) {
                std::array<u64, 6> args{};
                for (size_t i = 0; i < args.size(); ++i)
                    args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
                frame.registers.Set(Gpr::Rax, fn(args));
                return Ok();
            };
    };
    auto dialog_handler = [this](std::string_view nid) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            if (!save_dialog)
                return Status(MakeError(ErrorCategory::Unsupported, "SaveDialog",
                                        "platform dialog unavailable"));
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
            frame.registers.Set(Gpr::Rax, save_dialog->Invoke(nid, space, args));
            return Ok();
        };
    };
    for (auto nid : SaveDialogNids)
        dialog_handler(nid);
    dialog_handler("uoUpLGNkygk");
    dialog_handler("BQ3tey0JmQM");
    for (auto nid : GuestCommerceDialog::Nids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            if (!commerce_dialog)
                return Status(MakeError(ErrorCategory::Unsupported, "NpCommerce", "offline provider unavailable"));
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i) args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
            frame.registers.Set(Gpr::Rax, commerce_dialog->Invoke(space, nid, args));
            return Ok();
        };
    }
    for (auto nid : GuestMsgDialog::Nids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            if (!msg_dialog)
                return Status(MakeError(ErrorCategory::Unsupported, "MsgDialog", "message provider unavailable"));
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i) args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
            frame.registers.Set(Gpr::Rax, msg_dialog->Invoke(space, nid, args));
            return Ok();
        };
    }
    for (auto nid : GuestSigninDialog::Nids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            if (!signin_dialog)
                return Status(MakeError(ErrorCategory::Unsupported, "SigninDialog",
                                        "offline dialog provider unavailable"));
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
            frame.registers.Set(Gpr::Rax, signin_dialog->Invoke(space, nid, args));
            return Ok();
        };
    }
    for (auto nid : GuestImeDialog::Nids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            if (!ime_dialog)
                return Status(MakeError(ErrorCategory::Unsupported, "ImeDialog",
                                        "IME dialog unavailable"));
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
            frame.registers.Set(Gpr::Rax, ime_dialog->Invoke(nid, args));
            return Ok();
        };
    }
    for (const auto& entry : StorageEntries) {
        handlers[std::string(entry.nid)] = [this, entry](HleCallFrame& frame) {
            if (!storage)
                return Status(MakeError(ErrorCategory::Unsupported, "GuestStorage",
                                        "valid persistent title identity is required"));
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
            // Storage admits the complete buffer batch atomically. Only those
            // references survive the disk/descriptor operation.
            if (!entry.save && entry.op != StorageOp::Open && entry.op != StorageOp::Stat &&
                entry.op != StorageOp::CheckReachability &&
                entry.op != StorageOp::Mkdir && entry.op != StorageOp::Rmdir &&
                entry.op != StorageOp::Unlink &&
                entry.op != StorageOp::Rename && network && network->Sockets().Contains(s32(args[0]))) {
                std::string_view socket_nid;
                if (entry.op == StorageOp::Read) socket_nid = "Ez8xjo9UF4E";
                if (entry.op == StorageOp::Write) socket_nid = "fZOeZIOEmLw";
                if (entry.op == StorageOp::Close) socket_nid = "socket.close";
                auto error = [this, entry](int e) -> u64 {
                    return entry.posix ? PosixFailure(e)
                        : u64(s64(Libraries::Kernel::ErrnoToSceKernelError(e)));
                };
                if (!socket_nid.empty()) {
                    auto socket_args = args; socket_args[3] = 0;
                    frame.registers.Set(Gpr::Rax, network->Sockets().Dispatch(space, socket_nid,
                        socket_args, error, {}, HleScope::Current()->CancellationToken()));
                } else {
                    frame.registers.Set(Gpr::Rax, error(entry.op == StorageOp::Seek ||
                        entry.op == StorageOp::Pread || entry.op == StorageOp::Pwrite ||
                        entry.op == StorageOp::Preadv || entry.op == StorageOp::Pwritev
                        ? POSIX_ESPIPE : POSIX_EOPNOTSUPP));
                }
                return Ok();
            }
            const auto result = DispatchStorage(*storage, space, entry, args,
                                                [this](int error) { return PosixFailure(error); });
            if (entry.save)
                LOG_INFO(Lib_SaveData, "Session storage nid={} result={:#x}", entry.nid, result);
            frame.registers.Set(Gpr::Rax, result);
            return Ok();
        };
    }
    thread_attributes = std::make_unique<GuestThreadAttributeDomain>(
        space, [this] { return Allocate(0x4000, "GuestThreadAttr"); });
    for (const auto& entry : ThreadAttrEntries) {
        handlers[std::string(entry.nid)] = [this, entry](HleCallFrame& frame) {
            const u64 a = frame.registers.Get(Gpr::Rdi), b = frame.registers.Get(Gpr::Rsi),
                      c = frame.registers.Get(Gpr::Rdx);
            int error{};
            if (entry.op == ThreadAttrOp::GetThread) {
                GuestThreadAttributes value{};
                bool found{};
                {
                    std::lock_guard lock(threads_mutex);
                    for (auto& [id, owner] : owners) {
                        if (owner->handle_va != a || owner->finished) continue;
                        value = owner->attributes;
                        value.detached = owner->detached;
                        found = true;
                        break;
                    }
                }
                error = found ? thread_attributes->Store(b, value) : POSIX_ESRCH;
            } else error = thread_attributes->Invoke(entry, a, b, c);
            frame.registers.Set(Gpr::Rax, error && entry.sce ? u64(0x80020000u | error) : u64(error));
            return Ok();
        };
    }
    kernel_semaphores = std::make_unique<GuestKernelSemaphore>(space);
    for (const char* nid : {"188x57JYp0g", "Zxa0VhQVTsk", "4czppHBiriw", "12wOHk8ywb0", "4DM06U2BNEY", "R1Jvn8bSCW8"}) {
        bind({nid}, [this, nid](const auto& args) -> u64 {
            return kernel_semaphores->Dispatch(nid, args, Current()->attributes.priority,
                                               HleScope::Current()->CancellationToken());
        });
    }
    semaphore_domain = std::make_unique<GuestSemaphoreDomain>(space, [this] {

        return Allocate(0x4000, "GuestSemaphore");
    });
    auto sem_bind = [&](const char* posix, const char* sce, auto fn) {
        bind({posix}, [this, fn](const auto& a) -> u64 {
            const int error = fn(a);
            return error ? PosixFailure(error) : 0;
        });
        bind({sce}, [fn](const auto& a) -> u64 {
            const int error = fn(a);
            return error ? u64(0x80020000u | error) : 0;
        });
    };
    sem_bind("pDuPEf3m4fI", "GEnUkDZoUwY", [this](const auto& a) {
        return semaphore_domain->Init(a[0], s32(a[1]), u32(a[2]));
    });
    sem_bind("cDW233RAwWo", "Vwc+L05e6oE", [this](const auto& a) {
        return semaphore_domain->Destroy(a[0]);
    });
    sem_bind("IKP8typ0QUk", "aishVAiFaYM", [this](const auto& a) {
        return semaphore_domain->Post(a[0]);
    });
    sem_bind("Bq+LRV-N6Hk", "DjpBvGlaWbQ", [this](const auto& a) {
        return semaphore_domain->GetValue(a[0], a[1]);
    });
    sem_bind("YCV5dGGBcCo", "C36iRE0F5sE", [this](const auto& a) {
        return semaphore_domain->Wait(a[0], false, HleScope::Current()->CancellationToken());
    });
    sem_bind("WBWzsRifCEA", "H2a+IN9TP0E", [this](const auto& a) {
        return semaphore_domain->Wait(a[0], true, HleScope::Current()->CancellationToken());
    });
    sem_bind("4SbrhCozqQU", "fjN6NQHhK8k", [this](const auto& a) {
        return semaphore_domain->Wait(a[0], false, HleScope::Current()->CancellationToken(),
            std::chrono::steady_clock::now() + std::chrono::microseconds(u32(a[1])));
    });
    bind({"w5IHyvahg-o"}, [this](const auto& a) -> u64 {
        using Ts = Libraries::Kernel::OrbisKernelTimespec;
        if (!space.ValidateRange({GuestAddress{a[1]}, sizeof(Ts)}, GuestPermission::Read))
            return PosixFailure(POSIX_EFAULT);
        std::chrono::nanoseconds absolute;
        if (!GuestClock::Duration(Read<Ts>(a[1]), absolute)) return PosixFailure(POSIX_EINVAL);
        Ts now{};
        if (int error = clock.Read(Libraries::Kernel::ORBIS_CLOCK_REALTIME, now, false))
            return PosixFailure(error);
        const auto current = std::chrono::seconds(now.tv_sec) + std::chrono::nanoseconds(now.tv_nsec);
        const auto steady = std::chrono::steady_clock::now();
        const auto remaining = std::max(absolute - current, std::chrono::nanoseconds::zero());
        const auto room = std::chrono::steady_clock::time_point::max() - steady;
        const int error = semaphore_domain->Wait(a[0], false, HleScope::Current()->CancellationToken(),
            steady + std::min(remaining, std::chrono::duration_cast<std::chrono::nanoseconds>(room)));
        return error ? PosixFailure(error) : 0;
    });
    // Separate from the mutex arena: each domain serializes its own allocator.
    // Pack immutable handles instead of mapping 16 KiB for every rwlock/attr.
    rwlock_arena = std::make_unique<GuestSyncArena>([this] {
        void* address{};
        const auto result = memory->MapMemory(
            &address, GuestRuntime::ServiceAllocationBase, GuestSyncArena::BlockSize,
            MemoryProt::CpuReadWrite, MemoryMapFlags::NoFlags, VMAType::File,
            "GuestRwlockObjects");
        if (result == ORBIS_KERNEL_ERROR_ENOMEM) return u64{0};
        if (result) throw std::runtime_error("GuestRwlockObjects mapping failed");
        return reinterpret_cast<u64>(address);
    });
    rwlock_domain = std::make_unique<GuestRwlockDomain>(
        space, [this] { return rwlock_arena->Allocate(); });
    auto pthread_bind = [&](const char* posix, const char* sce, std::function<int(const std::array<u64, 6>&)> fn) {
        bind({posix}, [fn](const auto& a) -> u64 { return fn(a); });
        bind({sce}, [fn](const auto& a) -> u64 {
            const auto error = fn(a); return error ? u64(0x80020000u | error) : 0;
        });
    };
    auto event_flag = [this](u64 handle) {
        std::lock_guard lock(event_flags_mutex);
        auto it = event_flags.find(handle);
        return it == event_flags.end() ? std::shared_ptr<EventFlag>{} : it->second;
    };
    bind({"BpFoboUJoZU"}, [this](const auto& a) -> u64 {
        if (!a[0] || a[4] || (a[2] & ~u64{0x33}) ||
            (a[2] & 0xf) > 2 || (a[2] & 0xf0) > 0x20)
            return ORBIS_KERNEL_ERROR_EINVAL;
        if (!space.ValidateRange({GuestAddress{a[0]}, sizeof(u64)}, GuestPermission::Write))
            return ORBIS_KERNEL_ERROR_EFAULT;
        auto name = String(a[1], 32);
        if (name.size() >= 32) return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
        auto ef = std::make_shared<EventFlag>(name,
            (a[2] & 0x20) ? EventFlag::ThreadMode::Multi : EventFlag::ThreadMode::Single,
            (a[2] & 2) ? EventFlag::QueueMode::ThreadPrio : EventFlag::QueueMode::Fifo, a[3]);
        std::lock_guard lock(event_flags_mutex);
        const u64 handle = next_event_flag++;
        if (!space.WriteData(GuestAddress{a[0]}, std::as_bytes(std::span{&handle, 1})))
            return ORBIS_KERNEL_ERROR_EFAULT;
        event_flags.emplace(handle, std::move(ef));
        return 0;
    });
    bind({"8mql9OcQnd4"}, [this](const auto& a) -> u64 {
        std::shared_ptr<EventFlag> ef;
        { std::lock_guard lock(event_flags_mutex); auto it = event_flags.find(a[0]);
          if (it == event_flags.end()) return ORBIS_KERNEL_ERROR_ESRCH;
          ef = it->second; event_flags.erase(it); }
        ef->Delete(); // wake existing leases before the last shared owner retires
        return 0;
    });
    bind({"7uhBFWRAS60"}, [event_flag](const auto& a) -> u64 {
        auto ef = event_flag(a[0]); if (!ef) return ORBIS_KERNEL_ERROR_ESRCH;
        ef->Clear(a[1]); return 0; // Orbis Clear uses a keep-mask, not a remove-mask
    });
    bind({"IOnSvHzqu6A"}, [event_flag](const auto& a) -> u64 {
        auto ef = event_flag(a[0]); if (!ef) return ORBIS_KERNEL_ERROR_ESRCH;
        ef->Set(a[1]); return 0;
    });
    bind({"PZku4ZrXJqg"}, [this, event_flag](const auto& a) -> u64 {
        auto ef = event_flag(a[0]); if (!ef) return ORBIS_KERNEL_ERROR_ESRCH;
        if (a[2] && !space.ValidateRange({GuestAddress{a[2]}, sizeof(s32)}, GuestPermission::Write))
            return ORBIS_KERNEL_ERROR_EFAULT;
        int count{};
        ef->Cancel(a[1], &count);
        if (a[2] && !space.WriteData(GuestAddress{a[2]}, std::as_bytes(std::span{&count, 1})))
            return ORBIS_KERNEL_ERROR_EFAULT;
        return 0;
    });
    for (const auto nid : {"9lvj5DjHZiA", "JTvBflhYazQ"}) {
        bind({nid}, [this, nid, event_flag](const auto& a) -> u64 {
            auto ef = event_flag(a[0]);
            if (!ef) return ORBIS_KERNEL_ERROR_ESRCH;
            const u32 mode = u32(a[2]);
            if (!a[1] || (mode & 0xf) < 1 || (mode & 0xf) > 2 ||
                (mode & ~0x3fu) || (mode & 0xf0) == 0x30)
                return ORBIS_KERNEL_ERROR_EINVAL;
            const bool poll = nid == std::string_view{"9lvj5DjHZiA"};
            const auto wait = (mode & 0xf) == 1 ? EventFlag::WaitMode::And : EventFlag::WaitMode::Or;
            const auto clear = (mode & 0x10) ? EventFlag::ClearMode::All :
                (mode & 0x20) ? EventFlag::ClearMode::Bits : EventFlag::ClearMode::None;
            if (a[3] && !space.ValidateRange({GuestAddress{a[3]}, sizeof(u64)}, GuestPermission::Write))
                return ORBIS_KERNEL_ERROR_EFAULT;
            u32 timeout{};
            if (!poll && a[4] &&
                (!space.ReadData(GuestAddress{a[4]}, std::as_writable_bytes(std::span{&timeout, 1})) ||
                 !space.ValidateRange({GuestAddress{a[4]}, sizeof(u32)}, GuestPermission::Write)))
                return ORBIS_KERNEL_ERROR_EFAULT;
            u64 pattern{};
            bool pattern_written{};
            SyncMetrics::Phase park_phase{SyncMetrics::Stage::Park};
            const s32 result = poll ? ef->Poll(a[1], wait, clear, &pattern, &pattern_written) :
                ef->Wait(a[1], wait, clear, &pattern, a[4] ? &timeout : nullptr,
                         Current()->attributes.priority, HleScope::Current()->CancellationToken(),
                         &pattern_written);
            park_phase.End();
            if (a[3] && pattern_written &&
                !space.WriteData(GuestAddress{a[3]}, std::as_bytes(std::span{&pattern, 1})))
                return ORBIS_KERNEL_ERROR_EFAULT;
            if (!poll && a[4] && !space.WriteData(GuestAddress{a[4]}, std::as_bytes(std::span{&timeout, 1})))
                return ORBIS_KERNEL_ERROR_EFAULT;
            return result;
        });
    }
    bind({"WkwEd3N7w0Y"}, [](const auto& a) -> u64 {
        // Unity installs a guest callback for crash reporting. Installing a
        // native sigaction that jumps into a guest PC is unsafe on Android;
        // validate the signal and keep the callback as an explicit deferred
        // capability until the signal trampoline is bridged.
        constexpr u32 allowed[] = {1, 4, 8, 10, 11, 30};
        return std::ranges::find(std::begin(allowed), std::end(allowed), u32(a[0])) !=
                       std::end(allowed)
                   ? u64(ORBIS_OK)
                   : u64(ORBIS_KERNEL_ERROR_EINVAL);
    });
    bind({"Qhv5ARAoOEc"}, [](const auto& a) -> u64 {
        constexpr u32 allowed[] = {1, 4, 8, 10, 11, 30};
        return std::ranges::find(std::begin(allowed), std::end(allowed), u32(a[0])) !=
                       std::end(allowed)
                   ? u64(ORBIS_OK)
                   : u64(ORBIS_KERNEL_ERROR_EINVAL);
    });
    bind({"il03nluKfMk"}, [](const auto& a) -> u64 {
        // The guest exception callback is intentionally not installed as a
        // native signal trampoline. Preserve the accepted SIGUSR1 request as
        // a deferred no-op; invalid signal numbers retain kernel semantics.
        return a[1] == 30 ? u64(ORBIS_OK) : u64(ORBIS_KERNEL_ERROR_EINVAL);
    });
    bind({"Qs0wWulgl7U"}, [](const auto&) -> u64 {
        // Desktop semantics already define Android as a named no-device
        // result; keep Beat Saber on its normal fallback path without claiming
        // a mouse provider exists.
        return ORBIS_MOUSE_ERROR_NOT_INITIALIZED;
    });
    bind({"kGVLc3htQE8"}, [this](const auto& a) -> u64 {
        if (!a[1]) return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
        const u64 capability = 0;
        return space.WriteData({GuestAddress{a[1]}},
                                std::as_bytes(std::span{&capability, size_t{1}}))
                   ? u64(ORBIS_OK)
                   : u64(ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS);
    });
    bind({"ktP9j1fN-zE"}, [this](const auto& a) -> u64 {
        // Firmware libSceVideoOut +0xd690 is memset(options, 0, size),
        // with pointer in RDI and size in ESI. Beat Saber requests 16 bytes.
        if (!a[0] || a[1] != 16) return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
        auto pin = space.AcquireDataSpan({GuestAddress{a[0]}, a[1]}, true);
        if (!pin) return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
        Libraries::VideoOut::sceVideoOutConfigureOptionsInitialize_(pin.Value().WritableBytes().data(), u32(a[1]));
        return 0;
    });
    bind({"pjkDsgxli6c"}, [this](const auto& a) -> u64 {
        using Libraries::VideoOut::Mode;
        if (!a[0] || a[1] != sizeof(Mode)) return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
        Mode mode{};
        Libraries::VideoOut::sceVideoOutModeSetAny_(&mode, sizeof(mode));
        return space.WriteData(GuestAddress{a[0]}, std::as_bytes(std::span{&mode, 1}))
                   ? u64(0) : u64(ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS);
    });
    bind({"N1bEoJ4SRw4"}, [this](const auto& a) -> u64 {
        // SysV: RDX=mode, RCX=options, R8D=mode size, R9D=options size.
        GuestVideoModeRequest request{};
        if (const auto error = ReadGuestVideoMode(space, a, request)) return error;
        const auto& mode = request.mode;
        auto* port = Graphics().VideoOut().GetPort(s32(a[0]));
        if (!port || !port->is_open) return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
        const auto result = Libraries::VideoOut::sceVideoOutConfigureOutputMode_(
            s32(a[0]), 0, &mode, a[3] ? request.options.data() : nullptr, sizeof(mode), u32(a[5]));
        LOG_INFO(Lib_VideoOut, "Guest mode refresh={:#x} resolution={:#x} result={:#x} (host presentation cadence)",
                 mode.refresh_rate, mode.resolution, u32(result));
        return u32(result);
    });
    // Beat Saber uses the POSIX cancellation-state import during IL2CPP/FIOS
    // startup. Keep the state per guest owner and copy oldstate through the
    // checked guest address space; the desktop scalar pointer cannot cross this
    // session boundary.
    bind({"lZzFeSxPl08"}, [this](const auto& a) -> u64 {
        if (a[0] != 0 && a[0] != 1)
            return POSIX_EINVAL;
        auto owner = Current();
        const u32 old = owner->cancel_enabled ? 0u : 1u;
        if (a[1]) {
            if (!space.ValidateRange({GuestAddress{a[1]}, sizeof(u32)}, GuestPermission::Write))
                return POSIX_EFAULT;
            if (!space.WriteData({GuestAddress{a[1]}},
                                  std::as_bytes(std::span{&old, size_t{1}})))
                return POSIX_EFAULT;
        }
        owner->cancel_enabled = a[0] == 0;
        if (owner->cancel_enabled && HleScope::Current()->CancellationToken().stop_requested())
            return POSIX_EINTR;
        return 0;
    });
    pthread_bind("xFebsA4YsFI", "yOfGg-I1ZII", [this](const auto& a) { return rwlock_domain->Attribute(a[0], 0, a[1]); });
    pthread_bind("qsdmgXjqSgk", "i2ifZ3fS2fo", [this](const auto& a) { return rwlock_domain->Attribute(a[0], 1, a[1]); });
    pthread_bind("VqEMuCv-qHY", "LcOZBHGqbFk", [this](const auto& a) { return rwlock_domain->Attribute(a[0], 2, a[1]); });
    pthread_bind("OuKg+kRDD7U", "-ZvQH18j10c", [this](const auto& a) { return rwlock_domain->Attribute(a[0], 3, a[1]); });
    pthread_bind("l+bG5fsYkhg", "Kyls1ChFyrc", [this](const auto& a) { return rwlock_domain->Attribute(a[0], 4, a[1]); });
    pthread_bind("8NuOHiTr1Vw", "h-OifiouBd8", [this](const auto& a) { return rwlock_domain->Attribute(a[0], 5, a[1]); });
    pthread_bind("ytQULN-nhL4", "6ULAa0fq4jA", [this](const auto& a) { return rwlock_domain->Init(a[0], a[1]); });
    pthread_bind("1471ajPzxh0", "BB+kb08Tl9A", [this](const auto& a) { return rwlock_domain->Destroy(a[0]); });
    pthread_bind("EgmLo6EWgso", "+L98PIbGttk", [this](const auto& a) { return rwlock_domain->Unlock(a[0], Current()->id); });
    auto rw_lock = [this](const auto& a, bool write, bool attempt, bool timed) {
        std::optional<std::chrono::system_clock::time_point> deadline;
        if (timed) {
            Libraries::Kernel::OrbisKernelTimespec ts{};
            if (!space.ReadData(GuestAddress{a[1]}, std::as_writable_bytes(std::span{&ts, 1})))
                return POSIX_EFAULT;
            std::chrono::nanoseconds duration;
            if (!GuestClock::Duration(ts, duration)) return POSIX_EINVAL;
            deadline = std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(duration));
        }
        return rwlock_domain->Lock(a[0], Current()->id, write, attempt, HleScope::Current()->CancellationToken(), deadline);
    };
    pthread_bind("iGjsr1WAtI0", "Ox9i0c7L5w0", [rw_lock](const auto& a) { return rw_lock(a, false, false, false); });
    pthread_bind("sIlRvQqsN2Y", "mqdNorrB+gI", [rw_lock](const auto& a) { return rw_lock(a, true, false, false); });
    pthread_bind("SFxTMOfuCkE", "XD3mDeybCnk", [rw_lock](const auto& a) { return rw_lock(a, false, true, false); });
    pthread_bind("XhWHn6P5R7U", "bIHoZCTomsI", [rw_lock](const auto& a) { return rw_lock(a, true, true, false); });
    pthread_bind("lb8lnYo-o7k", "iPtZRWICjrM", [rw_lock](const auto& a) { return rw_lock(a, false, false, true); });
    pthread_bind("9zklzAl9CGM", "adh--6nIqTk", [rw_lock](const auto& a) { return rw_lock(a, true, false, true); });
    sync_arena = std::make_unique<GuestSyncArena>([this] {
        void* address{};
        const auto result = memory->MapMemory(
            &address, GuestRuntime::ServiceAllocationBase, GuestSyncArena::BlockSize, MemoryProt::CpuReadWrite,
            MemoryMapFlags::NoFlags, VMAType::File, "GuestSyncObjects");
        if (result == ORBIS_KERNEL_ERROR_ENOMEM) return u64{0};
        if (result) throw std::runtime_error("GuestSyncObjects mapping failed");
        const auto block = reinterpret_cast<u64>(address);
        // The guest fast path only touches objects inside the declared arena
        // window; a block placed elsewhere stays correct on the HLE path.
        if (block < SHAD_SYNC_ARENA_BASE || block + GuestSyncArena::BlockSize > SHAD_SYNC_ARENA_LIMIT)
            if (sync_arena_outside.fetch_add(1, std::memory_order_relaxed) == 0)
                LOG_WARNING(Core_Linker, "GuestSyncObjects block {:#x} is outside the fast-path arena window",
                            block);
        return block;
    });
    mutex_domain = std::make_unique<GuestMutexDomain>(
        space, [this] { return sync_arena->Allocate(); });
    // Host half of the guest fast path: expected-value wait and wake by guest
    // address. Only the app-shipped payload imports these synthetic NIDs.
    bind({SHAD_SYNC_NID_WAIT}, [this](const auto& a) -> u64 {
        return mutex_domain->Waiters().Wait(a[0], a[1], static_cast<unsigned>(a[2]),
                                            HleScope::Current()->CancellationToken());
    });
    bind({SHAD_SYNC_NID_WAKE}, [this](const auto& a) -> u64 {
        return mutex_domain->Waiters().Wake(a[0], a[1]);
    });
    if (profile_sync) mutex_domain->SetCondObserver([](const GuestMutexDomain::CondTraceEvent& e) noexcept {
        if (!Common::Profiler::Enabled()) return;
        Common::Profiler::Counter("GuestSync.CondObject", e.condition);
        Common::Profiler::Counter("GuestSync.MutexObject", e.mutex);
        Common::Profiler::Counter("GuestSync.WaitOwner", e.owner);
        Common::Profiler::Counter("GuestSync.WaitResult", e.result);
        Common::Profiler::Counter("GuestSync.CondPhase", static_cast<int>(e.kind));
    });
    pthread_bind("5txKfcMUAok", "1FGvU0i9saQ", [this](const auto& a) { return mutex_domain->AttributePolicy(a[0], a[1], false, false); });
    pthread_bind("yDaWxUE50s0", "GoTmFeui+hQ", [this](const auto& a) { return mutex_domain->AttributePolicy(a[0], a[1], true, false); });
    pthread_bind("EXv3ztGqtDM", "mxKx9bxXF2I", [this](const auto& a) { return mutex_domain->AttributePolicy(a[0], a[1], false, true); });
    pthread_bind("PmL-TwKUzXI", "losEubHc64c", [this](const auto& a) { return mutex_domain->AttributePolicy(a[0], a[1], true, true); });
    pthread_bind("dQHWEsJtoE4", "n2MMpvU8igI",
         [this](const auto& a) { return mutex_domain->AttributeInit(a[0]); });
    pthread_bind("HF7lK46xzjY", "smWEktiyyG0",
         [this](const auto& a) { return mutex_domain->Attribute(a[0], 0, 0); });
    pthread_bind("mDmgMOGVUqg", "iMp8QpE+XO4",
         [this](const auto& a) { return mutex_domain->Attribute(a[0], a[1], 1); });
    pthread_bind("GZFlI7RhuQo", "gquEhBrS2iw",
         [this](const auto& a) { return mutex_domain->Attribute(a[0], a[1], 2); });
    pthread_bind("ttHNfU+qDBU", "cmo1RIYva9o",
         [this](const auto& a) { return mutex_domain->Init(a[0], a[1]); });
    pthread_bind("7H0iTOciTLo", "9UK1vLZQft4", [this](const auto& a) {
        return mutex_domain->Lock(a[0], Current()->handle_va, false,
                                  HleScope::Current()->CancellationToken());
    });
    pthread_bind("K-jXhbt2gn4", "upoVrzMHFeE", [this](const auto& a) {
        return mutex_domain->Lock(a[0], Current()->handle_va, true,
                                  HleScope::Current()->CancellationToken());
    });
    pthread_bind("2Z+PpY6CaJg", "tn3VlD0hG60",
         [this](const auto& a) { return mutex_domain->Unlock(a[0], Current()->handle_va); });
    pthread_bind("ltCfaGr2JGE", "2Of0f+3mhhE",
         [this](const auto& a) { return mutex_domain->Destroy(a[0]); });
    bind({"gKqzW-zWhvY", "W6OrTBO95UY"},
         [this](const auto& a) { return mutex_domain->IsOwned(a[0], Current()->handle_va); });
    pthread_bind("mkx2fVhNMsg", "JGgj7Uvrl+A",
         [this](const auto& a) { return mutex_domain->CondNotify(a[0], true); });
    pthread_bind("2MOy+rUfuhQ", "kDh-NfxgMtE",
         [this](const auto& a) { return mutex_domain->CondNotify(a[0], false); });
    pthread_bind("0TyVk4MSLt0", "2Tb92quprl0",
         [this](const auto& a) { return mutex_domain->CondInit(a[0], a[1]); });
    pthread_bind("RXXqi4CtF8w", "g+PZd2hiacg",
         [this](const auto& a) { return mutex_domain->CondDestroy(a[0]); });
    pthread_bind("Op8TBGY5KHg", "WKAXJ4XBPQ4", [this](const auto& a) {
        return mutex_domain->CondWait(a[0], a[1], Current()->handle_va,
                                      HleScope::Current()->CancellationToken());
    });
    pthread_bind("K953PF5u6Pc", "BmMjYxmew1w", [this](const auto& a) {
        return mutex_domain->CondWait(a[0], a[1], Current()->handle_va,
                                      HleScope::Current()->CancellationToken(),
                                      {.relative_us = a[2]});
    });
    bind({"27bAgiJmOh0"}, [this](const auto& a) -> u64 {
        Libraries::Kernel::OrbisKernelTimespec absolute{};
        if (!space.ReadData(GuestAddress{a[2]}, std::as_writable_bytes(std::span{&absolute, 1})))
            return POSIX_EFAULT;
        std::chrono::nanoseconds duration{};
        if (!GuestClock::Duration(absolute, duration))
            return POSIX_EINVAL;
        return mutex_domain->CondWait(a[0], a[1], Current()->handle_va,
                                      HleScope::Current()->CancellationToken(),
                                      {.absolute = duration});
    });
    pthread_bind("CI6Qy73ae10", "o69RpYO-Mu0", [this](const auto& a) {
        if (a[1]) {
            std::lock_guard lock(threads_mutex);
            if (std::ranges::none_of(owners, [&](const auto& pair) {
                    return pair.second->handle_va == a[1] && !pair.second->finished;
                }))
                return POSIX_ESRCH;
        }
        return mutex_domain->CondNotify(a[0], false, a[1]);
    });
    using CondAttrOp = GuestMutexDomain::CondAttrOp;
    const auto condattr_bind = [&](const char* posix, const char* sce, CondAttrOp op) {
        pthread_bind(posix, sce, [this, op](const auto& a) {
            return mutex_domain->CondAttribute(a[0], a[1], op);
        });
    };
    condattr_bind("mKoTx03HRWA", "m5-2bsNfv7s", CondAttrOp::Init);
    condattr_bind("dJcuQVn6-Iw", "waPcxYiR3WA", CondAttrOp::Destroy);
    condattr_bind("cTDYxTUNPhM", "6qM3kO5S3Oo", CondAttrOp::GetClock);
    condattr_bind("EjllaAqAPZo", "c-bxj027czs", CondAttrOp::SetClock);
    condattr_bind("h0qUqSuOmC8", "Dn-DRWi9t54", CondAttrOp::GetShared);
    condattr_bind("3BpP850hBT4", "6xMew9+rZwI", CondAttrOp::SetShared);
    handlers["F8bUHwAG284"] = handlers.at("n2MMpvU8igI");
    handlers["qH1gXoq71RY"] = handlers.at("cmo1RIYva9o");
    pthread_bind("ZLvf6lVAc4M", "532IaQguwMg", [this](const auto& a) { return mutex_domain->AttributeCeiling(a[0], a[1], false); });
    pthread_bind("+m8+quqOwhM", "SgjMpyH9Z9I", [this](const auto& a) { return mutex_domain->AttributeCeiling(a[0], a[1], true); });
    // The legacy *_kind_np ABI takes/returns the handle/type directly, unlike
    // gettype's two pointers. Preserve its -1 + guest errno failure convention.
    bind({"U6SNV+RnyLQ"}, [this](const auto& a) -> u64 {
        const int kind = mutex_domain->AttributeKind(a[0]);
        return kind < 0 ? PosixFailure(POSIX_EINVAL) : u64(kind);
    });
    bind({"J9rlRuQ8H5s"}, [this](const auto& a) -> u64 {
        const int error = mutex_domain->Attribute(a[0], a[1], 1);
        return error ? PosixFailure(error) : 0;
    });
    for (const auto& pair : {std::pair{"rH2mWEndluc", "U6SNV+RnyLQ"}, std::pair{"UWZbVSFze24", "J9rlRuQ8H5s"}}) {
        auto posix = handlers.at(pair.second);
        handlers[pair.first] = [posix](HleCallFrame& frame) {
            auto status = posix(frame);
            const u32 result = frame.registers.Get(Gpr::Rax);
            if (status && result) frame.registers.Set(Gpr::Rax, u32(result + 0x80020000u));
            return status;
        };
    }
    bind({"mqULNdimTn0", "geDaqgH9lTg"}, [this](const auto& a) -> u64 {
        if (!space.ValidateRange({GuestAddress{a[0]}, 4}, GuestPermission::Write))
            return POSIX_EFAULT;
        if (a[1] && !space.ValidateRange({GuestAddress{a[1]}, 1}, GuestPermission::Execute))
            return POSIX_EINVAL;
        std::lock_guard lock(threads_mutex);
        for (u32 i = 0; i < specific_keys.size(); ++i) {
            auto& key = specific_keys[i];
            if (key.allocated || key.sequence == UINT64_MAX)
                continue;
            Write(a[0], i);
            key.allocated = true;
            ++key.sequence;
            key.destructor = a[1];
            return 0;
        }
        return POSIX_EAGAIN;
    });
    bind({"6BpEZuDT7YI", "PrdHuuDekhY"}, [this](const auto& a) -> u64 {
        std::lock_guard lock(threads_mutex);
        if (a[0] >= specific_keys.size() || !specific_keys[a[0]].allocated)
            return POSIX_EINVAL;
        specific_keys[a[0]].allocated = false;
        return 0;
    });
    bind({"0-KXaS70xy4", "eoht7mQOCmo"}, [this](const auto& a) -> u64 {
        auto owner = Current();
        std::lock_guard lock(threads_mutex);
        if (a[0] >= specific_keys.size())
            return 0;
        const auto& key = specific_keys[a[0]];
        const auto& value = owner->specific[a[0]];
        return key.allocated && value.sequence == key.sequence ? value.value : 0;
    });
    bind({"WrOLvHU0yQM", "+BzXYkqYeLE"}, [this](const auto& a) -> u64 {
        auto owner = Current();
        std::lock_guard lock(threads_mutex);
        if (a[0] >= specific_keys.size() || !specific_keys[a[0]].allocated)
            return POSIX_EINVAL;
        owner->specific[a[0]] = {specific_keys[a[0]].sequence, a[1]};
        return 0;
    });
    bind({"9BcDykPmo1I"}, [this](const auto&) -> u64 { return ErrnoAddress(); });
    // No guest AddressSanitizer runtime or shadow mapping in this profile.
    bind({"jh+8XiK4LeE", "bnZxYgAFeA0", "py6L8jiVAN8"},
         [](const auto&) -> u64 { return 0; });
    // Match the emulator's virtual process identity (GLOBAL_PID), not Android PID.
    bind({"HoLVWNanBBc"}, [](const auto&) -> u64 { return 0xBAD1; });
    bind({"3PtV6p3QNX4"}, [](const auto& a) -> u64 { return a[0] == a[1]; });
    bind({"EI-5-jlq2dE"}, [this](const auto&) -> u64 { return Current()->id; });
    bind({"959qrazPIrg"},
         [this](const auto&) -> u64 { return reinterpret_cast<u64>(linker->GetProcParam()); });
    bind({"NWtTN10cJzE"}, [this](const auto& a) -> u64 {
        struct TraceInfo {
            u64 size;
            u32 flags, get_segment_info;
            u64 mask, table;
        };
        auto info = Read<TraceInfo>(a[0]);
        if (info.size < sizeof(info))
            throw std::runtime_error("invalid guest heap trace info size");
        Require(space.ValidateRange({GuestAddress{a[0]}, sizeof(info)}, GuestPermission::Write));
        u64 trace;
        {

            std::call_once(heap_trace_once,
                           [&] { heap_trace = Allocate(0x4000, "GuestHeapTrace"); });
            trace = heap_trace;
        }
        info.get_segment_info = 0;
        info.mask = trace;
        info.table = trace + sizeof(u64); // 64 guest mstate pointers, initially zero.
        Write(a[0], info);
        return 0;
    });
    bind({"p5EcQeEeJAE"}, [this](const auto& a) -> u64 {
        const auto table = Read<decltype(guest_heap_api)>(a[0]);
        for (auto entry : table)
            if (entry)
                Require(space.ValidateRange({GuestAddress{entry}, 1}, GuestPermission::Execute));
        std::lock_guard lock(threads_mutex);
        guest_heap_api = table;
        return 0;
    });
    auto usleep = [this](const auto& a, bool sce) -> u64 {
        // Orbis accepts the full u32 microsecond domain, including >= 1 second.
        if (HleScope::Current()->WaitFor(std::chrono::microseconds(u32(a[0])))) return 0;
        return sce ? u64(0x80020000u | POSIX_EINTR) : PosixFailure(POSIX_EINTR);
    };
    bind({"QcteRwbsnV0"}, [usleep](const auto& a) { return usleep(a, false); });
    bind({"1jfXLRVzisc"}, [usleep](const auto& a) { return usleep(a, true); });
    auto sleep = [this](const auto& a, bool sce) -> u64 {
        using namespace std::chrono;
        const auto total = nanoseconds(seconds(u32(a[0])));
        const auto start = steady_clock::now();
        auto remaining = total;
        while (remaining.count() > 0) {
            const bool complete = HleScope::Current()->WaitFor(std::min(remaining, nanoseconds(seconds(1))));
            remaining = std::max(nanoseconds::zero(), total - duration_cast<nanoseconds>(steady_clock::now() - start));
            if (!complete) {
                if (sce) return u64(0x80020000u | POSIX_EINTR);
                (void)PosixFailure(POSIX_EINTR);
                return duration_cast<seconds>(remaining).count() + (remaining % seconds(1) != nanoseconds::zero());
            }
        }
        return 0;
    };
    bind({"0wu33hunNdE"}, [sleep](const auto& a) { return sleep(a, false); });
    bind({"-ZR+hG7aDHw"}, [sleep](const auto& a) { return sleep(a, true); });
    // Marshal without calling the desktop wrappers: those mutate native
    // pthread errno and depend on RegisterTime's process-global clock.
    {
        using Ts = Libraries::Kernel::OrbisKernelTimespec;
        auto clock_call = [this](const auto& a, bool resolution, bool sce) -> u64 {
            int error{};
            Ts value{};
            if ((!a[1] && !resolution) ||
                (a[1] &&
                 !space.ValidateRange({GuestAddress{a[1]}, sizeof(Ts)}, GuestPermission::Write)))
                error = POSIX_EFAULT;
            else
                error = clock.Read(static_cast<u32>(a[0]), value, resolution);
            if (!error && a[1])
                Write(a[1], value);
            if (!error)
                return 0;
            return sce ? u64(0x80020000u | error) : PosixFailure(error);
        };
        bind({"lLMT9vJAck0"}, [clock_call](const auto& a) { return clock_call(a, false, false); });
        bind({"smIj7eqzZE8"}, [clock_call](const auto& a) { return clock_call(a, true, false); });
        bind({"QBi7HCK03hw"}, [clock_call](const auto& a) { return clock_call(a, false, true); });
        bind({"wRYVA5Zolso"}, [clock_call](const auto& a) { return clock_call(a, true, true); });
        bind({"4J2sUJmuHZQ"}, [this](const auto&) { return clock.ticks.GetTimeUS(clock.origin); });
        bind({"fgxnMeTNUtY"},
             [this](const auto&) { return clock.GetProcessTimeCounter(); });
        bind({"-2IRUCO--PM"}, [this](const auto&) { return clock.ReadTsc(); });
        bind({"1j3S3n-tTW4", "BNowx2l588E"},
             [this](const auto&) { return clock.GetTscFrequency(); });
        auto nanosleep = [this](const auto& a, bool sce) -> u64 {
            auto fail = [&](int error) {
                return sce ? u64(0x80020000u | error) : PosixFailure(error);
            };
            if (!space.ValidateRange({GuestAddress{a[0]}, sizeof(Ts)}, GuestPermission::Read) ||
                (a[1] &&
                 !space.ValidateRange({GuestAddress{a[1]}, sizeof(Ts)}, GuestPermission::Write)))
                return fail(POSIX_EFAULT);
            const auto request = Read<Ts>(a[0]);
            std::chrono::nanoseconds total;
            if (!GuestClock::Duration(request, total))
                return fail(POSIX_EINVAL);
            const auto start = std::chrono::steady_clock::now();
            // Chunking also prevents steady_clock deadline addition overflow.
            auto remaining = total;
            while (remaining.count() > 0) {
                const auto slice =
                    std::min(remaining, std::chrono::nanoseconds(std::chrono::seconds(1)));
                if (!HleScope::Current()->WaitFor(slice)) {
                    remaining =
                        std::max(std::chrono::nanoseconds::zero(),
                                 total - std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - start));
                    if (a[1])
                        Write(a[1],
                              Ts{remaining.count() / 1000000000, remaining.count() % 1000000000});
                    return fail(POSIX_EINTR);
                }
                remaining = std::max(std::chrono::nanoseconds::zero(),
                                     total - std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now() - start));
            }
            return 0;
        };
        bind({"NhpspxdjEKU", "yS8U2TGCe1A"},
             [nanosleep](const auto& a) { return nanosleep(a, false); });
        bind({"QvsZxomvUHs"}, [nanosleep](const auto& a) { return nanosleep(a, true); });
    }
    auto gettimeofday = [this](const auto& a, bool sce) -> u64 {
        using Tv = Libraries::Kernel::OrbisKernelTimeval;
        using Tz = Libraries::Kernel::OrbisKernelTimezone;
        auto fail = [&](int error) { return sce ? u64(0x80020000u | error) : PosixFailure(error); };
        if ((a[0] &&
             !space.ValidateRange({GuestAddress{a[0]}, sizeof(Tv)}, GuestPermission::Write)) ||
            (a[1] &&
             !space.ValidateRange({GuestAddress{a[1]}, sizeof(Tz)}, GuestPermission::Write)))
            return fail(POSIX_EFAULT);
        Libraries::Kernel::OrbisKernelTimespec ts{};
        if (int error = clock.Read(Libraries::Kernel::ORBIS_CLOCK_REALTIME, ts, false))
            return fail(error);
        if (a[0])
            Write(a[0], Tv{ts.tv_sec, ts.tv_nsec / 1000});
        if (a[1])
            Write(a[1], Tz{}); // Current guest session timezone profile is UTC.
        return 0;
    };
    bind({"n88vx3C5nW8"}, [gettimeofday](const auto& a) { return gettimeofday(a, false); });
    for (const auto nid : KernelTimezoneNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            return DispatchKernelTimezone(space, nid, a);
        });
    }
    bind({"ejekcaNQNq0"}, [gettimeofday](const auto& a) {
        auto args = a;
        args[1] = 0;
        return gettimeofday(args, true);
    });
    handlers["rNhWz+lvOMU"] = [this](HleCallFrame& frame) -> Status {
        const u64 entry = frame.registers.Get(Gpr::Rdi);

        if (entry) {
            auto valid = space.ValidateRange({GuestAddress{entry}, 1}, GuestPermission::Execute);
            if (!valid) return valid;
        }
        thread_dtors.store(entry, std::memory_order_release);
        frame.registers.Set(Gpr::Rax, 0); // desktop ABI is void
        return Ok();
    };
    for (auto [nid, index] : {std::pair{"pB-yGZ2nQ9o", 0},
                              std::pair{"WhCc1w3EhSI", 1}}) {
        handlers[nid] = [this, index](HleCallFrame& frame) -> Status {
            const u64 entry = frame.registers.Get(Gpr::Rdi);

            if (entry) {
                auto valid = space.ValidateRange({GuestAddress{entry}, 1}, GuestPermission::Execute);
                if (!valid) return valid;
            }
            // libc supplies function pointers here (not the s32 scalars used
            // by the older ARM64 reference stubs). No reporting consumer exists
            // in desktop or this runtime; module unloading remains unsupported.
            thread_atexit_reports[index].store(entry, std::memory_order_release);
            frame.registers.Set(Gpr::Rax, 0);
            return Ok();
        };
    }
    bind({"FJrT5LuUBAU", "3kg7rT0NQIs"}, [this](const auto& a) -> u64 {
        if (Current()->destructors_started)
            throw std::logic_error("pthread_exit from a thread destructor");
        HleScope::Current()->ExitThread(a[0]);
        return a[0];
    });
    bind({"EotR8a3ASf4", "aI+OeCz8xrQ"},
         [this](const auto&) -> u64 { return Current()->handle_va; });
    // Desktop LoadAndStartModule returns the original handle immediately when
    // a module is already loaded; neither DT_INIT nor pRes is touched again.
    // Resolve only through guest mounts. Never pass an untrusted absolute guest
    // path to Linker's host-filesystem fallback.
    handlers["wzvqT4UqKX8"] = [this](HleCallFrame& frame) -> Status {
        std::array<u64, 6> a{};
        for (size_t i = 0; i < a.size(); ++i) a[i] = frame.registers.Get(kSysVIntegerOrder[i]);
        auto finish = [&](s32 code) -> Status {
            frame.registers.Set(Gpr::Rax, u32(code));
            return Ok();
        };
        if (a[3] || a[4] || a[1] > 16 * 1024 * 1024)
            return finish(ORBIS_KERNEL_ERROR_EINVAL);
        if ((a[1] && !space.ValidateRange({GuestAddress{a[2]}, a[1]}, GuestPermission::Read)) ||
            (a[5] && !space.ValidateRange({GuestAddress{a[5]}, 4}, GuestPermission::Write)))
            return finish(ORBIS_KERNEL_ERROR_EFAULT);
        std::string name;
        for (u64 i = 0;; ++i) {
            char c{};
            if (!a[0] || a[0] > UINT64_MAX - i ||
                !space.ReadData(GuestAddress{a[0] + i}, std::as_writable_bytes(std::span{&c, 1})))
                return finish(ORBIS_KERNEL_ERROR_EFAULT);
            if (!c) break;
            if (i >= 1023) return finish(ORBIS_KERNEL_ERROR_ENAMETOOLONG);
            name += c;
        }
        if (name.empty()) return finish(ORBIS_KERNEL_ERROR_ENOENT);
        std::filesystem::path path(name);
        for (const auto& part : path)
            if (part == "..") return finish(ORBIS_KERNEL_ERROR_EACCES);
        if (!path.is_absolute()) path = std::filesystem::path("/app0") / path;
        const auto guest = path.lexically_normal().generic_string();
        if (guest.size() > 255) return finish(ORBIS_KERNEL_ERROR_ENAMETOOLONG);
        const auto mount = mounts.GetMountSnapshot(guest);
        if (!mount) return finish(ORBIS_KERNEL_ERROR_ENOENT);
        const auto start = [&](u32 id) -> Status {
            const auto result = StartModule(id, a[1], a[2]);
            if (result.error) return finish(result.error);
            // Revalidate after callbacks: no pin may be held while guest DT_INIT
            // runs or waits for another owner. Preserve repeated-load pRes.
            if (result.initialized && a[5] &&
                !space.WriteData(GuestAddress{a[5]}, std::as_bytes(std::span{&result.value, 1})))
                return finish(ORBIS_KERNEL_ERROR_EFAULT);
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_INFO, "ProductionModule",
                "LoadStartModule path=%s handle=%u initialized=%d result=%d",
                guest.c_str(), id, result.initialized, result.value);
#endif
            return finish(id);
        };
        // Packaging may lowercase PRX names or retain a .debug_prx import.
        // Dynamic lookups use the same bounded identity rule as DT_NEEDED,
        // restricted to the requested directory and already prepared images.
        const auto alias = [&](const std::filesystem::path& requested) -> std::optional<s32> {
            std::optional<s32> result;
            for (s32 id = 0; auto* module = linker->GetModule(id); ++id) {
                const auto file = module->file.lexically_normal();
                if (file.parent_path() != requested.parent_path() ||
                    GuestModuleNameKey(file) != GuestModuleNameKey(requested)) continue;
                if (!std::ranges::any_of(module->GetExportModules(), [&](const auto& m) {
                        return GuestModuleAliasIdentity(requested, m.name);
                    })) continue;
                if (result) return ORBIS_KERNEL_ERROR_EINVAL;
                result = id;
            }
            return result;
        };
        if (FileSys::SplitArchivePath(mount->host_path)) {
            for (s32 id = 0; auto* module = linker->GetModule(id); ++id)
                if (module->file.lexically_normal().generic_string() == guest) return start(id);
            if (const auto id = alias(std::filesystem::path(guest)))
                return *id < 0 ? finish(*id) : start(*id);
            return finish(mounts.Exists(guest) ? ORBIS_KERNEL_ERROR_ENOTSUP : ORBIS_KERNEL_ERROR_ENOENT);
        }
        std::error_code ec;
        const auto root = std::filesystem::weakly_canonical(mount->host_path, ec);
        if (ec) return finish(ORBIS_KERNEL_ERROR_EIO);
        const auto candidate = std::filesystem::weakly_canonical(mounts.GetHostPath(guest), ec);
        if (ec) return finish(ORBIS_KERNEL_ERROR_EIO);
        const auto relative = candidate.lexically_relative(root);
        if (relative.empty() || *relative.begin() == "..") return finish(ORBIS_KERNEL_ERROR_EACCES);
        for (s32 id = 0; auto* module = linker->GetModule(id); ++id) {
            if (std::filesystem::weakly_canonical(module->file, ec) == candidate && !ec) {
#if defined(__ANDROID__)
                __android_log_print(ANDROID_LOG_INFO, "ProductionModule", "LoadStartModule existing path=%s handle=%d", guest.c_str(), id);
#endif
                return start(id);
            }
        }
        if (const auto id = alias(candidate)) return *id < 0 ? finish(*id) : start(*id);
        if (!std::filesystem::exists(candidate, ec))
            return finish(ec ? ORBIS_KERNEL_ERROR_EIO : ORBIS_KERNEL_ERROR_ENOENT);
        if (!std::filesystem::is_regular_file(candidate, ec))
            return finish(ec ? ORBIS_KERNEL_ERROR_EIO : ORBIS_KERNEL_ERROR_EACCES);
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "ProductionModule", "LoadStartModule new path=%s args=%llu", guest.c_str(), (unsigned long long)a[1]);
#endif
        // Only the prepared content graph can be activated. Live TLS extension
        // and arbitrary file loads remain unsupported, without misreporting ENOENT.
        return finish(ORBIS_KERNEL_ERROR_ENOTSUP);
    };
    for (const auto nid : {"RpQJJVKTiFM", "4fU5yvOkVG4", "f7KBOafysXo"}) {
        bind({nid}, [this, nid = std::string_view{nid}](const auto& a) -> u64 {
            const auto result = DispatchModuleInfo(space, nid != "f7KBOafysXo",
                nid == "4fU5yvOkVG4", a[0], s32(a[1]), a[2],
                [this](u64 address) -> std::optional<OrbisKernelModuleInfoEx> {
                    if (auto* module = linker->FindByAddress(address)) return module->GetModuleInfoEx();
                    return std::nullopt;
                }, Libraries::SysModule::shouldHideName);

            return result;
        });
    }
    bind({"crb5j7mkk1c"}, [this](const auto& a) -> u64 {
        return ClassifyGuestSignalReturn(space, a[0]);
    });
    handlers["Wl2o5hOVZdw"] = [this](HleCallFrame& frame) -> Status {
        const auto trace = CaptureGuestBacktrace(space, frame.registers);
        const std::string_view prefix{trace.prefix.data(), trace.prefix_size};
        LOG_INFO(Tty, "GUEST_BACKTRACE begin prefix={} prefix_status={} rsp={:#x} rbp={:#x}",
                 prefix, trace.prefix_status, frame.registers.Get(Gpr::Rsp),
                 frame.registers.Get(Gpr::Rbp));
        for (size_t i = 0; i < trace.count; ++i) {
            const u64 pc = trace.addresses[i];
            // Return PCs can point one byte beyond an executable segment.
            if (auto* module = linker->FindByAddress(pc ? pc - 1 : 0)) {
                LOG_INFO(Tty, "GUEST_BACKTRACE {} frame={} pc={:#x} module={} base={:#x} offset={:#x}",
                         prefix, i, pc, module->file.filename().string(), module->GetBaseAddress(),
                         pc - module->GetBaseAddress());
            } else {
                LOG_INFO(Tty, "GUEST_BACKTRACE {} frame={} pc={:#x} module=unknown", prefix, i, pc);
            }
        }
        LOG_INFO(Tty, "GUEST_BACKTRACE end count={} stop={} at={:#x}", trace.count, trace.stop,
                 trace.stopped_at);
        // Diagnostic, void ABI. No guest memory/register mutation, exception
        // swallowing, or change to the caller's error/abort control flow.
        return Ok();
    };
    for (const auto nid : AioNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            if (!aio) return u32(ORBIS_KERNEL_ERROR_ENXIO);
            std::array<u64, 6> args{}; std::copy_n(a.begin(), 6, args.begin());
            return aio->Dispatch(nid, args, HleScope::Current()->CancellationToken());
        });
    }
    for (const auto& entry : RemoteServiceEntries) {
        bind({entry.nid.data()}, [this, entry](const auto& a) -> u64 {
            std::array<u64, 6> args{}; std::copy_n(a.begin(), 6, args.begin());
            const auto result = DispatchRemoteService(entry, args);
            PrepareStage(fmt::format("RemoteService no_provider nid={} result={:#x}", entry.nid, result));
            return result;
        });
    }
    for (const auto nid : LiveStreamingUnavailableNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            std::array<u64, 6> args{}; std::copy_n(a.begin(), 6, args.begin());
            const auto result = DispatchLiveStreamingUnavailable(nid, args);
            PrepareStage(fmt::format("GameLiveStreaming no_provider nid={} result={:#x}", nid, result));
            return result;
        });
    }
    for (const auto nid : AudioInputNids) {
        bind({nid.data()}, [nid](const auto& a) -> u64 {
            std::array<u64, 6> args{}; std::copy_n(a.begin(), 6, args.begin());
            return DispatchAudioInput(nid, args);
        });
    }
    for (const auto nid : CoredumpUnavailableNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            const auto result = DispatchUnavailableCoredump(nid, a[0], a[1]);
            PrepareStage(fmt::format("Coredump no_provider nid={} result={:#x}", nid, result));
            return result;
        });
    }
    bind({"LwG8g3niqwA"}, [this](const auto& a) -> u64 {
        auto* module = linker->GetModule(s32(a[0]));
        if (!module || !module_lifecycle.Visible(s32(a[0])))
            return u32(ORBIS_KERNEL_ERROR_ESRCH);

        if (!space.ValidateRange({GuestAddress{a[2]}, 8}, GuestPermission::Write))
            return u32(ORBIS_KERNEL_ERROR_EFAULT);
        std::string name;
        for (u64 i = 0;; ++i) {
            char c{};
            if (!a[1] || a[1] > UINT64_MAX - i ||
                !space.ReadData(GuestAddress{a[1] + i}, std::as_writable_bytes(std::span{&c, 1})))
                return u32(ORBIS_KERNEL_ERROR_EFAULT);
            if (!c) break;
            if (i >= 1023) return u32(ORBIS_KERNEL_ERROR_ENAMETOOLONG);
            name += c;
        }
        const u64 address = reinterpret_cast<u64>(module->FindByName(name));
        if (address && !space.ValidateRange({GuestAddress{address}, 1}, GuestPermission::Read))
            return u32(ORBIS_KERNEL_ERROR_EFAULT);
        Write(a[2], address); // guest export (or null), never HLE/native host lookup
        return address ? 0 : u32(ORBIS_KERNEL_ERROR_ESRCH);
    });
    bind({"vNe1w4diLCs"}, [this](const auto& a) -> u64 {
        struct Index {
            u64 module, offset;
        };
        auto index = Read<Index>(a[0]);
        return TlsAddress(index.module, index.offset);
    });
    bind({"pO96TwzOm5E"}, [this](const auto&) { return memory->GetTotalDirectSize(); });
    bind({"aNz11fnnzi4"}, [this](const auto& a) -> u64 {
        if (!space.ValidateRange({GuestAddress{a[0]}, 8}, GuestPermission::Write))
            return ORBIS_KERNEL_ERROR_EFAULT;
        Write(a[0], memory->GetAvailableFlexibleSize());
        return 0;
    });
    bind({"n1-v6FgU7MQ"}, [this](const auto& a) -> u64 {
        if (!space.ValidateRange({GuestAddress{a[0]}, 8}, GuestPermission::Write))
            return ORBIS_KERNEL_ERROR_EFAULT;
        Write(a[0], memory->GetTotalFlexibleSize());
        return 0;
    });
    bind({"rTXw65xmLIA"}, [this](const auto& a) -> u64 {
        if (!space.ValidateRange({GuestAddress{a[5]}, 8}, GuestPermission::Write))
            return ORBIS_KERNEL_ERROR_EFAULT;
        s64 physical{};
        s32 result;
        {
            Common::Profiler::Scope allocation("VM.AllocatePhysical");
            result = Libraries::Kernel::sceKernelAllocateDirectMemory(a[0], a[1], a[2], a[3], a[4],
                                                                      &physical);
        }
        if (!result)
            Write(a[5], physical);
        return static_cast<u32>(result);
    });
    for (const auto nid : VideoEventGetterNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            return DispatchVideoEventGetter(space, nid, a[0], a[1]);
        });
    }
    bind({"L-Q3LEjIbgA"}, [this](const auto& a) -> u64 {
        if (!space.ValidateRange({GuestAddress{a[0]}, 8}, GuestPermission::Write))
            return ORBIS_KERNEL_ERROR_EFAULT;
        auto address = reinterpret_cast<void*>(Read<u64>(a[0]));
        s32 result;
        {
            result =
                Libraries::Kernel::sceKernelMapDirectMemory(&address, a[1], a[2], a[3], a[4], a[5]);
        }
        if (!result)
            Write(a[0], reinterpret_cast<u64>(address));
        else
            PrepareStage(fmt::format("MapDirectMemory failed in={:#x} len={:#x} prot={:#x} flags={:#x} physical={:#x} alignment={:#x} result={:#x}",
                Read<u64>(a[0]), a[1], a[2], a[3], a[4], a[5], u32(result)));
        return static_cast<u32>(result);
    });
    handlers["NcaWUxfMNIQ"] = [this](HleCallFrame& frame) {
        CallCursor cursor(frame);
        std::array<u64, 7> a{};
        for (auto& value : a)
            value = Require(cursor.NextInteger());
        if (!space.ValidateRange({GuestAddress{a[0]}, 8}, GuestPermission::Write)) {
            frame.registers.Set(Gpr::Rax, ORBIS_KERNEL_ERROR_EFAULT);
            return Ok();
        }
        auto name = String(a[6], 33);
        auto address = reinterpret_cast<void*>(Read<u64>(a[0]));
        s32 result;
        {
            result = Libraries::Kernel::sceKernelMapNamedDirectMemory(&address, a[1], a[2], a[3],
                                                                      a[4], a[5], name.c_str());
        }
        if (!result)
            Write(a[0], reinterpret_cast<u64>(address));
        frame.registers.Set(Gpr::Rax, static_cast<u32>(result));
        return Ok();
    };
    bind({"mL8NDH86iQI"}, [this](const auto& a) -> u64 {
        Require(space.ValidateRange({GuestAddress{a[0]}, sizeof(u64)}, GuestPermission::Write));
        auto address = Read<u64>(a[0]);
        auto name = String(a[4]);
        s32 result;
        {
            void* out = reinterpret_cast<void*>(address);

            result = Libraries::Kernel::sceKernelMapNamedFlexibleMemory(&out, a[1], a[2], a[3],
                                                                        name.c_str());
            address = reinterpret_cast<u64>(out);
        }
        if (!result)
            Write(a[0], address);
        return static_cast<u32>(result);
    });
    for (const auto nid : {"IWIBBdTHit4", "7oxv3PPCumo"}) {
        bind({nid}, [this, reserve = std::string_view(nid) == "7oxv3PPCumo"](const auto& a) -> u64 {
            u64 address{};
            if (!space.ReadData(GuestAddress{a[0]}, std::as_writable_bytes(std::span{&address, 1})) ||
                !space.ValidateRange({GuestAddress{a[0]}, 8}, GuestPermission::Write))
                return ORBIS_KERNEL_ERROR_EFAULT;
            void* out = reinterpret_cast<void*>(address);
            const s32 result = reserve
                ? Libraries::Kernel::sceKernelReserveVirtualRange(&out, a[1], a[2], a[3])
                : Libraries::Kernel::sceKernelMapFlexibleMemory(&out, a[1], a[2], a[3]);
            if (!result) Write(a[0], reinterpret_cast<u64>(out));
            return u32(result);
        });
    }
    bind({"MBuItvba6z8"}, [](const auto& a) -> u64 {
        return u32(Libraries::Kernel::sceKernelReleaseDirectMemory(a[0], a[1]));
    });
    bind({"rVjRvHJ0X6c"}, [this](const auto& a) -> u64 {
        Libraries::Kernel::OrbisVirtualQueryInfo info{};
        if (a[3] != sizeof(info)) return ORBIS_KERNEL_ERROR_EINVAL;
        auto pin = space.AcquireDataSpan({GuestAddress{a[2]}, sizeof(info)}, true);
        if (!pin) return ORBIS_KERNEL_ERROR_EFAULT;
        const auto result = Libraries::Kernel::sceKernelVirtualQuery(
            reinterpret_cast<void*>(a[0]), a[1], &info, sizeof(info));
        if (!result) std::memcpy(pin.Value().WritableBytes().data(), &info, sizeof(info));
        return u32(result);
    });
    for (const auto nid : DirectMemoryServiceNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            const auto result = DispatchDirectMemoryService(space, nid, a);
            if (result) PrepareStage(fmt::format(
                "DirectMemory failed nid={} args={:#x},{:#x},{:#x},{:#x},{:#x},{:#x} result={:#x}",
                nid, a[0], a[1], a[2], a[3], a[4], a[5], result));
            return result;
        });
    }
    for (auto nid : MemoryServiceNids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) -> Status {
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
            auto result = DispatchMemoryService(space, *memory, *storage, nid, args,
                                                [this](int e) { return PosixFailure(e); });
            if (!result) return result.GetError();
            frame.registers.Set(Gpr::Rax, result.Value());
            return Ok();
        };
    }
    for (const auto nid : {"WFcfL2lzido", "BC+OG5m9+bw"}) {
        bind({nid}, [this, direct = std::string_view(nid) == "BC+OG5m9+bw"](const auto& a) -> u64 {
            // QueryProtection permits any output to be omitted, including all three.
            // GetDirectMemoryType requires its outputs. Preserve each desktop ABI.
            const std::array<u64, 3> addresses{a[direct ? 2 : 1], a[direct ? 3 : 2],
                                                a[direct ? 1 : 3]};
            std::vector<GuestAddressSpace::DataRequest> requests;
            for (size_t i = 0; i < addresses.size(); ++i) {
                if (direct || addresses[i])
                    requests.push_back({{GuestAddress{addresses[i]}, i == 2 ? 4u : 8u},
                                        GuestPermission::Write});
            }
            auto pins = space.AcquireDataBatch(requests);
            if (!pins) return ORBIS_KERNEL_ERROR_EFAULT;
            void *start{}, *end{};
            u32 value{};
            s32 type{};
            const auto result = direct
                ? Libraries::Kernel::sceKernelGetDirectMemoryType(a[0], &type, &start, &end)
                : Libraries::Kernel::sceKernelQueryMemoryProtection(reinterpret_cast<void*>(a[0]), &start, &end, &value);
            if (!result) {
                if (direct) value = u32(type);
                const std::array<const void*, 3> values{&start, &end, &value};
                size_t pin_index = 0;
                for (size_t i = 0; i < addresses.size(); ++i) {
                    if (addresses[i])
                        std::memcpy(pins.Value()[pin_index++].WritableBytes().data(), values[i],
                                    i == 2 ? 4 : 8);
                }
            }
            return u32(result);
        });
    }
    for (const auto nid : {"2SKEx6bSq-4", "kBJzF8x4SyE"}) {
        bind({nid},[this,extended=std::string_view(nid)=="kBJzF8x4SyE"](const auto& a)->u64 {
            using Entry=Libraries::Kernel::OrbisKernelBatchMapEntry;
            if (s32(a[1])<0 || a[1]>65536) return ORBIS_KERNEL_ERROR_EINVAL;
            std::vector<Entry> entries(a[1]);
            std::vector<GuestAddressSpace::DataRequest> outputs;
            std::vector<std::vector<GuestAddressSpace::MappingIdentity>> identities;
            if (a[1]) outputs.push_back({{GuestAddress{a[0]},a[1]*sizeof(Entry)},GuestPermission::Read|GuestPermission::Write});
            if (a[2]) outputs.push_back({{GuestAddress{a[2]},4},GuestPermission::Write});
            {
                auto pins=space.AcquireDataBatch(outputs);
                if (!pins) return ORBIS_KERNEL_ERROR_EFAULT;
                if (!entries.empty()) std::memcpy(entries.data(),pins.Value()[0].Bytes().data(),entries.size()*sizeof(Entry));
                identities.resize(outputs.size());
                for (size_t i=0;i<outputs.size();++i) {
                    const auto range=outputs[i].range;
                    for (u64 cursor=range.base.value;cursor<range.End();) {
                        auto mapped=space.Query(GuestAddress{cursor});
                        if (!mapped) return ORBIS_KERNEL_ERROR_EFAULT;
                        const u64 end=std::min(range.End(),mapped.Value().range.End());
                        identities[i].push_back({cursor,end,mapped.Value().mapping_generation});
                        cursor=end;
                    }
                }
            } // Native mapping operations must never run with a guest output pin.
            for (const auto& entry : entries) {
                if (entry.operation<0 || entry.operation>4) return ORBIS_KERNEL_ERROR_EINVAL;
                const u64 begin=reinterpret_cast<u64>(entry.start);
                if (entry.length>UINT64_MAX-begin) return ORBIS_KERNEL_ERROR_EINVAL;
                for (const auto& out : outputs)
                    if (begin<out.range.End() && out.range.base.value<begin+entry.length)
                        return ORBIS_KERNEL_ERROR_EINVAL;
            }
            s32 processed{};
            const s32 result=extended ? Libraries::Kernel::sceKernelBatchMap2(entries.data(),s32(a[1]),&processed,s32(a[3]))
                                      : Libraries::Kernel::sceKernelBatchMap(entries.data(),s32(a[1]),&processed);
            for (size_t i=0;i<outputs.size();++i) outputs[i].identities=identities[i];
            auto pins=space.AcquireDataBatch(outputs);
            if (!pins) return ORBIS_KERNEL_ERROR_EFAULT;
            if (!entries.empty()) std::memcpy(pins.Value()[0].WritableBytes().data(),entries.data(),entries.size()*sizeof(Entry));
            if (a[2]) std::memcpy(pins.Value().back().WritableBytes().data(),&processed,4);
            return u32(result);
        });
    }
    bind({"cQke9UuBQOk"}, [this](const auto& a) -> u64 {
        return static_cast<u32>(
            Libraries::Kernel::sceKernelMunmap(reinterpret_cast<void*>(a[0]), a[1]));
    });
    bind({"vSMAm3cxYTY"}, [this](const auto& a) -> u64 {
        return static_cast<u32>(
            Libraries::Kernel::sceKernelMprotect(reinterpret_cast<void*>(a[0]), a[1], a[2]));
    });
    bind({"9bfdLIyuwCY"}, [](const auto& a) -> u64 {
        return u32(Libraries::Kernel::sceKernelMtypeprotect(
            reinterpret_cast<void*>(a[0]), a[1], s32(a[2]), s32(a[3])));
    });
    bind({"7Xl257M4VNI", "3PtV6p3QNX4"}, [](const auto& a) -> u64 { return a[0] == a[1]; });
    bind({"3eqs37G74-s", "EI-5-jlq2dE"}, [this](const auto&) -> u64 { return Current()->id; });
    bind({"B5GmVDKwpn0", "T72hz6ffq08", "6XG4B33N09g"}, [](const auto&) -> u64 {
        std::this_thread::yield(); return 0;
    });
    auto get_scheduling = [this](const auto& a) -> u64 {
        const std::array<GuestAddressSpace::DataRequest, 2> requests{
            {{{GuestAddress{a[1]}, 4}, GuestPermission::Write},
             {{GuestAddress{a[2]}, 4}, GuestPermission::Write}}};
        auto pinned = space.AcquireDataBatch(requests);
        if (!pinned)
            return POSIX_EFAULT;
        std::lock_guard lock(threads_mutex);
        for (const auto& [id, owner] : owners) {
            if (owner->handle_va != a[0] || owner->finished)
                continue;
            std::memcpy(pinned.Value()[0].WritableBytes().data(), &owner->attributes.policy, 4);
            std::memcpy(pinned.Value()[1].WritableBytes().data(), &owner->attributes.priority, 4);
            return 0;
        }
        return POSIX_ESRCH;
    };
    auto set_scheduling = [this](const auto& a) -> u64 {
        s32 priority{};
        if (!space.ReadData(GuestAddress{a[2]}, std::as_writable_bytes(std::span{&priority, 1})))
            return POSIX_EFAULT;
        if (!GuestThreadAttributeDomain::ValidPriority(a[1], priority)) return POSIX_EINVAL;
        std::lock_guard lock(threads_mutex);
        for (const auto& [id, owner] : owners) {
            if (owner->handle_va != a[0] || owner->finished) continue;
            owner->attributes.policy = a[1]; owner->attributes.priority = priority;
            return 0;
        }
        return POSIX_ESRCH;
    };
    bind({"FIs3-UQT9sg"}, get_scheduling);
    bind({"Xs9hdiD7sAA"}, set_scheduling);
    bind({"P41kTWUS3EI"}, [get_scheduling](const auto& a) -> u64 {
        const u64 error = get_scheduling(a); return error ? 0x80020000u | error : 0;
    });
    bind({"oIRFTjoILbg"}, [set_scheduling](const auto& a) -> u64 {
        const u64 error = set_scheduling(a); return error ? 0x80020000u | error : 0;
    });
    bind({"1tKyG7RlMJo"}, [this](const auto& a) -> u64 {
        auto pin=space.AcquireDataSpan({GuestAddress{a[1]},4},true);
        if (!a[1] || !pin) return ORBIS_KERNEL_ERROR_EFAULT;
        std::lock_guard lock(threads_mutex);
        for (const auto& [id,o] : owners) {
            if (o->handle_va!=a[0] || o->finished) continue;
            std::memcpy(pin.Value().WritableBytes().data(),&o->attributes.priority,4);
            return 0;
        }
        return ORBIS_KERNEL_ERROR_ESRCH;
    });
    auto set_priority=[this](const auto& a) -> u64 {
        std::lock_guard lock(threads_mutex);
        for (const auto& [id,o] : owners) {
            if (o->handle_va!=a[0] || o->finished) continue;
            // Desktop stores the guest priority; it does not promote Android owners to RT.
            o->attributes.priority=s32(a[1]);
            return 0;
        }
        return POSIX_ESRCH;
    };
    bind({"a2P9wYGeZvc"},set_priority);
    bind({"W0Hpm2X0uPE"},[set_priority](const auto& a)->u64 {
        const auto result=set_priority(a); return result ? 0x80020000u|result : 0;
    });
    auto rename=[this](const auto& a) -> u64 {
        std::string name;
        if (a[1]) {
            for (u64 i=0; i<256; ++i) {
                char c{};
                if (a[1]>UINT64_MAX-i || !space.ReadData(GuestAddress{a[1]+i},std::as_writable_bytes(std::span{&c,1}))) return POSIX_EFAULT;
                if (!c) break;
                if (i==255) return POSIX_ENAMETOOLONG;
                name+=c;
            }
        }
        const auto self=Current();
        std::lock_guard lock(threads_mutex);
        for (const auto& [id,o] : owners) {
            if (o->handle_va!=a[0] || o->finished) continue;
            o->name=std::move(name);
            if (o==self) Common::SetCurrentThreadName(o->name.c_str());
            else if (o->worker.joinable()) Common::SetThreadName(reinterpret_cast<void*>(o->worker.native_handle()),o->name.c_str());
            return 0;
        }
        return POSIX_ESRCH;
    };
    bind({"9vyP6Z7bqzc","oxMp8uPqa+U"},rename);
    bind({"GBUY7ywdULE"},[rename](const auto& a)->u64 {
        const auto result=rename(a); return result ? 0x80020000u|result : 0;
    });
    bind({"How7B8Oet6k"},[this](const auto& a)->u64 {
        auto pin=space.AcquireDataSpan({GuestAddress{a[1]},32},true);
        if (!a[1] || !pin) return ORBIS_KERNEL_ERROR_EFAULT;
        std::lock_guard lock(threads_mutex);
        for (const auto& [id,o] : owners) {
            if (o->handle_va!=a[0] || o->finished) continue;
            const auto count=std::min<size_t>(32,o->name.size());
            std::memcpy(pin.Value().WritableBytes().data(),o->name.data(),count);
            return 0;
        }
        return ORBIS_KERNEL_ERROR_ESRCH;
    });
    auto affinity = [this](const std::array<u64, 6>& a, bool get, bool scalar) -> int {
        if (!a[0]) return POSIX_EINVAL;
        if (!scalar && !a[2]) return POSIX_EINVAL;
        u64 mask{};
        if (!get) {
            if (int error = GuestThreadAttributeDomain::ReadAffinity(space, scalar, a[1], a[2], mask))
                return error;
        }
        bool found{};
        {
            std::lock_guard lock(threads_mutex);
            for (const auto& [id, owner] : owners) {
                if (owner->handle_va != a[0] || owner->finished) continue;
                if (get) mask = owner->attributes.affinity;
                else owner->attributes.affinity = mask;
                found = true;
                break;
            }
        }
        if (!found) return POSIX_ESRCH;
        return get ? GuestThreadAttributeDomain::WriteAffinity(space, scalar, a[1], a[2], mask) : 0;
    };
    bind({"Jb2uGFMr688"}, [affinity](const auto& a) -> u64 { return affinity(a, true, false); });
    bind({"5KWrg7-ZqvE"}, [affinity](const auto& a) -> u64 { return affinity(a, false, false); });
    bind({"rcrVFJsQWRY"}, [affinity](const auto& a) -> u64 {
        const int error = affinity(a, true, true); return error ? 0x80020000u | error : 0;
    });
    bind({"bt3CTBKmGyI"}, [affinity](const auto& a) -> u64 {
        const int error = affinity(a, false, true); return error ? 0x80020000u | error : 0;
    });
    auto create_thread = [this](const auto& a) -> u64 {
        GuestThreadAttributes attributes{};
        if (int error = thread_attributes->Snapshot(a[1], attributes)) return error;
        if (attributes.stack &&
            (attributes.stack % 16 || attributes.stack > UINT64_MAX - attributes.size ||
             !space.ValidateRange({GuestAddress{attributes.stack}, attributes.size},
                                  GuestPermission::Read | GuestPermission::Write)))
            return POSIX_EINVAL;
        // These are guest scheduling attributes, as in the desktop emulator;
        // they do not claim Android real-time scheduler privileges.
        if (attributes.inherit == 4) {
            auto current = Current();
            std::lock_guard lock(threads_mutex);
            attributes.policy = current->attributes.policy;
            attributes.priority = current->attributes.priority;
            attributes.scope = current->attributes.scope;
        }
        Require(space.ValidateRange({GuestAddress{a[0]}, sizeof(u64)}, GuestPermission::Write));
        auto code = Require(space.Query(GuestAddress{a[2]}));
        if (!HasPermission(code.permission, GuestPermission::Execute))
            return POSIX_EINVAL;
        std::shared_ptr<Owner> o;
        try { o = NewOwner(attributes); }
        catch (const std::exception&) { return POSIX_EAGAIN; }
        try {
            Write(a[0], o->handle_va);
            // Publish the native worker under the same lock used by join. A
            // guest may share its output handle before pthread_create returns.
            std::lock_guard publish(threads_mutex);
            o->worker = std::thread([this, o, entry = a[2], arg = a[3]] {
                {
                    std::lock_guard published(threads_mutex);
                }
                try {
                    // Match desktop Pthread::SetAffinity: preserve the guest mask
                    // without pinning host threads to heterogeneous Android cores.
                    Attach(o, entry);
                    {
                        std::lock_guard lock(threads_mutex);
                        o->startup_done = true;
                    }
                    threads_changed.notify_all();
                    GuestCallArgs args;
                    args.values[0] = arg;
                    args.count = 1;
                    auto result = Call(entry, args);
                    if (result)
                        o->result = result.Value();
                    else
                        o->error = result.GetError();
                } catch (const std::exception& e) {
                    o->error = MakeError(ErrorCategory::BackendFailure, "GuestThread", e.what());
                    {
                        std::lock_guard lock(threads_mutex);
                        if (!o->startup_done) { o->startup_error = POSIX_EAGAIN; o->startup_done = true; }
                    }
                    threads_changed.notify_all();
                }
                try {
                    Finish(o);
                } catch (const std::exception& e) {
                    std::lock_guard lock(threads_mutex);
                    o->error =
                        MakeError(ErrorCategory::BackendFailure, "GuestThread::Finish", e.what());
                    o->finished = true;
                    threads_changed.notify_all();
                }
            });
        } catch (...) {
            {
                std::lock_guard lock(threads_mutex);
                o->finished = true;
            }
            threads_changed.notify_all();
            Write(a[0], u64{0});
            return POSIX_EAGAIN;
        }
        // Report a native startup failure to the creator, instead of returning
        // success and leaving a silently dead child. No guest pin spans this wait.
        std::unique_lock started(threads_mutex);
        while (!o->startup_done) {
            if (HleScope::Current()->CancellationToken().stop_requested()) return POSIX_EINTR;
            threads_changed.wait_for(started, std::chrono::milliseconds(1));
        }
        const int error = o->startup_error;
        started.unlock();
        if (error) Write(a[0], u64{});
        return error;
    };
    bind({"OxhIB8LB-PQ", "Jmi+9w9u0E4"}, create_thread);
    bind({"6UgtwV+0zb4"}, [create_thread](const auto& a) -> u64 {
        const u64 error = create_thread(a);
        return error ? 0x80020000u | error : 0;
    });
    bind({"h9CcP3J0oVM", "onNY9Byn-W8"}, [this](const auto& a) -> u64 {
        if (a[1])
            Require(space.ValidateRange({GuestAddress{a[1]}, sizeof(u64)}, GuestPermission::Write));
        auto self = Current();
        std::shared_ptr<Owner> target;
        {
            std::lock_guard lock(threads_mutex);
            for (auto& [id, o] : owners)
                if (o->handle_va == a[0])
                    target = o;
            if (!target)
                return POSIX_ESRCH;
            if (target == self)
                return POSIX_EDEADLK;
            if (target->joined || target->detached)
                return POSIX_EINVAL;
            target->joined = true;
        }
        std::unique_lock lock(threads_mutex);
        while (!target->finished) {
            if (HleScope::Current()->CancellationToken().stop_requested()) {
                target->joined = false;
                return POSIX_EINTR;
            }
            threads_changed.wait_for(lock, std::chrono::milliseconds(5));
        }
        auto result = target->result;
        auto error = target->error;
        lock.unlock();
        if (target->worker.joinable())
            target->worker.join();
        if (error)
            throw std::runtime_error(Describe(*error));
        if (!result || result->reason != StopReason::Returned)
            throw std::runtime_error("joined guest thread did not return normally");
        if (a[1])
            Write(a[1], result->return_value);
        return 0;
    });
    bind({"+U1R4WtXvoc", "4qGrR6eoP9Y"}, [this](const auto& a) -> u64 {
        std::lock_guard lock(threads_mutex);
        for (auto& [id, o] : owners)
            if (o->handle_va == a[0]) {
                if (o->joined || o->detached)
                    return POSIX_EINVAL;
                o->detached = true;
                return 0;
            }
        return POSIX_ESRCH;
    });
    bind({"Z4QosVuAsA0", "14bOACANTBo"}, [this](const auto& a) -> u64 {
        if (a[0] % alignof(u32))
            return POSIX_EINVAL;
        // Never carry a guest pin across InvokeGuest: the initializer may itself
        // map memory or publish code. The host record coordinates owners; the
        // guest completion word remains authoritative after the callback returns.
        for (;;) {
            {
                std::lock_guard lock(once_mutex);
                if (Read<u32>(a[0]) == 1)
                    return 0;
                if (!once_owners.contains(a[0])) {
                    once_owners[a[0]] = Current()->id;
                    break;
                }
                if (once_owners[a[0]] == Current()->id)
                    return POSIX_EDEADLK;
            }
            if (!HleScope::Current()->WaitFor(std::chrono::milliseconds(1)))
                return POSIX_EINTR;
        }
        auto release = [&](void*) {
            std::lock_guard lock(once_mutex);
            once_owners.erase(a[0]);
        };
        std::unique_ptr<void, decltype(release)> owned(reinterpret_cast<void*>(1), release);
        GuestCallArgs args;
        auto result = Call(a[1], args);
        if (!result)
            throw std::runtime_error(Describe(result.GetError()));
        if (result.Value().reason != StopReason::Returned ||
            HleScope::Current()->ThreadExitResult())
            return POSIX_EINTR;
        Write(a[0], u32{1});
        return 0;
    });
    // Session-owned libSceSysmodule. Not the desktop global module table: a load
    // succeeds only when this session graph provides the module, and a missing
    // provider fails by name. The public API takes a u16 id; the internal API a
    // u32 id. Both resolve through the same table and refcount.
    bind({"g8cM39EUZ6o"},
         [this](const auto& a) -> u64 { return SysmoduleLoad(static_cast<u16>(a[0])); });
    bind({"39iV5E1HoCk"},
         [this](const auto& a) -> u64 { return SysmoduleLoad(static_cast<u32>(a[0])); });
    bind({"eR2bZFAAU0Q"},
         [this](const auto& a) -> u64 { return SysmoduleUnload(static_cast<u16>(a[0])); });
    bind({"fMP5NHUOaMk"},
         [this](const auto& a) -> u64 { return SysmoduleIsLoaded(static_cast<u16>(a[0])); });
    bind({"ynFKQ5bfGks"},
         [this](const auto& a) -> u64 { return SysmoduleIsLoaded(static_cast<u32>(a[0])); });
    bind({"D8cuU4d72xM"},
         [this](const auto& a) -> u64 { return SysmoduleGetHandle(static_cast<u32>(a[0]), a[1]); });
    bind({"hHrGoGoNf+s"}, [this](const auto& a) -> u64 {
        // A short reference/output transaction; no guest callback or wait with pins.
        sysmodules.PublishSbsVrProviders(GuestVrSensor::Instance().Read().enabled);
        const auto result = LoadInitializedSysmodule(sysmodules, space, a);
        LOG_INFO(Lib_SysModule, "session WithArg id={:#x} bytes={} result={:#x}", u32(a[0]),
                 s32(a[1]), result);
        return result;
    });
    for (auto nid : AudioNids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i) args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
            frame.registers.Set(Gpr::Rax, audio->Dispatch(nid, args, HleScope::Current()->CancellationToken()));
            return Ok();
        };
    }
    for (auto nid : Audio3dNids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i) args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
            frame.registers.Set(Gpr::Rax, audio3d->Dispatch(nid, args, HleScope::Current()->CancellationToken()));
            return Ok();
        };
    }
    for (auto nid : AjmNids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            std::array<u64, 10> args{};
            const size_t count = nid == "dmDybN--Fn8" ? 8 :
                (nid == "ElslOCpOIns" || nid == "7jdAXK+2fMo") ? 10 :
                nid == "fFFkk0xfGWs" ? 6 :
                (nid == "stlghnic3Jc" || nid == "-qLsfDAywIY" || nid == "AxoDrINp4J8" || nid == "eDFeTyi+G3Y") ? 4 :
                (nid == "Q3dyFuwGn64" || nid == "bkRHEYG6lEM") ? 3 :
                nid == "diXjQNiMu-s" ? 1 : 2;
            CallCursor cursor(frame);
            for (size_t i = 0; i < count; ++i) {
                auto value = cursor.NextInteger();
                if (!value) return Status(value.GetError());
                args[i] = value.Value();
            }
            frame.registers.Set(Gpr::Rax, ajm->Dispatch(nid, args, HleScope::Current()->CancellationToken()));
            return Ok();
        };
    }
    for (auto nid : PadNids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);

            frame.registers.Set(Gpr::Rax, pad->Dispatch(space, nid, args));
            return Ok();
        };
    }
    for (auto nid : MouseNids) {
        handlers[std::string(nid)] = [nid](HleCallFrame& frame) {
            // Keep the no-provider boundary explicit. Reads/opens do not
            // fabricate a device handle or guest mouse samples.
            frame.registers.Set(
                Gpr::Rax, nid == "cAnT0Rw-IwU" ? u32(ORBIS_MOUSE_ERROR_INVALID_HANDLE)
                                               : u32(ORBIS_MOUSE_ERROR_NOT_INITIALIZED));
            return Ok();
        };
    }
    for (const auto nid : AvPlayerNids) {
        bind({nid.data()},
             [this, nid](const auto& a) -> u64 { return avplayer->Dispatch(nid, a); });
    }
    for (const auto nid : GuestErrorDialog::Nids)
        bind({nid.data()}, [this, nid](const auto& a) -> u64 { return error_dialog.Invoke(space, nid, a); });
    for (const auto nid : GuestImeKeyboard::Nids)
        bind({nid.data()}, [this, nid](const auto& a) -> u64 { return ime_keyboard.Invoke(space, nid, a); });
    for (const auto nid : NpTusOfflineNids)
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            return DispatchNpTusOffline(nid, a, s32(elf_info.CompiledSdkVer()));
        });
    for (const auto nid : NpScoreOfflineNids)
        bind({nid.data()}, [nid](const auto& a) -> u64 { return DispatchNpScoreOffline(nid, a); });
    for (const auto nid : NpUtilityNids)
        bind({nid.data()}, [this, nid](const auto& a) -> u64 { return np_utility->Dispatch(space,nid,a); });
    for (const auto nid : NpWebApiControlNids) {
        if (nid == "gVNNyxf-1Sg") {
            handlers[std::string(nid)] = [this](HleCallFrame&) {
                np_webapi->CheckTimeout();
                return Ok();
            };
        } else {
            bind({nid.data()}, [this, nid](const auto& a) -> u64 { return np_webapi->Dispatch(space,nid,a); });
        }
    }
    for (const auto nid : NpControlNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 { return np->Dispatch(space,nid,a); });
    }
    for (const auto nid : NpOfflineNids) {
        bind({nid.data()},
             [this, nid,
              observed = std::make_shared<std::atomic_bool>(false)](const auto& a) -> u64 {
                 const auto result = np->Dispatch(space, nid, a);
                 if (!observed->exchange(true, std::memory_order_relaxed)) {
                     LOG_INFO(Lib_NpManager, "Session offline NP nid={} result={:#x}", nid, result);
                 }
                 return result;
             });
    }
    for (const auto nid : {"3Zl8BePTh9Y", "JELHf4xPufo"}) {
        handlers[nid] = [this](HleCallFrame& frame) -> Status {
            auto callbacks=np->control.BeginCallbacks();
            if (!callbacks) { frame.registers.Set(Gpr::Rax,u32(ORBIS_NP_ERROR_INVALID_ARGUMENT)); return Ok(); }
            auto end=[this](void*) { np->control.EndCallbacks(); };
            std::unique_ptr<void,decltype(end)> active(reinterpret_cast<void*>(1),end);
            for (const auto& cb : *callbacks) {
                if (!np->control.IsCurrent(cb)) continue;
                if (cb.unsupported_identity)
                    return Status(MakeError(ErrorCategory::Unsupported,"GuestNp","online callback in offline domain"));
                GuestCallArgs args;
                if (cb.kind != GuestNpControl::CallbackKind::Legacy) { args.values={u64(u32(cb.user)),cb.state,cb.argument}; args.count=3; }
                else { args.values={u64(u32(cb.user)),cb.state,0,cb.argument}; args.count=4; }
                auto result=Call(cb.function,args);
                if (!result) return Status(result.GetError());
                if (result.Value().reason!=StopReason::Returned || HleScope::Current()->ThreadExitResult()) {
                    if (result.Value().reason==StopReason::Cancelled || HleScope::Current()->ThreadExitResult()) return Ok();
                    return Status(MakeError(ErrorCategory::Unsupported,"GuestNp","callback did not return"));
                }
            }
            frame.registers.Set(Gpr::Rax,0); return Ok();
        };
    }
    for (auto nid : GuestSslOffline::Nids)
        bind({nid.data()}, [this, nid](const auto& a) -> u64 { return ssl->Dispatch(space, nid, a); });
    bind({"6xVpy0Fdq+I"}, [](const auto&) -> u64 {
        return u32(Libraries::Kernel::_sigprocmask());
    });
    for (const auto nid : TrophyNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            return trophy->Dispatch(space, nid, a);
        });
    }
    for (const auto nid : Matching2Nids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            return matching2->Dispatch(space, nid, a);
        });
    }
    for (const auto nid : PlayGoNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            return playgo->Dispatch(space, nid, a);
        });
    }
    for (const auto nid : HttpNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            return http->Dispatch(space, nid, a, HleScope::Current()->CancellationToken());
        });
    }
    for (const auto nid : Http2Nids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {

            const auto result = http2->Dispatch(space, nid, a);
            LOG_INFO(Lib_Http2, "Session offline HTTP2 nid={} id={} result={:#x}", nid, s32(a[0]), result);
            return result;
        });
    }
    auto network_handler = [this](std::string_view nid) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            if (!network)
                return Status(MakeError(ErrorCategory::Unsupported, "GuestNetwork",
                                        "missing session domain"));
            if (nid == "iQw3iQPhvUQ" || nid == "u5oqtlIP+Fw") {
                auto callbacks = network->BeginCallbacks(nid == "u5oqtlIP+Fw");
                if (!callbacks) {
                    frame.registers.Set(Gpr::Rax, u32(ORBIS_NET_CTL_ERROR_NOT_AVAIL));
                    return Ok();
                }
                auto end = [this](void*) { network->EndCallbacks(); };
                std::unique_ptr<void, decltype(end)> active(reinterpret_cast<void*>(1), end);
                // No VM/domain lock or guest pin may span a callback. Registration
                // revisions reject slots removed/reused by earlier callbacks.
                for (const auto& callback : *callbacks) {
                    if (!network->IsCurrent(callback))
                        continue;
                    GuestCallArgs args;
                    args.values = {u64(ORBIS_NET_CTL_EVENT_TYPE_DISCONNECTED), callback.argument};
                    args.count = 2;
                    auto result = Call(callback.function, args);
                    if (!result)
                        return Status(result.GetError());
                    if (result.Value().reason != StopReason::Returned ||
                        HleScope::Current()->ThreadExitResult()) {
                        // HleScope preserves the nested stop/fault for the outer
                        // dispatcher; do not swallow it or call another callback.
                        frame.registers.Set(Gpr::Rax, u32(ORBIS_NET_ERROR_EINTR));
                        return Ok();
                    }
                }
                frame.registers.Set(Gpr::Rax, 0);
                return Ok();
            }
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);

            // Independent guest TLS slots: net errno must not alias POSIX errno
            // or another owner. NewOwner reserves/zeros the final 64 TLS bytes.
            const u64 net_errno = Current()->handle_va + sizeof(u64) + sizeof(s32);
            frame.registers.Set(Gpr::Rax, network->Dispatch(space, nid, args, net_errno, HleScope::Current()->CancellationToken()));
            return Ok();
        };
    };
    bind({"PI7jIZj4pcE"}, [this](const auto& a) -> u64 { return GuestRandom(space, a[0], a[1]); });
    for (auto nid : PosixNetworkNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            const auto result = GuestPosixNetwork::Dispatch(*network, space, nid, a, ErrnoAddress(),
                [this](int error) { return PosixFailure(error); },
                [this](s32 fd) {
                    if (!storage) return -POSIX_EBADF;
                    const auto ready = storage->PollReady(fd);
                    return ready.error ? -Libraries::Kernel::NativeToPosixErrno(ready.error) : int(ready.value);
                }, HleScope::Current()->CancellationToken());
            return result;
        });
    }
    for (auto nid : NetNids)
        network_handler(nid);
    for (auto nid : NetCtlNids)
        network_handler(nid);
    for (auto nid : AppContentNids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            if (!app_content)
                return Status(MakeError(ErrorCategory::Unsupported, "AppContent",
                                        "missing installed title metadata"));
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);

            frame.registers.Set(Gpr::Rax, app_content->Dispatch(space, nid, args, storage.get()));
            return Ok();
        };
    }
    for (auto nid : RtcNids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);

            frame.registers.Set(Gpr::Rax,
                                DispatchRtc(space, clock, elf_info.CompiledSdkVer(), nid, args));
            return Ok();
        };
    }
    for (auto nid : DiscMapNids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);

            frame.registers.Set(Gpr::Rax, DispatchDiscMap(space, nid, args));
            return Ok();
        };
    }
    // Session guest-facing libSceUserService startup family. Each output pointer
    // is validated against the guest address space; the real emulator function
    // runs on a host-local object and the result is written back. No guest
    // pointer is reinterpreted as a host struct and no host state address is
    // handed to the guest.
    namespace UserService = Libraries::UserService;
    bind({"j3YMu1MVNNo"}, [this](const auto& a) -> u64 {
        if (a[0] && !space.ValidateRange({GuestAddress{a[0]}, sizeof(s32)}, GuestPermission::Read))
            return static_cast<u32>(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
        UserService::OrbisUserServiceInitializeParams params{};
        if (a[0])
            params.priority = Read<s32>(a[0]);
        if (a[0] && (params.priority < UserService::ORBIS_KERNEL_PRIO_FIFO_HIGHEST ||
                     params.priority > UserService::ORBIS_KERNEL_PRIO_FIFO_LOWEST))
            return static_cast<u32>(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
        const auto result = platform->Initialize();
        if (!result) {
            UserService::OrbisUserServiceLoginUserIdList users{};
            platform->LoginUsers(users);
            user_callbacks->Initialize(users);
            if (np_offline) for (s32 user : users.user_id) if (user >= 0)
                Libraries::Np::NpManager::NotifyNpStateFromUserServiceEvent(
                    UserService::OrbisUserServiceEventType::Login, user);
        }
        return static_cast<u32>(result);
    });
    bind({"bwFjS+bX9mA"},
         [this](const auto&) -> u64 {
             const auto result = platform->Terminate();
             if (!result) user_callbacks->Terminate();
             return static_cast<u32>(result);
         });
    for (const auto nid : {"wuI7c7UNk0A", "spW--yoLQ9o"})
        bind({nid}, [this, nid](const auto& a) -> u64 {
            return user_callbacks->Dispatch(std::string_view(nid) == "spW--yoLQ9o", a[0], a[1]);
        });
    bind({"CdWp0oHWGr0"}, [this](const auto& a) -> u64 {
        if (!space.ValidateRange({GuestAddress{a[0]}, sizeof(s32)}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
        int user_id{};
        const auto result = platform->InitialUser(user_id);
        if (result == 0)
            Write(a[0], user_id);
        return static_cast<u32>(result);
    });
    bind({"eNb53LQJmIM"}, [this](const auto& a) -> u64 {
        // The real API reports the foreground user through an out pointer; the
        // desktop stub omits it. Report the initial user so the guest sees a
        // consistent identity instead of an uninitialized value.
        if (!space.ValidateRange({GuestAddress{a[0]}, sizeof(s32)}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
        int user_id{};
        const auto result = platform->InitialUser(user_id);
        if (result == 0)
            Write(a[0], user_id);
        return static_cast<u32>(result);
    });
    bind({"fPhymKNvK-A"}, [this](const auto& a) -> u64 {
        UserService::OrbisUserServiceLoginUserIdList list{};
        if (!space.ValidateRange({GuestAddress{a[0]}, sizeof(list)}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
        const auto result = platform->LoginUsers(list);
        if (result == 0)
            Write(a[0], list);
        return static_cast<u32>(result);
    });
    bind({"1xxcMiGu2fo"}, [this](const auto& a) -> u64 {
        const auto size = a[2];
        if (size == 0 || size > 0x1000)
            return static_cast<u32>(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
        if (!space.ValidateRange({GuestAddress{a[1]}, size}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
        std::string name;
        const auto result = platform->UserName(static_cast<s32>(a[0]), name);
        if (result == 0) {
            if (name.size() + 1 > size)
                return static_cast<u32>(ORBIS_USER_SERVICE_ERROR_BUFFER_TOO_SHORT);
            Require(space.WriteData(GuestAddress{a[1]},
                                    std::as_bytes(std::span{name.c_str(), name.size() + 1})));
        }
        return static_cast<u32>(result);
    });
    bind({"lUoqwTQu4Go"}, [this](const auto& a) -> u64 {
        using UserColor = UserService::OrbisUserServiceUserColor;
        if (!space.ValidateRange({GuestAddress{a[1]}, sizeof(UserColor)}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
        UserColor color{};
        const auto result = platform->UserColor(static_cast<s32>(a[0]), color);
        if (result == 0)
            Write(a[1], color);
        return static_cast<u32>(result);
    });
    bind({"yH17Q6NWtVg"}, [this](const auto& a) -> u64 {
        UserService::OrbisUserServiceEvent event{};
        if (!space.ValidateRange({GuestAddress{a[0]}, sizeof(event)}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
        const auto result = platform->UserEvent(event);
        if (result == 0)
            Write(a[0], event);
        return static_cast<u32>(result);
    });
    // libkernel process-identity scalars. Pure, pointer-free; dispatch to the
    // real desktop implementations so the guest sees values consistent with the
    // rest of the emulator (Neo/devkit/SDK/cpu-mode), not fabricated constants.
    namespace Kernel = Libraries::Kernel;
    bind({"xeu-pV8wkKs"}, [](const auto&) -> u64 { return Kernel::sceKernelIsInSandbox(); });
    bind({"WslcK1FQcGI"}, [](const auto&) -> u64 { return Kernel::sceKernelIsNeoMode(); });
    bind({"rNRtm1uioyY"}, [](const auto&) -> u64 { return Kernel::sceKernelHasNeoMode(); });
    bind({"QNjGUdj1HPM"}, [](const auto&) -> u64 { return Kernel::sceKernelIsDevkit(); });
    bind({"mpxAdqW7dKY"}, [](const auto&) -> u64 { return Kernel::sceKernelIsProspero(); });
    bind({"8aCOCGoRkUI"}, [](const auto&) -> u64 { return Kernel::sceKernelIsCEX(); });
    bind({"0vTn5IDMU9A"}, [](const auto&) -> u64 { return Kernel::sceKernelGetMainSocId(); });
    bind({"VOx8NGmHXTs"}, [](const auto&) -> u64 { return Kernel::sceKernelGetCpumode(); });
    bind({"4oXYe9Xmk0Q"}, [](const auto&) -> u64 { return Kernel::sceKernelGetGPI(); });
    bind({"WB66evu8bsU"}, [this](const auto& a) -> u64 {
        if (!a[0]) return u32(ORBIS_KERNEL_ERROR_EINVAL);
        const GuestAddressSpace::DataRequest output{{GuestAddress{a[0]}, 4}, GuestPermission::Write};
        auto pin = space.AcquireDataBatch(std::span{&output, 1});
        if (!pin) return u32(ORBIS_KERNEL_ERROR_EFAULT);
        s32 version{};
        const auto result = Kernel::sceKernelGetCompiledSdkVersion(&version);
        if (!result) std::memcpy(pin.Value()[0].WritableBytes().data(), &version, sizeof(version));
        return u32(result);
    });
    bind({"VjBtg5Btl94"}, [](const auto& a) -> u64 {
        return u32(Kernel::sceKernelSetFsstParam(s32(a[0]), a[1]));
    });
    bind({"g0VTBxfJyu0"}, [](const auto&) -> u64 { return Kernel::sceKernelGetCurrentCpu(); });
    // libSceSystemService startup family. Output pointers are validated and the
    // real emulator functions run on host-local objects; results are copied back.
    namespace SystemService = Libraries::SystemService;
    bind({DisplaySafeAreaNid}, [this](const auto& a) -> u64 {
        return GuestDisplaySafeAreaInfo(space, a[0]);
    });
    bind({"fZo48un7LK4"}, [this](const auto& a) -> u64 {
        if (!space.ValidateRange({GuestAddress{a[1]}, sizeof(s32)}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER);
        int value{};
        const auto result =
            platform->Param(static_cast<SystemService::OrbisSystemServiceParamId>(a[0]), value);
        if (result == 0)
            Write(a[1], value);
        return static_cast<u32>(result);
    });
    bind({"rPo6tV8D9bM"}, [this](const auto& a) -> u64 {
        SystemService::OrbisSystemServiceStatus status{};
        if (!space.ValidateRange({GuestAddress{a[0]}, sizeof(status)}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER);
        status = platform->Status();
        const s32 result = 0;
        if (result == 0)
            Write(a[0], status);
        return static_cast<u32>(result);
    });
    bind({"656LMQSrg6U"}, [this](const auto& a) -> u64 {
        auto event = std::make_unique<SystemService::OrbisSystemServiceEvent>();
        if (!space.ValidateRange({GuestAddress{a[0]}, sizeof(*event)}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER);
        const auto result = platform->SystemEvent(*event);
        if (result == 0)
            Write(a[0], *event);
        return static_cast<u32>(result);
    });
    bind({"Vo5V8KAwCmk"}, [this](const auto&) -> u64 {
        platform->HideSplash();
        return 0;
    });
    bind({"1FZBKy8HeNU", "d1AjT2uZJn0"}, [this](const auto& a) -> u64 {
        using namespace Libraries::VideoOut;
        if (!space.ValidateRange({GuestAddress{a[1]}, sizeof(SceVideoOutVblankStatus)},
                                 GuestPermission::Write))
            return static_cast<u32>(ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS);
        auto* port = Graphics().VideoOut().GetPort(static_cast<s32>(a[0]));
        if (!port || !port->is_open)
            return static_cast<u32>(ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE);
        SceVideoOutVblankStatus status{};
        {
            std::scoped_lock lock(port->vo_mutex);
            status = port->vblank_status;
        }
        Write(a[1], status);
        return 0;
    });
    handlers["i6-sR91Wt-4"] = [this](HleCallFrame& frame) {
        using namespace Libraries::VideoOut;
        CallCursor cursor(frame);
        std::array<u64, 7> args{};
        for (auto& arg : args) {
            auto decoded = cursor.NextInteger();
            if (!decoded)
                return Status(decoded.GetError());
            arg = decoded.Value();
        }
        // The seventh SysV argument (pitch) is on the guest stack. Decode it
        // before native entry, and keep the output pinned throughout the copy.
        auto pin = space.AcquireDataSpan({GuestAddress{args[0]}, sizeof(BufferAttribute)}, true);
        if (!pin)
            return Status(pin.GetError());
        const auto format = static_cast<PixelFormat>(args[1]);
        switch (format) {
        case PixelFormat::A8R8G8B8Srgb:
        case PixelFormat::A8B8G8R8Srgb:
        case PixelFormat::A2R10G10B10:
        case PixelFormat::A2R10G10B10Srgb:
        case PixelFormat::A2R10G10B10Bt2020Pq:
        case PixelFormat::A16R16G16B16Float:
            break;
        default:
            return Status(MakeError(ErrorCategory::Unsupported, "sceVideoOutSetBufferAttribute",
                                    "unsupported pixel format"));
        }
        BufferAttribute value{};
        sceVideoOutSetBufferAttribute(&value, format, args[2], args[3], args[4], args[5], args[6]);
        std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
        return Ok(); // void ABI: do not fabricate a return code
    };
    bind({"Up36PTk687E"}, [this](const auto& a) -> u64 {
        using namespace Libraries::VideoOut;
        if (static_cast<s32>(a[1]) != SCE_VIDEO_OUT_BUS_TYPE_MAIN || a[2] != 0)
            return static_cast<u32>(ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE);
        // The optional service-thread structure is copied; its pointers can
        // never escape into a worker. Current native driver uses default QoS.
        ServiceThreadParams params{};
        if (a[3]) {
            if (!space.ValidateRange({GuestAddress{a[3]}, sizeof(params)}, GuestPermission::Read))
                return static_cast<u32>(ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS);
            params = Read<ServiceThreadParams>(a[3]);
        }
        auto& video = Graphics().VideoOut();
        if (cancelling)
            return static_cast<u32>(ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY);
        auto result = video.Open(a[3] ? &params : nullptr);
        LOG_INFO(Lib_VideoOut, "Production VideoOutOpen: handle={} (native Turnip/Presenter)",
                 result);
        return static_cast<u32>(result);
    });
    bind({"6kPnj51T62Y", "8XGijEoThE0"}, [this](const auto& a) -> u64 {
        using namespace Libraries::VideoOut;
        if (!space.ValidateRange({GuestAddress{a[1]}, sizeof(SceVideoOutResolutionStatus)},
                                 GuestPermission::Write))
            return static_cast<u32>(ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS);
        auto* port = Graphics().VideoOut().GetPort(static_cast<s32>(a[0]));
        if (!port || !port->is_open)
            return static_cast<u32>(ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE);
        Write(a[1], port->resolution);
        return 0;
    });

    // Guest-safe PSVR compatibility layer.  Desktop HMD/Tracker code uses
    // process globals and native pointers, so only records are copied through
    // this boundary.  Opaque callbacks, SBS buffer ownership and real camera
    // data remain unavailable until their provider is recovered.
    auto read_guest_record = [this](u64 address, void* output, size_t size) {
        return address && space.ValidateRange({GuestAddress{address}, size}, GuestPermission::Read) &&
               space.ReadData(GuestAddress{address},
                              std::span{static_cast<std::byte*>(output), size});
    };
    auto write_guest_record = [this](u64 address, const void* input, size_t size) {
        return address && space.ValidateRange({GuestAddress{address}, size}, GuestPermission::Write) &&
               space.WriteData(GuestAddress{address},
                               std::span{static_cast<const std::byte*>(input), size});
    };
    // Desktop HmdSetupDialog is a named no-provider implementation.  Android
    // uses the same lifecycle but GetResult reports success only while the
    // session-owned SBS sensor adapter is enabled.  Never pass a guest pointer
    // into the desktop routine.
    bind({"NB1Y2kA2jCY"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::HmdSetupDialog::sceHmdSetupDialogInitialize());
    });
    bind({"nmHzU4Gh0xs"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::HmdSetupDialog::sceHmdSetupDialogClose());
    });
    bind({"+z4OJmFreZc"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::HmdSetupDialog::sceHmdSetupDialogTerminate());
    });
    bind({"Ud7j3+RDIBg"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::HmdSetupDialog::sceHmdSetupDialogUpdateStatus());
    });
    bind({"J9eBpW1udl4"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::HmdSetupDialog::sceHmdSetupDialogGetStatus());
    });
    bind({"NNgiV4T+akU"}, [this, read_guest_record](const auto& a) -> u64 {
        Libraries::HmdSetupDialog::OrbisHmdSetupDialogParam param{};
        if (!read_guest_record(a[0], &param, sizeof(param)))
            return static_cast<u32>(Libraries::CommonDialog::Error::ARG_NULL);
        return static_cast<u32>(Libraries::HmdSetupDialog::sceHmdSetupDialogOpen(&param));
    });
    bind({"6lVRHMV5LY0"}, [this, write_guest_record](const auto& a) -> u64 {
        Libraries::HmdSetupDialog::OrbisHmdSetupDialogResult result{};
        const s32 status = Libraries::HmdSetupDialog::sceHmdSetupDialogGetResult(&result);
        if (!a[0] || !write_guest_record(a[0], &result, sizeof(result)))
            return static_cast<u32>(Libraries::CommonDialog::Error::ARG_NULL);
        return static_cast<u32>(status);
    });
    auto hmd_initialize = [this, read_guest_record](const std::array<u64, 6>& a,
                                                     bool firmware_315) -> u64 {
        Libraries::Hmd::OrbisHmdInitializeParam param{};
        if (!read_guest_record(a[0], &param, sizeof(param)))
            return static_cast<u32>(ORBIS_HMD_ERROR_PARAMETER_NULL);
        // reserved0 is a guest address used by desktop distortion code.  The
        // provider is absent on Android, so never hand that address to a
        // native routine; preserve the parameter validation only.
        param.reserved0 = nullptr;
        const s32 result = firmware_315 ? Libraries::Hmd::sceHmdInitialize315(&param)
                                         : Libraries::Hmd::sceHmdInitialize(&param);
        if (!result && vr_session)
            (void)vr_session->InitializeHmd(false);
        return static_cast<u32>(result);
    };
    bind({"K4KnH0QkT2c"}, [hmd_initialize](const auto& a) -> u64 {
        return hmd_initialize(a, false);
    });
    bind({"s-J66ar9g50"}, [hmd_initialize](const auto& a) -> u64 {
        return hmd_initialize(a, true);
    });
    bind({"d2g5Ij7EUzo"}, [this](const auto& a) -> u64 {
        if (a[3])
            return static_cast<u32>(ORBIS_HMD_ERROR_PARAMETER_INVALID);
        const s32 result = Libraries::Hmd::sceHmdOpen(
            static_cast<Libraries::UserService::OrbisUserServiceUserId>(a[0]), s32(a[1]),
            s32(a[2]), nullptr);
        if (result >= 0 && vr_session)
            (void)vr_session->OpenHmd();
        return static_cast<u32>(result);
    });
    bind({"thDt9upZlp8"}, [this, write_guest_record](const auto& a) -> u64 {
        Libraries::Hmd::OrbisHmdDeviceInformation info{};
        const s32 result = Libraries::Hmd::sceHmdGetDeviceInformation(&info);
        // The virtual headset belongs to this session's initialized local user
        // before Open. Unity passes this field straight back to sceHmdOpen;
        // the desktop global owner is still INVALID until that call succeeds.
        // Never invent a user when UserService has no initialized local profile.
        if (!result && GuestVrSensor::Instance().Read().enabled &&
            info.user_id == Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID) {
            s32 user_id{};
            if (platform && platform->InitialUser(user_id) == 0)
                info.user_id = user_id;
        }
        if (!result && !write_guest_record(a[0], &info, sizeof(info)))
            return static_cast<u32>(ORBIS_HMD_ERROR_PARAMETER_NULL);
        if (result || !a[0])
            return static_cast<u32>(result ? result : ORBIS_HMD_ERROR_PARAMETER_NULL);
        return 0;
    });
    bind({"1pxQfif1rkE"}, [this, write_guest_record](const auto& a) -> u64 {
        Libraries::Hmd::OrbisHmdDeviceInformation info{};
        const s32 result = Libraries::Hmd::sceHmdGetDeviceInformationByHandle(
            s32(a[0]), &info);
        if (!result && !write_guest_record(a[1], &info, sizeof(info)))
            return static_cast<u32>(ORBIS_HMD_ERROR_PARAMETER_NULL);
        if (result || !a[1])
            return static_cast<u32>(result ? result : ORBIS_HMD_ERROR_PARAMETER_NULL);
        return 0;
    });
    bind({HmdEyeOffsetNid}, [this](const auto& a) -> u64 {
        return GuestHmdEyeOffsets(space, s32(a[0]), a[1], a[2]);
    });
    bind({"NPQwYFqi0bs"}, [this, write_guest_record](const auto& a) -> u64 {
        Libraries::Hmd::OrbisHmdFieldOfView fov{};
        const s32 result = Libraries::Hmd::sceHmdGetFieldOfView(s32(a[0]), &fov);
        if (!result && !write_guest_record(a[1], &fov, sizeof(fov)))
            return static_cast<u32>(ORBIS_HMD_ERROR_PARAMETER_NULL);
        if (result || !a[1])
            return static_cast<u32>(result ? result : ORBIS_HMD_ERROR_PARAMETER_NULL);
        return 0;
    });
    bind({"6biw1XHTSqQ"}, [this](const auto& a) -> u64 {
        const s32 result = Libraries::Hmd::sceHmdClose(s32(a[0]));
        if (!result && vr_session)
            (void)vr_session->CloseHmd();
        return static_cast<u32>(result);
    });
    bind({"z-RMILqP6tE"}, [this](const auto&) -> u64 {
        const s32 result = Libraries::Hmd::sceHmdTerminate();
        if (!result && vr_session)
            (void)vr_session->TerminateHmd();
        return static_cast<u32>(result);
    });
    for (const auto nid : ReprojectionNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            const auto result = reprojection->Dispatch(nid, a, GuestVrSensor::Instance().Read().enabled);
            const auto n = ++reprojection_calls;
            if (n <= 32 || (result && (n & 1023) == 0))
                LOG_INFO(Lib_Hmd, "SBS passthrough nid={} args={:#x}/{:#x}/{:#x} result={:#x}",
                         nid, a[0], a[1], a[2], result);
            return result;
        });
    }
    bind({"z0KtN1vqF2E"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::Hmd::sceHmdReprojectionQueryGarlicBuffSize());
    });
    bind({"kLUAkN6a1e8"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::Hmd::sceHmdReprojectionQueryOnionBuffSize());
    });
    bind({"TkcANcGM0s8"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::Hmd::sceHmdReprojectionQueryGarlicBuffAlign());
    });
    bind({"IWybWbR-xvA"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::Hmd::sceHmdReprojectionQueryOnionBuffAlign());
    });
    // This entry uses the SysV vector argument class (xmm0..xmm2), so it
    // cannot use the integer-only bind helper above.
    handlers["LjdLRysHU6Y"] = [](HleCallFrame& frame) {
        const auto as_float = [](const Xmm& value) {
            return std::bit_cast<float>(static_cast<u32>(value.low));
        };
        frame.registers.Set(
            Gpr::Rax,
            static_cast<u32>(Libraries::Hmd::sceHmdReprojectionSetOutputMinColor(
                as_float(frame.registers.xmm[0]), as_float(frame.registers.xmm[1]),
                as_float(frame.registers.xmm[2]))));
        return Ok();
    };

    auto tracker_read = [this, read_guest_record](u64 address, auto& value) {
        return read_guest_record(address, &value, sizeof(value));
    };
    bind({"K7yhYrsIBPc"}, [this, tracker_read, write_guest_record](const auto& a) -> u64 {
        Libraries::VrTracker::OrbisVrTrackerQueryMemoryParam param{};
        Libraries::VrTracker::OrbisVrTrackerQueryMemoryResult result{};
        if (!tracker_read(a[0], param) ||
            !space.ValidateRange({GuestAddress{a[1]}, sizeof(result)}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        const s32 status = Libraries::VrTracker::sceVrTrackerQueryMemory(&param, &result);
        if (!status && !write_guest_record(a[1], &result, sizeof(result)))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        return static_cast<u32>(status);
    });
    bind({"QkRl7pART9M"}, [this, tracker_read](const auto& a) -> u64 {
        Libraries::VrTracker::OrbisVrTrackerInitParam param{};
        if (!tracker_read(a[0], param))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        const auto valid_buffer = [this](void* pointer, u32 size, u32 alignment) {
            const u64 address = reinterpret_cast<u64>(pointer);
            return address && size && alignment == Libraries::VrTracker::ORBIS_VR_TRACKER_MEMORY_ALIGNMENT &&
                   !(address % alignment) &&
                   space.ValidateRange({GuestAddress{address}, size},
                                       GuestPermission::Read | GuestPermission::Write);
        };
        if (!valid_buffer(param.direct_memory_onion, param.direct_memory_onion_size,
                          param.direct_memory_onion_alignment) ||
            !valid_buffer(param.direct_memory_garlic, param.direct_memory_garlic_size,
                          param.direct_memory_garlic_alignment) ||
            !valid_buffer(param.work_memory, param.work_memory_size, param.work_memory_alignment))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        const s32 status = Libraries::VrTracker::sceVrTrackerInit(&param);
        if (!status && vr_session)
            (void)vr_session->InitializeTracker();
        return static_cast<u32>(status);
    });
    bind({"sIh8GwcevaQ"}, [](const auto& a) -> u64 {
        return static_cast<u32>(Libraries::VrTracker::sceVrTrackerRegisterDevice(
            static_cast<Libraries::VrTracker::OrbisVrTrackerDeviceType>(a[0]), s32(a[1])));
    });
    bind({"5IFOAYv-62g"}, [tracker_read](const auto& a) -> u64 {
        Libraries::VrTracker::OrbisVrTrackerCpuProcessParam param{};
        if (!tracker_read(a[0], param)) return u32(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        return u32(Libraries::VrTracker::sceVrTrackerCpuProcess(&param));
    });
    bind({"VItTwN8DmS8"}, [tracker_read](const auto& a) -> u64 {
        Libraries::VrTracker::OrbisVrTrackerNotifyEndOfCpuProcessParam param{};
        if (!tracker_read(a[0], param)) return u32(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        return u32(Libraries::VrTracker::sceVrTrackerNotifyEndOfCpuProcess(&param));
    });
    bind({"TVegDMLaBB8"}, [this, tracker_read](const auto& a) -> u64 {
        Libraries::VrTracker::OrbisVrTrackerGpuSubmitParam param{};
        if (!tracker_read(a[0], param))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        return static_cast<u32>(Libraries::VrTracker::sceVrTrackerGpuSubmit(&param));
    });
    bind({"gkGuO9dd57M"}, [this, tracker_read](const auto& a) -> u64 {
        Libraries::VrTracker::OrbisVrTrackerGpuWaitParam param{};
        if (!tracker_read(a[0], param))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        return static_cast<u32>(Libraries::VrTracker::sceVrTrackerGpuWait(&param));
    });
    bind({"ARhgpXvwoR0"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::VrTracker::sceVrTrackerGpuWaitAndCpuProcess());
    });
    bind({"76OBvrrQXUc"}, [this, tracker_read, write_guest_record](const auto& a) -> u64 {
        Libraries::VrTracker::OrbisVrTrackerGetResultParam param{};
        if (!tracker_read(a[0], param) ||
            !space.ValidateRange({GuestAddress{a[1]}, sizeof(Libraries::VrTracker::OrbisVrTrackerResultData)},
                                  GuestPermission::Write))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        Libraries::VrTracker::OrbisVrTrackerResultData result{};
        const s32 status = Libraries::VrTracker::sceVrTrackerGetResult(&param, &result);
        if (!status && !write_guest_record(a[1], &result, sizeof(result)))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        return static_cast<u32>(status);
    });
    bind({"XoeWzXlrnMw"}, [this, write_guest_record](const auto& a) -> u64 {
        u64 time{};
        const s32 status = Libraries::VrTracker::sceVrTrackerGetTime(&time);
        if (!status && !write_guest_record(a[0], &time, sizeof(time)))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        return static_cast<u32>(status);
    });
    bind({"EUCaQtXXYNI"}, [this, tracker_read](const auto& a) -> u64 {
        Libraries::VrTracker::OrbisVrTrackerRecalibrateParam param{};
        if (!tracker_read(a[0], param))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        return static_cast<u32>(Libraries::VrTracker::sceVrTrackerRecalibrate(&param));
    });
    bind({"E0P0sN-wy+4"}, [](const auto& a) -> u64 {
        return static_cast<u32>(Libraries::VrTracker::sceVrTrackerResetOrientationRelative(
            static_cast<Libraries::VrTracker::OrbisVrTrackerDeviceType>(a[0]), s32(a[1])));
    });
    bind({"9fvHMUbsom4"}, [this, tracker_read](const auto& a) -> u64 {
        Libraries::VrTracker::OrbisVrTrackerUpdateMotionSensorDataParam param{};
        if (!tracker_read(a[0], param))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        return static_cast<u32>(Libraries::VrTracker::sceVrTrackerUpdateMotionSensorData(&param));
    });
    bind({"Q8skQqEwn5c"}, [](const auto& a) -> u64 {
        return static_cast<u32>(Libraries::VrTracker::sceVrTrackerUnregisterDevice(s32(a[0])));
    });
    bind({"IBv4P3q1pQ0"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::VrTracker::sceVrTrackerTerm());
    });
    // Firmware copies a fixed 0x40-byte record.  The shared no-provider
    // implementation validates its size/reserved fields and never writes
    // fabricated tracking data. Probe the lifecycle before reading guest
    // memory, matching firmware's NOT_INIT precedence.
    bind({"zvyKP0Z3UvU"}, [this, read_guest_record](const auto& a) -> u64 {
        using namespace Libraries::VrTracker;
        const s32 lifecycle = sceVrTrackerGetPlayAreaWarningInfo(nullptr);
        if (lifecycle == ORBIS_VR_TRACKER_ERROR_NOT_INIT)
            return static_cast<u32>(lifecycle);
        OrbisVrTrackerPlayAreaWarningInfo info{};
        if (!a[0] || !space.ValidateRange({GuestAddress{a[0]}, sizeof(info)},
                                         GuestPermission::Read | GuestPermission::Write) ||
            !read_guest_record(a[0], &info, sizeof(info)))
            return static_cast<u32>(ORBIS_VR_TRACKER_ERROR_ARGUMENT_INVALID);
        return static_cast<u32>(sceVrTrackerGetPlayAreaWarningInfo(&info));
    });

    // Desktop records/configuration with session-owned fixed SBS camera buffers.
    // The adapter validates/copies guest records; it never invokes SDL or maps
    // desktop's fixed system-pool address into the Android process.
    for (const auto nid : CameraNids) {
        bind({nid.data()}, [this, nid](const auto& a) -> u64 {
            if (!camera) return u32(ORBIS_CAMERA_ERROR_NOT_INIT);
            const auto result = camera->Dispatch(nid, a, GuestVrSensor::Instance().Read().enabled,
                                                clock.ticks.GetTimeUS(clock.origin));
            if (nid != "mxgMmR+1Kr0" && nid != "U3BVwQl2R5Q")
                LOG_INFO(Lib_Camera, "SBS camera nid={} handle={} result={:#x}", nid, s32(a[0]), result);
            return result;
        });
    }

    // Move keeps its desktop lifecycle and explicit no-controller results.
    // Guest output records are validated before being exposed to the native
    // implementation, and are only copied back if that implementation ever
    // reports success.
    bind({"j1ITE-EoJmE"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::Move::sceMoveInit());
    });
    bind({"HzC60MfjJxU"}, [](const auto& a) -> u64 {
        return static_cast<u32>(Libraries::Move::sceMoveOpen(
            static_cast<Libraries::UserService::OrbisUserServiceUserId>(a[0]), s32(a[1]),
            s32(a[2])));
    });
    bind({"GWXTyxs4QbE"}, [this](const auto& a) -> u64 {
        using namespace Libraries::Move;
        if (!space.ValidateRange({GuestAddress{a[1]}, sizeof(OrbisMoveDeviceInfo)},
                                  GuestPermission::Write))
            return static_cast<u32>(ORBIS_MOVE_ERROR_INVALID_ARG);
        OrbisMoveDeviceInfo info{};
        const s32 status = sceMoveGetDeviceInfo(s32(a[0]), &info);
        if (!status && !space.WriteData(GuestAddress{a[1]},
                                         std::span{reinterpret_cast<const std::byte*>(&info),
                                                   sizeof(info)}))
            return static_cast<u32>(ORBIS_MOVE_ERROR_INVALID_ARG);
        return static_cast<u32>(status);
    });
    bind({"f2bcpK6kJfg"}, [this](const auto& a) -> u64 {
        using namespace Libraries::Move;
        if (s64(a[1]) < 0 ||
            !space.ValidateRange({GuestAddress{a[2]}, sizeof(OrbisMoveData)},
                                  GuestPermission::Write) ||
            !space.ValidateRange({GuestAddress{a[3]}, sizeof(s32)}, GuestPermission::Write))
            return static_cast<u32>(ORBIS_MOVE_ERROR_INVALID_ARG);
        OrbisMoveData data{};
        s32 out_count{};
        const s32 status = sceMoveReadStateRecent(s32(a[0]), s64(a[1]), &data, &out_count);
        if (!status) {
            if (!space.WriteData(GuestAddress{a[2]},
                                  std::span{reinterpret_cast<const std::byte*>(&data),
                                            sizeof(data)}) ||
                !space.WriteData(GuestAddress{a[3]},
                                  std::span{reinterpret_cast<const std::byte*>(&out_count),
                                            sizeof(out_count)}))
                return static_cast<u32>(ORBIS_MOVE_ERROR_INVALID_ARG);
        }
        return static_cast<u32>(status);
    });
    bind({"IFQwtT2CeY0"}, [](const auto& a) -> u64 {
        return static_cast<u32>(Libraries::Move::sceMoveSetVibration(s32(a[0]), u8(a[1])));
    });
    bind({"T8KYHPs1JE8"}, [](const auto& a) -> u64 {
        return static_cast<u32>(Libraries::Move::sceMoveSetLightSphere(s32(a[0]), u8(a[1]),
                                                                         u8(a[2]), u8(a[3])));
    });
    bind({"tsZi60H4ypY"}, [](const auto&) -> u64 {
        return static_cast<u32>(Libraries::Move::sceMoveTerm());
    });

    InstallGraphicsHandlers(handlers, graphics_gnm_nids, graphics_video_nids, space,
                            [this]() -> GuestGraphics& { return Graphics(); });
    // libSceGnmDriver owner registration. On retail firmware this is not
    // available and returns failure; the guest tolerates that. Validate the
    // guest name string (when present) and return the real retail code. The GPU
    // command-submission and display-flip path is renderer/Turnip work and is
    // deliberately NOT bound here; those functions still name-fault.
    bind({"ZFqKFl23aMc"}, [this](const auto& a) -> u64 {
        if (a[1])
            (void)String(a[1], 256);
        return static_cast<u32>(Libraries::GnmDriver::sceGnmRegisterOwner(nullptr, nullptr));
    });
    // Remaining legacy error-returning handlers. Mutex/condition wrappers are
    // paired explicitly above and must not be wrapped a second time here.
    for (const char* nid :
         {"6UgtwV+0zb4", "onNY9Byn-W8", "4qGrR6eoP9Y", "14bOACANTBo",
          "geDaqgH9lTg", "PrdHuuDekhY", "+BzXYkqYeLE"}) {
        auto posix = handlers.at(nid);
        handlers[nid] = [posix](HleCallFrame& frame) {
            auto status = posix(frame);
            const auto result = frame.registers.Get(Gpr::Rax);
            if (status && result)
                frame.registers.Set(Gpr::Rax, 0x80020000u | static_cast<u32>(result));
            return status;
        };
    }
    if (rwlock_diagnostics) {
        for (auto& [nid, handler] : handlers) {
            const auto* entry = AeroLib::FindByNid(nid.c_str());
            if (!entry || !GuestRwlockDiagnostics::Matches(entry->name))
                continue;
            handler = [this, inner = std::move(handler), name = std::string(entry->name),
                       trace = std::make_shared<GuestRwlockDiagnostics>(rwlock_diagnostic_slot)]
                      (HleCallFrame& frame) {
                return trace->Call(frame, inner, [&](const GuestRwlockDiagnostics::Record& r) {
                    const auto* scope = HleScope::Current();
                    const auto line = nlohmann::json{
                        {"context", cpu.ContextId()}, {"pid", getpid()}, {"api", name},
                        {"thread", scope ? scope->Thread().id : 0},
                        {"thread_generation", scope ? scope->Thread().generation : 0},
                        {"invocation", scope ? scope->InvocationId() : 0},
                        {"operation", frame.operation}, {"call", r.call}, {"args", r.args},
                        {"result", r.result}, {"adapter_ok", r.adapter_ok}, {"rsp", r.rsp},
                        {"caller", r.caller}, {"caller_readable", r.caller_readable}}.dump();
                    LOG_INFO(Core, "RWLOCK_TRACE {}", line);
#ifdef __ANDROID__
                    __android_log_print(ANDROID_LOG_INFO, "GuestRwlockTrace", "%s", line.c_str());
#endif
                });
            };
        }
    }
    if (network_diagnostics) {
        // Bounded, opt-in ABI observations. Do not dereference title secrets,
        // account records, URLs or request bodies. Pair by sequence across threads.
        // Keep logcat copies: GPU debug traffic can bury the host log's startup tail.
        for (auto& [nid, handler] : handlers) {
            const auto* entry = AeroLib::FindByNid(nid.c_str());
            if (!entry)
                continue;
            const std::string_view name{entry->name};
            if (!name.starts_with("sceNet") && !name.starts_with("sceNp") &&
                !name.starts_with("sceHttp") && !name.starts_with("sceSsl") &&
                !name.starts_with("sceErrorDialog"))
                continue;
            handler = [this, inner = std::move(handler), name = std::string(name),
                       count = std::make_shared<std::atomic<u64>>()](HleCallFrame& frame) {
                const u64 call = ++*count;
                if (call > 16)
                    return inner(frame);
                const u64 sequence = ++network_diagnostic_calls;
                const auto record = [&](bool output, bool adapter_ok) noexcept {
                    try {
                        auto log = nlohmann::json{{"context", cpu.ContextId()},
                            {"pid", getpid()}, {"api", name}, {"call", call},
                            {"sequence", sequence}, {"phase", output ? "out" : "in"}};
                        if (output) {
                            log["result"] = frame.registers.Get(Gpr::Rax);
                            log["adapter_ok"] = adapter_ok;
                        } else {
                            std::array<u64, 6> args{};
                            for (unsigned i = 0; i < args.size(); ++i)
                                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
                            log["args"] = args;
                        }
                        const auto line = log.dump();
                        LOG_INFO(Lib_Net, "NETWORK_TRACE {}", line);
#ifdef __ANDROID__
                        __android_log_print(ANDROID_LOG_INFO, "GuestNetworkTrace", "%s", line.c_str());
#endif
                    } catch (...) {} // Diagnostics must not change the HLE result.
                };
                record(false, true);
                auto status = inner(frame);
                record(true, bool(status));
                return status;
            };
        }
    }
    if (hmd_diagnostics) {
        LOG_INFO(Lib_Hmd, "HMD_TRACE {}", HmdDiagnostics::Json{
            {"context", cpu.ContextId()}, {"pid", getpid()}, {"phase", "enabled"},
            {"sampling", "first16_then_powers_of_two_through65536_per_entry"}}.dump());
        // Wrap the installed ABI adapters, so return values and output records
        // describe what the guest actually receives (including rejected calls).
        for (auto& [nid, handler] : handlers) {
            const auto* entry = AeroLib::FindByNid(nid.c_str());
            if (!entry || !std::string_view(entry->name).starts_with("sceHmd")) continue;
            handler = [this, inner = std::move(handler), name = std::string(entry->name),
                       count = std::make_shared<std::atomic<u64>>()](HleCallFrame& frame) {
                const u64 call = ++*count;
                if (!HmdDiagnostics::Sample(call)) return inner(frame);
                HmdDiagnostics::Args args{};
                for (unsigned i = 0; i < args.size(); ++i)
                    args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
                const auto record = [&](bool output, u64 result, bool adapter_ok) {
                    try {
                        auto log = HmdDiagnostics::Json{{"context", cpu.ContextId()}, {"pid", getpid()},
                            {"api", name}, {"call", call}, {"phase", output ? "out" : "in"},
                            {"args", args}, {"records", HmdDiagnostics::Records(space, name, args, output, result)}};
                        if (output) { log["result"] = result; log["adapter_ok"] = adapter_ok; }
                        if (!output && name == "sceHmdReprojectionSetOutputMinColor") {
                            log["rgb"] = {std::bit_cast<float>(u32(frame.registers.xmm[0].low)),
                                std::bit_cast<float>(u32(frame.registers.xmm[1].low)),
                                std::bit_cast<float>(u32(frame.registers.xmm[2].low))};
                        }
                        LOG_INFO(Lib_Hmd, "HMD_TRACE {}", log.dump());
                    } catch (...) {} // logging allocation/read failures must not break HLE
                };
                record(false, 0, true);
                auto status = inner(frame);
                record(true, frame.registers.Get(Gpr::Rax), bool(status));
                return status;
            };
        }
    }
}

GuestRuntime::GuestRuntime(CpuContext& cpu, GuestAddressSpace& space, HleCallRegistry& registry)
    : impl(std::make_unique<Impl>(cpu, space, registry)) {}
GuestRuntime::~GuestRuntime() = default;
void GuestRuntime::ConfigureSaveDialog(std::shared_ptr<GuestSaveDialog> dialog) {
    if (impl->prepared)
        throw std::logic_error("dialog must be installed before Prepare");
    impl->save_dialog = std::move(dialog);
}
void GuestRuntime::ConfigureGraphics(std::shared_ptr<Frontend::Window> window,
                                     std::shared_ptr<const Vulkan::Driver> driver) {
    if (impl->prepared || !window || !driver)
        throw std::logic_error("ConfigureGraphics requires a complete platform before Prepare");
    impl->graphics_window = std::move(window);
    impl->graphics_driver = std::move(driver);
#if defined(__ANDROID__)
    // SessionCore admits the Android Surface before the guest enters Run. Feed
    // that foreground edge to Unity's named suspend gate so startup can leave
    // its lifecycle wait and reach the first real frame.
    impl->kernel_semaphores->AdmitForeground();
    __android_log_print(ANDROID_LOG_INFO, "ProductionRuntime",
                        "android foreground admission: Suspend/ResumeSemaphore enabled");
#endif
}
void GuestRuntime::Prepare(const std::filesystem::path& executable,
                           const std::vector<std::filesystem::path>& modules) {
    Common::Profiler::Scope profile{"Startup.PrepareModulesAndServices"};
    Common::Profiler::Phase stage{"Startup.ModulesAndServices"};
    if (impl->prepared)
        throw std::logic_error("runtime already prepared");
    const bool archive = FileSys::IsZArchiveFile(executable);
    if (archive) (void)FileSys::InspectArchiveInstall(executable);
    const auto install_root = std::filesystem::canonical(archive ? executable : executable.parent_path());
    const auto main_path = archive ? std::filesystem::path("/app0/eboot.bin")
                                   : std::filesystem::canonical(executable);
    impl->program_name = main_path.filename().string();
    Impl::CodePublication vm(*impl);
    impl->mounts.Mount(install_root, "/app0", true);
    impl->mounts.Mount(install_root, "/hostapp", true);
    if (impl->linker->LoadModule(main_path) != 0)
        throw std::runtime_error("main executable could not be loaded");
    PrepareStage("prepare: main module loaded");
    // Discover game-provided DT_NEEDED modules inside the installed content
    // root. System providers stay HLE imports; no host dlopen or host function
    // pointers are substituted for guest modules. Explicit roots use this same
    // graph, with canonical-path deduplication and a finite module count.
    const auto content_root = archive ? std::filesystem::path("/app0") : install_root;
    auto module_path = [&](const std::filesystem::path& path) {
        if (archive && path.generic_string().starts_with("/app0/")) return path.lexically_normal();
        return std::filesystem::canonical(path);
    };
    std::map<std::filesystem::path, u32> loaded{{main_path, 0}};
    std::set<u32> visiting, visited;
    std::map<u32, std::vector<u32>> dependencies;
    const std::vector<std::filesystem::path> search_roots{
        content_root / "sce_module", content_root / "Media/Modules",
        content_root / "Media/Plugins", content_root / "modules", content_root / "prx",
        content_root};
    std::function<void(u32)> visit;
    auto load = [&](const std::filesystem::path& path) -> u32 {
        const auto canonical = module_path(path);
        if (auto it = loaded.find(canonical); it != loaded.end())
            return it->second;
        if (loaded.size() >= 256)
            throw std::runtime_error("module graph exceeds 256 modules");
        const auto id = impl->linker->LoadModule(canonical);
        if (id < 0)
            throw std::runtime_error("dependency could not be loaded: " + canonical.string());
        loaded.emplace(canonical, id);
        const auto stage = std::string{"prepare: module loaded "} + canonical.filename().string();
        PrepareStage(stage);
        return id;
    };
    visit = [&](u32 id) {
        if (visited.contains(id) || visiting.contains(id))
            return; // ELF dependency cycles are legal.
        visiting.insert(id);
        const auto visit_stage = std::string{"prepare: visit module "} +
            impl->linker->GetModule(id)->file.filename().string();
        PrepareStage(visit_stage);
        for (const auto* needed : impl->linker->GetModule(id)->dynamic_info.needed) {
            const std::filesystem::path name(needed);
            if (name.empty() || name.has_parent_path() || name == "." || name == "..")
                throw std::runtime_error("invalid DT_NEEDED name");
            std::optional<std::filesystem::path> found;
            // Unity/IL2CPP titles commonly keep guest PRX modules under
            // `Media/Modules` and native plug-ins under `Media/Plugins`.
            // These are game modules, so they must enter the same dependency
            // graph as the conventional `/app0/sce_module` layout instead of
            // being treated as missing HLE providers.
            for (const auto& parent : search_roots) {
                const auto candidate = parent / name;
                if (archive ? !impl->mounts.Exists(candidate.generic_string())
                            : !std::filesystem::exists(candidate))
                    continue;
                const auto relative = module_path(candidate).lexically_relative(content_root);
                if (relative.empty() || *relative.begin() == "..")
                    throw std::runtime_error("dependency escapes content root");
                found = candidate;
                break;
            }
            bool alias = false;
            if (!found) {
                std::set<std::filesystem::path> candidates;
                for (const auto& parent : search_roots) {
                    const auto guest_parent = std::filesystem::path("/app0") /
                        parent.lexically_relative(content_root);
                    impl->mounts.IterateDirectory(guest_parent.generic_string(),
                        [&](const auto& path, bool is_file) {
                            if (!IsGuestPluginFile(path, is_file) ||
                                GuestModuleNameKey(path) != GuestModuleNameKey(name)) return;
                            const auto canonical = module_path(path);
                            const auto relative = canonical.lexically_relative(content_root);
                            if (relative.empty() || *relative.begin() == "..")
                                throw std::runtime_error("dependency alias escapes content root");
                            candidates.insert(canonical);
                        });
                }
                if (candidates.size() > 1)
                    throw std::runtime_error("ambiguous guest dependency: " + name.string());
                if (!candidates.empty()) { found = *candidates.begin(); alias = true; }
            }
            if (found) {
                const auto dependency = load(*found);
                if (alias) {
                    const auto exports = impl->linker->GetModule(dependency)->GetExportModules();
                    if (!std::ranges::any_of(exports, [&](const auto& m) {
                            return GuestModuleAliasIdentity(name, m.name);
                        })) throw std::runtime_error("guest dependency alias identity mismatch: " + name.string());
                    LOG_INFO(Core_Linker, "Guest dependency alias {} -> {} (export identity checked)",
                             name.string(), found->string());
                }
                dependencies[id].push_back(dependency);
                visit(dependency);
            } else {
                const auto stem = name.stem().string();
                if (stem != "libkernel" && !stem.starts_with("libSce"))
                    throw std::runtime_error("missing guest dependency: " + name.string());
                impl->hle_modules.push_back(name.string());
            }
        }
        visiting.erase(id);
        visited.insert(id);
        const auto done_stage = std::string{"prepare: visited module "} +
            impl->linker->GetModule(id)->file.filename().string();
        PrepareStage(done_stage);
        if (id != 0)
            impl->init_order.push_back(id);
    };
    // Load explicit roots before walking dependencies to preserve their module
    // IDs; a dependency discovered twice is initialized only once.
    std::vector<u32> roots{0};
    // Desktop prefers a user-supplied complete LibcInternal over partial HLE.
    // Execute it through FEX as another guest module, with the same TLS/init graph.
    const auto system_libc = EmulatorSettings.GetSysModulesDir() / "libSceLibcInternal.sprx";
    if (std::filesystem::is_regular_file(system_libc))
        roots.push_back(load(system_libc));
    for (const auto& module : modules)
        roots.push_back(load(module));
    for (auto id : roots)
        visit(id);
    // Unity native plug-ins are dynamically requested by name and need not be
    // DT_NEEDED by the player. Prepare their entire dependency graph now, before
    // freezing TLS and relocations; do not execute their constructors eagerly.
    // Iterate the mounted view so split/update ZAR and loose installs agree.
    std::vector<std::filesystem::path> plugins;
    for (const auto* directory : {"/app0/Media/Plugins", "/app0/prx"})
    impl->mounts.IterateDirectory(directory, [&](const auto& path, bool is_file) {
        if (!IsGuestPluginFile(path, is_file)) return;
        const auto canonical = module_path(path);
        const auto relative = canonical.lexically_relative(content_root);
        if (relative.empty() || *relative.begin() == "..")
            throw std::runtime_error("plug-in escapes content root");
        plugins.push_back(canonical);
        if (plugins.size() > 256) throw std::runtime_error("too many plug-in roots");
    });
    std::ranges::sort(plugins);
    for (const auto& plugin : plugins) visit(load(plugin));
    // Like desktop's LLE allowlist, use user-provided local libraries for their
    // complete implementation. Discover them before TLS and relocation freeze;
    // only an actual import pulls a provider into the graph.
    // Do not auto-load arbitrary firmware services or replace a game provider.
    struct LocalLibrary { std::string_view library, module, filename; };
    for (const auto& [library, module, filename] : {
             LocalLibrary{"libSceSystemGesture", "libSceSystemGesture", "libSceSystemGesture.sprx"},
             // Json2's export module is libSceJson (firmware 11.00), not its filename.
             LocalLibrary{"libSceJson2", "libSceJson", "libSceJson2.sprx"}}) {
        std::vector<u32> consumers, providers;
        for (u32 id = 0; auto* m = impl->linker->GetModule(id); ++id) {
            const auto matches = [&](const auto& l) {
                return l.name == library && l.version == 1;
            };
            if (std::ranges::any_of(m->GetImportLibs(), matches)) consumers.push_back(id);
            if (std::ranges::any_of(m->GetExportLibs(), matches)) providers.push_back(id);
        }
        if (consumers.empty()) continue;
        if (providers.size() > 1)
            throw std::runtime_error("duplicate guest provider: " + std::string(library));
        if (providers.empty()) {
            const auto path = EmulatorSettings.GetSysModulesDir() / filename;
            if (!std::filesystem::is_regular_file(path)) continue;
            const auto id = load(path);
            const auto* m = impl->linker->GetModule(id);
            if (!std::ranges::any_of(m->GetExportLibs(), [&](const auto& l) {
                    return l.name == library && l.version == 1;
                }) || !std::ranges::any_of(m->GetExportModules(), [&](const auto& mod) {
                    return mod.name == module;
                }))
                throw std::runtime_error("system guest provider identity mismatch: " + path.string());
            visit(id);
            providers.push_back(id);
        }
        for (const auto id : consumers)
            if (id != providers.front()) dependencies[id].push_back(providers.front());
    }
    std::set<u32> parameterized;
    for (const auto& [path, id] : loaded)
        if (GuestModuleNeedsLoadArguments(path.lexically_relative(content_root)))
            parameterized.insert(id);
    auto initialization = PlanGuestModuleInitialization(dependencies, roots, parameterized);
    impl->init_order = std::move(initialization.order);
    for (u32 id = 0; impl->linker->GetModule(id); ++id) {
        impl->module_lifecycle.Add(id, initialization.dependencies[id], id == 0);
        if (!initialization.startup.contains(id)) impl->deferred_modules.insert(id);
    }
    for (u32 id = 0; auto* loaded_module = impl->linker->GetModule(id); ++id) {
        const auto& libraries = loaded_module->GetExportLibs();
        if (std::any_of(libraries.begin(), libraries.end(), [](const auto& library) {
                return library.name == "libSceLibcInternal" && library.version == 1;
            })) {
            if (impl->has_system_libc) throw std::runtime_error("duplicate guest LibcInternal provider");
            impl->has_system_libc = true;
        }
    }
    if (std::filesystem::is_regular_file(system_libc) && !impl->has_system_libc)
        throw std::runtime_error("system LibcInternal does not export the expected library");
    u32 sdk{};
    if (auto* param = impl->linker->GetProcParam()) {
        Require(impl->space.ValidateRange({GuestAddress{reinterpret_cast<u64>(param)}, 0x20},
                                          GuestPermission::Read));
        if (param->size < 0x20)
            throw std::runtime_error("invalid process parameters");
        sdk = param->sdk_version;
    }
    std::string serial, title, version, save_title;
    auto export_source = std::make_shared<Diagnostics::ExecutableExport::Source>();
    export_source->app = *impl->mounts.GetMountSnapshot("/app0");
    export_source->context_id = impl->cpu.ContextId();
    for (const auto& [path, id] : loaded) {
        const auto relative = path.lexically_relative(content_root);
        if (relative.empty() || *relative.begin() == "..")
            export_source->external_modules.push_back(path);
    }
    u32 attributes{}, system_version{};
    std::array<s32, 5> app_parameters{
        Libraries::AppContent::ORBIS_APP_CONTENT_APPPARAM_SKU_FLAG_FULL, 0, 0, 0, 0};
    if (auto bytes = impl->mounts.ReadFile("/app0/sce_sys/param.sfo")) {
        PSF psf;
        if (!psf.Open(*bytes))
            throw std::runtime_error("invalid installed param.sfo");
        serial = psf.GetString("TITLE_ID").value_or("");
        save_title = psf.GetString("INSTALL_DIR_SAVEDATA").value_or(serial);
        title = psf.GetString("TITLE").value_or("");
        version = psf.GetString("APP_VER").value_or("");
        attributes = psf.GetInteger("ATTRIBUTE").value_or(0);
        system_version = psf.GetInteger("SYSTEM_VER").value_or(0);
        for (unsigned i = 1; i < app_parameters.size(); ++i)
            app_parameters[i] =
                psf.GetInteger("USER_DEFINED_PARAM_" + std::to_string(i)).value_or(0);
    }
    impl->elf_info.InitializeGuestMetadata(install_root, sdk, serial, title, version,
                                           attributes, system_version);
    impl->elf_info.SetTrophyIndexMap(ExtractTrophies("/app0/sce_sys/npbind.dat", "/app0/sce_sys/trophy"));
    impl->memory->SetGuestSdkVersion(sdk);
    impl->camera = std::make_unique<GuestCamera>(impl->space, [this](u64 size) {
        void* address{};
        const auto result = impl->memory->MapMemory(&address, GuestRuntime::ServiceAllocationBase,
            Common::AlignUp(size, 0x4000ULL), MemoryProt::CpuReadWrite | MemoryProt::GpuRead,
            MemoryMapFlags::NoFlags, VMAType::File, "SbsCameraDefaultFrames");
        if (result) throw std::runtime_error("SBS camera buffer allocation failed");
        return reinterpret_cast<u64>(address);
    }, sdk);
    // Snapshot only the local gameplay identity, never credentials or desktop events.
    std::array<GuestUser, 4> users{};
    if (const auto* user = UserManagement.GetUserByPlayerIndex(1))
        users[0] = {user->user_id, user->user_name};
    else
        users[0] = {1000, "shadPS4"}; // Same initial local profile as UserManager.
    if (GuestStorage::ValidTitle(save_title)) {
        impl->storage = std::make_unique<GuestStorage>(impl->mounts, EmulatorSettings.GetHomeDir(),
                                                       save_title, users[0].id, impl->descriptor_ids);
        impl->aio = std::make_unique<GuestAio>(impl->space, *impl->storage);
#ifdef __ANDROID__
    char io_property[PROP_VALUE_MAX]{};
    __system_property_get("debug.shadps4.profile_io", io_property);
    if (impl->storage && std::string_view(io_property) == "1") impl->storage->SetIoObserver([](const GuestStorage::IoEvent& e) {
        if (!Common::Profiler::Enabled()) return;
        if (e.kind == GuestStorage::IoEvent::Opened) {
            const auto text = fmt::format("Storage.Open.{}:{}", e.fd, e.path);
            Common::Profiler::Bookmark(text.c_str());
        } else if (e.kind == GuestStorage::IoEvent::ReadDone) {
            Common::Profiler::Counter("Storage.Read.Fd", e.fd);
            Common::Profiler::Counter("Storage.Read.Offset", e.offset);
            Common::Profiler::Counter("Storage.Read.Bytes", e.bytes);
            Common::Profiler::Counter("Storage.Read.Result", e.result.value);
            Common::Profiler::Counter("Storage.Read.Error", e.result.error);
            Common::Profiler::Counter("Storage.Read.PrepareNs", e.syscall_ns - e.begin_ns);
            Common::Profiler::Counter("Storage.Read.SyscallNs", e.end_ns - e.syscall_ns);
            Common::Profiler::Bookmark("Storage.Read.Complete");
        }
    });
#endif
        impl->sysmodules.Publish("libSceSaveData", 0x10000005);
        if (impl->save_dialog) {
            impl->save_dialog->Configure(EmulatorSettings.GetHomeDir(), save_title, users[0].id);
        impl->aio = std::make_unique<GuestAio>(impl->space, *impl->storage);
            impl->sysmodules.Publish("libSceSaveDataDialog", 0x10000006);
            impl->sysmodules.Publish("libSceCommonDialog", 0x10000007);
            std::vector<s32> dialog_users;
            for (const auto& user : users) if (user.id >= 0) dialog_users.push_back(user.id);
            impl->commerce_dialog = std::make_unique<GuestCommerceDialog>(impl->save_dialog->CommonDomain(), dialog_users, sdk);
            impl->msg_dialog = std::make_shared<GuestMsgDialog>(impl->save_dialog->CommonDomain(), std::move(dialog_users), EmulatorSettings.AreGuestDialogsSilent());
            impl->sysmodules.Publish("libSceMsgDialog", 0x1000001e);
            impl->sysmodules.Publish("libSceNpCommerce", 0x1000001f);
        }
    }
    if (GuestStorage::ValidTitle(serial)) {
        auto roots =
            Core::FileSys::ListContentRoots(EmulatorSettings.GetAddonInstallDir() / serial);
        const auto sibling =
            Core::FileSys::OverlayPath(install_root, Core::FileSys::DlcSuffix);
        if (const auto root = Core::FileSys::ResolveGameRoot(sibling)) {
            auto found = Core::FileSys::IsZArchiveFile(*root)
                             ? Core::FileSys::ExpandBundleRoots(*root)
                             : Core::FileSys::ListContentRoots(*root);
            roots.insert(roots.end(), found.begin(), found.end());
        }
        if (FileSys::IsAllInOneArchive(install_root)) {
            auto found = FileSys::ListContentRoots(install_root / FileSys::AllInOneDlc);
            roots.insert(roots.end(), found.begin(), found.end());
        }
        export_source->additional_content = roots;
        impl->app_content = std::make_unique<GuestAppContent>(impl->mounts, sdk, serial,
                                                              app_parameters, std::move(roots),
            [p = impl.get()] { return p->platform->EntitlementsChanged(); });
        impl->sysmodules.Publish("libSceAppContent", 0x1000000a);
    }
    export_source->title_id = serial;
    export_source->app_version = version;
    impl->executable_export_source = std::move(export_source);
    Diagnostics::ExecutableExport::Publish(impl->executable_export_source);
    impl->sysmodules.Publish("libSceRandom", 0x1000001d);
    impl->sysmodules.Publish("libSceDiscMap", 0x10000008);
    impl->sysmodules.Publish("libSceRtc", 0x10000009);
    impl->platform = std::make_unique<GuestPlatform>(std::move(users), sdk,
                                                     EmulatorSettings.GetConsoleLanguage(),
                                                     EmulatorSettings.IsCircleEnter());
    LOG_INFO(Lib_SystemService, "Guest console language={} (session snapshot)",
             EmulatorSettings.GetConsoleLanguage());
    impl->pad = std::make_unique<GuestPad>(GlobalPadAdapter(), *impl->platform);
    impl->sysmodules.Publish("libScePad", 0x1000000d);
    impl->audio = std::make_unique<GuestAudio>(impl->space, impl->clock);
    impl->audio3d = std::make_unique<GuestAudio3d>(impl->space, *impl->audio);
    impl->ajm = std::make_unique<GuestAjm>(impl->space);
    impl->avplayer =
        std::make_unique<GuestAvPlayer>(impl->space, [p = impl.get()] { return p->CallbackOwner("AvPlayerCallbackScratch"); });
    impl->sysmodules.Publish("libSceAvPlayer", 0x1000000e);
    std::map<s32, bool> signup;
    for (const auto& user : UserManagement.GetAllUsers())
        signup.emplace(user.user_id, !user.shadnet_npid.empty());
    impl->user_callbacks = std::make_unique<GuestUserCallbacks>(impl->space, impl->CallbackOwner("UserServiceCallbackScratch"));
    impl->np = std::make_unique<GuestNpOffline>(s32(sdk), std::move(signup));
    impl->np_offline = !EmulatorSettings.IsShadNetEnabled() &&
                       !EmulatorSettings.IsConnectedToNetwork();
    impl->network = std::make_unique<GuestNetwork>(EmulatorSettings.IsConnectedToNetwork(), impl->descriptor_ids);
    if (impl->np_offline) {
        std::vector<s32> local_users;
        for (const auto& user : UserManagement.GetAllUsers()) local_users.push_back(user.user_id);
        if (local_users.empty()) local_users.push_back(1000);
        impl->signin_dialog = std::make_unique<GuestSigninDialog>(std::move(local_users));
        impl->sysmodules.Publish("libSceSigninDialog", 0x1000001b);
        impl->np_utility = std::make_unique<GuestNpUtility>(s32(sdk));
        impl->sysmodules.Publish("libSceNpUtility", 0x10000019);
        impl->sysmodules.Publish("libSceNpScoreRanking", 0x10000020);
        impl->sysmodules.Publish("libSceNpTus", 0x10000021);
        impl->matching2 = std::make_unique<GuestMatching2Offline>();
        impl->sysmodules.Publish("libSceNpMatching2", 0x10000012);
    }
    impl->trophy = std::make_unique<GuestTrophy>();
    impl->sysmodules.Publish("libSceNpTrophy", 0x10000013);
    impl->playgo = std::make_unique<GuestPlayGo>();
    impl->sysmodules.Publish("libScePlayGo", 0x10000011);
    if (!EmulatorSettings.IsConnectedToNetwork()) {
        impl->ssl = std::make_unique<GuestSslOffline>();
        impl->sysmodules.Publish("libSceSsl", 0x1000001c);
        impl->http = std::make_unique<GuestHttp>();
        if (impl->np_offline) {
            impl->np_webapi = std::make_unique<GuestNpWebApiControl>(*impl->http);
            impl->sysmodules.Publish("libSceNpWebApi", 0x1000001a);
        }
        impl->sysmodules.Publish("libSceHttp", 0x10000010);
        impl->http2 = std::make_unique<GuestHttp2>();
        impl->sysmodules.Publish("libSceHttp2", 0x1000000f);
    }
    LOG_INFO(Lib_Net, "Session network control: requested_online={}, online transport unavailable",
             EmulatorSettings.IsConnectedToNetwork());
    // Firmware 11.00 DT_INIT invokes its constructor at +0x47f0, which enters
    // sceNetInit. Merely publishing a cold provider makes PoolCreate fail in
    // games that rely on module initialization.
    if (impl->network->Initialize() == 0)
        impl->sysmodules.Publish("libSceNet", 0x1000000b);
    if (impl->network->InitializeControl() == 0)
        impl->sysmodules.Publish("libSceNetCtl", 0x1000000c);
    // Desktop explicitly allows Json2 load bookkeeping when its optional LLE
    // is absent and no HLE exists (sysmodule_internal.cpp). Preserve that
    // offline compatibility without publishing a JSON/network implementation.
    // Any actual Json2 import still goes through Bind's unsupported gate.
    impl->sysmodules.AllowDesktopJson2Compatibility();
    // These services have actual per-session implementations. DT_NEEDED alone
    // never publishes a provider (VideoOut/Audio/AppContent must initialize theirs).
    s32 handle = 0x10000000;
    for (const char* name :
         {"libkernel", "libSceSysmodule", "libSceUserService", "libSceSystemService"})
        impl->sysmodules.Publish(name, handle++);
    // The IME implementation reuses the desktop ImGui panel, but its
    // provider is session-owned because guest text buffers stay pinned while
    // the panel is open. A real guest module, when present, is published by
    // Run's normal module loop instead.
    impl->sysmodules.Publish("libSceIme", 0x10000022);
    impl->sysmodules.Publish("libSceErrorDialog", 0x10000023);
    bool guest_ime_provider = false;
    for (u32 id = 0; auto* module = impl->linker->GetModule(id); ++id)
        guest_ime_provider |= std::filesystem::path(module->name).stem() == "libSceImeDialog";
    if (!guest_ime_provider)
        impl->sysmodules.Publish("libSceImeDialog", handle++);

    // Production imports are registered by Bind with an explicit guest policy.
    // Desktop InitHLELibs also starts graphics/audio/global worker services; those
    // require the platform session lifecycle and cannot run during relocation.
    // An unadapted import remains a named, faulting operation, never a native PC.
    if (impl->http2)
        RestoreOfflineHttp2Imports(*impl->linker->GetModule(0), impl->space, *impl->CodeToken());
    impl->linker->PrepareGuest();
    // Complete load-time coverage, including every actually loaded SPRX. Keep
    // this artifact separate from the UI summary, which may be truncated.
    try {
        nlohmann::json rows=nlohmann::json::array();
        const std::set<std::string> refused(impl->refused.begin(),impl->refused.end());
        for (s32 index=0;auto* module=impl->linker->GetModule(index);++index) {
            for (const auto& symbol:module->import_sym.GetSymbols()) {
                const auto nid=symbol.name.substr(0,symbol.name.find('#'));
                const auto* named=AeroLib::FindByNid(nid.c_str());
                const Linker::GuestImportBinding* resolved{};
                for (const auto& binding : impl->linker->GuestImportBindings())
                    if (binding.importer == module && binding.symbol.name == symbol.name)
                        resolved = &binding;
                const auto status = impl->hle_status.find(symbol.name);
                rows.push_back({{"importer",module->file.filename().string()},
                    {"symbol",symbol.name},{"name",named ? named->name : symbol.nid_name},
                    {"binding", !resolved ? "not_relocated" : refused.contains(symbol.name) ? "refused" :
                        resolved->provider == "guest_export" ? "guest_export" : "runtime_bound"},
                    {"hle_status", status == impl->hle_status.end() ?
                        (resolved && resolved->provider == "guest_export" ? "guest_export" : "not_registered") :
                        status->second},
                    {"provider", resolved ? resolved->provider : "none"},
                    {"guest_address", resolved ? resolved->symbol.virtual_address : 0}});
            }
        }
        // Some lazily synthesized import records are not in the static symbol table.
        nlohmann::json rejected=nlohmann::json::array();
        for (const auto& symbol:refused) {
            const auto nid=symbol.substr(0,symbol.find('#'));
            const auto* named=AeroLib::FindByNid(nid.c_str());
            rejected.push_back({{"symbol",symbol},{"name",named?named->name:nid}});
        }
        const nlohmann::json audit={{"schema",3},{"title_id",serial},{"pid",getpid()},
            {"offline",impl->np_offline},{"imports",std::move(rows)},
            {"refused",std::move(rejected)},{"bridges",impl->veneers.size()}};
        std::ofstream out(Common::FS::GetUserPath(Common::FS::PathType::LogDir)/"guest-imports.json");
        out<<audit.dump(2);
    } catch (const std::exception& e) {
        LOG_WARNING(Core_Linker,"Could not write import coverage: {}",e.what());
    }

    // Explicit opt-in, read once before constructors/owners. The normal path
    // neither hashes game files nor allocates patch state or SDK gateways.
    const auto module_sha = [&](const std::filesystem::path& path) {
        if (auto file = impl->mounts.Open(path.generic_string()))
            return GuestPatch::StreamSha256([&](void* dst, u64 size) { return file->Read(dst, size); });
        return GuestPatch::FileSha256(path);
    };
    std::string auto_tag_path;
#if defined(__ANDROID__)
    char auto_tag_property[PROP_VALUE_MAX]{};
    __system_property_get("debug.shadps4.guest_auto_tag", auto_tag_property);
    auto_tag_path=auto_tag_property;
#else
    if (const char* path=std::getenv("SHADPS4_GUEST_AUTO_TAG")) auto_tag_path=path;
#endif
    if (!auto_tag_path.empty()) {
        // Resolve the declared module, while Profile validates all remaining identity.
        std::ifstream input(auto_tag_path);
        if (std::filesystem::file_size(auto_tag_path)>1024*1024) throw std::runtime_error("auto tag profile too large");
        const auto profile=nlohmann::json::parse(input);
        const auto name=profile.at("identity").at("module").get<std::string>();
        Module* target{};
        for (u32 id=0; auto* m=impl->linker->GetModule(id); ++id) if (m->name==name) {
            if (target) throw std::runtime_error("ambiguous auto tag module");
            target=m;
        }
        if (!target) throw std::runtime_error("auto tag module not loaded");
        impl->auto_tag=GuestAutoTag::Profile::Load(auto_tag_path,
            {serial,target->name,module_sha(target->file),target->GetBaseAddress(),target->aligned_base_size},
            impl->space,impl->cpu.ContextId());
    }
    std::string patch_path;
#if defined(__ANDROID__)
    char patch_property[PROP_VALUE_MAX]{};
    __system_property_get("debug.shadps4.guest_patch", patch_property);
    patch_path = patch_property;
#else
    if (const char* path = std::getenv("SHADPS4_GUEST_PATCH")) patch_path = path;
#endif
    if (!patch_path.empty()) {
        const auto package = GuestPatch::Package::Load(patch_path);
        Module* target{};
        for (u32 id = 0; auto* m = impl->linker->GetModule(id); ++id) {
            if (m->name == package.module) {
                if (target) throw std::runtime_error("ambiguous guest patch module");
                target = m;
            }
        }
        if (!target) throw std::runtime_error("guest patch module not loaded");
        impl->guest_patch = std::make_unique<GuestPatch::Manager>(
            impl->cpu, impl->space, impl->registry,
            [](const char* name, int64_t value) { Common::Profiler::Counter(name, value); });
        impl->guest_patch->Install(
            package,
            {serial, target->name, module_sha(target->file), target->GetBaseAddress(),
             target->aligned_base_size,
             package.executable_sha256.empty() ? std::string{}
                                               : module_sha(main_path)},
            *impl->CodeToken(), [this](u64 size, u64 near) {
                return GuestPatch::Allocation{impl->Allocate(size, "GuestFunctionPatch", near),
                                              Common::AlignUp(size, 0x4000ULL)};
            });
        // Keep the desktop VM ledger and GPU permission view consistent with
        // the patch loader's page-level publication, before locking reservations.
        for (const auto& debug_module : impl->guest_patch->DebugModules()) {
            for (const auto& range : debug_module.segments) {
                for (const auto& mapping : impl->space.Mappings()) {
                    if (mapping.range.base.value >= range.base.value && mapping.range.End() <= range.End()) {
                        MemoryProt prot = MemoryProt::NoAccess;
                        if (HasPermission(mapping.permission, GuestPermission::Read)) prot |= MemoryProt::CpuRead;
                        if (HasPermission(mapping.permission, GuestPermission::Write)) prot |= MemoryProt::CpuWrite;
                        if (HasPermission(mapping.permission, GuestPermission::Execute)) prot |= MemoryProt::CpuExec;
                        if (impl->memory->Protect(mapping.range.base.value, mapping.range.size, prot))
                            throw std::runtime_error("patch VM permission synchronization failed");
                    }
                }
            }
        }
        impl->patch_reservations = impl->guest_patch->ProtectedRanges();
        LOG_INFO(Core_Linker, "Guest patch installed: {}", impl->guest_patch->Status());
    }
    if (impl->auto_tag) {
        if (impl->guest_patch) impl->auto_tag->Remap(*impl->guest_patch);
        Require(impl->cpu.InstallExecutionProbes(impl->auto_tag));
        GuestAutoTag::SetControl(impl->auto_tag);
        LOG_INFO(Core_Linker, "Guest auto tag installed: {}", impl->auto_tag->Status());
    }
    impl->patch_control = std::make_shared<PatchControl>();
    impl->patch_control->command = [this](const std::vector<std::string>& args) {
        if (!impl->guest_patch) return std::string("status: disabled_at_startup\n");
        if (args.empty() || (args.size()==1 && args[0]=="status"))
            return impl->guest_patch->Status();
        if ((args.size()!=2 && args.size()!=3) || (args[0]!="enable" && args[0]!="disable") ||
            args[1]!=std::to_string(impl->cpu.ContextId()))
            return std::string("status: invalid_arguments\ndetail: enable|disable requires current context ID\n");
        Impl::CodePublication vm(*impl);
        impl->guest_patch->SetEnabled(args[0] == "enable", *impl->CodeToken(),
                                      args.size() == 3 ? args[2] : std::string_view{});
        return impl->guest_patch->Status();
    };
    GuestPatch::SetControl(impl->patch_control);
    std::vector<DebugModule> debug_modules;
    for (u32 id = 0; auto* m = impl->linker->GetModule(id); ++id) {
        DebugModule debug_module{m->name, m->GetBaseAddress(), {}};
        const auto info = m->GetModuleInfo();
        for (u32 i = 0; i < std::min<u32>(info.num_segments, info.segments.size()); ++i) {
            const auto& segment = info.segments[i];
            if (segment.size)
                debug_module.segments.push_back({GuestAddress{segment.address}, segment.size});
        }
        debug_modules.push_back(std::move(debug_module));
    }
    if (impl->guest_patch) {
        auto patch_modules = impl->guest_patch->DebugModules();
        debug_modules.insert(debug_modules.end(), patch_modules.begin(), patch_modules.end());
    }
    impl->cpu.SetDebugModules(std::move(debug_modules));
    impl->prepared = true;
}
Result<GuestCallResult> GuestRuntime::Run(const std::vector<std::string>& args) {
    Common::Profiler::Bookmark("Startup.RunBegin");
    Common::Profiler::Phase bootstrap_stage{"Startup.GuestBootstrap"};
    if (!impl->prepared)
        return MakeError(ErrorCategory::WrongState, "GuestRuntime::Run", "Prepare is required");
    auto* module = impl->linker->GetModule(0);
    auto owner = impl->NewOwner(GuestThreadAttributes{.size = 2 << 20});
    try {
        impl->Attach(owner, module->GetEntryAddress());
        // Like the desktop libkernel bootstrap, initialize only the system libc
        // here. The game's libc.prx owns allocator initialization in DT_INIT;
        // calling it early initializes its default heap mutex twice (MHR aborts).
        // These exports are guest PCs and must go through FEX.
        for (u32 id = 0; auto* m = impl->linker->GetModule(id); ++id) {
            if (m->name != "libSceLibcInternal.sprx")
                continue;
            if (const auto entry = m->FindByName("_malloc_init")) {
                Common::Profiler::Scope profile{"Startup.GuestMallocInit"};
                Common::Profiler::Phase stage{"Startup.GuestMallocInit"};
                auto result = Require(impl->Call(reinterpret_cast<u64>(entry), {}));
                if (result.reason != StopReason::Returned)
                    throw GuestCallbackStop{result};
                if (static_cast<s32>(result.return_value) != 0)
                    throw std::runtime_error("guest _malloc_init failed: " + m->name);
            }
            if (const auto entry = m->FindByName("sceLibcInternalMemoryMutexEnable")) {
                auto result = Require(impl->Call(reinterpret_cast<u64>(entry), {}));
                if (result.reason != StopReason::Returned)
                    throw GuestCallbackStop{result};
            }
        }
        for (auto id : impl->init_order) {
            auto* m = impl->linker->GetModule(id);
            const auto profile_name = "Startup.ModuleInit." + m->name;
            Common::Profiler::Scope profile{profile_name.c_str()};
            Common::Profiler::Phase stage{profile_name.c_str()};
            const auto result = impl->StartModule(id, 0, 0);
            if (result.error || (result.initialized && result.value))
                throw std::runtime_error("module initialization failed: " + m->name);
            LOG_INFO(Core_Linker, "Guest module {}: {}", m->name,
                     m->dynamic_info.has_init ? "DT_INIT completed" : "no DT_INIT");
        }
        u64 params_address;
        {

            params_address = impl->Allocate(0x4000, "EntryParams");
        }
        EntryParams params{};
        params.argc = std::min<size_t>(args.size(), 32);
        params.entry_addr = module->GetEntryAddress();
        size_t cursor = sizeof(EntryParams);
        for (int i = 0; i < params.argc; ++i) {
            if (args[i].size() + 1 > 0x4000 - cursor)
                throw std::runtime_error("guest argv exceeds bound");
            params.argv[i] = reinterpret_cast<const char*>(params_address + cursor);
            Require(impl->space.WriteData(
                GuestAddress{params_address + cursor},
                std::as_bytes(std::span{args[i].c_str(), args[i].size() + 1})));
            cursor += args[i].size() + 1;
        }
        impl->Write(params_address, params);
        auto snapshot = Require(impl->cpu.ReadRegisters(owner->handle));
        RegisterPatch patch;
        patch.fields = RegisterValidity::Gpr | RegisterValidity::Rip;
        patch.gpr_mask =
            (1u << Index(Gpr::Rdi)) | (1u << Index(Gpr::Rsi)) | (1u << Index(Gpr::Rsp));
        patch.values.Set(Gpr::Rdi, params_address);
        patch.values.Set(Gpr::Rsi, impl->cpu.Capabilities().return_gate_address);
        const u64 stack = Common::AlignDown(owner->stack + owner->stack_size, 16ULL) - 24;
        patch.values.Set(Gpr::Rsp, stack);
        patch.values.rip = module->GetEntryAddress();
        impl->Write(stack, static_cast<u64>(params.argc));
        impl->Write(stack + 8, reinterpret_cast<u64>(params.argv[0]));
        Require(impl->cpu.WriteRegisters(owner->handle, patch, snapshot.stop_epoch));
        bootstrap_stage.End();
        Common::Profiler::Bookmark("Startup.GuestEntry");
        auto run = impl->cpu.Run(owner->handle, {});
        if (!run) {
            impl->Finish(owner);
            if (auto failure = impl->ChildFailure()) return *failure;
            return run.GetError();
        }
        GuestCallResult result;
        result.reason = run.Value().primary_reason;
        result.return_value = run.Value().snapshot.registers.Get(Gpr::Rax);
        result.snapshot = run.Value().snapshot;
        result.fault = run.Value().fault;
        result.pending_reasons = run.Value().pending_reasons;
        result.invocation_id = run.Value().invocation_id;
        result.stop_epoch = run.Value().snapshot.stop_epoch;
        owner->result = result;
        impl->Finish(owner);
        if (impl->graphics)
            impl->graphics->CheckHealth();
        if (auto failure = impl->ChildFailure()) return *failure;
        if (owner->error)
            return *owner->error;
        return *owner->result;
    } catch (const GuestCallbackStop& stopped) {
        owner->result = stopped.result;
        (void)impl->Cancel();
        impl->Finish(owner);
        if (auto failure = impl->ChildFailure()) return *failure;
        return stopped.result;
    } catch (const std::exception& e) {
        auto error = MakeError(ErrorCategory::BackendFailure, "GuestRuntime::Run", e.what());
        (void)impl->Cancel();
        impl->Finish(owner);
        if (auto failure = impl->ChildFailure()) return *failure;
        return error;
    }
}
Status GuestRuntime::RequestCancel() {
    return impl->Cancel();
}
Status GuestRuntime::WaitStopped(u64 timeout_ns) {
    std::unique_lock lock(impl->threads_mutex);
    if (!impl->threads_changed.wait_for(lock, std::chrono::nanoseconds(timeout_ns), [&] {
            for (auto& [id, o] : impl->owners)
                if (!o->finished)
                    return false;
            return true;
        }))
        return MakeError(ErrorCategory::Timeout, "GuestRuntime::WaitStopped",
                         "guest owners are still unwinding");
    return Ok();
}
std::string GuestRuntime::Diagnostics() const {
    std::lock_guard lock(impl->threads_mutex);
    std::string text = "production Linker/VM; owners=" + std::to_string(impl->owners.size()) +
                       " veneers=" + std::to_string(impl->veneers.size());
    std::map<std::string, u32> hle_counts;
    for (const auto& [name, status] : impl->hle_status)
        ++hle_counts[status];
    for (const auto& [status, count] : hle_counts)
        text += "\nHLE_STATUS " + status + "=" + std::to_string(count);
    for (auto& name : impl->refused)
        text += "\nUNSUPPORTED_IMPORT " + name;
    for (auto& name : impl->hle_modules)
        text += "\nHLE_MODULE " + name;
    {
        std::scoped_lock lock(impl->graphics_mutex);
        text += impl->graphics ? " graphics=ready" : " graphics=not-created";
        if (impl->graphics) text += " guest_presents=" + std::to_string(impl->graphics->VideoOut().guest_presents.load());
    }
    text += " modules=" + std::to_string(impl->init_order.size() + impl->deferred_modules.size() + 1);
    return text;
}
std::string GuestRuntime::OperationName(u64 operation) const {
    // Bindings are immutable once Prepare finishes, before any owner executes.
    const auto found = impl->operation_names.find(operation);
    return found == impl->operation_names.end() ? std::string{} : found->second;
}
u64 GuestRuntime::PresentCount() const {
    std::scoped_lock lock(impl->graphics_mutex);
    return impl->graphics ? impl->graphics->VideoOut().guest_presents.load() : 0;
}
} // namespace Core::HostRuntime
