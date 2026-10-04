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
#include "core/host_runtime/guest_patch_format.h"

namespace Core {
class Module;
}

namespace Core::GuestPatch::Desktop {
// Packages for a game live in user/guest_patches/<TITLE_ID>/<name>.json (builder
// output `patch.json`, renamed). The per-game setting General.guest_patches lists
// the ones to install, in order (General.guest_patch, a single name, is read when the
// list is empty); SHADPS4_GUEST_PATCH=<path>[;<path>...] overrides both for development.
std::filesystem::path PackageDirectory(std::string_view title);
// Valid packages in the directory whose title matches; other files are skipped.
std::vector<PackageInfo> ListPackages(std::string_view title);

// Called for every module at the end of Module::LoadModuleToMemory, before any
// code of that module can run. Installs every selected package that targets the
// module; a package that does not match the game or conflicts with one installed
// before it is skipped, and the game runs without it.
void OnModuleLoaded(Module& module, bool main_executable);
// DebugBus `guest_patch`: status | enable [package|hook|package/hook] | disable [...].
std::string Command(const std::vector<std::string>& args);

// Installed packages, for the window menu.
struct InstalledPackage {
    std::string id;           // package id
    std::string display_name; // package `name`, else the id
    bool switchable{};        // has hooks or sites (code patches cannot be switched)
    bool enabled{};           // any hook or site currently enabled
};
std::vector<InstalledPackage> Installed();
// Enables or disables every hook and site of an installed package.
void SetEnabled(std::string_view id, bool enabled);
} // namespace Core::GuestPatch::Desktop
