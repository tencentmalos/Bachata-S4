// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "core/file_sys/fs.h"

namespace Core::Diagnostics::ExecutableExport {

// Files on disk, not relocated/JIT-patched guest memory. The backend leases keep
// the selected mount stack alive without retaining a runtime, VM or CPU owner.
struct Source {
    FileSys::MntPoints::MntPair app;
    std::vector<std::filesystem::path> additional_content;
    std::vector<std::filesystem::path> external_modules;
    std::string title_id, app_version;
    u64 context_id{};
};

// Weak publication: a new `start current` cannot reuse a destroyed session.
// An already accepted export owns its file snapshot and may finish after Stop.
void Publish(const std::shared_ptr<const Source>& source);
void Retire(const std::shared_ptr<const Source>& source);

// One asynchronous job at a time. No filesystem scan/copy on a dumpsys thread.
// start [current|absolute_path] | start path_hex HEX | status [id] | cancel id
std::string Command(const std::vector<std::string>& args);

} // namespace Core::Diagnostics::ExecutableExport
