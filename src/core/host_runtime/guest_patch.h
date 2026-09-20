// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include "core/guest_cpu/api/context.h"
#include "core/guest_cpu/hle/call_adapter.h"

namespace Core::GuestPatch {
using namespace GuestCpu;
using Bytes = std::vector<std::byte>;
struct Relocated {
    Bytes code;
    size_t stolen{};
    // Original -> relocated instruction addresses, also exported for debugging.
    std::vector<std::pair<uint64_t, uint64_t>> instructions;
};
// Complete instructions, RIP-relative memory, rel32 calls/jumps and short/near
// Jcc. Unsupported PC-sensitive/control forms are refused, never blindly copied.
Relocated Relocate(std::span<const std::byte> code, uint64_t source, uint64_t destination,
                   size_t minimum = 5);
std::string Sha256(std::span<const std::byte> bytes);
std::string FileSha256(const std::filesystem::path& file);
std::string StreamSha256(const std::function<std::int64_t(void*, std::uint64_t)>& read);

struct Segment {
    uint64_t offset{}, size{};
    bool executable{};
    Bytes bytes;
};
struct Import {
    std::string name;
    uint64_t slot{};
};
struct Hook {
    std::string name, replacement, original, prototype, evidence;
    uint64_t offset{};
    Bytes expected;
};
// Same-module guest addresses only. Function bindings call guest code directly;
// data bindings are immutable pointer slots into the original guest storage.
struct Binding {
    std::string name, kind, evidence;
    uint64_t offset{}, size{}, alignment{1};
    bool writable{};
    Bytes expected;
};
struct Package {
    std::string id, title, module, module_sha256, executable_sha256, digest;
    std::vector<Segment> segments;
    std::map<std::string, uint64_t> exports;
    std::vector<uint64_t> rebase64;
    std::vector<Import> imports;
    std::vector<Hook> hooks;
    std::vector<Binding> bindings;
    std::map<uint64_t, std::string> counters;
    std::map<uint64_t, std::string> logs;
    uint64_t image_size{};
    static Package Load(const std::filesystem::path& file);
};
struct ModuleIdentity {
    std::string title, name, sha256;
    uint64_t base{}, size{};
    std::string executable_sha256;
};
struct Allocation {
    // A fresh RW mapping owned by the runtime VM, not an assumed code cave.
    uint64_t base{}, size{};
};
using Allocator = std::function<Allocation(uint64_t size, uint64_t near)>;
using CounterSink = std::function<void(const char*, int64_t)>;

// One package per session, all hooks installed before ANY guest thread exists.
// Resident trampolines and payload survive disable and all in-flight frames.
// Destroy only after CPU owners drain; VM itself owns and releases mappings.
class Manager {
public:
    Manager(CpuContext&, GuestAddressSpace&, Hle::HleCallRegistry&, CounterSink = {});
    ~Manager();
    void Install(const Package&, const ModuleIdentity&, const QuiescenceToken&, const Allocator&);
    void SetEnabled(bool enabled, const QuiescenceToken&, std::string_view hook = {});
    // Physical entry restoration requires zero live handles and the VM token.
    void Uninstall(const QuiescenceToken&);
    std::string Status() const;
    std::vector<DebugModule> DebugModules() const;
    std::vector<GuestRange> ProtectedRanges() const;
    std::uint64_t OriginalInstruction(std::uint64_t pc) const;
    uint64_t Export(std::string_view name) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Session-owned command endpoint. Registration uses weak ownership so a command
// cannot race runtime destruction or accidentally control a later generation.
class Control {
public:
    virtual ~Control() = default;
    virtual std::string Command(const std::vector<std::string>&) = 0;
};
void SetControl(const std::shared_ptr<Control>&);
std::string Command(const std::vector<std::string>&);
} // namespace Core::GuestPatch
