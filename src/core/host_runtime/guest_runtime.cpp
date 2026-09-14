// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/host_runtime/guest_graphics_hle.h"
#include <atomic>
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
#include "common/elf_info.h"
#include "common/singleton.h"
#include "core/aerolib/aerolib.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/guest_cpu/hle/scope.h"
#include "core/guest_cpu/hle/veneer_allocator.h"
#include "core/host_runtime/guest_ajm.h"
#include "core/host_runtime/guest_app_content.h"
#include "core/host_runtime/guest_audio.h"
#include "core/host_runtime/guest_avplayer.h"
#include "core/host_runtime/guest_clock.h"
#include "core/host_runtime/guest_graphics.h"
#include "core/host_runtime/guest_kernel_semaphore.h"
#include "core/host_runtime/guest_libc_policy.h"
#include "core/host_runtime/guest_mutex.h"
#include "core/host_runtime/guest_network.h"
#include "core/host_runtime/guest_np.h"
#include "core/host_runtime/guest_pad.h"
#include "core/host_runtime/guest_platform.h"
#include "core/host_runtime/guest_rtc.h"
#include "core/host_runtime/guest_runtime.h"
#include "core/host_runtime/guest_rwlock.h"
#include "core/host_runtime/guest_save_dialog.h"
#include "core/host_runtime/guest_semaphore.h"
#include "core/host_runtime/guest_storage_hle.h"
#include "core/host_runtime/guest_sysmodule_hle.h"
#include "core/host_runtime/guest_thread_attributes.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"
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
#endif

namespace Core::HostRuntime {
using namespace GuestCpu;
using namespace GuestCpu::Hle;
namespace {
void Require(Status status) {
    if (!status)
        throw std::runtime_error(Describe(status.GetError()));
}
template <class T>
T Require(Result<T> result) {
    if (!result)
        throw std::runtime_error(Describe(result.GetError()));
    return std::move(result).Value();
}
struct FunctionAdapter final : HleCallAdapter {
    std::function<Status(HleCallFrame&)> call;
    explicit FunctionAdapter(std::function<Status(HleCallFrame&)> f) : call(std::move(f)) {}
    Status Invoke(HleCallFrame& frame) const override {
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
    GuestClock clock;
    std::unique_ptr<GuestStorage> storage;
    std::unique_ptr<GuestNetwork> network;
    std::unique_ptr<GuestNpOffline> np;
    bool np_offline{};
    std::unique_ptr<GuestAjm> ajm;
    std::unique_ptr<GuestAvPlayer> avplayer;
    std::unique_ptr<GuestAudio> audio;
    std::unique_ptr<GuestKernelSemaphore> kernel_semaphores;
    std::unique_ptr<GuestPad> pad;
    std::shared_ptr<GuestSaveDialog> save_dialog;
    std::shared_ptr<Frontend::Window> graphics_window;
    std::shared_ptr<const Vulkan::Driver> graphics_driver;
    std::unique_ptr<GuestGraphics> graphics;
    // CPU VM permissions and GPU watch permissions are separate. Signal-time
    // watch retirement must not take the VM transaction lock or retire FEX code.
    std::unique_ptr<std::atomic<u8>[]> gpu_pages =
        std::make_unique<std::atomic<u8>[]>(ReservationEnd / 4096);
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
                VmGuard vm(*this);
                video_labels = Allocate(0x4000, "VideoOutLabelsAndShaders");
                for (u32 i = 0; i < 3; ++i)
                    Require(space.PublishCode(
                        *vm_token,
                        {GuestAddress{video_labels + (i + 1) * 4096},
                         Libraries::GnmDriver::GetEmbeddedShader(i).size_bytes()},
                        std::as_bytes(Libraries::GnmDriver::GetEmbeddedShader(i))));
            }
            auto created = std::make_unique<GuestGraphics>(
                graphics_window, graphics_driver,
                [this] { return clock.ticks.GetTimeUS(clock.origin); },
                [this] { return clock.ticks.GetUptime(); },
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
    std::unique_ptr<GuestRwlockDomain> rwlock_domain;
    std::unique_ptr<GuestSemaphoreDomain> semaphore_domain;
    int backing_fd{-1};
    u8* backing{};
    static constexpr u64 BackingSize = 12ULL << 30;
    std::recursive_mutex vm_mutex;
    std::optional<QuiescenceToken> vm_token;
    std::unique_ptr<MemoryManager> memory;
    std::unique_ptr<Common::Singleton<MemoryManager>::Binding> memory_binding;
    std::unique_ptr<Linker> linker;
    std::unique_ptr<Common::Singleton<Linker>::Binding> linker_binding;
    Common::ElfInfo elf_info;
    std::unique_ptr<Common::Singleton<Common::ElfInfo>::Binding> elf_binding;
    FileSys::MntPoints mounts;
    std::unique_ptr<GuestAppContent> app_content;
    std::unique_ptr<Common::Singleton<FileSys::MntPoints>::Binding> mount_binding;
    std::map<std::string, u64> veneers;
    std::map<u64, std::string> operation_names;
    u64 stack_guard{}, progname_object{}, environ_object{}, heap_trace{};
    // Guest PCs copied from the libc heap table. Never install them in the
    // desktop Linker's native function-pointer HeapAPI.
    std::array<u64, sizeof(HeapAPI) / sizeof(u64)> guest_heap_api{};
    std::string program_name;
    std::map<std::string, std::function<Status(HleCallFrame&)>> handlers;
    std::vector<std::string> refused;
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
        int startup_error{};
        bool startup_done{};
        std::array<SpecificValue, 256> specific{};
        ThreadHandle handle{};
        std::thread worker;
        std::optional<GuestCallResult> result;
        std::optional<Error> error;
        bool started{}, finished{}, joined{}, detached{}, destructors_started{};
    };
    std::map<u64, std::shared_ptr<Owner>> owners;
    u64 next_id{1};
    std::optional<GuestCallResult> child_fault;
    std::optional<Error> child_error;
    bool prepared{}, has_system_libc{};
    std::vector<u32> init_order;
    std::vector<std::string> hle_modules;
    std::mutex once_mutex;
    std::map<u64, u64> once_owners;
    std::unique_ptr<GuestPlatform> platform;
    GuestSysmodules sysmodules{[](u32 id) -> std::optional<std::string> {
        const char* name{};
        if (!Libraries::SysModule::LookupSysmodule(id, &name, nullptr))
            return std::nullopt;
        return name;
    }};

