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
#include "core/host_runtime/guest_patch_format.h"

namespace Core::GuestPatch {
using namespace GuestCpu;
// Package format, Relocate and the install planner are shared with the desktop
// loader; this header adds the Android FEX/VM backend.
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
