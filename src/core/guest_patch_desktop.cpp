// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
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
#include <utility>
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

struct Tag {
    std::string name;
    u64 count{};
    s64 last{};
};
// One selected package.
struct Entry {
    std::string name; // selection: file stem, or the override path
    std::filesystem::path path;
    std::optional<Package> package;
    bool attempted{}, installed{};
    std::string error;
    Plan plan;
    std::map<u64, Tag> counters, logs;
};
struct State {
    std::mutex mutex;
    bool read{}; // selection read, at the first module
    // Reserved to kMaxPackages before the first package loads: SDK services index it.
    std::vector<Entry> entries;
    std::string selection_error, executable_sha256;
    u64 switches{};
    std::atomic<u64> sdk_calls{};
};
State& GetState() {
    static State state;
    return state;
}

// SDK v1 services. The payload calls them through its import slots with the
// SysV ABI, on whatever guest thread runs the hook. Counter and log ids belong to
// one package, so each package's imports bind to its own instantiation below.
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
u64 SdkCounter(std::size_t index, u64 id, s64 value) {
    auto& st = GetState();
    st.sdk_calls.fetch_add(1, std::memory_order_relaxed);
    std::string name;
    {
        std::scoped_lock lock(st.mutex);
        if (index >= st.entries.size())
            return u64(-22);
        auto& counters = st.entries[index].counters;
        const auto it = counters.find(id);
        if (it == counters.end())
            return u64(-22);
        ++it->second.count;
        it->second.last = value;
        name = it->second.name;
    }
    Common::Profiler::Counter(name.c_str(), value);
    return 0;
}
u64 SdkLog(std::size_t index, u64 tag, s64 value) {
    auto& st = GetState();
    st.sdk_calls.fetch_add(1, std::memory_order_relaxed);
    std::string package, name;
    {
        std::scoped_lock lock(st.mutex);
        if (index >= st.entries.size() || !st.entries[index].package)
            return u64(-22);
        auto& entry = st.entries[index];
        const auto it = entry.logs.find(tag);
        if (it == entry.logs.end())
            return u64(-22);
        ++it->second.count;
        it->second.last = value;
        package = entry.package->id;
        name = it->second.name;
    }
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    Common::GuestPatchLog::Write(package, name, value, 0, thread, 0, 0);
    return 0;
}
template <std::size_t I>
u64 PS4_SYSV_ABI SdkCounterAt(u64 id, s64 value) {
    return SdkCounter(I, id, value);
}
template <std::size_t I>
u64 PS4_SYSV_ABI SdkLogAt(u64 tag, s64 value) {
    return SdkLog(I, tag, value);
}
template <std::size_t... I>
std::array<std::array<u64, 4>, sizeof...(I)> SdkTable(std::index_sequence<I...>) {
    return {{{reinterpret_cast<u64>(&SdkQuery), reinterpret_cast<u64>(&SdkClockNs),
              reinterpret_cast<u64>(&SdkCounterAt<I>), reinterpret_cast<u64>(&SdkLogAt<I>)}...}};
}
const auto kSdkFunctions = SdkTable(std::make_index_sequence<kMaxPackages>{});
static_assert(std::tuple_size_v<decltype(kSdkFunctions)::value_type> == std::size(kSdkImports));

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

void CheckConflicts(const State& st, const Entry& entry) {
    const auto mine = PackageCodeRanges(*entry.package);
    for (const auto& other : st.entries) {
        if (&other == &entry || !other.installed || other.package->module != entry.package->module)
            continue;
        if (const auto overlap =
                FindCodeOverlap(mine, PackageCodeRanges(*other.package, other.plan.hooks)))
            throw std::runtime_error("conflicts with " + other.name + ": " + *overlap);
    }
}