    struct VmGuard {
        Impl& rt;
        std::unique_lock<std::recursive_mutex> lock;
        bool outer;
        explicit VmGuard(Impl& rt) : rt(rt), lock(rt.vm_mutex), outer(!rt.vm_token) {
            if (outer) {
                rt.vm_token.emplace(Require(rt.cpu.QuiesceContext(2'000'000'000)));
                try {
                    if (rt.graphics)
                        rt.graphics->WaitIdle();
                } catch (...) {
                    rt.vm_token.reset();
                    throw;
                }
            }
        }
        ~VmGuard() {
            if (outer)
                rt.vm_token.reset();
        }
    };
    Impl(CpuContext& c, GuestAddressSpace& s, HleCallRegistry& r) : cpu(c), space(s), registry(r) {
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
            VmGuard vm(*this);
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
                        VmGuard vm(*this);
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
                    VmGuard vm(*this);
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
        (void)Cancel();
        avplayer.reset(); // join native decode and owned FEX callback workers before VM teardown
        for (auto& [id, o] : owners)
            if (o->worker.joinable())
                o->worker.join();
        audio.reset(); // joins audio workers before clock/VM teardown
        ajm.reset(); // joins decoder worker before VM/backing teardown
        storage.reset();
        app_content.reset();
        graphics.reset();
        graphics_window.reset();
        graphics_driver.reset();
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
    void Update(GuestAddressSpace::VmOperation op, u64 address, u64 size,
                GuestPermission permission, int fd = -1, u64 offset = 0) {
        if (!vm_token)
            throw std::logic_error("VM mutation outside production transaction");

        Require(space.UpdateVmUnderToken(*vm_token, op, {GuestAddress{address}, size}, permission,
                                         fd, offset));
        const u8 state =
            op == GuestAddressSpace::VmOperation::Unmap ? 0 : 0x80 | static_cast<u8>(permission);
        for (u64 page = address / 4096; page < (address + size) / 4096; ++page) {
            const auto watches =
                op == GuestAddressSpace::VmOperation::Protect ? gpu_pages[page].load() & 24 : 0;
            gpu_pages[page].store(state | watches, std::memory_order_release);
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
    void* MapFile(VAddr address, u64 size, u64 offset, u32 prot, uintptr_t fd) override {
        Update(GuestAddressSpace::VmOperation::Map, address, size, Permission(prot), fd, offset);
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
        const u8 state = gpu_pages[address / 4096].load(std::memory_order_acquire);
        const u8 access = write ? 2 : 1;
        return (state & 0x80) && (state & access) && (state & ((access << 3) | (access << 5)));
    }
    void ProtectGpu(VAddr address, u64 size, MemoryPermission permission) override {
        if (!space.OwnsRange({GuestAddress{address}, size}) || (address | size) % 4096)
            throw std::runtime_error("invalid GPU watch range");
        const u8 watch = (~static_cast<u8>(permission) & 3) << 3;
        for (u64 page = address / 4096; page < (address + size) / 4096; ++page) {
            const auto old = gpu_pages[page].load(std::memory_order_acquire);
            // Never let GPU tracking alter executable code or revive an unmapped page.
            if (!(old & 0x80) || (old & 4))
                throw std::runtime_error("GPU watch requires mapped non-executable memory");
            // Publish before protection; retain a retired-watch bit until the
            // next VM mutation for faults already pending on another owner.
            const u8 retired = ((old & 24) & ~watch) << 2;
            gpu_pages[page].store((old & ~u8(24)) | watch | retired, std::memory_order_release);
            const int prot = (old & 3) & static_cast<u8>(permission);
            if (::mprotect(reinterpret_cast<void*>(page * 4096), 4096, prot) != 0)
                throw std::runtime_error("GPU watch mprotect failed");
        }
    }
    u64 Allocate(u64 size, std::string_view name, u64 base = 0x1000000000ULL) {
        void* address{};
        auto result = memory->MapMemory(&address, base, Common::AlignUp(size, 0x4000ULL),
                                        MemoryProt::CpuReadWrite, MemoryMapFlags::NoFlags,
                                        VMAType::File, std::string(name));
        if (result)
            throw std::runtime_error("guest allocation failed: " + std::string(name));
        return reinterpret_cast<u64>(address);
    }
    template <class T>
    T Read(u64 address) {
        T result{};
        Require(space.Read(GuestAddress{address}, std::as_writable_bytes(std::span{&result, 1})));
        return result;
    }
    template <class T>
    void Write(u64 address, const T& value) {
        // Short outputs (clock/TLS/handles included) cannot race a different
        // owner's VM publication. This gate is released before guest waits or callbacks.
        std::lock_guard vm(vm_mutex);
        Require(space.Write(GuestAddress{address}, std::as_bytes(std::span{&value, 1})));
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
        }
        {
            VmGuard vm(*this);
            o->stack_size = attributes.size;
            if (attributes.stack) {
                Require(space.ValidateRange({GuestAddress{attributes.stack}, attributes.size},
                                            GuestPermission::Read | GuestPermission::Write));
                o->stack = attributes.stack;
                o->attributes.guard = 0; // Caller owns neighboring memory; never protect it.
            } else {
                const u64 guard = Common::AlignUp(std::max<u64>(0x4000, attributes.guard), 0x4000ULL);
                const u64 allocation = Allocate(o->stack_size + guard, "GuestStack");
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
                    Require(space.Read(GuestAddress{m->tls.image_virtual_addr},
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
        std::lock_guard lock(threads_mutex);
        auto it = owners.find(active_thread);
        if (active_runtime != this || it == owners.end())
            throw std::logic_error("no production guest owner");
        return it->second;
    }
    Result<GuestCallResult> Call(u64 entry, const GuestCallArgs& args) {
        if (auto* scope = HleScope::Current())
            return scope->InvokeGuest(GuestCodeAddress{entry}, args);
        return cpu.InvokeGuest(Current()->handle, GuestCodeAddress{entry}, args, {});
    }
    void Attach(const std::shared_ptr<Owner>& o, u64 entry) {
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
            std::scoped_lock vm(vm_mutex);
            handle = Require(cpu.CreateThread(init));
        }
        {
            std::lock_guard lock(threads_mutex);
            o->handle = handle;
            o->started = true;
            if (cancelling)
                Require(cpu.RequestInterrupt(handle, InterruptReason::Cancel));
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
            // A finished owner has already released its Run lease. Serialize
            // backend retirement with publication so a new VM token cannot turn
            // ordinary pthread exit into a Busy failure (or strand its handle).
            std::scoped_lock vm(vm_mutex);
            Require(cpu.DestroyThread(o->handle));
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
    GuestAvPlayer::Callbacks AvCallbacks() {
        struct State {
            std::shared_ptr<Owner> owner;
            u64 scratch{};
            std::atomic_bool closing{};
        };
        auto state = std::make_shared<State>();
        return {
            .begin =
                [this, state](u64 entry) {
                    auto o = NewOwner();
                    {
                        std::lock_guard lock(threads_mutex);
                        state->owner = o;
                    }
                    {
                        VmGuard vm(*this);
                        state->scratch =
                            Allocate(GuestAvPlayer::ScratchSize, "AvPlayerCallbackScratch");
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
                        std::optional<VmGuard> vm;
                        if (!cancelling) {
                            try {
                                vm.emplace(*this);
                            } catch (...) {
                                // Stop may race entry to Quiesce/WaitIdle. Only
                                // defer admission failure; mutation errors below
                                // remain failures even if Stop arrives meanwhile.
                                if (!cancelling) throw;
                            }
                        }
                        if (vm && !cancelling) {
                            if ((state->scratch &&
                                 memory->UnmapMemory(state->scratch, GuestAvPlayer::ScratchSize)) ||
                                memory->UnmapMemory(o->tls, o->tls_size) ||
                                memory->UnmapMemory(o->stack - o->attributes.guard,
                                                    o->stack_size + o->attributes.guard))
                                throw std::runtime_error("AvPlayer callback VM release failed");
                        }
                        vm.reset();
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
        if (avplayer)
            avplayer->RequestStop();
        if (audio) audio->RequestStop();
        if (ajm) ajm->RequestStop();
        if (storage)
            storage->Cancel();
        if (save_dialog)
            save_dialog->Cancel();
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
        // Only the sysmodule NIDs this runtime implements are admitted under the
        // libSceSysmodule library suffix; every other sysmodule function still
        // name-faults so a missing capability is never silently a success.
        static const std::set<std::string> sysmodule_functions{
            "g8cM39EUZ6o", "39iV5E1HoCk", "eR2bZFAAU0Q", "fMP5NHUOaMk",
            "ynFKQ5bfGks", "D8cuU4d72xM", "hHrGoGoNf+s"};
        // Guest-facing libSceUserService startup family; same policy as above.
        static const std::set<std::string> userservice_functions{
            "j3YMu1MVNNo", "bwFjS+bX9mA", "CdWp0oHWGr0", "eNb53LQJmIM",
            "fPhymKNvK-A", "1xxcMiGu2fo", "yH17Q6NWtVg"};
        // Guest-facing libSceSystemService startup family; same policy as above.
        static const std::set<std::string> systemservice_functions{"fZo48un7LK4", "rPo6tV8D9bM",
                                                                   "656LMQSrg6U", "Vo5V8KAwCmk"};
        // Explicit checked GPU policies plus retail owner registration; unknown
        // GNM entry points still produce a named unsupported-import fault.
        auto gnmdriver_functions = graphics_gnm_nids;
        gnmdriver_functions.insert("ZFqKFl23aMc");
        std::set<std::string> videoout_functions{"Up36PTk687E", "6kPnj51T62Y", "8XGijEoThE0",
                                                 "i6-sR91Wt-4", "1FZBKy8HeNU", "d1AjT2uZJn0"};
        videoout_functions.insert(graphics_video_nids.begin(), graphics_video_nids.end());
        const bool save_nid = std::any_of(std::begin(StorageEntries), std::end(StorageEntries),
                                          [&](const auto& e) { return e.save && e.nid == nid; });
        const bool dialog_nid = GuestSaveDialog::IsSaveNid(nid);
        const bool common_nid = GuestSaveDialog::IsCommonNid(nid);
        const bool np_nid = IsNpOfflineNid(nid);
        const bool ssl_nid = nid == "hdpVEUDFW3s" || nid == "0K1yQ6Lv-Yc";
        const bool kernel_nid =
            !np_nid && !ssl_nid && !IsAvPlayerNid(nid) && !IsAudioNid(nid) && !IsAjmNid(nid) &&
            !IsPadNid(nid) && !IsNetNid(nid) && !IsNetCtlNid(nid) && !IsAppContentNid(nid) &&
            !IsRtcNid(nid) && !IsDiscMapNid(nid) && !dialog_nid && !common_nid && !save_nid &&
            !videoout_functions.contains(nid) && !sysmodule_functions.contains(nid) &&
            !userservice_functions.contains(nid) && !systemservice_functions.contains(nid) &&
            !gnmdriver_functions.contains(nid) && nid != "NWtTN10cJzE";
        std::shared_ptr<HleCallAdapter> adapter;
        if (auto it = handlers.find(nid);
            it != handlers.end() &&
            ((avplayer && IsAvPlayerNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceAvPlayer#1#libSceAvPlayer#Function") ||
             (audio && IsAudioNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceAudioOut#1#libSceAudioOut#Function") ||
             (ajm && IsAjmNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceAjm#1#libSceAjm#Function") ||
             (pad && IsPadNid(nid) &&
              symbol.name.substr(nid.size()) == "#libScePad#1#libScePad#Function") ||
             (np && AdmitsNpOffline(nid, symbol.name.substr(nid.size()), np_offline)) ||
             (ssl_nid && symbol.name.substr(nid.size()) == "#libSceSsl#1#libSceSsl#Function") ||
             (network && IsNetNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceNet#1#libSceNet#Function") ||
             (network && IsNetCtlNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceNetCtl#1#libSceNetCtl#Function") ||
             (app_content && IsAppContentNid(nid) &&
              symbol.name.substr(nid.size()) ==
                  "#libSceAppContent#1#libSceAppContentUtil#Function") ||
             (IsRtcNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceRtc#1#libSceRtc#Function") ||
             (IsDiscMapNid(nid) &&
              symbol.name.substr(nid.size()) == "#libSceDiscMap#1#libSceDiscMap#Function") ||
             (kernel_nid &&
              (symbol.name.substr(nid.size()) == "#libkernel#1#libkernel#Function" ||
               symbol.name.substr(nid.size()) == "#libScePosix#1#libkernel#Function")) ||
             (save_dialog && dialog_nid &&
              symbol.name.substr(nid.size()) ==
                  "#libSceSaveDataDialog#1#libSceSaveDataDialog#Function") ||
             (save_dialog && common_nid &&
              symbol.name.substr(nid.size()) ==
                  "#libSceCommonDialog#1#libSceCommonDialog#Function") ||
             (save_nid &&
              symbol.name.substr(nid.size()) == "#libSceSaveData#1#libSceSaveData#Function") ||
             (symbol.name.substr(nid.size()) == "#libSceSysmodule#1#libSceSysmodule#Function" &&
              sysmodule_functions.contains(nid)) ||
             (symbol.name.substr(nid.size()) == "#libSceUserService#1#libSceUserService#Function" &&
              userservice_functions.contains(nid)) ||
             (symbol.name.substr(nid.size()) ==
                  "#libSceSystemService#1#libSceSystemService#Function" &&
              systemservice_functions.contains(nid)) ||
             (symbol.name.substr(nid.size()) == "#libSceGnmDriver#1#libSceGnmDriver#Function" &&
              gnmdriver_functions.contains(nid)) ||
             (graphics_window &&
              symbol.name.substr(nid.size()) == "#libSceVideoOut#1#libSceVideoOut#Function" &&
              videoout_functions.contains(nid)) ||
             symbol.name == "NWtTN10cJzE#libSceLibcInternalExt#1#libSceLibcInternal#Function"))
            adapter = std::make_shared<FunctionAdapter>(it->second);
        else {
            // No unspecialized raw pointers, host stubs, or scalar functions with
            // hidden desktop/thread state are callable merely by having a descriptor.
            refused.push_back(symbol.name);
            adapter = std::make_shared<FunctionAdapter>([name = symbol.name](HleCallFrame&) {
                return MakeError(ErrorCategory::Unsupported, "ProductionHle",
                                 "unimplemented import policy: " + name);
            });
        }
        const auto op = Require(registry.Adopt(std::move(adapter), symbol.name));
        operation_names.emplace(op, symbol.name);
        VmGuard vm(*this);
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
    for (const auto& entry : StorageEntries) {
        handlers[std::string(entry.nid)] = [this, entry](HleCallFrame& frame) {
            if (!storage)
                return Status(MakeError(ErrorCategory::Unsupported, "GuestStorage",
                                        "valid persistent title identity is required"));
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
            // Regular-file HLE has no guest callbacks. Serialize its pin set
            // with VM publication; never let a coordinator drain between pins.
            std::unique_lock vm(vm_mutex, std::defer_lock);
            if (!entry.save)
                vm.lock();
            const auto result = DispatchStorage(
                *storage, space, entry, args, [this](int error) { return PosixFailure(error); });
            if (entry.save)
                LOG_INFO(Lib_SaveData, "Session storage nid={} result={:#x}", entry.nid, result);
            frame.registers.Set(Gpr::Rax, result);
            return Ok();
        };
    }
    thread_attributes = std::make_unique<GuestThreadAttributeDomain>(space, [this] {
        VmGuard vm(*this);
        return Allocate(0x4000, "GuestThreadAttr");
    }, &vm_mutex);
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
    kernel_semaphores = std::make_unique<GuestKernelSemaphore>(space, vm_mutex);
    for (const char* nid : {"188x57JYp0g", "Zxa0VhQVTsk", "4czppHBiriw", "12wOHk8ywb0", "4DM06U2BNEY", "R1Jvn8bSCW8"}) {
        bind({nid}, [this, nid](const auto& args) -> u64 {
            return kernel_semaphores->Dispatch(nid, args, Current()->attributes.priority,
                                               HleScope::Current()->CancellationToken());
        });
    }
    semaphore_domain = std::make_unique<GuestSemaphoreDomain>(space, [this] {
        VmGuard vm(*this);
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
    rwlock_domain = std::make_unique<GuestRwlockDomain>(space, [this] {
        VmGuard vm(*this);
        return Allocate(0x4000, "GuestRwlock");
    }, &vm_mutex);
    auto pthread_bind = [&](const char* posix, const char* sce, std::function<int(const std::array<u64, 6>&)> fn) {
        bind({posix}, [fn](const auto& a) -> u64 { return fn(a); });
        bind({sce}, [fn](const auto& a) -> u64 {
            const auto error = fn(a); return error ? u64(0x80020000u | error) : 0;
        });
    };
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
            if (!space.Read(GuestAddress{a[1]}, std::as_writable_bytes(std::span{&ts, 1}))) return POSIX_EFAULT;
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
    mutex_domain = std::make_unique<GuestMutexDomain>(space, [this] {
        VmGuard vm(*this);
        return Allocate(0x4000, "GuestMutex");
    }, &vm_mutex);
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
        if (!space.Read(GuestAddress{a[2]}, std::as_writable_bytes(std::span{&absolute, 1})))
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
            VmGuard vm(*this);
            if (!heap_trace)
                heap_trace = Allocate(0x4000, "GuestHeapTrace");
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
             [this](const auto&) { return clock.ticks.GetUptime() - clock.origin; });
        bind({"-2IRUCO--PM"}, [this](const auto&) { return clock.ticks.GetUptime(); });
        bind({"1j3S3n-tTW4", "BNowx2l588E"},
             [this](const auto&) { return clock.ticks.GetTscFrequency(); });
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
    bind({"ejekcaNQNq0"}, [gettimeofday](const auto& a) {
        auto args = a;
        args[1] = 0;
        return gettimeofday(args, true);
    });
    handlers["rNhWz+lvOMU"] = [this](HleCallFrame& frame) -> Status {
        const u64 entry = frame.registers.Get(Gpr::Rdi);
        std::lock_guard vm(vm_mutex);
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
            std::lock_guard vm(vm_mutex);
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
        // This branch only looks up existing modules; no guest callback or VM
        // mutation occurs under the short read/validation gate.
        std::lock_guard vm(vm_mutex);
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
                !space.Read(GuestAddress{a[0] + i}, std::as_writable_bytes(std::span{&c, 1})))
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
                return finish(id);
            }
        }
        if (!std::filesystem::exists(candidate, ec))
            return finish(ec ? ORBIS_KERNEL_ERROR_EIO : ORBIS_KERNEL_ERROR_ENOENT);
        if (!std::filesystem::is_regular_file(candidate, ec))
            return finish(ec ? ORBIS_KERNEL_ERROR_EIO : ORBIS_KERNEL_ERROR_EACCES);
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "ProductionModule", "LoadStartModule new path=%s args=%llu", guest.c_str(), (unsigned long long)a[1]);
#endif
        // Static TLS is already installed on live owners. Do not map/relocate a
        // new provider and pretend its dynamic TLS/initializer lifetime is ready.
        return MakeError(ErrorCategory::Unsupported, "LoadStartModule", "new guest module requires dynamic loading: " + guest);
    };
    bind({"LwG8g3niqwA"}, [this](const auto& a) -> u64 {
        auto* module = linker->GetModule(s32(a[0]));
        if (!module) return u32(ORBIS_KERNEL_ERROR_ESRCH);
        std::lock_guard vm(vm_mutex);
        if (!space.ValidateRange({GuestAddress{a[2]}, 8}, GuestPermission::Write))
            return u32(ORBIS_KERNEL_ERROR_EFAULT);
        std::string name;
        for (u64 i = 0;; ++i) {
            char c{};
            if (!a[1] || a[1] > UINT64_MAX - i ||
                !space.Read(GuestAddress{a[1] + i}, std::as_writable_bytes(std::span{&c, 1})))
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
            VmGuard vm(*this);
            result = Libraries::Kernel::sceKernelAllocateDirectMemory(a[0], a[1], a[2], a[3], a[4],
                                                                      &physical);
        }
        if (!result)
            Write(a[5], physical);
        return static_cast<u32>(result);
    });
    bind({"L-Q3LEjIbgA"}, [this](const auto& a) -> u64 {
        if (!space.ValidateRange({GuestAddress{a[0]}, 8}, GuestPermission::Write))
            return ORBIS_KERNEL_ERROR_EFAULT;
        auto address = reinterpret_cast<void*>(Read<u64>(a[0]));
        s32 result;
        {
            VmGuard vm(*this);
            result =
                Libraries::Kernel::sceKernelMapDirectMemory(&address, a[1], a[2], a[3], a[4], a[5]);
        }
        if (!result)
            Write(a[0], reinterpret_cast<u64>(address));
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
            VmGuard vm(*this);
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
            VmGuard vm(*this);
            void* out = reinterpret_cast<void*>(address);

            result = Libraries::Kernel::sceKernelMapNamedFlexibleMemory(&out, a[1], a[2], a[3],
                                                                        name.c_str());
            address = reinterpret_cast<u64>(out);
        }
        if (!result)
            Write(a[0], address);
        return static_cast<u32>(result);
    });
    bind({"cQke9UuBQOk"}, [this](const auto& a) -> u64 {
        VmGuard vm(*this);
        return static_cast<u32>(
            Libraries::Kernel::sceKernelMunmap(reinterpret_cast<void*>(a[0]), a[1]));
    });
    bind({"vSMAm3cxYTY"}, [this](const auto& a) -> u64 {
        VmGuard vm(*this);
        return static_cast<u32>(
            Libraries::Kernel::sceKernelMprotect(reinterpret_cast<void*>(a[0]), a[1], a[2]));
    });
    bind({"7Xl257M4VNI", "3PtV6p3QNX4"}, [](const auto& a) -> u64 { return a[0] == a[1]; });
    bind({"3eqs37G74-s", "EI-5-jlq2dE"}, [this](const auto&) -> u64 { return Current()->id; });
    bind({"B5GmVDKwpn0", "T72hz6ffq08", "6XG4B33N09g"}, [](const auto&) -> u64 {
        std::this_thread::yield(); return 0;
    });
    auto get_scheduling = [this](const auto& a) -> u64 {
        auto policy = space.AcquirePinnedSpan({GuestAddress{a[1]}, 4}, true);
        auto priority = space.AcquirePinnedSpan({GuestAddress{a[2]}, 4}, true);
        if (!policy || !priority) return POSIX_EFAULT;
        std::lock_guard lock(threads_mutex);
        for (const auto& [id, owner] : owners) {
            if (owner->handle_va != a[0] || owner->finished) continue;
            std::memcpy(policy.Value().WritableBytes().data(), &owner->attributes.policy, 4);
            std::memcpy(priority.Value().WritableBytes().data(), &owner->attributes.priority, 4);
            return 0;
        }
        return POSIX_ESRCH;
    };
    auto set_scheduling = [this](const auto& a) -> u64 {
        s32 priority{};
        if (!space.Read(GuestAddress{a[2]}, std::as_writable_bytes(std::span{&priority, 1})))
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
#if !defined(__linux__)
        if (attributes.affinity) return POSIX_ENOTSUP;
#endif
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
#if defined(__linux__)
                    if (o->attributes.affinity) {
                        // Map eight logical guest CPU IDs onto the inherited allowed
                        // native set; never use a guest mask as a host cpu_set_t.
                        cpu_set_t available, selected;
                        CPU_ZERO(&available); CPU_ZERO(&selected);
                        std::vector<int> cpus;
                        if (::sched_getaffinity(0, sizeof(available), &available) == 0)
                            for (int i = 0; i < CPU_SETSIZE; ++i)
                                if (CPU_ISSET(i, &available)) cpus.push_back(i);
                        if (cpus.empty()) throw std::runtime_error("no native CPUs available");
                        for (unsigned i = 0; i < 8; ++i)
                            if (o->attributes.affinity & (u64{1} << i)) CPU_SET(cpus[i % cpus.size()], &selected);
                        if (::sched_setaffinity(0, sizeof(selected), &selected))
                            throw std::runtime_error("native guest CPU affinity rejected");
                    }
#endif
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
        std::lock_guard vm(vm_mutex);
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
            std::lock_guard vm(vm_mutex); // bounded copy; no input wait/callback
            frame.registers.Set(Gpr::Rax, pad->Dispatch(space, nid, args));
            return Ok();
        };
    }
    for (const auto nid : AvPlayerNids) {
        bind({nid.data()},
             [this, nid](const auto& a) -> u64 { return avplayer->Dispatch(nid, a); });
    }
    for (const auto nid : NpOfflineNids) {
        bind({nid.data()}, [this, nid, observed = false](const auto& a) mutable -> u64 {
            std::lock_guard vm(vm_mutex); // bounded marshalling; no NP callback/network
            const auto result = np->Dispatch(space, nid, a);
            if (!observed) {
                observed = true;
                LOG_INFO(Lib_NpManager, "Session offline NP nid={} result={:#x}", nid, result);
            }
            return result;
        });
    }
    // User policy: reuse the existing desktop SSL compatibility entry points.
    // sceSslInit allocates only a dummy id; it does NOT establish TLS, a pool,
    // certificate validation or a connection. Do not broaden this to pointer APIs.
    bind({"hdpVEUDFW3s"}, [this](const auto& a) -> u64 {
        std::lock_guard vm(vm_mutex); // serialize desktop's non-atomic dummy id
        return u32(Libraries::Ssl2::sceSslInit(a[0]));
    });
    bind({"0K1yQ6Lv-Yc"}, [](const auto&) -> u64 {
        return u32(Libraries::Ssl2::sceSslTerm());
    });
    auto network_handler = [this](std::string_view nid) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            if (!network)
                return Status(MakeError(ErrorCategory::Unsupported, "GuestNetwork",
                                        "missing session domain"));
            if (nid == "iQw3iQPhvUQ") {
                auto callbacks = network->BeginCallbacks();
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
            std::lock_guard vm(vm_mutex);
            // Independent guest TLS slots: net errno must not alias POSIX errno
            // or another owner. NewOwner reserves/zeros the final 64 TLS bytes.
            const u64 net_errno = Current()->handle_va + sizeof(u64) + sizeof(s32);
            frame.registers.Set(Gpr::Rax, network->Dispatch(space, nid, args, net_errno));
            return Ok();
        };
    };
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
            std::lock_guard vm(vm_mutex);
            frame.registers.Set(Gpr::Rax, app_content->Dispatch(space, nid, args, storage.get()));
            return Ok();
        };
    }
    for (auto nid : RtcNids) {
        handlers[std::string(nid)] = [this, nid](HleCallFrame& frame) {
            std::array<u64, 6> args{};
            for (size_t i = 0; i < args.size(); ++i)
                args[i] = frame.registers.Get(kSysVIntegerOrder[i]);
            std::lock_guard vm(vm_mutex);
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
            std::lock_guard vm(vm_mutex);
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
        return static_cast<u32>(platform->Initialize());
    });
    bind({"bwFjS+bX9mA"},
         [this](const auto&) -> u64 { return static_cast<u32>(platform->Terminate()); });
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
            Require(space.Write(GuestAddress{a[1]},
                                std::as_bytes(std::span{name.c_str(), name.size() + 1})));
        }
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
    bind({"g0VTBxfJyu0"}, [](const auto&) -> u64 { return Kernel::sceKernelGetCurrentCpu(); });
    // libSceSystemService startup family. Output pointers are validated and the
    // real emulator functions run on host-local objects; results are copied back.
    namespace SystemService = Libraries::SystemService;
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
        auto pin = space.AcquirePinnedSpan({GuestAddress{args[0]}, sizeof(BufferAttribute)}, true);
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
}
void GuestRuntime::Prepare(const std::filesystem::path& executable,
                           const std::vector<std::filesystem::path>& modules) {
    if (impl->prepared)
        throw std::logic_error("runtime already prepared");
    impl->program_name = executable.filename().string();
    Impl::VmGuard vm(*impl);
    impl->mounts.Mount(executable.parent_path(), "/app0", true);
    if (impl->linker->LoadModule(executable) != 0)
        throw std::runtime_error("main executable could not be loaded");
    // Discover game-provided DT_NEEDED modules inside the installed content
    // root. System providers stay HLE imports; no host dlopen or host function
    // pointers are substituted for guest modules. Explicit roots use this same
    // graph, with canonical-path deduplication and a finite module count.
    const auto content_root = std::filesystem::canonical(executable.parent_path());
    std::map<std::filesystem::path, u32> loaded{{std::filesystem::canonical(executable), 0}};
    std::set<u32> visiting, visited;
    std::function<void(u32)> visit;
    auto load = [&](const std::filesystem::path& path) -> u32 {
        const auto canonical = std::filesystem::canonical(path);
        if (auto it = loaded.find(canonical); it != loaded.end())
            return it->second;
        if (loaded.size() >= 256)
            throw std::runtime_error("module graph exceeds 256 modules");
        const auto id = impl->linker->LoadModule(canonical);
        if (id < 0)
            throw std::runtime_error("dependency could not be loaded: " + canonical.string());
        loaded.emplace(canonical, id);
        return id;
    };
    visit = [&](u32 id) {
        if (visited.contains(id) || visiting.contains(id))
            return; // ELF dependency cycles are legal.
        visiting.insert(id);
        for (const auto* needed : impl->linker->GetModule(id)->dynamic_info.needed) {
            const std::filesystem::path name(needed);
            if (name.empty() || name.has_parent_path() || name == "." || name == "..")
                throw std::runtime_error("invalid DT_NEEDED name");
            std::optional<std::filesystem::path> found;
            for (const auto& parent :
                 {content_root / "sce_module", content_root / "modules", content_root}) {
                const auto candidate = parent / name;
                if (!std::filesystem::exists(candidate))
                    continue;
                const auto relative =
                    std::filesystem::canonical(candidate).lexically_relative(content_root);
                if (relative.empty() || *relative.begin() == "..")
                    throw std::runtime_error("dependency escapes content root");
                found = candidate;
                break;
            }
            if (found)
                visit(load(*found));
            else {
                const auto stem = name.stem().string();
                if (stem != "libkernel" && !stem.starts_with("libSce"))
                    throw std::runtime_error("missing guest dependency: " + name.string());
                impl->hle_modules.push_back(name.string());
            }
        }
        visiting.erase(id);
        visited.insert(id);
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
    u32 attributes{};
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
        for (unsigned i = 1; i < app_parameters.size(); ++i)
            app_parameters[i] =
                psf.GetInteger("USER_DEFINED_PARAM_" + std::to_string(i)).value_or(0);
    }
    impl->elf_info.InitializeGuestMetadata(executable.parent_path(), sdk, serial, title, version,
                                           attributes);
    impl->memory->SetGuestSdkVersion(sdk);
    // Snapshot only the local gameplay identity, never credentials or desktop events.
    std::array<GuestUser, 4> users{};
    if (const auto* user = UserManagement.GetUserByPlayerIndex(1))
        users[0] = {user->user_id, user->user_name};
    else
        users[0] = {1000, "shadPS4"}; // Same initial local profile as UserManager.
    if (GuestStorage::ValidTitle(save_title)) {
        impl->storage = std::make_unique<GuestStorage>(impl->mounts, EmulatorSettings.GetHomeDir(),
                                                       save_title, users[0].id);
        impl->sysmodules.Publish("libSceSaveData", 0x10000005);
        if (impl->save_dialog) {
            impl->save_dialog->Configure(EmulatorSettings.GetHomeDir(), save_title, users[0].id);
            impl->sysmodules.Publish("libSceSaveDataDialog", 0x10000006);
            impl->sysmodules.Publish("libSceCommonDialog", 0x10000007);
        }
    }
    if (GuestStorage::ValidTitle(serial)) {
        auto roots =
            Core::FileSys::ListContentRoots(EmulatorSettings.GetAddonInstallDir() / serial);
        const auto sibling =
            Core::FileSys::OverlayPath(executable.parent_path(), Core::FileSys::DlcSuffix);
        if (const auto root = Core::FileSys::ResolveGameRoot(sibling)) {
            auto found = Core::FileSys::IsZArchiveFile(*root)
                             ? Core::FileSys::ExpandBundleRoots(*root)
                             : Core::FileSys::ListContentRoots(*root);
            roots.insert(roots.end(), found.begin(), found.end());
        }
        impl->app_content = std::make_unique<GuestAppContent>(impl->mounts, sdk, serial,
                                                              app_parameters, std::move(roots),
            [p = impl.get()] { return p->platform->EntitlementsChanged(); });
        impl->sysmodules.Publish("libSceAppContent", 0x1000000a);
    }
    impl->sysmodules.Publish("libSceDiscMap", 0x10000008);
    impl->sysmodules.Publish("libSceRtc", 0x10000009);
    impl->platform = std::make_unique<GuestPlatform>(std::move(users), sdk,
                                                     EmulatorSettings.GetConsoleLanguage(),
                                                     EmulatorSettings.IsCircleEnter());
    impl->pad = std::make_unique<GuestPad>(GlobalPadAdapter(), *impl->platform);
    impl->sysmodules.Publish("libScePad", 0x1000000d);
    impl->audio = std::make_unique<GuestAudio>(impl->space, impl->vm_mutex, impl->clock);
    impl->ajm = std::make_unique<GuestAjm>(impl->space, impl->vm_mutex);
    impl->avplayer = std::make_unique<GuestAvPlayer>(impl->space, impl->vm_mutex,
                                                     [p = impl.get()] { return p->AvCallbacks(); });
    impl->sysmodules.Publish("libSceAvPlayer", 0x1000000e);
    std::map<s32, bool> signup;
    for (const auto& user : UserManagement.GetAllUsers())
        signup.emplace(user.user_id, !user.shadnet_npid.empty());
    impl->np = std::make_unique<GuestNpOffline>(s32(sdk), std::move(signup));
    impl->np_offline = !EmulatorSettings.IsShadNetEnabled() &&
                       !EmulatorSettings.IsConnectedToNetwork();
    impl->network = std::make_unique<GuestNetwork>(EmulatorSettings.IsConnectedToNetwork());
    LOG_INFO(Lib_Net, "Session network control: requested_online={}, online transport unavailable",
             EmulatorSettings.IsConnectedToNetwork());
    impl->sysmodules.Publish("libSceNet", 0x1000000b);
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

    // Production imports are registered by Bind with an explicit guest policy.
    // Desktop InitHLELibs also starts graphics/audio/global worker services; those
    // require the platform session lifecycle and cannot run during relocation.
    // An unadapted import remains a named, faulting operation, never a native PC.
    impl->linker->PrepareGuest();
    impl->prepared = true;
}
Result<GuestCallResult> GuestRuntime::Run(const std::vector<std::string>& args) {
    if (!impl->prepared)
        return MakeError(ErrorCategory::WrongState, "GuestRuntime::Run", "Prepare is required");
    auto* module = impl->linker->GetModule(0);
    auto owner = impl->NewOwner(GuestThreadAttributes{.size = 2 << 20});
    try {
        impl->Attach(owner, module->GetEntryAddress());
        // libkernel initializes each guest libc allocator before constructors.
        // DT_INIT alone leaves libc's replaceable malloc table on its bootstrap
        // implementation. These exports are guest PCs and must go through FEX.
        for (u32 id = 0; auto* m = impl->linker->GetModule(id); ++id) {
            if (m->name != "libc.prx" && m->name != "libSceLibcInternal.sprx")
                continue;
            if (const auto entry = m->FindByName("_malloc_init")) {
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
            if (m->Start(0, nullptr, nullptr) != 0)
                throw std::runtime_error("module initialization failed: " + m->name);
            LOG_INFO(Core_Linker, "Guest module {}: {}", m->name,
                     m->dynamic_info.has_init ? "DT_INIT completed" : "no DT_INIT");
            impl->sysmodules.Publish(std::filesystem::path(m->name).stem().string(),
                                     static_cast<s32>(id));
        }
        u64 params_address;
        {
            Impl::VmGuard vm(*impl);
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
            Require(
                impl->space.Write(GuestAddress{params_address + cursor},
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
    for (auto& name : impl->refused)
        text += "\nUNSUPPORTED_IMPORT " + name;
    for (auto& name : impl->hle_modules)
        text += "\nHLE_MODULE " + name;
    {
        std::scoped_lock lock(impl->graphics_mutex);
        text += impl->graphics ? " graphics=ready" : " graphics=not-created";
        if (impl->graphics) text += " guest_presents=" + std::to_string(impl->graphics->VideoOut().guest_presents.load());
    }
    text += " modules=" + std::to_string(impl->init_order.size() + 1);
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
