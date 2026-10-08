// SPDX-License-Identifier: GPL-2.0-or-later
#include "imgui/online_content/online_content.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <regex>
#include <string>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>
#include <spatial/online_content/content.h>
#include "common/path_util.h"
#include "imgui/imgui_std.h"

namespace OnlineContent {
namespace {
using Json = nlohmann::json;
const Json empty = Json::object();
const Json& Object(const Json& value, const char* key) {
    auto it = value.find(key);
    return it != value.end() && it->is_object() ? *it : empty;
}
std::string String(const Json& value, const char* key) {
    auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
std::string Path(const std::filesystem::path& value) {
    return Common::FS::PathToUTF8String(value);
}
std::string Error(const std::string& code) {
    if (code.empty()) return {};
    if (code == "auth_required" || code == "auth_expired_or_restricted" || code == "account_changed")
        return "Sign in to the Baidu account used by this download.";
    if (code == "auth_check_failed" || code == "baidu_network_error")
        return "Cannot reach Baidu. Check the connection and retry.";
    if (code.starts_with("worker_") || code == "baidu_helper_unavailable")
        return "The Baidu component could not be loaded. Check the application installation.";
    if (code == "archive_tool_missing")
        return "The archive tool is missing. Install the complete application and retry.";
    if (code == "archive_password_parts_or_corruption")
        return "Check all archive volumes, or enter an archive password before resuming.";
    if (code == "baidu_worker_busy") return "Wait for the current Baidu operation, or pause it.";
    if (code == "share_password_required" || code == "share_password_or_verification_required")
        return "Check the share extraction code or complete verification on the Baidu website.";
    if (code == "insufficient_disk_space") return "Free disk space before continuing.";
    if (code == "paused") return "Paused. Saved files and verified download ranges are retained.";
    if (code == "task_already_exists") return "This selection already exists in Downloads.";
    return code;
}
std::string Bytes(const Json& value) {
    double count = 0;
    try {
        if (value.is_string()) count = std::stod(value.get<std::string>());
        else if (value.is_number()) count = value.get<double>();
    } catch (...) {}
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    while (count >= 1024 && unit < 4) { count /= 1024; ++unit; }
    char text[64];
    SDL_snprintf(text, sizeof(text), "%.1f %s", count, units[unit]);
    return text;
}
void Text(const std::string& text) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
}
void OpenFolder(const std::filesystem::path& path) {
    const auto bytes = Path(std::filesystem::absolute(path));
    std::string url = "file://";
#ifdef _WIN32
    url += '/';
#endif
    for (unsigned char c : bytes) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '/' || c == ':' || c == '-' || c == '_' || c == '.' || c == '~') url += c;
        else {
            char escaped[4]; SDL_snprintf(escaped, sizeof(escaped), "%%%02X", c); url += escaped;
        }
    }
    SDL_OpenURL(url.c_str());
}
}
struct Page::State {
    std::filesystem::path root;
    std::filesystem::path login_helper;
    void* owner = nullptr;
    SDL_Process* login = nullptr;
    Json snapshot = Json::object();
    Json plan = Json::object();
    std::map<std::string, bool> selection;
    std::string link, title, cookies, archive_password;
    std::string message, remove_id;
    uint64_t request = 0;
    bool pending_plan = false, show_plan = false, downloads = false;
    std::chrono::steady_clock::time_point next_poll{};
    bool Send(Json command) {
        if (owner && foundation_content_command(owner, command.dump().c_str())) return true;
        message = "The online service is unavailable. Close and reopen shadPS4 to retry.";
        return false;
    }
    void Poll() {
        if (login) {
            int exit_code = 0;
            if (SDL_WaitProcess(login, false, &exit_code)) {
                SDL_DestroyProcess(login); login = nullptr;
                Send({{"op", "auth_status"}});
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (!owner || now < next_poll) return;
        next_poll = now + std::chrono::milliseconds(250);
        char* raw = foundation_content_snapshot(owner);
        auto value = Json::parse(raw ? raw : "{}", nullptr, false);
        foundation_content_string_free(raw);
        if (value.is_object()) snapshot = std::move(value);
        const auto& baidu = Object(snapshot, "baidu");
        const auto& result = Object(baidu, "plan");
        if (pending_plan && result.value("request_id", uint64_t{}) == request &&
            !result.value("busy", false)) {
            pending_plan = false;
            const auto error = String(result, "error");
            if (!error.empty()) message = Error(error);
            else if (!String(result, "id").empty()) {
                plan = result; selection.clear();
                for (const auto& file : plan.value("files", Json::array()))
                    selection[String(file, "id")] = true;
                show_plan = true;
            }
        }
        if (pending_plan && !baidu.value("busy", false) && !String(baidu, "error").empty()) {
            pending_plan = false; message = Error(String(baidu, "error"));
        }
    }
    void Login() {
        if (login) return;
        const auto executable = Path(login_helper);
        const auto destination = Path(root / "account.json");
        const char* args[] = {executable.c_str(), destination.c_str(), nullptr};
        login = SDL_CreateProcess(args, false);
        if (!login) message = "Could not open Baidu sign-in: " + std::string(SDL_GetError());
    }
};
Page::Page(const std::filesystem::path& bin) : state(std::make_unique<State>()) {
    auto& s = *state;
    s.root = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "online_content" / "ps4";
#ifdef _WIN32
    const auto worker = bin / "foundation_baidu.dll";
    const auto archive = bin / "7zz.exe";
    s.login_helper = bin / "shadps4-baidu-login.exe";
#elif defined(__APPLE__)
    const auto worker = bin / "libfoundation_baidu.dylib";
    const auto archive = bin / "7zz";
    s.login_helper = bin / "shadps4-baidu-login";
#else
    const auto worker = bin / "libfoundation_baidu.so";
    const auto archive = bin / "7zz";
    s.login_helper = bin / "shadps4-baidu-login";
#endif
    Json config{{"root", Path(s.root)}, {"worker_library", Path(worker)},
                {"credentials_file", Path(s.root / "account.json")}, {"platform", "ps4"},
                {"cloud_root", "/ShadPS4OnlineContent"},
                {"source", {{"base_url", "https://2468c.com"}, {"category", 0}}}};
    if (std::filesystem::is_regular_file(archive)) config["archive_tool"] = Path(archive);
    s.owner = foundation_content_open(config.dump().c_str());
    if (s.owner) s.Send({{"op", "auth_status"}});
    else s.message = "Could not open the PS4 online-content store. Another instance may be using its data directory.";
}
Page::~Page() {
    if (state->login) {
        SDL_KillProcess(state->login, true);
        SDL_WaitProcess(state->login, true, nullptr);
        SDL_DestroyProcess(state->login);
    }
    if (state->owner) foundation_content_close(state->owner);
}
void Page::Draw(const std::function<void(const std::filesystem::path&)>& add_library) {
    auto& s = *state;
    s.Poll();
    const auto& baidu = Object(s.snapshot, "baidu");
    const auto& auth = Object(baidu, "auth");
    const bool busy = baidu.value("busy", false) || auth.value("busy", false) || s.login;
    const bool connected = auth.value("authorized", false);
    ImGui::TextUnformatted("Baidu Online  /  PS4 & PSVR");
    ImGui::SameLine();
    ImGui::TextColored(connected ? ImVec4(0.35f, 0.9f, 0.65f, 1) : ImVec4(0.95f, 0.72f, 0.35f, 1),
                       "%s", s.login ? "Sign-in window open" : busy ? "Connecting..." : connected ? "Connected" : "Not connected");
    ImGui::Spacing();
    ImGui::BeginDisabled(busy || !s.owner);
    if (std::filesystem::is_regular_file(s.login_helper)) {
        if (ImGui::Button(connected ? "Change Baidu account" : "Sign in to Baidu")) s.Login();
        ImGui::SameLine();
    }
    if (ImGui::Button("Check connection")) s.Send({{"op", "auth_status"}});
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open downloads folder")) OpenFolder(s.root);
    const auto auth_error = Error(String(auth, "error"));
    if (!connected && !busy && !auth_error.empty()) Text(auth_error);
    ImGui::Separator();
    if (ImGui::Selectable("Add share", !s.downloads, 0, ImVec2(180, 0))) s.downloads = false;
    ImGui::SameLine();
    if (ImGui::Selectable("Downloads", s.downloads, 0, ImVec2(180, 0))) s.downloads = true;
    ImGui::Spacing();
    if (!s.message.empty()) {
        Text(s.message);
        if (ImGui::SmallButton("Dismiss")) s.message.clear();
        ImGui::Separator();
    }
    const auto error = Error(String(baidu, "error"));
    if (!error.empty()) Text(error);
    if (!s.downloads) {
        ImGui::TextUnformatted("Add a PS4 or PSVR Baidu share");
        Text("Review files before downloading. PSVR packages use the PS4 format; game compatibility is checked separately.");
        ImGui::SetNextItemWidth(-160);
        ImGui::InputTextWithHint("Share link", "https://pan.baidu.com/s/1...?pwd=xxxx", &s.link);
        ImGui::SetNextItemWidth(-160);
        ImGui::InputTextWithHint("Name", "Optional title for this download", &s.title);
        ImGui::BeginDisabled(!connected || busy || s.pending_plan || s.link.empty());
        if (ImGui::Button(s.pending_plan ? "Reading share..." : "Review files", ImVec2(220, 0))) {
            static const std::regex valid_share(R"(^https://pan\.baidu\.com/s/[A-Za-z0-9_-]+(\?pwd=[A-Za-z0-9]{4})?$)");
            if (s.link.size() > 2048 || !std::regex_match(s.link, valid_share)) {
                s.message = "Enter a valid HTTPS pan.baidu.com/s/ share URL. Include its extraction code as ?pwd=xxxx.";
            } else {
                s.message.clear();
                s.pending_plan = s.Send({{"op", "plan_share"}, {"link", s.link},
                    {"title", s.title.empty() ? "PS4 Baidu share" : s.title}, {"request_id", ++s.request}});
            }
        }
        ImGui::EndDisabled();
        if (!connected) Text("Sign in to Baidu to list the files in your share.");
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Use existing session cookies")) {
            Text("For hosts without a sign-in helper. Cookies are used for this session only.");
            ImGui::SetNextItemWidth(-160);
            ImGui::InputText("BDUSS / STOKEN", &s.cookies, ImGuiInputTextFlags_Password);
            ImGui::BeginDisabled(busy || s.cookies.empty());
            if (ImGui::Button("Connect session")) {
                s.Send({{"op", "set_credentials"}, {"cookies", s.cookies}});
                std::fill(s.cookies.begin(), s.cookies.end(), '\0'); s.cookies.clear();
                s.Send({{"op", "auth_status"}});
            }
            ImGui::EndDisabled();
        }
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        ImGui::TextUnformatted("PS4 / PSVR catalog");
        Text("The linked PS4 site currently returns HTTP 403. Its category list has not been verified, so catalog browsing is unavailable. You can still add a Baidu share above.");
        if (ImGui::Button("Open PS4 website")) SDL_OpenURL("https://2468c.com");
    } else {
        const auto tasks = s.snapshot.value("tasks", Json::array());
        if (tasks.empty()) {
            Text("No downloads yet.");
            Text("Add a Baidu share, review its files and confirm the download to create a task.");
        }
        ImGui::SetNextItemWidth(160);
        int count = baidu.value("connections", 8);
        const char* labels[] = {"4 connections", "8 connections", "12 connections", "16 connections", "20 connections"};
        int index = std::clamp(count / 4 - 1, 0, 4);
        ImGui::BeginDisabled(busy);
        if (ImGui::Combo("Download connections", &index, labels, 5))
            s.Send({{"op", "download_connections"}, {"connections", (index + 1) * 4}});
        ImGui::EndDisabled();
        ImGui::InputText("Archive password (optional)", &s.archive_password, ImGuiInputTextFlags_Password);
        for (const auto& task : tasks) {
            const auto id = String(task, "id");
            const auto status = String(task, "state");
            ImGui::PushID(id.c_str());
            ImGui::Separator();
            Text(String(task, "title"));
            ImGui::Text("%s  /  %s", status.c_str(), String(task, "phase").c_str());
            const auto& progress = Object(task, "progress");
            if (progress.contains("done") && progress.contains("total")) {
                Text(Bytes(progress["done"]) + " / " + Bytes(progress["total"]));
            }
            Text(Error(String(task, "error")));
            if (status == "running" || status == "queued") {
                if (ImGui::Button("Pause")) s.Send({{"op", "pause"}, {"id", id}});
                ImGui::SameLine();
                if (ImGui::Button("Cancel download")) s.Send({{"op", "cancel"}, {"id", id}});
            } else {
                ImGui::BeginDisabled(busy);
                if (status != "completed" && ImGui::Button("Resume / retry")) {
                    s.Send({{"op", "resume"}, {"id", id}, {"password", s.archive_password}});
                    std::fill(s.archive_password.begin(), s.archive_password.end(), '\0'); s.archive_password.clear();
                }
                if (status == "completed") {
                    const auto directory = String(Object(task, "result"), "directory");
                    if (!directory.empty() && ImGui::Button("Add prepared PKGs to library")) {
                        add_library(std::filesystem::u8path(directory));
                        s.message = "Download folder added to the game library. Only recognized PS4 packages appear there.";
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete local files")) s.remove_id = id;
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
    }
    if (s.show_plan) { ImGui::OpenPopup("Review Baidu files"); s.show_plan = false; }
    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Review Baidu files", nullptr)) {
        Text("Select all volumes of each archive. Confirming copies the selection into " + String(s.plan, "cloud_path") + " in your Baidu account and downloads it to shadPS4 storage.");
        if (ImGui::Button("Select all")) for (auto& [id, checked] : s.selection) checked = true;
        ImGui::SameLine();
        if (ImGui::Button("Select none")) for (auto& [id, checked] : s.selection) checked = false;
        ImGui::BeginChild("Files", ImVec2(0, -100), ImGuiChildFlags_Borders);
        for (const auto& file : s.plan.value("files", Json::array())) {
            const auto id = String(file, "id");
            ImGui::PushID(id.c_str());
            ImGui::Checkbox((String(file, "path") + "  (" + Bytes(file.value("size", Json{})) + ")").c_str(), &s.selection[id]);
            ImGui::PopID();
        }
        ImGui::EndChild();
        Json selected = Json::array();
        for (const auto& [id, checked] : s.selection) if (checked) selected.push_back(id);
        ImGui::BeginDisabled(selected.empty() || busy);
        if (ImGui::Button("Transfer and download")) {
            s.Send({{"op", "enqueue_plan"}, {"id", String(s.plan, "id")}, {"selected", selected}});
            s.downloads = true; ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::Button("Back")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!s.remove_id.empty()) ImGui::OpenPopup("Delete local download?");
    if (ImGui::BeginPopupModal("Delete local download?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Text("Delete this task and its local downloads and prepared files? Baidu cloud copies remain.");
        if (ImGui::Button("Delete")) {
            s.Send({{"op", "remove"}, {"id", s.remove_id}});
            s.remove_id.clear(); ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep files")) { s.remove_id.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}
}
