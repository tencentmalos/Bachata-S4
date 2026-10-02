// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include "common/elf_info.h"
#include "common/guest_patch_log.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/profiler.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "core/guest_patch_desktop.h"
#include "core/host_runtime/guest_patch_format.h"
#include "core/loader/elf.h"
#include "core/memory.h"
#include "core/module.h"

namespace Core::GuestPatch::Desktop {
namespace {
void Require(bool value, const std::string& why) {
    if (!value)
        throw std::runtime_error(why);
}
u64 PageAlign(u64 n) {
    return (n + 4095) & ~4095ULL;
}

struct State {
    std::mutex mutex;
    bool configured{}, attempted{}, installed{};
    std::filesystem::path path;
    std::optional<Package> package;
    std::string error, executable_sha256;
    Plan plan;
    u64 switches{};
    struct Tag {
        std::string name;
        u64 count{};
        s64 last{};
    };
    std::map<u64, Tag> counters, logs;
    std::atomic<u64> sdk_calls{};
};
State& GetState() {
    static State state;
    return state;
}

// SDK v1 services. The payload calls them through its import slots with the
// SysV ABI, on whatever guest thread runs the hook.
u64 PS4_SYSV_ABI SdkQuery(u64 version) {
    GetState().sdk_calls.fetch_add(1, std::memory_order_relaxed);
    return version == 1 ? 7 : 0; // bit0 clock, bit1 counters, bit2 logs
}
u64 PS4_SYSV_ABI SdkClockNs() {
    GetState().sdk_calls.fetch_add(1, std::memory_order_relaxed);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
u64 PS4_SYSV_ABI SdkCounter(u64 id, s64 value) {
    auto& st = GetState();
    st.sdk_calls.fetch_add(1, std::memory_order_relaxed);
    std::string name;
    {
        std::scoped_lock lock(st.mutex);
        const auto it = st.counters.find(id);
        if (it == st.counters.end())
            return u64(-22);
        ++it->second.count;
        it->second.last = value;
        name = it->second.name;
    }
    Common::Profiler::Counter(name.c_str(), value);
    return 0;
}
u64 PS4_SYSV_ABI SdkLog(u64 tag, s64 value) {
    auto& st = GetState();
    st.sdk_calls.fetch_add(1, std::memory_order_relaxed);
    std::string package, name;
    {
        std::scoped_lock lock(st.mutex);
        const auto it = st.logs.find(tag);
        if (it == st.logs.end())
            return u64(-22);
        ++it->second.count;
        it->second.last = value;
        package = st.package->id;
        name = it->second.name;
    }
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    Common::GuestPatchLog::Write(package, name, value, 0, thread, 0, 0);
    return 0;
}
const u64 kSdkFunctions[] = {
    reinterpret_cast<u64>(&SdkQuery),
    reinterpret_cast<u64>(&SdkClockNs),
    reinterpret_cast<u64>(&SdkCounter),
    reinterpret_cast<u64>(&SdkLog),
};
static_assert(std::size(kSdkFunctions) == std::size(kSdkImports));

// Identity of the module as the loader read it (loose file or archive member).
std::string ModuleSha256(Module& module) {
    auto* file = module.elf.GetFile();
    Require(file && file->IsOpen(), "module file is not open: " + module.name);
    u64 offset = 0;
    const u64 cursor = file->Tell();
    bool positioned = true;
    auto sha = StreamSha256([&](void* dst, std::uint64_t size) -> std::int64_t {
        s64 n = positioned ? file->ReadAt(dst, size, offset) : -1;
        if (n < 0 && offset == 0 && positioned) {
            // Backend without positioned reads: stream from the start once.
            positioned = false;
            if (!file->Seek(0, Common::FS::SeekOrigin::SetOrigin))
                return -1;
        }
        if (!positioned)
            n = file->Read(dst, size);
        if (n > 0)
            offset += n;
        return n;
    });
    if (!positioned)
        file->Seek(static_cast<s64>(cursor), Common::FS::SeekOrigin::SetOrigin);
    return sha;
}

// Segment of the module containing [offset, offset + size), by ELF flags.
bool InSegment(Module& module, u64 offset, u64 size, u32 required_flags) {
    for (const auto& ph : module.elf.GetProgramHeader()) {
        if ((ph.p_type != PT_LOAD && ph.p_type != PT_SCE_RELRO) || !ph.p_memsz)
            continue;
        if ((ph.p_flags & required_flags) != required_flags)
            continue;
        if (offset >= ph.p_vaddr && size <= ph.p_memsz && offset - ph.p_vaddr <= ph.p_memsz - size)
            return true;
    }
    return false;
}

bool Matches(u64 address, const Bytes& expected) {
    return std::memcmp(reinterpret_cast<const void*>(address), expected.data(), expected.size()) ==
           0;
}

void Install(State& st, Module& module, std::string module_sha256) {
    const auto& p = *st.package;
    const ModuleIdentity identity{std::string(Common::ElfInfo::Instance().GameSerial()),
                                  module.name,
                                  std::move(module_sha256),
                                  module.GetBaseAddress(),
                                  module.aligned_base_size,
                                  st.executable_sha256};
    Require(p.title == identity.title, "title mismatch: package " + p.title + ", game " +
                                           identity.title);
    Require(p.module_sha256 == identity.sha256,
            "module SHA256 mismatch: package " + p.module_sha256 + ", " + module.name + " " +
                identity.sha256);
    Require(p.executable_sha256.empty() || p.executable_sha256 == identity.executable_sha256,
            "main executable SHA256 mismatch");
    // Preimages are checked against the module as loaded, after the loader's own
    // load-time code rewrites, so a conflicting edit is refused instead of merged.
    for (const auto& h : p.hooks) {
        Require(InSegment(module, h.offset, h.expected.size(), PF_EXEC),
                "hook outside executable segment: " + h.name);
        Require(Matches(identity.base + h.offset, h.expected), "hook preimage mismatch: " + h.name);
    }
    for (const auto& c : p.patches) {
        Require(InSegment(module, c.offset, c.expected.size(), PF_EXEC),
                "code patch outside executable segment: " + c.name);
        Require(Matches(identity.base + c.offset, c.expected),
                "code patch preimage mismatch: " + c.name);
    }
    std::map<std::string, u64> bindings;
    for (const auto& b : p.bindings) {
        const bool function = b.kind == "function";
        Require(b.alignment && (b.alignment & (b.alignment - 1)) == 0 &&
                    b.offset % b.alignment == 0,
                "unaligned guest binding: " + b.name);
        Require(InSegment(module, b.offset, b.size,
                          function ? PF_EXEC : (b.writable ? PF_READ | PF_WRITE : PF_READ)),
                "guest binding permission mismatch: " + b.name);
        if (function) {
            Require(b.expected.size() == b.size && Matches(identity.base + b.offset, b.expected),
                    "function binding preimage mismatch: " + b.name);
            for (const auto& h : p.hooks)
                Require(b.offset + b.size <= h.offset || h.offset + h.expected.size() <= b.offset,
                        "guest binding overlaps hook: " + b.name);
            for (const auto& c : p.patches)
                Require(b.offset + b.size <= c.offset || c.offset + c.expected.size() <= b.offset,
                        "guest binding overlaps code patch: " + b.name);
        }
        bindings.emplace(b.name, identity.base + b.offset);
    }
    // Payload and pool in one mapping after the module's own trampoline area, within
    // rel32 reach of its entries. RWX like that trampoline area: the slot page is
    // rewritten by enable/disable while guest threads run.
    const auto [code_size, pool_size] = PoolGeometry(p);
    const u64 image_size = PageAlign(p.image_size);
    void* mapped = nullptr;
    constexpr u64 kModuleTrampolineSize = 8_MB; // Module::LoadModuleToMemory
    const s32 result = module.memory->MapMemory(
        &mapped, identity.base + identity.size + kModuleTrampolineSize, image_size + pool_size,
        MemoryProt::CpuReadWrite | MemoryProt::CpuExec, MemoryMapFlags::NoFlags, VMAType::Code,
        "GuestPatch:" + p.id);
    Require(result == 0 && mapped, "cannot map guest patch payload");
    const u64 image = reinterpret_cast<u64>(mapped);
    const u64 pool = image + image_size;
    auto plan = BuildPlan(p, identity, image, pool, bindings,
                          [](unsigned op, std::span<std::byte, 16>, uint64_t) {
                              return kSdkFunctions[op];
                          });
    for (const auto& [id, name] : p.counters)
        st.counters.emplace(id, State::Tag{"GuestPatch." + p.id + "." + name});
    for (const auto& [id, name] : p.logs)
        st.logs.emplace(id, State::Tag{"GuestPatch." + p.id + "." + name});
    std::memcpy(reinterpret_cast<void*>(image), plan.payload.data(), plan.payload.size());
    std::memcpy(reinterpret_cast<void*>(pool), plan.stubs.data(), plan.stubs.size());
    std::memcpy(reinterpret_cast<void*>(pool + code_size), plan.slots.data(), plan.slots.size());
    // The module is still being loaded: no thread can execute these bytes yet.
    for (const auto& e : plan.hooks)
        std::memcpy(reinterpret_cast<void*>(e.entry), e.patch.data(), e.patch.size());
    for (const auto& c : plan.patches)
        std::memcpy(reinterpret_cast<void*>(c.address), c.patch.replacement.data(),
                    c.patch.replacement.size());
    st.plan = std::move(plan);
    st.installed = true;
}

std::string StatusLocked(State& st) {
    std::ostringstream o;
    if (!st.configured)
        return "status: disabled\ndetail: no package selected (Launch Options, General.guest_patch "
               "or SHADPS4_GUEST_PATCH)\n" +
               (st.error.empty() ? std::string{} : "error: " + st.error + "\n");
    o << "package: " << (st.package ? st.package->id : std::string("<unloaded>"))
      << "\npath: " << st.path.string()
      << "\nsha256: " << (st.package ? st.package->digest : std::string()) << "\ninstalled: "
      << st.installed << "\nswitches: " << st.switches
      << "\nsdk_calls: " << st.sdk_calls.load(std::memory_order_relaxed) << "\n";
    if (!st.error.empty())
        o << "error: " << st.error << "\n";
    if (st.package && !st.attempted)
        o << "waiting_for_module: " << st.package->module << "\n";
    for (const auto& e : st.plan.hooks) {
        const auto slot = std::atomic_ref<u64>(*reinterpret_cast<u64*>(e.slot))
                              .load(std::memory_order_relaxed);
        o << (e.hook.kind == HookKind::Site ? "site: " : "hook: ") << e.hook.name;
        if (e.hook.kind == HookKind::Site)
            o << " mode=" << (e.hook.site_mode == SiteMode::Replace ? "replace" : "before");
        o << " entry=0x" << std::hex << e.entry << " handler=0x" << e.replacement
          << " original=0x" << e.original << std::dec << " stolen=" << e.saved.size()
          << " enabled=" << (slot == e.replacement) << "\n";
    }
    for (const auto& c : st.plan.patches)
        o << "code_patch: " << c.patch.name << " address=0x" << std::hex << c.address << std::dec
          << " size=" << c.patch.replacement.size() << "\n";
    for (const auto& [id, c] : st.counters)
        o << "counter: " << id << " " << c.name << " samples=" << c.count << " last=" << c.last
          << "\n";
    for (const auto& [id, t] : st.logs)
        o << "log_tag: " << id << " " << t.name << " samples=" << t.count << " last=" << t.last
          << "\n";
    return o.str();
}
// A package name is a bare file stem: no separators, no relative components.
bool ValidPackageName(std::string_view name) {
    return !name.empty() && name.size() <= 96 && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string_view::npos;
}

// Package selected for this process: the development override, else the game's setting.
std::filesystem::path SelectedPackage() {
    if (const char* path = std::getenv("SHADPS4_GUEST_PATCH"); path && *path) {
        return path;
    }
    const auto name = EmulatorSettings.GetGuestPatch();
    if (name.empty()) {
        return {};
    }
    Require(ValidPackageName(name), "invalid guest_patch name: " + name);
    return PackageDirectory(Common::ElfInfo::Instance().GameSerial()) / (name + ".json");
}
} // namespace

std::filesystem::path PackageDirectory(std::string_view title) {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "guest_patches" /
           std::string(title);
}

std::vector<PackageInfo> ListPackages(std::string_view title) {
    std::vector<PackageInfo> packages;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(PackageDirectory(title), ec)) {
        const auto name = entry.path().stem().string();
        if (entry.path().extension() != ".json" || !entry.is_regular_file() ||
            !ValidPackageName(name)) {
            continue;
        }
        try {
            const auto package = Package::Load(entry.path());
            if (package.title == title) {
                packages.push_back({name, package.id, package.module});
            }
        } catch (const std::exception& e) {
            LOG_WARNING(Loader, "Guest patch package {} skipped: {}", entry.path().string(),
                        e.what());
        }
    }
    std::ranges::sort(packages, {}, &PackageInfo::name);
    return packages;
}

void OnModuleLoaded(Module& module, bool main_executable) {
    auto& st = GetState();
    std::scoped_lock lock(st.mutex);
    if (!st.configured && !st.attempted) {
        // Read once, at the first module (the main executable).
        try {
            st.path = SelectedPackage();
        } catch (const std::exception& e) {
            st.attempted = true;
            st.error = e.what();
            LOG_ERROR(Loader, "Guest patch not loaded: {}", st.error);
            return;
        }
        if (st.path.empty()) {
            st.attempted = true;
            return;
        }
        st.configured = true;
        try {
            st.package = Package::Load(st.path);
            LOG_INFO(Loader, "Guest patch package {} ({}) targets {} {}", st.package->id,
                     st.package->digest, st.package->title, st.package->module);
        } catch (const std::exception& e) {
            st.attempted = true;
            st.error = e.what();
            LOG_ERROR(Loader, "Guest patch package {} rejected: {}", st.path.string(), st.error);
            return;
        }
    }
    if (!st.package || st.attempted)
        return;
    try {
        const bool target = module.name == st.package->module;
        if (!target && !(main_executable && !st.package->executable_sha256.empty()))
            return;
        // Hashes the whole module file, so only for the target or a required executable.
        auto sha = ModuleSha256(module);
        if (main_executable)
            st.executable_sha256 = sha;
        if (!target)
            return;
        st.attempted = true;
        Install(st, module, std::move(sha));
        LOG_INFO(Loader, "Guest patch installed:\n{}", StatusLocked(st));
    } catch (const std::exception& e) {
        st.attempted = true;
        st.error = e.what();
        LOG_ERROR(Loader, "Guest patch {} not installed, game runs unpatched: {}",
                  st.package->id, st.error);
    }
}

std::string Command(const std::vector<std::string>& args) {
    auto& st = GetState();
    std::scoped_lock lock(st.mutex);
    if (args.empty() || (args.size() == 1 && args[0] == "status"))
        return StatusLocked(st);
    if ((args[0] != "enable" && args[0] != "disable") || args.size() > 2)
        return "status: invalid_arguments\ndetail: status | enable [name] | disable [name]\n";
    if (!st.installed || st.plan.hooks.empty())
        return "status: error\ndetail: no installed hooks or sites\n";
    const bool enable = args[0] == "enable";
    bool found = false;
    for (const auto& e : st.plan.hooks) {
        if (args.size() == 2 && e.hook.name != args[1])
            continue;
        found = true;
        // An aligned 8-byte store: a thread at the stub jumps either way, never torn.
        std::atomic_ref<u64>(*reinterpret_cast<u64*>(e.slot))
            .store(enable ? e.replacement : e.original, std::memory_order_release);
    }
    if (!found)
        return "status: error\ndetail: unknown hook or site name\n";
    ++st.switches;
    return StatusLocked(st);
}
} // namespace Core::GuestPatch::Desktop
