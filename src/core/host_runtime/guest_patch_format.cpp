// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include "core/host_runtime/guest_patch_format.h"

namespace Core::GuestPatch {
namespace {
using Json = nlohmann::json;
void Check(bool value, std::string_view why) {
    if (!value)
        throw std::runtime_error(std::string(why));
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
bool IsSdkImport(std::string_view name) {
    return std::find(std::begin(kSdkImports), std::end(kSdkImports), name) !=
           std::end(kSdkImports);
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
    // sdk_version 2 adds mid-function sites and same-length code patches; the
    // payload ABI and SDK imports are unchanged, so version 1 packages still load.
    Check(j.at("schema") == "shadps4.guest-functions.v1" && j.at("abi") == "x86_64-sysv" &&
              (Number(j.at("sdk_version")) == 1 || Number(j.at("sdk_version")) == 2),
          "unsupported patch format/ABI/SDK");
    Package p;
    p.sdk_version = Number(j.at("sdk_version"));
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
    const size_t patch_count = j.contains("patches") ? j.at("patches").size() : 0;
    Check(p.sdk_version == 2 || patch_count == 0, "code patches require sdk_version 2");
    Check(j.at("hooks").size() <= 64 && j.at("hooks").size() + patch_count >= 1,
          "invalid hook count");
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
        Check(!IsSdkImport(h.original) && h.original != h.replacement,
              "hook aliases SDK/replacement import");
        const auto mode = item.value("mode", std::string("typed"));
        Check(mode == "typed" || mode == "entry-observer-x86_64-avx" ||
                  (mode == "site-x86_64-avx" && p.sdk_version == 2),
              "unsupported hook mode");
        if (mode == "entry-observer-x86_64-avx") {
            h.kind = HookKind::EntryObserver;
            Check(h.prototype == "opaque-machine-entry" && Symbol(Text(item.at("observer"), 96)),
                  "invalid entry observer contract");
        } else if (mode == "site-x86_64-avx") {
            // `original` is the continuation: the relocated original instructions
            // (before) or the instruction after `expected` (replace).
            h.kind = HookKind::Site;
            const auto site_mode = Text(item.at("site_mode"), 16);
            Check(site_mode == "before" || site_mode == "replace", "invalid site mode");
            h.site_mode = site_mode == "replace" ? SiteMode::Replace : SiteMode::Before;
            Check(h.prototype == "opaque-machine-site" && Symbol(Text(item.at("handler"), 96)),
                  "invalid site contract");
            // Replace sites steal exactly `expected`; the relocator handles <= 32 bytes.
            Check(h.site_mode == SiteMode::Before || h.expected.size() <= 32,
                  "replace site covers more than 32 bytes");
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
        Check(originals.contains(i.name) || bound_names.contains(i.name) || IsSdkImport(i.name),
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
    if (patch_count) {
        Check(j.at("patches").is_array() && patch_count <= 256, "too many code patches");
        std::set<std::string> patch_names;
        for (const auto& item : j.at("patches")) {
            CodePatch c{Text(item.at("name"), 96), Text(item.at("evidence"), 4096),
                        Number(item.at("offset")), Unhex(item.at("expected"), 256),
                        Unhex(item.at("replacement"), 256)};
            Check(Symbol(c.name) && patch_names.insert(c.name).second && !c.expected.empty() &&
                      c.expected.size() == c.replacement.size() && c.expected != c.replacement,
                  "invalid/duplicate code patch");
            p.patches.push_back(std::move(c));
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

std::pair<uint64_t, uint64_t> PoolGeometry(const Package& p) {
    // Hook stubs, then 16 bytes of code per SDK operation, then one slot page.
    const uint64_t code_size = Align(p.hooks.size() * kStubStride + std::size(kSdkImports) * 16);
    return {code_size, code_size + 4096};
}
Plan BuildPlan(const Package& p, const ModuleIdentity& module, uint64_t image, uint64_t pool,
               const std::map<std::string, uint64_t>& bindings, const SdkResolver& sdk) {
    Check(p.hooks.size() <= 64 && p.hooks.size() + p.patches.size() >= 1 &&
              !p.segments.empty() && p.image_size && p.image_size <= 1 << 20,
          "invalid package geometry");
    Check(module.base <= INT64_MAX - module.size, "invalid module geometry");
    Plan plan;
    plan.image = image;
    plan.pool = pool;
    std::tie(plan.code_size, plan.pool_size) = PoolGeometry(p);
    plan.payload.resize(p.image_size);
    plan.stubs.assign(plan.code_size, std::byte{0xcc});
    plan.slots.resize(4096);
    for (const auto& s : p.segments)
        std::copy(s.bytes.begin(), s.bytes.end(), plan.payload.begin() + s.offset);
    for (auto off : p.rebase64) {
        uint64_t value;
        std::memcpy(&value, plan.payload.data() + off, 8);
        Check(value < p.image_size, "absolute relocation target outside payload");
        Put(plan.payload, off, value + image);
    }
    std::map<std::string, uint64_t> imports = bindings;
    for (size_t n = 0; n < p.hooks.size(); ++n) {
        const auto& h = p.hooks[n];
        Check(h.offset <= module.size && h.expected.size() <= module.size - h.offset,
              "hook outside module");
        const bool replace = h.kind == HookKind::Site && h.site_mode == SiteMode::Replace;
        PlannedHook e;
        e.hook = h;
        e.entry = module.base + h.offset;
        e.stub = pool + n * kStubStride;
        e.original = e.stub + 16;
        e.slot = pool + plan.code_size + n * 8;
        e.replacement = image + p.exports.at(h.replacement);
        // The relocated original is the disabled path for every kind. A replace
        // site steals exactly its expected instructions, so disabling restores them.
        e.relocation = Relocate(h.expected, e.entry, e.original, replace ? h.expected.size() : 5);
        Check(!replace || e.relocation.stolen == h.expected.size(),
              "replace site must cover whole instructions: " + h.name);
        Check(e.relocation.code.size() <= kStubStride - 16, "trampoline exceeded slot");
        e.saved.assign(h.expected.begin(), h.expected.begin() + e.relocation.stolen);
        const auto displacement = int64_t(e.stub) - int64_t(e.entry + 5);
        Check(displacement >= INT32_MIN && displacement <= INT32_MAX,
              "entry stub outside rel32 range");
        e.patch.resize(e.saved.size(), std::byte{0x90});
        e.patch[0] = std::byte{0xe9};
        Put(e.patch, 1, int32_t(displacement));
        const auto stub = n * kStubStride;
        plan.stubs[stub] = std::byte{0xff};
        plan.stubs[stub + 1] = std::byte{0x25};
        Put(plan.stubs, stub + 2, int32_t(e.slot - (e.stub + 6)));
        std::copy(e.relocation.code.begin(), e.relocation.code.end(),
                  plan.stubs.begin() + stub + 16);
        Put(plan.slots, n * 8, e.replacement);
        // Continuation the payload jumps to after its handler/replacement.
        Check(imports.emplace(h.original, replace ? e.entry + h.expected.size() : e.original).second,
              "duplicate hook continuation");
        plan.hooks.push_back(std::move(e));
    }
    // Code a hook rewrites (stolen bytes) and every checked preimage stay disjoint,
    // so preimage checks before installation stay valid regardless of write order.
    std::vector<std::tuple<uint64_t, uint64_t, std::string>> ranges;
    for (const auto& e : plan.hooks)
        ranges.emplace_back(e.entry, e.entry + e.hook.expected.size(), e.hook.name);
    for (const auto& c : p.patches) {
        Check(c.offset <= module.size && c.expected.size() <= module.size - c.offset,
              "code patch outside module");
        plan.patches.push_back({c, module.base + c.offset});
        ranges.emplace_back(module.base + c.offset, module.base + c.offset + c.expected.size(),
                            c.name);
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i)
        Check(std::get<1>(ranges[i - 1]) <= std::get<0>(ranges[i]),
              "overlapping hooks/patches: " + std::get<2>(ranges[i - 1]) + ", " +
                  std::get<2>(ranges[i]));
    for (unsigned op = 0; op < std::size(kSdkImports); ++op) {
        const auto offset = p.hooks.size() * kStubStride + op * 16;
        const auto address = sdk(op, std::span<std::byte, 16>(plan.stubs.data() + offset, 16),
                                 pool + offset);
        Check(address != 0, "unresolved SDK operation");
        imports.emplace(kSdkImports[op], address);
    }
    for (const auto& i : p.imports) {
        uint64_t old;
        std::memcpy(&old, plan.payload.data() + i.slot, 8);
        Check(old == 0, "nonzero import slot");
        const auto it = imports.find(i.name);
        Check(it != imports.end(), "unresolved import: " + i.name);
        Put(plan.payload, i.slot, it->second);
    }
    for (const auto& [name, off] : p.exports)
        plan.exports.emplace(name, image + off);
    return plan;
}

} // namespace Core::GuestPatch
