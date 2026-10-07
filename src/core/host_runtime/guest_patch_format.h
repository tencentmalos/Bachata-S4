// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Guest function package format and install plan. Shared by the Android FEX
// runtime (guest_patch.cpp) and the desktop native loader (core/guest_patch_desktop.cpp):
// parsing, trampoline relocation and the final byte layout are identical, only
// validation of live memory, mapping and publication differ per backend.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core::GuestPatch {
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
enum class HookKind {
    Typed,         // replacement has the original C prototype; original = trampoline
    EntryObserver, // read-only machine state at function entry, then the original
    Site,          // mutable machine state at any instruction boundary (sdk_version 2)
};
enum class SiteMode {
    Before,  // handler runs, then the stolen original instructions
    Replace, // handler replaces exactly the `expected` instructions
};
struct Hook {
    std::string name, replacement, original, prototype, evidence;
    uint64_t offset{};
    Bytes expected;
    // sdk_version 3: the entry is the module's PLT entry for this import NID instead of a
    // fixed offset. ResolveImportHooks fills offset and expected from the loaded module
    // before anything else looks at the hook.
    std::string import_nid;
    HookKind kind{HookKind::Typed};
    SiteMode site_mode{SiteMode::Before};
};
// Same-length code edit with a checked preimage (sdk_version 2). Applied once at
// install; enable/disable switches hooks and sites, never these bytes.
struct CodePatch {
    std::string name, evidence;
    uint64_t offset{};
    Bytes expected, replacement;
};
// Same-module guest addresses only. Function bindings call guest code directly;
// data bindings are immutable pointer slots into the original guest storage.
struct Binding {
    std::string name, kind, evidence;
    uint64_t offset{}, size{}, alignment{1};
    bool writable{};
    Bytes expected;
};
// sdk_version 3: bytes a build has at an exact offset of its loaded image. A package
// without module_sha256 names its build by these instead: every one must match, nothing
// is searched for.
struct Signature {
    uint64_t offset{};
    Bytes bytes;
};
struct Package {
    std::string id, title, module, module_sha256, executable_sha256, digest;
    std::vector<Signature> signatures;
    // Optional, for patch lists: display name and what the package does.
    std::string name, description;
    uint64_t sdk_version{1};
    std::vector<Segment> segments;
    std::map<std::string, uint64_t> exports;
    std::vector<uint64_t> rebase64;
    std::vector<Import> imports;
    std::vector<Hook> hooks;
    std::vector<CodePatch> patches;
    std::vector<Binding> bindings;
    std::map<uint64_t, std::string> counters;
    std::map<uint64_t, std::string> logs;
    uint64_t image_size{};
    static Package Load(const std::filesystem::path& file);
};
// The module's PLT entry for an import: its offset in the module and its bytes
// (`jmp [rip+disp32]` through the import's GOT slot); nullopt when there is not exactly one.
using ImportEntryLookup =
    std::function<std::optional<std::pair<uint64_t, Bytes>>(std::string_view nid)>;
// Gives every import hook its entry. Throws for an import the module has no single entry for.
void ResolveImportHooks(Package&, const ImportEntryLookup&);
// Whether every signature of a package holds; `read` reads module bytes at an offset.
bool SignaturesMatch(const Package&,
                     const std::function<bool(uint64_t offset, std::span<std::byte>)>& read);
struct ModuleIdentity {
    std::string title, name, sha256;
    uint64_t base{}, size{};
    std::string executable_sha256;
};

// The four host services every package may import, in SDK operation order.
inline constexpr const char* kSdkImports[] = {"shad_sdk_query", "shad_sdk_clock_ns",
                                               "shad_sdk_counter", "shad_sdk_log"};
// Per hook: `jmp [slot]` at the stub, the relocated original 16 bytes later.
inline constexpr uint64_t kStubStride = 512;

struct PlannedHook {
    Hook hook;
    bool enabled{true};
    // entry: patched instruction; stub: `jmp [slot]`; original: relocated trampoline;
    // replacement: exported handler/adapter the slot points at while enabled.
    uint64_t entry{}, stub{}, original{}, slot{}, replacement{};
    Bytes saved, patch;
    Relocated relocation;
};
struct PlannedPatch {
    CodePatch patch;
    uint64_t address{};
};
struct Plan {
    uint64_t image{}, pool{}, code_size{}, pool_size{};
    // payload at image; stubs (RX) then slots (one page) at pool.
    Bytes payload, stubs, slots;
    std::vector<PlannedHook> hooks;
    std::vector<PlannedPatch> patches;
    std::map<std::string, uint64_t> exports;
};
// Pool bytes needed by a package: stubs for every hook and the SDK, plus one slot page.
std::pair<uint64_t, uint64_t> PoolGeometry(const Package&); // {code_size, pool_size}
// Resolves SDK operation `op` to an address callable from guest code. It may emit
// up to 16 bytes of code into `stub`, which is published at `stub_address`.
using SdkResolver =
    std::function<uint64_t(unsigned op, std::span<std::byte, 16> stub, uint64_t stub_address)>;
// Builds all bytes at their final addresses. Callers validate preimages and
// permissions beforehand; this checks geometry, relocation and overlaps.
Plan BuildPlan(const Package&, const ModuleIdentity&, uint64_t image, uint64_t pool,
               const std::map<std::string, uint64_t>& bindings, const SdkResolver&);

// Module bytes a package changes (hooks, sites, code patches) or calls directly
// (function bindings), as offsets in its module. Two packages conflict when one
// changes bytes the other changes or calls: the later one would find a foreign
// jump instead of its preimage, or call code that no longer is the original.
struct CodeRange {
    uint64_t begin{}, end{};
    bool changes{};
    std::string what;
};
// `planned`: the package's installed hooks, whose relocated instructions can
// extend past the declared preimage.
std::vector<CodeRange> PackageCodeRanges(const Package&, std::span<const PlannedHook> planned = {});
// The first overlap where either side changes the bytes, as "<a> overlaps <b>".
std::optional<std::string> FindCodeOverlap(std::span<const CodeRange> a,
                                           std::span<const CodeRange> b);

// Packages for a game live in <user>/guest_patches/<TITLE_ID>/<name>.json; the per-game
// selection lists file stems in install order. At most kMaxPackages are installed.
inline constexpr size_t kMaxPackages = 8;
std::filesystem::path PackageDirectory(const std::filesystem::path& user_dir,
                                       std::string_view title);
// A selection entry is a bare file stem: no separators, no relative components.
bool ValidPackageName(std::string_view name);
struct PackageInfo {
    std::string name;          // file stem, the value stored in the selection
    std::string id;            // package id
    std::string module;        // target module basename
    std::string module_sha256; // module the package was built for
    std::string display_name;  // package `name`, else the id
    std::string description;   // package `description`, may be empty
    // Other listed packages changing code this one changes or calls: of two selected
    // packages that conflict, only the first is installed.
    std::vector<std::string> conflicts;
};
struct PackageListing {
    std::vector<PackageInfo> packages; // by name
    std::vector<std::string> skipped;  // "<file>: <reason>" for invalid or foreign files
};
PackageListing ListPackages(const std::filesystem::path& directory, std::string_view title);
} // namespace Core::GuestPatch
