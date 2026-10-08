// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <functional>
#include <memory>

namespace OnlineContent {
// UI lifetime is limited to Big Picture. The portable Foundation owner implements
// networking, plans and downloads; the host supplies paths and credentials.
class Page {
public:
    explicit Page(const std::filesystem::path& executable_directory);
    ~Page();
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;
    void Draw(const std::function<void(const std::filesystem::path&)>& add_library);
private:
    struct State;
    std::unique_ptr<State> state;
};
}
