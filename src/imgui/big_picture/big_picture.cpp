//  SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <stb_image.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/file_sys/ifile.h"
#include "core/guest_patch_desktop.h"
#include "emulator.h"
#include "imgui/big_picture/big_picture.h"
#include "imgui/big_picture/imgui_impl_sdl3_big_picture.h"
#include "imgui/big_picture/imgui_impl_sdlrenderer3.h"
#include "imgui/big_picture/settings_dialog_imgui.h"
#include "imgui/imgui_std.h"
#include "imgui/renderer/font_stack.h"
#include "sdl_window.h"

namespace BigPictureMode {

constexpr float gameImageSize = 200.f;

bool done = false;
bool showSettings = false;

std::filesystem::path runEbootPath = "";
std::vector<IconInfo> gameIcons = {};

float uiScale = 1.0f;
SDL_Renderer* renderer;

namespace {

std::filesystem::path UpdateChecker(const std::string sceItem, std::filesystem::path game_folder) {
    std::filesystem::path updatedPath = "";
    std::filesystem::path basePath = game_folder.parent_path();
    std::string fileName;
    std::string item = "sce_sys/" + sceItem;

    if (Core::FileSys::IsZArchiveFile(game_folder)) {
        fileName = Core::FileSys::StripZArchiveExtension(game_folder).filename().string();
    } else {
        fileName = game_folder.filename().string();
    }

    if (std::filesystem::exists(basePath / (fileName + "-UPDATE") / item)) {
        updatedPath = basePath / (fileName + "-UPDATE") / item;
    } else if (Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-UPDATE.zar"), item)
                   .has_value()) {
        updatedPath =
            Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-UPDATE.zar"), item).value();
    } else if (std::filesystem::exists(basePath / (fileName + "-patch") / item)) {
        updatedPath = basePath / (fileName + "-patch") / item;
    } else if (Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-patch.zar"), item)
                   .has_value()) {
        updatedPath =
            Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-patch.zar"), item).value();
    } else if (Core::FileSys::ResolveGameFilePath(game_folder, item).has_value()) {
        updatedPath = Core::FileSys::ResolveGameFilePath(game_folder, item).value();
    }

    return updatedPath;
}

// Launch options: the desktop counterpart of the Android Launch sheet. Choosing a
// game opens it; the draft is written to that game's custom config only on Launch,
// so Back leaves every setting untouched.
constexpr float kScaleChoices[] = {25.f, 37.5f, 50.f, 75.f, 100.f};
constexpr const char* kScaleLabels[] = {"0.25", "0.375", "0.5", "0.75", "1.0"};

// One guest function package in user/guest_patches/<TITLE_ID>/.
struct PatchChoice {
    std::string name;  // file stem, the value stored in General.guest_patches
    std::string label; // package name, else its id
    std::string description;
    std::string module;
    std::vector<std::string> conflicts; // file stems of packages changing the same code
    int fits = 0; // 1 built for the module the game loads, -1 for another, 0 not readable
    bool missing = false; // selected, but the file is gone
};

struct LaunchDraft {
    int game = -1; // index into gameIcons
    bool ignorePatches = false;
    float scale = 100.f;
    bool fsr = false;
    bool rcas = true;
    // Guest function packages (General.guest_patches), in install order (the order
    // of `patchChoices`, by file stem).
    std::vector<std::string> patches;
    std::vector<PatchChoice> patchChoices;
    std::string error;
};
LaunchDraft launchDraft;
bool openLaunchOptions = false;

std::filesystem::path GameConfigPath(const std::string& serial) {
    return Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) / (serial + ".json");
}

// SHA-256 of the module a launch would load, read through the same mount stack as the
// game (mods, the update unless ignored, then the game; loose files or archives).
// Cached for the launcher session; empty when the module is missing or unreadable.
std::string EffectiveModuleSha(const IconInfo& game, bool ignorePatches, const std::string& module) {
    namespace vfs = Core::FileSys;
    const auto base =
        vfs::IsZArchiveFile(game.ebootPath) ? game.ebootPath : game.ebootPath.parent_path();
    const auto key = base.string() + (ignorePatches ? "|base|" : "|mounted|") + module;
    static std::map<std::string, std::string> cache;
    if (const auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    std::string sha;
    try {
        // The launcher is single threaded: the flag is set only for this mount.
        const bool previous = vfs::MntPoints::ignore_game_patches;
        vfs::MntPoints::ignore_game_patches = ignorePatches;
        vfs::MntPoints mount;
        try {
            mount.Mount(base, "/app0", true);
        } catch (...) {
            vfs::MntPoints::ignore_game_patches = previous;
            throw;
        }
        vfs::MntPoints::ignore_game_patches = previous;
        const auto path = module == "eboot.bin" ? "/app0/eboot.bin" : "/app0/sce_module/" + module;
        if (auto file = mount.Open(path)) {
            sha = Core::GuestPatch::StreamSha256(
                [&](void* dst, std::uint64_t size) -> std::int64_t { return file->Read(dst, size); });
        }
    } catch (const std::exception& e) {
        LOG_WARNING(ImGui, "Cannot read {} of {}: {}", module, base.string(), e.what());
        sha.clear();
    }
    cache.emplace(key, sha);
    return sha;
}

bool PatchSelected(const std::string& name) {
    return std::ranges::find(launchDraft.patches, name) != launchDraft.patches.end();
}

const PatchChoice* FindPatchChoice(const std::string& name) {
    const auto it = std::ranges::find(launchDraft.patchChoices, name, &PatchChoice::name);
    return it == launchDraft.patchChoices.end() ? nullptr : &*it;
}

// Why a package cannot be added to the current selection; empty when it can.
std::string PatchBlocker(const PatchChoice& choice) {
    if (choice.missing) {
        return "File not found";
    }
    if (choice.fits < 0) {
        return "Built for another version of " + choice.module;
    }
    for (const auto& other : choice.conflicts) {
        if (PatchSelected(other)) {
            const auto* conflict = FindPatchChoice(other);
            return "Changes the same code as " + (conflict ? conflict->label : other);
        }
    }
    return {};
}

void SetPatchSelected(const std::string& name, bool on) {
    std::erase(launchDraft.patches, name);
    if (on) {
        launchDraft.patches.push_back(name);
    }
    // Keep the panel's order, which is the install order.
    std::ranges::stable_sort(launchDraft.patches, {}, [](const std::string& n) {
        const auto it = std::ranges::find(launchDraft.patchChoices, n, &PatchChoice::name);
        return std::distance(launchDraft.patchChoices.begin(), it);
    });
}

// Effective values for one game without loading its profile into the launcher's own
// settings: the global value, replaced by whatever the game's custom config sets.
void OpenLaunchOptions(int game, bool ignorePatches) {
    launchDraft = {};
    launchDraft.game = game;
    launchDraft.ignorePatches = ignorePatches;
    launchDraft.scale = EmulatorSettings.GetConfiguredInternalScalePercent();
    launchDraft.fsr = EmulatorSettings.IsFsrEnabled();
    launchDraft.rcas = EmulatorSettings.IsRcasEnabled();
    // Same precedence as the loader: the list, else the single legacy name.
    auto patches = EmulatorSettings.GetGuestPatches();
    auto legacyPatch = EmulatorSettings.GetGuestPatch();
    try {
        if (std::ifstream in{GameConfigPath(gameIcons[game].serial)}; in) {
            const auto j = nlohmann::json::parse(in);
            if (j.contains("GPU")) {
                const auto& gpu = j.at("GPU");
                launchDraft.scale = gpu.value("internal_scale_percent", launchDraft.scale);
                launchDraft.fsr = gpu.value("fsr_enabled", launchDraft.fsr);
                launchDraft.rcas = gpu.value("rcas_enabled", launchDraft.rcas);
            }
            if (j.contains("General")) {
                const auto& general = j.at("General");
                patches = general.value("guest_patches", patches);
                legacyPatch = general.value("guest_patch", legacyPatch);
            }
        }
    } catch (const std::exception& e) {
        launchDraft.error = std::string("Cannot read this game's settings: ") + e.what();
    }
    if (patches.empty() && !legacyPatch.empty()) {
        patches.push_back(legacyPatch);
    }
    // Same normalization the renderer applies (EmulatorSettings::GetInternalScalePercent).
    if (std::ranges::find(kScaleChoices, launchDraft.scale) == std::end(kScaleChoices)) {
        launchDraft.scale = 100.f;
    }
    const auto& icon = gameIcons[game];
    std::map<std::string, std::string> moduleSha; // module -> SHA-256 of the file a launch loads
    for (const auto& package : Core::GuestPatch::Desktop::ListPackages(icon.serial)) {
        PatchChoice choice{package.name, package.display_name, package.description,
                           package.module, package.conflicts};
        auto [it, inserted] = moduleSha.try_emplace(package.module);
        if (inserted) {
            it->second = EffectiveModuleSha(icon, ignorePatches, package.module);
        }
        // A package naming its build by located signatures (no file SHA) is checked against
        // the loaded image at launch: unknown here.
        if (!it->second.empty() && !package.module_sha256.empty()) {
            choice.fits = it->second == package.module_sha256 ? 1 : -1;
        }
        launchDraft.patchChoices.push_back(std::move(choice));
    }
    // Keep selected packages whose file is gone visible, so they can be switched off.
    for (const auto& name : patches) {
        if (!FindPatchChoice(name)) {
            PatchChoice missing{.name = name, .label = name, .missing = true};
            launchDraft.patchChoices.push_back(std::move(missing));
        }
        if (!PatchSelected(name)) {
            SetPatchSelected(name, true);
        }
    }
    openLaunchOptions = true;
}

// Merges only these keys into the game's custom config; other per-game and global
// settings keep their current values and keep following later global changes.
bool SaveLaunchDraft() {
    const auto path = GameConfigPath(gameIcons[launchDraft.game].serial);
    try {
        nlohmann::json j = nlohmann::json::object();
        if (std::ifstream in{path}; in) {
            j = nlohmann::json::parse(in);
        }
        auto& gpu = j["GPU"];
        gpu["internal_scale_percent"] = launchDraft.scale;
        gpu["fsr_enabled"] = launchDraft.fsr;
        gpu["rcas_enabled"] = launchDraft.rcas;
        // The list replaces the single legacy name, which is cleared so it cannot come back.
        j["General"]["guest_patches"] = launchDraft.patches;
        j["General"]["guest_patch"] = "";
        std::filesystem::create_directories(path.parent_path());
        auto temp = path;
        temp += ".tmp";
        {
            std::ofstream out{temp, std::ios::trunc};
            out << j.dump(4) << '\n';
            if (!out.flush()) {
                throw std::runtime_error("write failed");
            }
        }
        std::filesystem::rename(temp, path);
        return true;
    } catch (const std::exception& e) {
        launchDraft.error = "Cannot save this game's settings: " + std::string(e.what());
        LOG_ERROR(ImGui, "Launch options for {}: {}", path.string(), launchDraft.error);
        return false;
    }
}

// "Guest Patches" row of Launch Options: a summary button opening a panel where the
// game's packages are switched on and off. Called inside the Launch Options popup, so
// the panel is a nested modal.
void DrawPatchRow(float labelWidth, float minWidth) {
    const auto& choices = launchDraft.patchChoices;
    const auto& serial = gameIcons[launchDraft.game].serial;
    const ImVec4 warn(1.f, 0.7f, 0.3f, 1.f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Guest Patches");
    ImGui::SameLine(labelWidth);
    if (choices.empty()) {
        ImGui::TextDisabled(
            "None in %s",
            Core::GuestPatch::Desktop::PackageDirectory(serial).string().c_str());
        return;
    }
    std::string summary;
    for (const auto& name : launchDraft.patches) {
        const auto* choice = FindPatchChoice(name);
        summary += (summary.empty() ? "" : ", ") + (choice ? choice->label : name);
    }
    summary = std::to_string(launchDraft.patches.size()) + " of " +
              std::to_string(choices.size()) + (summary.empty() ? "" : ": " + summary);
    const float fit = ImGui::CalcTextSize(summary.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.f;
    if (ImGui::Button((summary + "###patch_summary").c_str(), ImVec2(std::max(minWidth, fit), 0.f))) {
        ImGui::OpenPopup("Guest Patches");
    }
    // Selected packages the loader will skip.
    for (const auto& name : launchDraft.patches) {
        const auto* choice = FindPatchChoice(name);
        if (const auto blocker = choice ? PatchBlocker(*choice) : std::string(); !blocker.empty()) {
            ImGui::TextColored(warn, "%s: %s", choice->label.c_str(), blocker.c_str());
        }
    }

    // Centered every frame: the list's height is only known a frame after appearing.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Guest Patches", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextDisabled("%s", Core::GuestPatch::Desktop::PackageDirectory(serial).string().c_str());
    ImGui::Separator();
    const auto& style = ImGui::GetStyle();
    const float textIndent = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;
    const float wrapWidth = 560.f * uiScale;
    // The list scrolls inside the panel, so Done/None stay on screen.
    const float listWidth = textIndent + wrapWidth + style.ScrollbarSize + style.WindowPadding.x * 2.f;
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(listWidth, 0.f), ImVec2(listWidth, ImGui::GetMainViewport()->WorkSize.y * 0.65f));
    ImGui::BeginChild("packages", ImVec2(listWidth, 0.f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
    for (std::size_t i = 0; i < choices.size(); ++i) {
        const auto& choice = choices[i];
        ImGui::PushID(static_cast<int>(i));
        bool on = PatchSelected(choice.name);
        const auto blocker = PatchBlocker(choice);
        // A selected package can always be switched off.
        ImGui::BeginDisabled(!on && !blocker.empty());
        if (ImGui::Checkbox(choice.label.c_str(), &on)) {
            SetPatchSelected(choice.name, on);
        }
        ImGui::EndDisabled();
        if (i == 0 && ImGui::IsWindowAppearing()) {
            ImGui::SetItemDefaultFocus();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", choice.name.c_str());
        ImGui::Indent(textIndent);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
        if (!choice.description.empty()) {
            ImGui::TextUnformatted(choice.description.c_str());
        }
        if (!blocker.empty()) {
            ImGui::TextColored(warn, "%s", blocker.c_str());
        } else if (choice.fits > 0) {
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.f), "Built for this game's %s",
                               choice.module.c_str());
        } else {
            ImGui::TextDisabled("Patches %s; could not read it here, checked at launch",
                                choice.module.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::Unindent(textIndent);
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::Separator();
    const ImVec2 buttonSize(160.f * uiScale, 0.f);
    if (ImGui::Button("Done", buttonSize) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("None", buttonSize)) {
        launchDraft.patches.clear();
    }
    ImGui::EndPopup();
}

void DrawLaunchOptions() {
    if (openLaunchOptions) {
        ImGui::OpenPopup("Launch Options");
        openLaunchOptions = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Launch Options", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    const auto& game = gameIcons[launchDraft.game];
    // Back keys belong to the patch panel while it is open (it opened before this frame).
    const bool patchPanelOpen = ImGui::IsPopupOpen("Guest Patches");
    ImGui::TextUnformatted(game.title.c_str());
    ImGui::TextDisabled("%s", game.serial.c_str());
    ImGui::Separator();

    const float labelWidth = 300.f * uiScale;
    const ImVec2 choiceSize(130.f * uiScale, 0.f);
    // One horizontal row of choices; the selected one uses the header colour.
    const auto row = [&](const char* name, int count, auto label, auto selected, auto select,
                         bool enabled) {
        ImGui::PushID(name);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name);
        ImGui::BeginDisabled(!enabled);
        for (int c = 0; c < count; ++c) {
            ImGui::SameLine(c == 0 ? labelWidth : 0.f);
            const bool on = selected(c);
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
            }
            ImGui::PushID(c);
            const float fit =
                ImGui::CalcTextSize(label(c)).x + ImGui::GetStyle().FramePadding.x * 2.f;
            if (ImGui::Button(label(c), ImVec2(std::max(choiceSize.x, fit), 0.f))) {
                select(c);
            }
            ImGui::PopID();
            if (on) {
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    };
    static constexpr const char* kOffOn[] = {"Off", "On"};
    static constexpr const char* kUpscalers[] = {"Off", "FSR1"};
    row(
        "Render Scale", int(std::size(kScaleChoices)), [](int c) { return kScaleLabels[c]; },
        [](int c) { return launchDraft.scale == kScaleChoices[c]; },
        [](int c) { launchDraft.scale = kScaleChoices[c]; }, true);
    row(
        "Screen Upscaler", 2, [](int c) { return kUpscalers[c]; },
        [](int c) { return launchDraft.fsr == (c == 1); },
        [](int c) { launchDraft.fsr = c == 1; }, true);
    row(
        "FSR Sharpening", 2, [](int c) { return kOffOn[c]; },
        [](int c) { return launchDraft.rcas == (c == 1); },
        [](int c) { launchDraft.rcas = c == 1; }, launchDraft.fsr);
    DrawPatchRow(labelWidth, choiceSize.x);

    ImGui::Separator();
    ImGui::TextDisabled("FSR runs only while the game image is smaller than the window.");
    ImGui::TextDisabled("Saved for this game when you launch.");
    if (!launchDraft.error.empty()) {
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", launchDraft.error.c_str());
    }

    const ImVec2 buttonSize(200.f * uiScale, 0.f);
    if (ImGui::Button("Launch", buttonSize) && SaveLaunchDraft()) {
        Core::FileSys::MntPoints::ignore_game_patches = launchDraft.ignorePatches;
        runEbootPath = game.ebootPath;
        done = true;
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetItemDefaultFocus();
    }
    ImGui::SameLine();
    if (ImGui::Button("Back", buttonSize) ||
        (!patchPanelOpen && (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                             ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)))) {
        launchDraft.game = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void SetGameIcons(std::vector<IconInfo>& gameIcons) {
    ImGuiStyle& style = ImGui::GetStyle();
    const float maxAvailableWidth = ImGui::GetContentRegionAvail().x;
    const float itemSpacing = style.ItemSpacing.x; // already scaled
    const float padding = 10.0f * uiScale;
    float rowContentWidth = gameImageSize * uiScale + itemSpacing;

    for (int i = 0; i < gameIcons.size(); i++) {
        ImGui::BeginGroup();
        std::string ButtonName = "Button" + std::to_string(i);
        const char* ButtonNameChar = ButtonName.c_str();

        bool buttonFocused = (ImGui::GetID(ButtonNameChar) == ImGui::GetFocusID());
        if (buttonFocused) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
        }

        ImTextureID id = gameIcons[i].textureId;
        const ImVec2 iconSize(gameImageSize * uiScale, gameImageSize * uiScale);
        // A game without icon0.png still gets a selectable tile of the same size.
        const bool pressed = id != nullptr
                                 ? ImGui::ImageButton(ButtonNameChar, id, iconSize)
                                 : ImGui::Button((std::string("No Icon##") + ButtonName).c_str(),
                                                 iconSize);
        if (pressed) {
            OpenLaunchOptions(i, ImGui::IsKeyDown(ImGuiKey::ImGuiKey_LeftCtrl));
        }

        if (buttonFocused) {
            ImGui::PopStyleColor();
        }

        // Scroll to item only when newly-focused
        if (ImGui::IsItemFocused() && !gameIcons[i].focusState) {
            ImGui::SetScrollHereY(0.5f);
        }

        if (ImGui::IsWindowFocused()) {
            gameIcons[i].focusState = ImGui::IsItemFocused();
        }

        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + gameImageSize * uiScale);
        ImGui::TextWrapped("%s", gameIcons[i].title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        // Use same line if content fits horizontally, move to next line if not
        rowContentWidth += (gameImageSize * uiScale + itemSpacing * 2 + padding);
        if (rowContentWidth < maxAvailableWidth) {
            ImGui::SameLine(0.0f, padding);
        } else {
            ImGui::Dummy(ImVec2(0.0f, padding));
            rowContentWidth = gameImageSize * uiScale + itemSpacing;
        }
    }
}

} // namespace

SDL_Texture* LoadSdlTextureData(std::vector<u8> data) {
    int image_width = 0;
    int image_height = 0;
    int channels = 4;
    unsigned char* image_data = stbi_load_from_memory(
        (const unsigned char*)data.data(), (int)data.size(), &image_width, &image_height, NULL, 4);
    if (image_data == nullptr) {
        LOG_ERROR(ImGui, "Failed to load image: {}", stbi_failure_reason());
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(image_width, image_height, SDL_PIXELFORMAT_RGBA32,
                                                 (void*)image_data, channels * image_width);
    if (surface == nullptr) {
        LOG_ERROR(ImGui, "Unable to create SDL surface: {}", SDL_GetError());
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture == nullptr) {
        LOG_ERROR(ImGui, "Unable to create SDL texture: {}", SDL_GetError());
    }

    SDL_DestroySurface(surface);
    stbi_image_free(image_data);

    return texture;
}

SDL_Texture* LoadSdlTextureDataFromFile(std::filesystem::path filePath) {
    std::ifstream file(filePath, std::ios::binary);
    std::vector<u8> data =
        std::vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return LoadSdlTextureData(data);
}

void GetGameIconInfo(std::vector<IconInfo>& icons) {
    icons.clear();

    for (const auto& installLoc : EmulatorSettings.GetAllGameInstallDirs()) {
        if (installLoc.enabled && std::filesystem::exists(installLoc.path)) {
            for (const auto& entry : std::filesystem::directory_iterator(installLoc.path)) {

                std::string pathstring = entry.path().filename().string();
                if (pathstring.ends_with("-UPDATE") || pathstring.ends_with("-patch") ||
                    (!entry.is_directory() && !Core::FileSys::IsZArchiveFile(entry))) {
                    continue;
                }

                if (Core::FileSys::IsZArchiveFile(entry)) {
                    size_t start = pathstring.length() - 3;
                    for (size_t i = start; i < pathstring.length(); ++i) {
                        pathstring[i] = static_cast<char>(
                            std::tolower(static_cast<unsigned char>(pathstring[i])));
                    }

                    if (pathstring.ends_with("-UPDATE.zar") || pathstring.ends_with("-patch.zar")) {
                        continue;
                    }
                }

                IconInfo icon;
                PSF psf;
                if (Core::FileSys::IsZArchiveFile(entry.path())) {
                    // Same metadata view the runtime mounts: handles bundled app/ + update
                    // archives and -UPDATE/-patch siblings, which a root sce_sys lookup misses.
                    Core::FileSys::ArchiveInstallMetadata metadata;
                    try {
                        metadata = Core::FileSys::InspectArchiveInstall(entry.path());
                    } catch (const std::exception& e) {
                        LOG_WARNING(ImGui, "Skipping {}: {}", entry.path().string(), e.what());
                        continue;
                    }
                    if (!psf.Open(metadata.param_sfo)) {
                        continue;
                    }
                    icon.title = psf.GetString("TITLE").value_or("");
                    icon.serial = psf.GetString("TITLE_ID").value_or("");
                    icon.textureId =
                        metadata.icon_png.empty()
                            ? ImTextureID{}
                            : ImTextureID(LoadSdlTextureData(std::move(metadata.icon_png)));
                    icon.ebootPath = entry.path();
                    icon.focusState = false;
                    icons.push_back(icon);
                    continue;
                }

                const std::string sfoFileName = "param.sfo";
                std::filesystem::path sfoPath = UpdateChecker(sfoFileName, entry.path());

                if (std::filesystem::exists(sfoPath) && psf.Open(sfoPath)) {
                    if (const auto title = psf.GetString("TITLE"); title.has_value()) {
                        icon.title = *title;
                    }

                    if (const auto title_id = psf.GetString("TITLE_ID"); title_id.has_value()) {
                        icon.serial = *title_id;
                    }
                } else {
                    continue;
                }

                const std::string iconFileName = "icon0.png";
                std::filesystem::path iconPath = UpdateChecker(iconFileName, entry.path());

                SDL_Texture* texture = LoadSdlTextureDataFromFile(iconPath);
                icon.textureId = ImTextureID(texture);

                icon.ebootPath = entry.path() / "eboot.bin";
                if (Core::FileSys::IsZArchiveFile(entry.path())) {
                    icon.ebootPath = entry.path();
                }

                icon.focusState = false;
                icons.push_back(icon);
            }
        }
    }

    std::sort(icons.begin(), icons.end(), [](const IconInfo& a, const IconInfo& b) {
        return a.title < b.title; // Alphabetical order
    });
}

void Launch(char* executableName, bool sameProcess) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR(ImGui, "SDL_INIT_VIDEO Error: {}", SDL_GetError());
        SDL_Quit();
        return;
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOG_ERROR(ImGui, "SDL_INIT_GAMEPAD Error: {}", SDL_GetError());
    }

    SDL_Window* window =
        SDL_CreateWindow("shadPS4 Big Picture Mode", 1280, 720,
                         EmulatorSettings.IsFullScreen() ? SDL_WINDOW_FULLSCREEN : 0);
    if (window == nullptr) {
        LOG_ERROR(ImGui, "SDL Window Creation Error: {}", SDL_GetError());
        SDL_Quit();
        return;
    }

    Frontend::SetDefaultWindowIcon(window);
    renderer = SDL_CreateRenderer(window, nullptr);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAlways = true;

    ImFontConfig cfgBase;
    cfgBase.OversampleH = 2;
    cfgBase.OversampleV = 1;

    io.FontDefault = ImGui::FontStack::AddPrimaryUiFont(
        io.Fonts, 64.0f, EmulatorSettings.GetConsoleLanguage(), cfgBase, true);
    io.FontGlobalScale = 0.5f;
    // size the big picture font atlas cap from the renderer limit
    const auto max_dim = SDL_GetNumberProperty(SDL_GetRendererProperties(renderer),
                                               SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 8192);
    const int atlas_max = static_cast<int>(std::bit_floor(std::max<u64>(max_dim, 512)));
    io.Fonts->TexMaxWidth = atlas_max;
    io.Fonts->TexMaxHeight = atlas_max;
    io.Fonts->Build();

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    ImGuiEmuSettings::SettingsWindow settingsWindow(
        false, {.load_texture = LoadSdlTextureData, .load_profiles = GetGameIconInfo});

    float sliderScale = 1.0f;
    auto applySettings = [&] {
        uiScale = EmulatorSettings.GetBigPictureScale() / 1000.f;
        sliderScale = uiScale;
        GetGameIconInfo(gameIcons);
        SDL_SetWindowFullscreen(window,
                                EmulatorSettings.IsFullScreen() ? SDL_WINDOW_FULLSCREEN : 0);
    };
    applySettings();

    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                done = true;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 1.00f)); // black
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.40f, 0.70f, 1.00f));   // blue
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                              ImVec4(0.25f, 0.50f, 0.85f, 1.00f)); // lighter blue
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,
                              ImVec4(0.26f, 0.59f, 0.98f, 0.80f)); // another light blue
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,
                              ImVec4(0.26f, 0.59f, 0.98f, 0.80f)); // another light blue

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * uiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * uiScale, 10.0f * uiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * uiScale, 10.0f * uiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.5f * uiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * uiScale, 20.0f * uiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 20.0f * uiScale);

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags window_flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::Begin("Game Window", &done, window_flags);
        ImGui::DrawPrettyBackground();
        ImGui::SetWindowFontScale(uiScale);

        ImGuiChildFlags child_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;

        ImGuiWindowFlags child_window_flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

        if (ImGui::IsWindowAppearing()) {
            ImGui::SetNextWindowFocus();
        }

        ImGui::BeginChild("ContentRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                          child_flags, child_window_flags);

        Overlay::TextCentered("Select Game");
        ImGui::Dummy(ImVec2(0.0f, 10.f * uiScale));

        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        SetGameIcons(gameIcons);
        ImGui::EndChild();
        ImGui::Separator();

        ImGui::SetNextItemWidth(300.0f * uiScale);

        if (ImGui::IsWindowAppearing()) {
            sliderScale = uiScale;
        }
        ImGui::SliderFloat("UI Scale", &sliderScale, 0.25f, 3.0f);

        // Only update when user is not interacting with slider
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            uiScale = sliderScale;
        }

        ImGui::SameLine();

        // Align buttons right
        float buttonsWidth = ImGui::CalcTextSize("Settings").x + ImGui::CalcTextSize("Exit").x +
                             ImGui::GetStyle().FramePadding.x * 4.0f +
                             ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonsWidth);

        if (ImGui::Button("Settings")) {
            EmulatorSettings.SetBigPictureScale(static_cast<int>(uiScale * 1000));
            EmulatorSettings.Save();
            settingsWindow.Prepare();
            showSettings = true;
        }

        ImGui::SameLine();

        if (ImGui::Button("Exit")) {
            ImGui::OpenPopup("Confirm Exit");
        }

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Confirm Exit", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("This will exit shadPS4!\nAre you sure?");
            ImGui::Separator();

            if (ImGui::Button("OK", ImVec2(120 * uiScale, 0))) {
                ImGui::CloseCurrentPopup();
                done = true;
            }
            ImGui::SameLine();

            if (ImGui::Button("Cancel", ImVec2(120 * uiScale, 0))) {
                ImGui::CloseCurrentPopup();
            }

            if (ImGui::IsWindowAppearing()) {
                ImGui::SetItemDefaultFocus();
            }

            ImGui::EndPopup();
        }

        DrawLaunchOptions();

        if (showSettings) {
            settingsWindow.DrawSettings(&showSettings, applySettings);
        }

        ImGui::PopStyleVar(8);
        ImGui::PopStyleColor(5);
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 100, 100, 100, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    renderer = nullptr;
    SDL_DestroyWindow(window);
    SDL_Quit();

    EmulatorSettings.SetBigPictureScale(static_cast<int>(uiScale * 1000));
    EmulatorSettings.Save();

    if (runEbootPath != "") {
        auto* emulator = Common::Singleton<Core::Emulator>::Instance();
        emulator->executableName = executableName;
        if (sameProcess) {
            emulator->Run(runEbootPath);
        } else {
            emulator->Relaunch(
                {"--log-append", "--game", Common::FS::PathToUTF8String(runEbootPath)});
        }
    }
}

} // namespace BigPictureMode
