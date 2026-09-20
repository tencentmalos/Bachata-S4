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
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include "common/guest_patch_log.h"
#include "core/guest_cpu/hle/scope.h"
#include "core/guest_cpu/hle/veneer_allocator.h"
#include "core/host_runtime/guest_patch.h"

namespace Core::GuestPatch {
namespace {
using Json = nlohmann::json;
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
bool Symbol(const std::string& s) {
    return !s.empty() && s.size() <= 96 &&
           (std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_') &&
           std::all_of(s.begin(), s.end(),
                       [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}
bool Digest(const std::string& s) {
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
Bytes Unhex(const std::string& s, size_t limit) {
    Check(s.size() % 2 == 0 && s.size() / 2 <= limit, "invalid hex size");
    Bytes result;
    auto digit = [](char c) {
        auto p = std::string_view("0123456789abcdef").find(c);
        Check(p != std::string_view::npos, "invalid hex digit");
        return p;
    };
    for (size_t i = 0; i < s.size(); i += 2)
        result.push_back(std::byte(digit(s[i]) * 16 + digit(s[i + 1])));
    return result;
}
uint64_t Number(const Json& j) {
    Check(j.is_number_unsigned(), "expected unsigned integer");
    return j.get<uint64_t>();
}
std::string Text(const Json& j, size_t max = 1024) {
    auto s = j.get<std::string>();
    Check(!s.empty() && s.size() <= max && s.find('\0') == s.npos, "invalid bounded string");
    return s;
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
std::string Sha256(std::span<const std::byte> bytes) {
    std::array<unsigned char, 32> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest.data());
    std::string out;
    for (auto b : digest) {
        out += "0123456789abcdef"[b >> 4];
        out += "0123456789abcdef"[b & 15];
    }
    return out;
}
std::string FileSha256(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    Check(f.good(), "cannot open identity file");
    return StreamSha256([&](void* dst, std::uint64_t size) -> std::int64_t {
        f.read(static_cast<char*>(dst), size);
        if (f.bad() || (f.fail() && !f.eof())) return -1;
        return f.gcount();
    });
}
std::string StreamSha256(const std::function<std::int64_t(void*, std::uint64_t)>& read) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    std::array<char, 65536> buffer;
    for (;;) {
        const auto count = read(buffer.data(), buffer.size());
        Check(count >= 0 && std::uint64_t(count) <= buffer.size(), "identity file read failed");
        if (!count) break;
        SHA256_Update(&ctx, buffer.data(), count);
    }
    std::array<unsigned char, 32> d;
    SHA256_Final(d.data(), &ctx);
    std::string out;
    for (auto b : d) {
        out += "0123456789abcdef"[b >> 4];
        out += "0123456789abcdef"[b & 15];
    }
    return out;
}
Package Package::Load(const std::filesystem::path& file) {
    Check(std::filesystem::file_size(file) <= 4 * 1024 * 1024, "package exceeds 4 MiB");
    std::ifstream f(file, std::ios::binary);
    Check(f.good(), "cannot open patch package");
    std::string source(4 * 1024 * 1024 + 1, '\0');
    f.read(source.data(), source.size());
    source.resize(f.gcount());
    Check(f.eof() && source.size() <= 4 * 1024 * 1024, "package grew/read failed");
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        Check(depth <= 24, "package nesting too deep");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        if (event == Json::parse_event_t::key)
            Check(keys.back().insert(value.get<std::string>()).second, "duplicate package key");
        if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    };
    const auto j = Json::parse(source, callback);
    Check(j.at("schema") == "shadps4.guest-functions.v1" && j.at("abi") == "x86_64-sysv" &&
              Number(j.at("sdk_version")) == 1,
          "unsupported patch format/ABI/SDK");
    Package p;
    p.digest = Sha256(std::as_bytes(std::span{source.data(), source.size()}));
    p.id = Text(j.at("id"), 96);
    Check(Symbol(p.id), "invalid package ID");
    p.title = Text(j.at("title"), 64);
    p.module = Text(j.at("module"), 128);
    Check(p.module == std::filesystem::path(p.module).filename(), "module must be a basename");
    p.module_sha256 = Text(j.at("module_sha256"));
    Check(Digest(p.module_sha256), "invalid module SHA256");
    if (j.contains("executable_sha256")) {
        p.executable_sha256 = Text(j.at("executable_sha256"));
        Check(Digest(p.executable_sha256), "invalid executable SHA256");
    }
    Check(j.at("segments").size() >= 1 && j.at("segments").size() <= 2, "invalid segment count");
    for (const auto& item : j.at("segments")) {
        Segment s;
        s.offset = Number(item.at("offset"));
        s.size = Number(item.at("size"));
        s.executable = item.at("executable").get<bool>();
        s.bytes = Unhex(item.at("hex"), 1 << 20);
        Check(s.size && s.offset % 4096 == 0 && s.size % 4096 == 0 && s.offset <= 1 << 20 &&
                  s.size <= (1 << 20) - s.offset && s.bytes.size() <= s.size,
              "invalid segment bounds");
        Check(Sha256(s.bytes) == Text(item.at("sha256")), "payload segment SHA mismatch");
        Check(s.offset == p.image_size, "noncontiguous/overlapping segments");
        Check(s.executable == p.segments.empty(), "expected RX then optional RW");
        p.image_size = s.offset + s.size;
        p.segments.push_back(std::move(s));
    }
    auto contains = [&](uint64_t off, uint64_t n, bool exec) {
        return std::any_of(p.segments.begin(), p.segments.end(), [&](const auto& s) {
            return s.executable == exec && off >= s.offset && off - s.offset <= s.size &&
                   n <= s.size - (off - s.offset);
        });
    };
    Check(j.at("exports").size() <= 128, "too many exports");
    for (auto it = j.at("exports").begin(); it != j.at("exports").end(); ++it) {
        Check(Symbol(it.key()), "invalid export");
        auto off = Number(it.value());
        Check(contains(off, 1, true), "export outside RX");
        p.exports.emplace(it.key(), off);
    }
    Check(j.at("rebase64").size() <= 8192, "too many relocations");
    std::set<uint64_t> fixups;
    for (const auto& item : j.at("rebase64")) {
        auto off = Number(item);
        Check((contains(off, 8, true) || contains(off, 8, false)) && fixups.insert(off).second,
              "invalid/duplicate relocation");
        p.rebase64.push_back(off);
    }
    std::set<std::string> imports;
    Check(j.at("imports").size() <= 195, "too many imports");
    for (const auto& item : j.at("imports")) {
        Import i{Text(item.at("name"), 96), Number(item.at("slot"))};
        Check(Symbol(i.name) && imports.insert(i.name).second && i.slot % 8 == 0 &&
                  contains(i.slot, 8, true) && fixups.insert(i.slot).second,
              "invalid import slot");
        p.imports.push_back(std::move(i));
    }
    Check(j.at("hooks").size() >= 1 && j.at("hooks").size() <= 64, "invalid hook count");
    std::set<std::string> hook_names, originals;
    for (const auto& item : j.at("hooks")) {
        Hook h{Text(item.at("name"), 96),       Text(item.at("replacement"), 96),
               Text(item.at("original"), 96),   Text(item.at("prototype")),
               Text(item.at("evidence"), 4096), Number(item.at("offset")),
               Unhex(item.at("expected"), 256)};
        Check(Symbol(h.name) && Symbol(h.original) && h.expected.size() >= 5 &&
                  p.exports.contains(h.replacement) && imports.contains(h.original) &&
                  hook_names.insert(h.name).second && originals.insert(h.original).second,
              "invalid/duplicate hook contract");
        Check(h.original != "shad_sdk_query" && h.original != "shad_sdk_clock_ns" &&
                  h.original != "shad_sdk_counter" && h.original != "shad_sdk_log" &&
                  h.original != h.replacement,
              "hook aliases SDK/replacement import");
        const auto mode = item.value("mode", std::string("typed"));
        Check(mode == "typed" || mode == "entry-observer-x86_64-avx", "unsupported hook mode");
        if (mode == "entry-observer-x86_64-avx") {
            Check(h.prototype == "opaque-machine-entry" && Symbol(Text(item.at("observer"), 96)),
                  "invalid entry observer contract");
        } else {
            Check(h.prototype.find(h.original + "(") != std::string::npos ||
                      h.prototype.find(h.original + " (") != std::string::npos,
                  "missing typed original prototype");
        }
        p.hooks.push_back(std::move(h));
    }
    std::set<std::string> bound_names;
    if (j.contains("bindings")) {
        Check(j.at("bindings").is_array() && j.at("bindings").size() <= 128,
              "too many guest bindings");
        for (const auto& item : j.at("bindings")) {
            Binding b;
            b.name = Text(item.at("name"), 96);
            b.kind = Text(item.at("kind"), 16);
            b.evidence = Text(item.at("evidence"), 4096);
            b.offset = Number(item.at("offset"));
            Check(Symbol(b.name) && imports.contains(b.name) && !originals.contains(b.name) &&
                      !p.exports.contains(b.name) && !b.name.starts_with("shad_sdk_") &&
                      bound_names.insert(b.name).second,
                  "guest binding aliases import/export");
            if (b.kind == "function") {
                b.expected = Unhex(item.at("expected"), 256);
                Check(b.expected.size() >= 5, "function binding requires preimage");
                b.size = b.expected.size();
                const auto prototype = Text(item.at("prototype"));
                Check(prototype.find(b.name + "(") != std::string::npos ||
                          prototype.find(b.name + " (") != std::string::npos,
                      "missing typed guest function prototype");
            } else {
                Check(b.kind == "data", "unknown guest binding kind");
                b.size = Number(item.at("size"));
                b.alignment = Number(item.at("alignment"));
                const auto access = Text(item.at("access"), 8);
                Check(access == "r" || access == "rw", "invalid guest data access");
                b.writable = access == "rw";
                Check(b.size && b.size <= 1 << 20 && b.alignment && b.alignment <= 4096 &&
                          (b.alignment & (b.alignment - 1)) == 0,
                      "invalid guest data geometry");
                Text(item.at("type"), 512);
            }
            p.bindings.push_back(std::move(b));
        }
    }
    for (const auto& i : p.imports)
        Check(originals.contains(i.name) || bound_names.contains(i.name) ||
                  i.name == "shad_sdk_query" || i.name == "shad_sdk_clock_ns" ||
                  i.name == "shad_sdk_counter" || i.name == "shad_sdk_log",
              "unknown host/guest import");
    Check(j.at("counters").size() <= 64, "too many SDK counters");
    for (const auto& item : j.at("counters")) {
        auto id = Number(item.at("id"));
        auto name = Text(item.at("name"), 96);
        Check(id && Symbol(name) && p.counters.emplace(id, std::move(name)).second,
              "invalid counter declaration");
    }
    if (j.contains("logs")) {
        Check(j.at("logs").is_array() && j.at("logs").size() <= 64, "too many SDK log tags");
        for (const auto& item : j.at("logs")) {
            const auto id = Number(item.at("id"));
            auto name = Text(item.at("name"), 96);
            Check(id && Symbol(name) && p.logs.emplace(id, std::move(name)).second,
                  "invalid SDK log tag declaration");
        }
    }
    // Relocations may not overlap each other, including unaligned absolute pointers.
    uint64_t end = 0;
    for (auto off : fixups) {
        Check(off >= end, "overlapping relocations");
        end = off + 8;
    }
    return p;
}

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
    struct Entry {
        Hook hook;
        bool enabled{true};
        uint64_t entry{}, stub{}, original{}, slot{}, replacement{};
        Bytes saved, patch;
        Relocated relocation;
    };
    std::vector<Entry> entries;
    std::vector<DebugModule> modules;
    std::vector<std::pair<Binding, GuestRange>> bindings;
    std::map<std::string, uint64_t> exports;
    std::string id, digest;
    mutable std::mutex mutex;
    bool attempted{}, installed{}, enabled{}, failed{};
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
    Check(!p.hooks.empty() && p.hooks.size() <= 64 && !p.segments.empty() && p.image_size &&
              p.image_size <= 1 << 20,
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
        for (const auto& h : p.hooks) {
            Check(h.offset <= module.size && h.expected.size() <= module.size - h.offset,
                  "hook outside module");
            const auto address = module.base + h.offset;
            Must(x.space.ValidateRange({GuestAddress{address}, h.expected.size()}, RX));
            Check(Read(x.space, address, h.expected.size()) == h.expected,
                  "entry preimage mismatch: " + h.name);
            for (auto cursor = address; cursor < address + h.expected.size();) {
                const auto info = Must(x.space.Query(GuestAddress{cursor}));
                Check(info.permission == RX, "entry must be immutable RX");
                cursor = info.range.End();
            }
        }
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
            }
            x.bindings.push_back({b, {GuestAddress{address}, b.size}});
        }
        const auto image = allocate(p.image_size, module.base + Align(module.size));
        const uint64_t code_size = Align(p.hooks.size() * 512 + 48), pool_size = code_size + 4096;
        const auto pool = allocate(pool_size, image.base + image.size);
        Check(image.size >= p.image_size && pool.size >= pool_size && image.base % 4096 == 0 &&
                  pool.base % 4096 == 0,
              "allocator returned invalid patch reservation");
        x.image = image.base;
        x.pool = pool.base;
        x.pool_size = pool_size;
        Bytes payload(p.image_size), stubs(code_size, std::byte{0xcc}), slots(4096);
        for (const auto& s : p.segments)
            std::copy(s.bytes.begin(), s.bytes.end(), payload.begin() + s.offset);
        for (auto off : p.rebase64) {
            uint64_t value;
            std::memcpy(&value, payload.data() + off, 8);
            Check(value < p.image_size, "absolute relocation target outside payload");
            Put(payload, off, value + image.base);
        }
        std::map<std::string, uint64_t> imports;
        for (const auto& [b, range] : x.bindings)
            Check(imports.emplace(b.name, range.base.value).second, "duplicate guest binding");
        for (size_t n = 0; n < p.hooks.size(); ++n) {
            const auto& h = p.hooks[n];
            Impl::Entry e;
            e.hook = h;
            e.entry = module.base + h.offset;
            e.stub = pool.base + n * 512;
            e.original = e.stub + 16;
            e.slot = pool.base + code_size + n * 8;
            e.replacement = image.base + p.exports.at(h.replacement);
            e.relocation = Relocate(h.expected, e.entry, e.original);
            Check(e.relocation.code.size() <= 496, "trampoline exceeded slot");
            e.saved.assign(h.expected.begin(), h.expected.begin() + e.relocation.stolen);
            const auto displacement = int64_t(e.stub) - int64_t(e.entry + 5);
            Check(displacement >= INT32_MIN && displacement <= INT32_MAX,
                  "entry stub outside rel32 range");
            e.patch.resize(e.saved.size(), std::byte{0x90});
            e.patch[0] = std::byte{0xe9};
            Put(e.patch, 1, int32_t(displacement));
            stubs[n * 512] = std::byte{0xff};
            stubs[n * 512 + 1] = std::byte{0x25};
            Put(stubs, n * 512 + 2, int32_t(e.slot - (e.stub + 6)));
            std::copy(e.relocation.code.begin(), e.relocation.code.end(),
                      stubs.begin() + n * 512 + 16);
            Put(slots, n * 8, e.replacement);
            imports.emplace(h.original, e.original);
            x.entries.push_back(std::move(e));
        }
        for (size_t a = 0; a < x.entries.size(); ++a)
            for (size_t b = a + 1; b < x.entries.size(); ++b) {
                const auto& e = x.entries[a];
                const auto& f = x.entries[b];
                Check(e.entry + e.saved.size() <= f.entry || f.entry + f.saved.size() <= e.entry,
                      "overlapping hooks");
            }
        for (const auto& [id, name] : p.counters)
            x.stats->counters.emplace(id, Impl::Stats::Counter{"GuestPatch." + p.id + "." + name});
        for (const auto& [id, name] : p.logs)
            x.stats->logs.emplace(id, Impl::Stats::LogTag{"GuestPatch." + p.id + "." + name});
        const char* service_names[] = {"shad_sdk_query", "shad_sdk_clock_ns", "shad_sdk_counter",
                                       "shad_sdk_log"};
        for (unsigned op = 0; op < 4; ++op) {
            auto adapter = std::make_shared<Impl::Service>();
            adapter->stats = x.stats;
            adapter->sink = x.sink;
            adapter->op = op;
            const auto operation = Must(
                x.registry.Adopt(adapter, "GuestPatch.SDK.v1." + std::string(service_names[op])));
            const auto offset = p.hooks.size() * 512 + op * 16;
            const auto veneer = Hle::HleVeneerAllocator::Encode(operation);
            std::copy(veneer.begin(), veneer.end(), stubs.begin() + offset);
            imports.emplace(service_names[op], pool.base + offset);
        }
        for (const auto& i : p.imports) {
            uint64_t old;
            std::memcpy(&old, payload.data() + i.slot, 8);
            Check(old == 0, "nonzero import slot");
            Put(payload, i.slot, imports.at(i.name));
        }
        // Prepare resident code/data first. No guest has run; any failure aborts
        // Prepare. Publication failure retains the address-space poison.
        Publish(x.space, token, image.base, payload);
        Publish(x.space, token, pool.base, stubs);
        Publish(x.space, token, pool.base + code_size, slots);
        for (const auto& s : p.segments)
            Protect(x.space, token, image.base + s.offset, s.size, s.executable ? RX : RW);
        Protect(x.space, token, pool.base, code_size, RX);
        // Dispatch targets are read-only to the guest; controls briefly publish under token.
        Protect(x.space, token, pool.base + code_size, 4096, R);
        // Allocate metadata before the first entry write too: a later host
        // allocation failure must not leave a half-reported installed package.
        for (const auto& [name, off] : p.exports)
            x.exports.emplace(name, image.base + off);
        DebugModule patch{"patch:" + p.id, image.base, {}};
        for (const auto& s : p.segments)
            patch.segments.push_back({GuestAddress{image.base + s.offset}, s.size});
        x.modules.push_back(std::move(patch));
        x.modules.push_back(
            {"trampolines:" + p.id, pool.base, {{GuestAddress{pool.base}, pool_size}}});
        for (const auto& e : x.entries)
            Replace(x.space, token, e.entry, e.patch);
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
    try {
        for (const auto& e : x.entries)
            Replace(x.space, token, e.entry, e.saved);
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
        for (auto [a, b] : e.relocation.instructions)
            o << "instruction: 0x" << std::hex << a << " -> 0x" << b << std::dec << "\n";
    }
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