void Install(State& st, std::size_t index, Module& module, std::string module_sha256) {
    auto& entry = st.entries[index];
    const auto& p = *entry.package;
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
    CheckConflicts(st, entry);
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
    // rel32 reach of its entries (a second package lands in the next free range).
    // RWX like that trampoline area: the slot page is rewritten by enable/disable
    // while guest threads run.
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
                          [index](unsigned op, std::span<std::byte, 16>, uint64_t) {
                              return kSdkFunctions[index][op];
                          });
    for (const auto& [id, name] : p.counters)
        entry.counters.emplace(id, Tag{"GuestPatch." + p.id + "." + name});
    for (const auto& [id, name] : p.logs)
        entry.logs.emplace(id, Tag{"GuestPatch." + p.id + "." + name});
    std::memcpy(reinterpret_cast<void*>(image), plan.payload.data(), plan.payload.size());
    std::memcpy(reinterpret_cast<void*>(pool), plan.stubs.data(), plan.stubs.size());
    std::memcpy(reinterpret_cast<void*>(pool + code_size), plan.slots.data(), plan.slots.size());
    // The module is still being loaded: no thread can execute these bytes yet.
    for (const auto& e : plan.hooks)
        std::memcpy(reinterpret_cast<void*>(e.entry), e.patch.data(), e.patch.size());
    for (const auto& c : plan.patches)
        std::memcpy(reinterpret_cast<void*>(c.address), c.patch.replacement.data(),
                    c.patch.replacement.size());
    entry.plan = std::move(plan);
    entry.installed = true;
}

void EntryStatus(std::ostringstream& o, const Entry& entry) {
    o << "package: " << (entry.package ? entry.package->id : std::string("<unloaded>"))
      << "\nname: " << entry.name << "\npath: " << entry.path.string()
      << "\nsha256: " << (entry.package ? entry.package->digest : std::string())
      << "\ninstalled: " << entry.installed << "\n";
    if (!entry.error.empty())
        o << "error: " << entry.error << "\n";
    if (entry.package && !entry.attempted)
        o << "waiting_for_module: " << entry.package->module << "\n";
    for (const auto& e : entry.plan.hooks) {
        const auto slot = std::atomic_ref<u64>(*reinterpret_cast<u64*>(e.slot))
                              .load(std::memory_order_relaxed);
        o << (e.hook.kind == HookKind::Site ? "site: " : "hook: ") << e.hook.name;
        if (e.hook.kind == HookKind::Site)
            o << " mode=" << (e.hook.site_mode == SiteMode::Replace ? "replace" : "before");
        o << " entry=0x" << std::hex << e.entry << " handler=0x" << e.replacement
          << " original=0x" << e.original << std::dec << " stolen=" << e.saved.size()
          << " enabled=" << (slot == e.replacement) << "\n";
    }
    for (const auto& c : entry.plan.patches)
        o << "code_patch: " << c.patch.name << " address=0x" << std::hex << c.address << std::dec
          << " size=" << c.patch.replacement.size() << "\n";
    for (const auto& [id, c] : entry.counters)
        o << "counter: " << id << " " << c.name << " samples=" << c.count << " last=" << c.last
          << "\n";
    for (const auto& [id, t] : entry.logs)
        o << "log_tag: " << id << " " << t.name << " samples=" << t.count << " last=" << t.last
          << "\n";
}

std::string StatusLocked(State& st) {
    std::ostringstream o;
    if (st.entries.empty()) {
        o << "status: disabled\ndetail: no package selected (Launch Options, "
             "General.guest_patches or SHADPS4_GUEST_PATCH)\n";
        if (!st.selection_error.empty())
            o << "error: " << st.selection_error << "\n";
        return o.str();
    }
    const auto installed = std::ranges::count_if(st.entries, &Entry::installed);
    o << "packages: " << st.entries.size() << "\ninstalled: " << installed
      << "\nswitches: " << st.switches
      << "\nsdk_calls: " << st.sdk_calls.load(std::memory_order_relaxed) << "\n";
    if (!st.selection_error.empty())
        o << "error: " << st.selection_error << "\n";
    for (const auto& entry : st.entries)
        EntryStatus(o, entry);
    return o.str();
}

