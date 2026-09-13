// SPDX-License-Identifier: GPL-2.0-or-later
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
#include <unistd.h>
#include "common/alignment.h"
#include "common/elf_info.h"
#include "common/singleton.h"
#include "core/aerolib/aerolib.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/guest_cpu/hle/scope.h"
#include "core/guest_cpu/hle/veneer_allocator.h"
#include "core/host_runtime/guest_clock.h"
#include "core/host_runtime/guest_mutex.h"
#include "core/host_runtime/guest_runtime.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/tls.h"
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
        const bool trace = traces.fetch_add(1) < 64;
        if (trace)
            __android_log_print(ANDROID_LOG_INFO, "ProductionHLE", "enter op=%llu rip=%llx",
                                (unsigned long long)frame.operation,
                                (unsigned long long)frame.registers.rip);
#endif
        auto status = call(frame);
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
    std::unique_ptr<GuestMutexDomain> mutex_domain;
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
    struct Owner {
        u64 id{}, stack{}, stack_size{}, tls{}, tls_size{}, tcb{}, dtv{}, handle_va{};
        std::array<SpecificValue, 256> specific{};
        ThreadHandle handle{};
        std::thread worker;
        std::optional<GuestCallResult> result;
        std::optional<Error> error;
        bool started{}, finished{}, joined{}, detached{};
    };
    std::map<u64, std::shared_ptr<Owner>> owners;
    u64 next_id{1};
    bool prepared{};
    std::vector<u32> init_order;
    std::vector<std::string> hle_modules;
    std::mutex once_mutex;
    std::map<u64, u64> once_owners;

    struct VmGuard {
        Impl& rt;
        std::unique_lock<std::recursive_mutex> lock;
        bool outer;
        explicit VmGuard(Impl& rt) : rt(rt), lock(rt.vm_mutex), outer(!rt.vm_token) {
            if (outer) {
                rt.vm_token.emplace(Require(rt.cpu.QuiesceContext(2'000'000'000)));
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
                    // Reuse real guest-owned stream objects and the libc's real
                    // 4-byte dependency tag instead of fabricated zero objects.
                    const auto suffix = "#libSceLibcInternal#1#libSceLibcInternal#Object";
                    const auto nid = symbol.name.substr(0, symbol.name.find('#'));
                    const auto* known = AeroLib::FindByNid(nid.c_str());
                    const bool standard_stream =
                        known && (std::string_view(known->name) == "_Stdin" ||
                                  std::string_view(known->name) == "_Stdout" ||
                                  std::string_view(known->name) == "_Stderr");
                    if (symbol.name.substr(nid.size()) == suffix &&
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
        for (auto& [id, o] : owners)
            if (o->worker.joinable())
                o->worker.join();
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
    std::shared_ptr<Owner> NewOwner() {
        auto o = std::make_shared<Owner>();
        {
            std::lock_guard lock(threads_mutex);
            if (cancelling)
                throw std::runtime_error("session is stopping");
            o->id = next_id++;
        }
        {
            VmGuard vm(*this);
            o->stack_size = 2 << 20;
            o->stack = Allocate(o->stack_size + 0x4000, "GuestStack");
            // Guard page stays in the VMA ledger with no access.
            if (memory->Protect(o->stack, 0x4000, MemoryProt::NoAccess))
                throw std::runtime_error("guest stack guard failed");
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
        init.initial_rsp = GuestAddress{o->stack + 0x4000 + o->stack_size - 16};
        init.guest_tid = o->id;
        init.initial_state.fields = RegisterValidity::SegmentBases;
        init.initial_state.values.fs_base = o->tcb;
        auto handle = Require(cpu.CreateThread(init));
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
        if (o->result && o->result->reason == StopReason::Returned && !cancelling) {
            bool stopped{};
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

        if (o->handle.IsValid())
            Require(cpu.DestroyThread(o->handle));
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
    Status Cancel() {
        std::vector<ThreadHandle> handles;
        {
            std::lock_guard lock(threads_mutex);
            cancelling = true;
            for (auto& [id, o] : owners)
                if (o->handle.IsValid())
                    handles.push_back(o->handle);
        }
        for (auto handle : handles) {
            auto s = cpu.RequestInterrupt(handle, InterruptReason::Cancel);
            if (!s && s.GetError().category != ErrorCategory::InvalidHandle)
                return s.GetError();
        }
        threads_changed.notify_all();
        return Ok();
    }
    u64 Bind(const Loader::SymbolRecord& symbol) {
        if (auto it = veneers.find(symbol.name); it != veneers.end())
            return it->second;
        const auto nid = symbol.name.substr(0, symbol.name.find('#'));
        std::shared_ptr<HleCallAdapter> adapter;
        if (auto it = handlers.find(nid);
            it != handlers.end() &&
            (symbol.name.substr(nid.size()) == "#libkernel#1#libkernel#Function" ||
             symbol.name.substr(nid.size()) == "#libScePosix#1#libkernel#Function" ||
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
    mutex_domain = std::make_unique<GuestMutexDomain>(space, [this] {
        VmGuard vm(*this);
        return Allocate(0x4000, "GuestMutex");
    });
    bind({"dQHWEsJtoE4", "n2MMpvU8igI", "F8bUHwAG284"},
         [this](const auto& a) { return mutex_domain->AttributeInit(a[0]); });
    bind({"HF7lK46xzjY", "smWEktiyyG0"},
         [this](const auto& a) { return mutex_domain->Attribute(a[0], 0, 0); });
    bind({"mDmgMOGVUqg", "J9rlRuQ8H5s", "iMp8QpE+XO4", "UWZbVSFze24"},
         [this](const auto& a) { return mutex_domain->Attribute(a[0], a[1], 1); });
    bind({"GZFlI7RhuQo", "U6SNV+RnyLQ", "gquEhBrS2iw", "rH2mWEndluc"},
         [this](const auto& a) { return mutex_domain->Attribute(a[0], a[1], 2); });
    bind({"ttHNfU+qDBU", "cmo1RIYva9o", "qH1gXoq71RY"},
         [this](const auto& a) { return mutex_domain->Init(a[0], a[1]); });
    bind({"7H0iTOciTLo", "9UK1vLZQft4"}, [this](const auto& a) {
        return mutex_domain->Lock(a[0], Current()->handle_va, false,
                                  HleScope::Current()->CancellationToken());
    });
    bind({"K-jXhbt2gn4", "upoVrzMHFeE"}, [this](const auto& a) {
        return mutex_domain->Lock(a[0], Current()->handle_va, true,
                                  HleScope::Current()->CancellationToken());
    });
    bind({"2Z+PpY6CaJg", "tn3VlD0hG60"},
         [this](const auto& a) { return mutex_domain->Unlock(a[0], Current()->handle_va); });
    bind({"ltCfaGr2JGE", "2Of0f+3mhhE"},
         [this](const auto& a) { return mutex_domain->Destroy(a[0]); });
    bind({"gKqzW-zWhvY", "W6OrTBO95UY"},
         [this](const auto& a) { return mutex_domain->IsOwned(a[0], Current()->handle_va); });
    bind({"mkx2fVhNMsg", "JGgj7Uvrl+A"},
         [this](const auto& a) { return mutex_domain->CondNotify(a[0], true); });
    bind({"2MOy+rUfuhQ", "kDh-NfxgMtE"},
         [this](const auto& a) { return mutex_domain->CondNotify(a[0], false); });
    bind({"0TyVk4MSLt0", "2Tb92quprl0"},
         [this](const auto& a) { return mutex_domain->CondInit(a[0], a[1]); });
    bind({"RXXqi4CtF8w", "g+PZd2hiacg"},
         [this](const auto& a) { return mutex_domain->CondDestroy(a[0]); });
    bind({"Op8TBGY5KHg", "WKAXJ4XBPQ4"}, [this](const auto& a) {
        return mutex_domain->CondWait(a[0], a[1], Current()->handle_va,
                                      HleScope::Current()->CancellationToken());
    });
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
    bind({"jh+8XiK4LeE"}, [](const auto&) -> u64 { return 0; });
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
    bind({"QcteRwbsnV0"}, [this](const auto& a) -> u64 {
        if (a[0] >= 1'000'000)
            return PosixFailure(POSIX_EINVAL);
        auto* scope = HleScope::Current();
        return scope->WaitFor(std::chrono::microseconds(a[0])) ? 0 : PosixFailure(POSIX_EINTR);
    });
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
    bind({"FJrT5LuUBAU", "3kg7rT0NQIs"}, [](const auto& a) -> u64 {
        HleScope::Current()->ExitThread(a[0]);
        return a[0];
    });
    bind({"EotR8a3ASf4", "aI+OeCz8xrQ"},
         [this](const auto&) -> u64 { return Current()->handle_va; });
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
    bind({"OxhIB8LB-PQ", "Jmi+9w9u0E4", "6UgtwV+0zb4"}, [this](const auto& a) -> u64 {
        // Non-default attributes must be handled by the attribute domain, never
        // reinterpret an unvalidated guest pointer as a host PthreadAttr.
        if (a[1] && Read<u64>(a[1]))
            return POSIX_ENOTSUP;
        Require(space.ValidateRange({GuestAddress{a[0]}, sizeof(u64)}, GuestPermission::Write));
        auto code = Require(space.Query(GuestAddress{a[2]}));
        if (!HasPermission(code.permission, GuestPermission::Execute))
            return POSIX_EINVAL;
        auto o = NewOwner();
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
                    Attach(o, entry);
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
        return 0;
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
    for (const char* nid :
         {"6UgtwV+0zb4", "onNY9Byn-W8", "4qGrR6eoP9Y", "14bOACANTBo", "n2MMpvU8igI",
          "F8bUHwAG284", "smWEktiyyG0", "iMp8QpE+XO4", "UWZbVSFze24", "gquEhBrS2iw",
          "rH2mWEndluc", "cmo1RIYva9o", "qH1gXoq71RY", "9UK1vLZQft4", "upoVrzMHFeE",
          "tn3VlD0hG60", "2Of0f+3mhhE", "JGgj7Uvrl+A", "kDh-NfxgMtE", "2Tb92quprl0",
          "g+PZd2hiacg", "WKAXJ4XBPQ4", "geDaqgH9lTg", "PrdHuuDekhY", "+BzXYkqYeLE"}) {
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
    for (const auto& module : modules)
        roots.push_back(load(module));
    for (auto id : roots)
        visit(id);
    u32 sdk{};
    if (auto* param = impl->linker->GetProcParam()) {
        Require(impl->space.ValidateRange({GuestAddress{reinterpret_cast<u64>(param)}, 0x20},
                                          GuestPermission::Read));
        if (param->size < 0x20)
            throw std::runtime_error("invalid process parameters");
        sdk = param->sdk_version;
    }
    std::string serial, title, version;
    u32 attributes{};
    if (auto bytes = impl->mounts.ReadFile("/app0/sce_sys/param.sfo")) {
        PSF psf;
        if (!psf.Open(*bytes))
            throw std::runtime_error("invalid installed param.sfo");
        serial = psf.GetString("TITLE_ID").value_or("");
        title = psf.GetString("TITLE").value_or("");
        version = psf.GetString("APP_VER").value_or("");
        attributes = psf.GetInteger("ATTRIBUTE").value_or(0);
    }
    impl->elf_info.InitializeGuestMetadata(executable.parent_path(), sdk, serial, title, version,
                                           attributes);
    impl->memory->SetGuestSdkVersion(sdk);
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
    auto owner = impl->NewOwner();
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
            LOG_INFO(Core_Linker, "Guest DT_INIT completed: {}", m->name);
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
        const u64 stack = owner->stack + 0x4000 + owner->stack_size - 24;
        patch.values.Set(Gpr::Rsp, stack);
        patch.values.rip = module->GetEntryAddress();
        impl->Write(stack, static_cast<u64>(params.argc));
        impl->Write(stack + 8, reinterpret_cast<u64>(params.argv[0]));
        Require(impl->cpu.WriteRegisters(owner->handle, patch, snapshot.stop_epoch));
        auto run = impl->cpu.Run(owner->handle, {});
        if (!run) {
            impl->Finish(owner);
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
        if (owner->error)
            return *owner->error;
        return *owner->result;
    } catch (const GuestCallbackStop& stopped) {
        owner->result = stopped.result;
        (void)impl->Cancel();
        impl->Finish(owner);
        return stopped.result;
    } catch (const std::exception& e) {
        auto error = MakeError(ErrorCategory::BackendFailure, "GuestRuntime::Run", e.what());
        (void)impl->Cancel();
        impl->Finish(owner);
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
    text += " modules=" + std::to_string(impl->init_order.size() + 1);
    return text;
}
std::string GuestRuntime::OperationName(u64 operation) const {
    // Bindings are immutable once Prepare finishes, before any owner executes.
    const auto found = impl->operation_names.find(operation);
    return found == impl->operation_names.end() ? std::string{} : found->second;
}
} // namespace Core::HostRuntime
