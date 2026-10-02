// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Desktop backend for guest function packages (tools/guest-functions). The guest
// runs natively here, so the package payload executes as-is: the loader maps it
// next to the target module, writes the planned stubs/entry jumps directly while
// the module is still being loaded, and binds SDK imports to SysV host functions.
// Same package format and planner as the Android FEX runtime (guest_patch.cpp).
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Core {
class Module;
}

namespace Core::GuestPatch::Desktop {
// Packages for a game live in user/guest_patches/<TITLE_ID>/<name>.json (builder
// output `patch.json`, renamed). The per-game setting General.guest_patch names
// the one to install; SHADPS4_GUEST_PATCH=<path> overrides it for development.
std::filesystem::path PackageDirectory(std::string_view title);
struct PackageInfo {
    std::string name;   // file stem, the value stored in General.guest_patch
    std::string id;     // package id
    std::string module; // target module basename
};
// Valid packages in the directory whose title matches; invalid files are skipped.
std::vector<PackageInfo> ListPackages(std::string_view title);

// Called for every module at the end of Module::LoadModuleToMemory, before any
// code of that module can run. Installs at most one package per process.
void OnModuleLoaded(Module& module, bool main_executable);
// DebugBus `guest_patch`: status | enable [name] | disable [name].
std::string Command(const std::vector<std::string>& args);
} // namespace Core::GuestPatch::Desktop