// Packages selected for this process, in install order: the development override,
// else the game's list (else its legacy single setting). Invalid or repeated names
// and anything past kMaxPackages are reported and left out.
std::vector<Entry> Selection(std::string& error) {
    std::vector<Entry> entries;
    const auto add = [&](std::string name, std::filesystem::path path) {
        if (std::ranges::any_of(entries, [&](const Entry& e) { return e.path == path; }))
            return;
        if (entries.size() == kMaxPackages) {
            error += "more than " + std::to_string(kMaxPackages) + " packages, " + name +
                     " left out; ";
            return;
        }
        Entry entry;
        entry.name = std::move(name);
        entry.path = std::move(path);
        entries.push_back(std::move(entry));
    };
    if (const char* paths = std::getenv("SHADPS4_GUEST_PATCH"); paths && *paths) {
        std::string_view rest = paths;
        while (!rest.empty()) {
            const auto end = rest.find(';');
            const auto item = rest.substr(0, end);
            if (!item.empty())
                add(std::string(item), std::filesystem::path(item));
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        }
        return entries;
    }
    auto names = EmulatorSettings.GetGuestPatches();
    if (names.empty() && !EmulatorSettings.GetGuestPatch().empty())
        names.push_back(EmulatorSettings.GetGuestPatch());
    const auto directory = PackageDirectory(Common::ElfInfo::Instance().GameSerial());
    for (auto& name : names) {
        if (!ValidPackageName(name)) {
            error += "invalid package name '" + name + "'; ";
            continue;
        }
        auto path = directory / (name + ".json");
        add(std::move(name), std::move(path));
    }
    return entries;
}

void ReadSelection(State& st) {
    st.read = true;
    auto entries = Selection(st.selection_error);
    if (!st.selection_error.empty())
        LOG_ERROR(Loader, "Guest patch selection: {}", st.selection_error);
    st.entries.reserve(kMaxPackages);
    for (auto& entry : entries) {
        try {
            entry.package = Package::Load(entry.path);
            for (const auto& other : st.entries) {
                if (other.package && other.package->id == entry.package->id)
                    throw std::runtime_error("same package id as " + other.name);
            }
            LOG_INFO(Loader, "Guest patch package {} ({}) targets {} {}", entry.package->id,
                     entry.package->digest, entry.package->title, entry.package->module);
        } catch (const std::exception& e) {
            entry.package.reset();
            entry.attempted = true;
            entry.error = e.what();
            LOG_ERROR(Loader, "Guest patch package {} rejected: {}", entry.path.string(),
                      entry.error);
        }
        st.entries.push_back(std::move(entry));
    }
}
} // namespace

std::filesystem::path PackageDirectory(std::string_view title) {
    return GuestPatch::PackageDirectory(Common::FS::GetUserPath(Common::FS::PathType::UserDir),
                                        title);
}

std::vector<PackageInfo> ListPackages(std::string_view title) {
    auto listing = GuestPatch::ListPackages(PackageDirectory(title), title);
    for (const auto& skipped : listing.skipped)
        LOG_WARNING(Loader, "Guest patch package skipped: {}", skipped);
    return std::move(listing.packages);
}

