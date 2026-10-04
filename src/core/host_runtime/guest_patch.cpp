// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include "common/guest_patch_log.h"
#include "core/guest_cpu/hle/scope.h"
#include "core/guest_cpu/hle/veneer_allocator.h"
#include "core/host_runtime/guest_patch.h"

namespace Core::GuestPatch {
namespace {
constexpr auto R = GuestPermission::Read;
constexpr auto RW = R | GuestPermission::Write;
constexpr auto RX = R | GuestPermission::Execute;
void Check(bool value, std::string_view why) {
    if (!value)
        throw std::runtime_error(std::string(why));
}
void Must(Status s) {
    if (!s)
        throw std::runtime_error(Describe(s.GetError()));
}
template <class T>
T Must(Result<T> s) {
    if (!s)
        throw std::runtime_error(Describe(s.GetError()));
    return std::move(s).Value();
}
uint64_t Align(uint64_t n) {
    Check(n <= UINT64_MAX - 4095, "size overflow");
    return (n + 4095) & ~4095ULL;
}
template <class T>
void Put(Bytes& b, size_t off, T value) {
    Check(off <= b.size() && sizeof(T) <= b.size() - off, "fixup outside image");
    std::memcpy(b.data() + off, &value, sizeof(value));
}
Bytes Read(GuestAddressSpace& space, uint64_t address, size_t size) {
    Bytes b(size);
    Must(space.Read(GuestAddress{address}, b));
    return b;
}
void Publish(GuestAddressSpace& space, const QuiescenceToken& t, uint64_t address,
             std::span<const std::byte> b) {
    Must(space.PublishCode(t, {GuestAddress{address}, b.size()}, b));
}
void Protect(GuestAddressSpace& space, const QuiescenceToken& t, uint64_t base, uint64_t size,
             GuestPermission p) {
    Must(space.UpdateVmUnderToken(t, GuestAddressSpace::VmOperation::Protect,
                                  {GuestAddress{base}, size}, p));
}
void Replace(GuestAddressSpace& space, const QuiescenceToken& t, uint64_t address,
             std::span<const std::byte> bytes) {
    const uint64_t base = address & ~4095ULL, end = Align(address + bytes.size());
    // Only immutable RX entries are admitted, so restoring RX is exact.
    Protect(space, t, base, end - base, RW);
    Publish(space, t, address, bytes);
    Protect(space, t, base, end - base, RX);
}
} // namespace
struct Manager::Impl {
    CpuContext& cpu;
    GuestAddressSpace& space;
    Hle::HleCallRegistry& registry;
    CounterSink sink;
    struct Stats {
        struct Counter {
            std::string name;
            uint64_t count{};
            int64_t last{};
        };
        struct LogTag {
            std::string name;
            uint64_t count{};
            int64_t last{};
        };
        std::mutex mutex;
        std::map<uint64_t, Counter> counters;
        std::map<uint64_t, LogTag> logs;
        std::string package;
        uint64_t calls{}, context{}, thread{}, generation{}, invocation{};
    };
    std::shared_ptr<Stats> stats = std::make_shared<Stats>();
    struct Service final : Hle::HleCallAdapter {
        std::shared_ptr<Stats> stats;
        CounterSink sink;
        unsigned op;
        bool SignatureSupported() const noexcept override {
            return true;
        }
        std::string SignatureDescription() const override {
            return "shad guest SDK v1 SysV scalar";
        }
        GuestCpu::Status Invoke(Hle::HleCallFrame& f) const override {
            auto* scope = Hle::HleScope::Current();
            if (!scope)
                return MakeError(ErrorCategory::WrongState, "GuestPatch.SDK",
                                 "missing owner scope");
            uint64_t result = 0;
            std::string log_tag;
            std::string log_package;
            int64_t log_value{};
            uint64_t log_context{}, log_thread{}, log_generation{}, log_invocation{};
            bool write_log = false;
            {
                std::scoped_lock lock(stats->mutex);
                ++stats->calls;
                stats->context = scope->Context().ContextId();
                stats->thread = scope->Thread().id;
                stats->generation = scope->Thread().generation;
                stats->invocation = scope->InvocationId();
                if (op == 0)
                    result = f.registers.Get(Gpr::Rdi) == 1 ? 7 : 0;
                if (op == 1)
                    result = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
                if (op == 2) {
                    const auto it = stats->counters.find(f.registers.Get(Gpr::Rdi));
                    if (it == stats->counters.end())
                        result = uint64_t(-22);
                    else {
                        auto& c = it->second;
                        ++c.count;
                        c.last = int64_t(f.registers.Get(Gpr::Rsi));
                        if (sink)
                            sink(c.name.c_str(), c.last);
                    }
                }
                if (op == 3) {
                    const auto it = stats->logs.find(f.registers.Get(Gpr::Rdi));
                    if (it == stats->logs.end()) {
                        result = uint64_t(-22);
                    } else {
                        auto& tag = it->second;
                        ++tag.count;
                        tag.last = int64_t(f.registers.Get(Gpr::Rsi));
                        log_package = stats->package;
                        log_tag = tag.name;
                        log_value = tag.last;
                        log_context = stats->context;
                        log_thread = stats->thread;
                        log_generation = stats->generation;
                        log_invocation = stats->invocation;
                        write_log = true;
                    }
                }
            }
            f.registers.Set(Gpr::Rax, result);
            if (write_log) {
                Common::GuestPatchLog::Write(log_package, log_tag, log_value, log_context,
                                             log_thread, log_generation, log_invocation);
            }
            return Ok();
        }
    };
    using Entry = PlannedHook;
    std::vector<Entry> entries;
    std::vector<PlannedPatch> patches;
    std::vector<DebugModule> modules;
    std::vector<std::pair<Binding, GuestRange>> bindings;
    std::map<std::string, uint64_t> exports;
    std::vector<CodeRange> ranges;
    std::string id, digest;
    mutable std::mutex mutex;
    bool attempted{}, modified{}, installed{}, enabled{}, failed{};
    uint64_t switches{}, image{}, pool{}, pool_size{};
    Impl(CpuContext& c, GuestAddressSpace& s, Hle::HleCallRegistry& r, CounterSink out)
        : cpu(c), space(s), registry(r), sink(std::move(out)) {}
};
Manager::Manager(CpuContext& c, GuestAddressSpace& s, Hle::HleCallRegistry& r, CounterSink out)
    : impl(std::make_unique<Impl>(c, s, r, std::move(out))) {}
Manager::~Manager() = default;
void Manager::Install(const Package& p, const ModuleIdentity& module, const QuiescenceToken& token,
                      const Allocator& allocate) {
    std::scoped_lock lock(impl->mutex);
    auto& x = *impl;
    Check(token.IsValid() && token.IsFrom(&x.space), "invalid/foreign patch transaction");
    Check(!x.attempted, "package install is single-shot per session");
    Check(x.cpu.LiveThreadCount() == 0, "entry installation requires no guest thread handles");
    Check(p.hooks.size() <= 64 && p.hooks.size() + p.patches.size() >= 1 && !p.segments.empty() &&
              p.image_size && p.image_size <= 1 << 20,
          "invalid package geometry");
    Check(p.title == module.title && p.module == module.name && p.module_sha256 == module.sha256,
          "title/module SHA256 identity mismatch");
    Check(p.executable_sha256.empty() || p.executable_sha256 == module.executable_sha256,
          "main executable SHA256 identity mismatch");
    x.attempted = true;
    x.id = p.id;
    x.digest = p.digest;
    x.stats->package = p.id;
    try {
        Check(module.base <= INT64_MAX - module.size, "invalid module geometry");
        // Validate ALL preimages and entries before allocations, services or mutations.
        const auto check_code = [&](uint64_t offset, const Bytes& expected, const std::string& name,
                                    const std::string& what) {
            Check(offset <= module.size && expected.size() <= module.size - offset,
                  what + " outside module");
            const auto address = module.base + offset;
            Must(x.space.ValidateRange({GuestAddress{address}, expected.size()}, RX));
            Check(Read(x.space, address, expected.size()) == expected,
                  what + " preimage mismatch: " + name);
            for (auto cursor = address; cursor < address + expected.size();) {
                const auto info = Must(x.space.Query(GuestAddress{cursor}));
                Check(info.permission == RX, what + " must be immutable RX");
                cursor = info.range.End();
            }
        };
        for (const auto& h : p.hooks)
            check_code(h.offset, h.expected, h.name, "entry");
        for (const auto& c : p.patches)
            check_code(c.offset, c.expected, c.name, "code patch");
        std::map<std::string, uint64_t> bound;
        for (const auto& b : p.bindings) {
            Check(b.offset <= module.size && b.size && b.size <= module.size - b.offset &&
                      b.alignment && b.alignment <= 4096 && (b.alignment & (b.alignment - 1)) == 0,
                  "guest binding outside module");
            const auto address = module.base + b.offset;
            Check(address % b.alignment == 0, "unaligned guest binding");
            const bool function = b.kind == "function";
            Check(function || b.kind == "data", "unknown binding kind");
            Must(x.space.ValidateRange({GuestAddress{address}, b.size},
                                       function ? RX : (b.writable ? RW : R)));
            for (auto cursor = address; cursor < address + b.size;) {
                const auto info = Must(x.space.Query(GuestAddress{cursor}));
                Check(function ? info.permission == RX
                               : (info.permission == R || info.permission == RW),
                      "guest binding permission mismatch");
                cursor = info.range.End();
            }
            if (function) {
                Check(b.expected.size() == b.size && b.size >= 5 &&
                          Read(x.space, address, b.size) == b.expected,
                      "function binding preimage mismatch: " + b.name);
                for (const auto& h : p.hooks)
                    Check(b.offset + b.size <= h.offset || h.offset + h.expected.size() <= b.offset,
                          "guest binding overlaps hook; use original trampoline");
                for (const auto& c : p.patches)
                    Check(b.offset + b.size <= c.offset || c.offset + c.expected.size() <= b.offset,
                          "guest binding overlaps code patch");
            }
            x.bindings.push_back({b, {GuestAddress{address}, b.size}});
            Check(bound.emplace(b.name, address).second, "duplicate guest binding");
        }
        x.modified = true;
        const auto image = allocate(p.image_size, module.base + Align(module.size));
        const auto [code_size, pool_size] = PoolGeometry(p);
        const auto pool = allocate(pool_size, image.base + image.size);
        Check(image.size >= p.image_size && pool.size >= pool_size && image.base % 4096 == 0 &&
                  pool.base % 4096 == 0,
              "allocator returned invalid patch reservation");
        x.image = image.base;
        x.pool = pool.base;
        x.pool_size = pool_size;
        for (const auto& [id, name] : p.counters)
            x.stats->counters.emplace(id, Impl::Stats::Counter{"GuestPatch." + p.id + "." + name});
        for (const auto& [id, name] : p.logs)
            x.stats->logs.emplace(id, Impl::Stats::LogTag{"GuestPatch." + p.id + "." + name});
        auto plan = BuildPlan(
            p, module, image.base, pool.base, bound,
            [&](unsigned op, std::span<std::byte, 16> stub, uint64_t address) -> uint64_t {
                auto adapter = std::make_shared<Impl::Service>();
                adapter->stats = x.stats;
                adapter->sink = x.sink;
                adapter->op = op;
                const auto operation = Must(x.registry.Adopt(
                    adapter, "GuestPatch.SDK.v1." + std::string(kSdkImports[op])));
                const auto veneer = Hle::HleVeneerAllocator::Encode(operation);
                Check(veneer.size() <= stub.size(), "SDK veneer exceeds its stub");
                std::copy(veneer.begin(), veneer.end(), stub.begin());
                return address;
            });
        x.entries = std::move(plan.hooks);
        x.patches = std::move(plan.patches);
        x.ranges = PackageCodeRanges(p, x.entries);
        // Prepare resident code/data first. No guest has run; any failure aborts
        // Prepare. Publication failure retains the address-space poison.
        Publish(x.space, token, image.base, plan.payload);
        Publish(x.space, token, pool.base, plan.stubs);
        Publish(x.space, token, pool.base + code_size, plan.slots);
        for (const auto& s : p.segments)
            Protect(x.space, token, image.base + s.offset, s.size, s.executable ? RX : RW);
        Protect(x.space, token, pool.base, code_size, RX);
        // Dispatch targets are read-only to the guest; controls briefly publish under token.
        Protect(x.space, token, pool.base + code_size, 4096, R);
        // Allocate metadata before the first entry write too: a later host
        // allocation failure must not leave a half-reported installed package.
        x.exports = std::move(plan.exports);
        DebugModule patch{"patch:" + p.id, image.base, {}};
        for (const auto& s : p.segments)
            patch.segments.push_back({GuestAddress{image.base + s.offset}, s.size});
        x.modules.push_back(std::move(patch));
        x.modules.push_back(
            {"trampolines:" + p.id, pool.base, {{GuestAddress{pool.base}, pool_size}}});
        for (const auto& e : x.entries)
            Replace(x.space, token, e.entry, e.patch);
        for (const auto& c : x.patches)
            Replace(x.space, token, c.address, c.patch.replacement);
        x.installed = true;
        x.enabled = true;
    } catch (...) {
        x.failed = true;
        throw;
    }
}
void Manager::SetEnabled(bool enabled, const QuiescenceToken& token, std::string_view hook) {
    std::scoped_lock lock(impl->mutex);
    auto& x = *impl;
    Check(token.IsValid() && token.IsFrom(&x.space), "invalid/foreign patch transaction");
    Check(x.installed && !x.failed, "patch not installed/healthy");
    Check(!x.entries.empty(), "package has no switchable hooks or sites");
    Check(hook.empty() || std::any_of(x.entries.begin(), x.entries.end(),
                                      [&](const auto& e) { return e.hook.name == hook; }),
          "unknown hook name");
    // No code is overwritten while a thread can have a continuation in it.
    // Token validates the owner/context and blocks execution/mapping mutation.
    for (const auto& e : x.entries) {
        Check(Read(x.space, e.entry, e.patch.size()) == e.patch,
              "entry changed after installation");
        Check(Read(x.space, e.original, e.relocation.code.size()) == e.relocation.code,
              "trampoline changed");
    }
    const auto page = x.entries.front().slot & ~4095ULL;
    auto bytes = Read(x.space, page, 4096);
    for (const auto& e : x.entries) {
        const auto selected = hook.empty() || e.hook.name == hook;
        Put(bytes, e.slot - page, (selected ? enabled : e.enabled) ? e.replacement : e.original);
    }
    try {
        Protect(x.space, token, page, 4096, RW);
        Publish(x.space, token, page, bytes);
        Protect(x.space, token, page, 4096, R);
        for (auto& e : x.entries)
            if (hook.empty() || e.hook.name == hook)
                e.enabled = enabled;
        x.enabled = std::any_of(x.entries.begin(), x.entries.end(),
                                [](const auto& e) { return e.enabled; });
        ++x.switches;
    } catch (...) {
        x.failed = true;
        throw;
    }
}
void Manager::Uninstall(const QuiescenceToken& token) {
    std::scoped_lock lock(impl->mutex);
    auto& x = *impl;
    Check(token.IsValid() && token.IsFrom(&x.space), "invalid/foreign patch transaction");
    Check(x.installed && !x.failed, "patch not installed/healthy");
    Check(x.cpu.LiveThreadCount() == 0, "uninstall requires all guest handles destroyed");
    for (const auto& e : x.entries)
        Check(Read(x.space, e.entry, e.patch.size()) == e.patch, "entry was modified");
    for (const auto& c : x.patches)
        Check(Read(x.space, c.address, c.patch.replacement.size()) == c.patch.replacement,
              "code patch was modified");
    try {
        for (const auto& e : x.entries)
            Replace(x.space, token, e.entry, e.saved);
        for (const auto& c : x.patches)
            Replace(x.space, token, c.address, c.patch.expected);
        x.installed = false;
        x.enabled = false;
    } catch (...) {
        x.failed = true;
        throw;
    }
}
uint64_t Manager::Export(std::string_view name) const {
    std::scoped_lock lock(impl->mutex);
    return impl->exports.at(std::string(name));
}
bool Manager::Modified() const {
    std::scoped_lock lock(impl->mutex);
    return impl->modified;
}
std::string Manager::Id() const {
    std::scoped_lock lock(impl->mutex);
    return impl->id;
}
std::vector<std::string> Manager::HookNames() const {
    std::scoped_lock lock(impl->mutex);
    std::vector<std::string> names;
    for (const auto& e : impl->entries)
        names.push_back(e.hook.name);
    return names;
}
std::vector<CodeRange> Manager::CodeRanges() const {
    std::scoped_lock lock(impl->mutex);
    return impl->ranges;
}
std::vector<DebugModule> Manager::DebugModules() const {
    std::scoped_lock lock(impl->mutex);
    return impl->modules;
}
std::vector<GuestRange> Manager::ProtectedRanges() const {
    std::scoped_lock lock(impl->mutex);
    std::vector<GuestRange> ranges;
    for (const auto& m : impl->modules)
        ranges.insert(ranges.end(), m.segments.begin(), m.segments.end());
    for (const auto& e : impl->entries)
        ranges.push_back({GuestAddress{e.entry & ~4095ULL},
                          Align(e.entry + e.saved.size()) - (e.entry & ~4095ULL)});
    for (const auto& c : impl->patches)
        ranges.push_back({GuestAddress{c.address & ~4095ULL},
                          Align(c.address + c.patch.expected.size()) - (c.address & ~4095ULL)});
    for (const auto& [binding, range] : impl->bindings)
        ranges.push_back({GuestAddress{range.base.value & ~4095ULL},
                          Align(range.End()) - (range.base.value & ~4095ULL)});
    return ranges;
}
std::string Manager::Status() const {
    std::scoped_lock lock(impl->mutex);
    const auto& x = *impl;
    std::ostringstream o;
    o << "package: " << x.id << "\nsha256: " << x.digest << "\ninstalled: " << x.installed
      << "\nenabled: " << x.enabled << "\nfailed: " << x.failed << "\nswitches: " << x.switches
      << "\ncontext: " << x.cpu.ContextId() << "\n";
    for (const auto& [b, range] : x.bindings)
        o << "binding: " << b.name << " kind=" << b.kind << " address=0x" << std::hex
          << range.base.value << std::dec << " size=" << range.size << " access="
          << (b.kind == "function" ? "rx"
              : b.writable         ? "rw"
                                   : "r")
          << "\n";
    for (const auto& e : x.entries) {
        o << "hook: " << e.hook.name << " entry=0x" << std::hex << e.entry << " original=0x"
          << e.original << " replacement=0x" << e.replacement << " slot=0x" << e.slot << std::dec
          << " stolen=" << e.saved.size() << " enabled=" << (x.installed && e.enabled) << "\n";
        if (e.hook.kind == HookKind::Site)
            o << "site: " << e.hook.name << " mode="
              << (e.hook.site_mode == SiteMode::Replace ? "replace" : "before") << "\n";
        for (auto [a, b] : e.relocation.instructions)
            o << "instruction: 0x" << std::hex << a << " -> 0x" << b << std::dec << "\n";
    }
    for (const auto& c : x.patches)
        o << "code_patch: " << c.patch.name << " address=0x" << std::hex << c.address << std::dec
          << " size=" << c.patch.replacement.size() << " installed=" << x.installed << "\n";
    std::scoped_lock stats(x.stats->mutex);
    o << "sdk_calls: " << x.stats->calls << "\nlast_owner: " << x.stats->context << ":"
      << x.stats->thread << ":" << x.stats->generation << ":" << x.stats->invocation << "\n";
    for (const auto& [id, c] : x.stats->counters)
        o << "counter: " << id << " " << c.name << " samples=" << c.count << " last=" << c.last
          << "\n";
    for (const auto& [id, tag] : x.stats->logs)
        o << "log_tag: " << id << " " << tag.name << " samples=" << tag.count
          << " last=" << tag.last << "\n";
    return o.str();
}
namespace {
std::mutex control_mutex;
std::weak_ptr<Control> active_control;
} // namespace
void SetControl(const std::shared_ptr<Control>& c) {
    std::scoped_lock lock(control_mutex);
    active_control = c;
}
std::string Command(const std::vector<std::string>& args) {
    std::shared_ptr<Control> c;
    {
        std::scoped_lock lock(control_mutex);
        c = active_control.lock();
    }
    if (!c)
        return "status: no_session\n";
    try {
        return c->Command(args);
    } catch (const std::exception& e) {
        return "status: error\ndetail: " + std::string(e.what()) + "\n";
    }
}
std::uint64_t Manager::OriginalInstruction(std::uint64_t pc) const {
    std::lock_guard guard(impl->mutex);
    for (const auto& e : impl->entries) {
        if (pc < e.entry || pc >= e.entry + e.saved.size())
            continue;
        for (const auto& [original, relocated] : e.relocation.instructions)
            if (original == pc)
                return relocated;
        throw std::runtime_error("auto tag points inside a relocated instruction");
    }
    return pc;
}
} // namespace Core::GuestPatch