void OnModuleLoaded(Module& module, bool main_executable) {
    auto& st = GetState();
    std::scoped_lock lock(st.mutex);
    if (!st.read)
        ReadSelection(st); // once, at the first module (the main executable)
    bool target = false, executable = false;
    for (const auto& entry : st.entries) {
        if (!entry.package || entry.attempted)
            continue;
        target |= module.name == entry.package->module;
        executable |= main_executable && !entry.package->executable_sha256.empty();
    }
    if (!target && !executable)
        return;
    std::string sha;
    try {
        // Hashes the whole module file, so only for a target or a required executable.
        sha = ModuleSha256(module);
    } catch (const std::exception& e) {
        for (auto& entry : st.entries) {
            if (entry.package && !entry.attempted && module.name == entry.package->module) {
                entry.attempted = true;
                entry.error = e.what();
            }
        }
        LOG_ERROR(Loader, "Guest patches for {} not installed: {}", module.name, e.what());
        return;
    }
    if (main_executable)
        st.executable_sha256 = sha;
    for (std::size_t i = 0; i < st.entries.size(); ++i) {
        auto& entry = st.entries[i];
        if (!entry.package || entry.attempted || module.name != entry.package->module)
            continue;
        entry.attempted = true;
        try {
            Install(st, i, module, sha);
            std::ostringstream o;
            EntryStatus(o, entry);
            LOG_INFO(Loader, "Guest patch installed:\n{}", o.str());
        } catch (const std::exception& e) {
            entry.error = e.what();
            LOG_ERROR(Loader, "Guest patch {} not installed, game runs without it: {}",
                      entry.package->id, entry.error);
        }
    }
}

std::string Command(const std::vector<std::string>& args) {
    auto& st = GetState();
    std::scoped_lock lock(st.mutex);
    if (args.empty() || (args.size() == 1 && args[0] == "status"))
        return StatusLocked(st);
    if ((args[0] != "enable" && args[0] != "disable") || args.size() > 2)
        return "status: invalid_arguments\ndetail: status | enable [package|hook|package/hook] | "
               "disable [...]\n";
    const bool enable = args[0] == "enable";
    // No name: every hook. A package name or id: its hooks. "package/hook": that hook.
    // Anything else: hooks of that name in every package.
    std::string_view package_name, hook_name;
    if (args.size() == 2) {
        const std::string_view name = args[1];
        const auto slash = name.find('/');
        const bool is_package = std::ranges::any_of(st.entries, [&](const Entry& e) {
            return e.name == name || (e.package && e.package->id == name);
        });
        if (slash != std::string_view::npos) {
            package_name = name.substr(0, slash);
            hook_name = name.substr(slash + 1);
        } else if (is_package) {
            package_name = name;
        } else {
            hook_name = name;
        }
    }
    bool found = false;
    for (const auto& entry : st.entries) {
        if (!entry.installed)
            continue;
        if (!package_name.empty() && entry.name != package_name && entry.package->id != package_name)
            continue;
        for (const auto& e : entry.plan.hooks) {
            if (!hook_name.empty() && e.hook.name != hook_name)
                continue;
            found = true;
            // An aligned 8-byte store: a thread at the stub jumps either way, never torn.
            std::atomic_ref<u64>(*reinterpret_cast<u64*>(e.slot))
                .store(enable ? e.replacement : e.original, std::memory_order_release);
        }
    }
    if (!found)
        return "status: error\ndetail: no installed hook or site matches\n";
    ++st.switches;
    return StatusLocked(st);
}

std::vector<InstalledPackage> Installed() {
    auto& st = GetState();
    std::scoped_lock lock(st.mutex);
    std::vector<InstalledPackage> packages;
    for (const auto& entry : st.entries) {
        if (!entry.installed)
            continue;
        InstalledPackage package{entry.package->id,
                                 entry.package->name.empty() ? entry.package->id
                                                             : entry.package->name,
                                 !entry.plan.hooks.empty(), entry.plan.hooks.empty()};
        for (const auto& e : entry.plan.hooks) {
            package.enabled |= std::atomic_ref<u64>(*reinterpret_cast<u64*>(e.slot))
                                   .load(std::memory_order_relaxed) == e.replacement;
        }
        packages.push_back(std::move(package));
    }
    return packages;
}

void SetEnabled(std::string_view id, bool enabled) {
    const auto result = Command({enabled ? "enable" : "disable", std::string(id)});
    if (result.starts_with("status: error") || result.starts_with("status: invalid")) {
        LOG_WARNING(Loader, "Guest patch {} {}: {}", id, enabled ? "enable" : "disable", result);
    }
}
} // namespace Core::GuestPatch::Desktop
