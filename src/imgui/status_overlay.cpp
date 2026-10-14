// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/diagnostics/overlay_control.h"
#include "core/emulator_settings.h"
#include "core/game_options.h"
#include "video_core/renderer_vulkan/vk_pipeline_stats.h"
#ifdef __ANDROID__
#include "video_core/renderer_vulkan/host_passes/spatial_upscale.h"
#else
#include "video_core/renderer_vulkan/vk_presenter.h"
#endif
#include "imgui/renderer/imgui_core.h"
#include "spatial/imgui/VulkanRenderer.hpp"
#include "imgui/status_overlay.h"
#ifndef __ANDROID__
#include "imgui/window_chrome.h"
#endif

#ifndef __ANDROID__
extern std::unique_ptr<Vulkan::Presenter> presenter;
#endif

namespace ImGui {
namespace ov = spatial::imgui::overlay;
namespace si = spatial::imgui;
namespace diag = ::Core::Diagnostics;
namespace game_options = ::Core::GameOptions;
namespace {
// Shader compilation choices, in the profile's spelling (vulkan.pipeline_compile_mode).
constexpr std::array<std::pair<const char*, const char*>, 3> kCompileModes{{
    {"sync", "Wait"}, {"async_accurate", "Background"}, {"async_graphics_skip", "Skip until ready"}}};
#ifdef __ANDROID__
// gpu.screen_upscaler, and gpu.xr_upscale_sharpness (the 2D screen shares the XR sharpness key).
constexpr std::array<std::pair<const char*, const char*>, 3> kUpscalers{{
    {"off", "Off (bilinear)"}, {"fsr1", "AMD FSR 1"}, {"sgsr1", "Qualcomm SGSR 1"}}};
constexpr std::array<u32, 5> kSharpness{0, 25, 50, 75, 100};
#else
// gpu.rcas_attenuation in thousandths: lower is sharper.
constexpr std::array<std::pair<int, const char*>, 5> kRcasAttenuation{{
    {0, "Strongest (0.0)"}, {250, "Strong (0.25)"}, {500, "Medium (0.5)"}, {1000, "Light (1.0)"},
    {2000, "Lightest (2.0)"}}};
#endif
} // namespace
StatusOverlay::StatusOverlay() : owner(diag::StatusOverlayMailbox().Begin()) {}
StatusOverlay::~StatusOverlay() {
    diag::StatusOverlayMailbox().End(owner);
    if (pad_captured)
        Core::ReleaseGamepadInputCapture();
}
void StatusOverlay::submit(const ov::OverlayIntent& intent) {
    auto& controller = shell.controller();
    controller.processIntents();
    if (intent.kind == ov::OverlayIntentKind::CycleStatusMode) {
        ov::OverlayIntent mode;
        mode.kind = ov::OverlayIntentKind::SetStatusMode;
        mode.status_mode = ov::StatusMode::Summary;
        controller.submitIntent(mode);
        ov::OverlayIntent toggle;
        toggle.kind = ov::OverlayIntentKind::ToggleDetail;
        controller.submitIntent(toggle);
        return;
    }
    if (intent.kind == ov::OverlayIntentKind::SetStatusMode && controller.detailOpen()) {
        ov::OverlayIntent close;
        close.kind = ov::OverlayIntentKind::ToggleDetail;
        controller.submitIntent(close);
    }
    if (intent.kind == ov::OverlayIntentKind::ToggleDetail && !controller.detailOpen()) {
        ov::OverlayIntent summary;
        summary.kind = ov::OverlayIntentKind::SetStatusMode;
        summary.status_mode = ov::StatusMode::Summary;
        controller.submitIntent(summary);
    }
    auto normalized = intent;
    if (normalized.kind == ov::OverlayIntentKind::SetStatusMode &&
        normalized.status_mode == ov::StatusMode::Simple)
        normalized.status_mode = ov::StatusMode::Summary;
    controller.submitIntent(normalized);
}
void StatusOverlay::emit(const ov::OverlayCommand& command) {
    shell.controller().emitCommand(command);
}
bool StatusOverlay::WantsMetrics() const {
    return shell.controller().statusMode() == ov::StatusMode::Summary || WantsDetail();
}
bool StatusOverlay::WantsDetail() const {
    return shell.controller().detailOpen();
}
void StatusOverlay::Request(const std::string& command) {
    ov::OverlayIntent intent;
    if (command == "detail")
        intent.kind = ov::OverlayIntentKind::ToggleDetail;
    else if (command == "controls")
        intent.kind = shell.controller().controlPanelOpen()
                          ? ov::OverlayIntentKind::CloseControlPanel
                          : ov::OverlayIntentKind::OpenControlPanel;
    else if (command == "hide")
        intent.kind = ov::OverlayIntentKind::HideAllOverlays;
    else if (command.starts_with("text ")) {
        text_size = command == "text small"   ? ov::OverlayTextSize::Small
                    : command == "text large" ? ov::OverlayTextSize::Large
                                              : ov::OverlayTextSize::Medium;
        shell.setTypography(ov::makeOverlayTypography(text_size));
        Save();
        return;
    } else {
        intent.kind = ov::OverlayIntentKind::SetStatusMode;
        intent.status_mode = ov::StatusMode::Summary;
    }
    submit(intent);
}
void StatusOverlay::Save() {
    try {
        nlohmann::json json{{"shell", ov::serializeOverlayState(shell.persistentState())},
                            {"text_size", int(text_size)},
                            {"status_anchor", int(status_anchor)},
                            {"opacity", theme.background.a},
                            {"perf_hud", spatial::perf::serializePerfHudSettings(perf_hud)}};
        auto temp = settings_path;
        temp += ".tmp";
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << json.dump(2);
        out.close();
        if (!out)
            throw std::runtime_error("write failed");
        std::error_code error;
        std::filesystem::rename(temp, settings_path, error);
        if (error)
            throw std::runtime_error(error.message());
    } catch (const std::exception& e) {
        LOG_WARNING(ImGui, "Cannot save status overlay: {}", e.what());
    }
}
void StatusOverlay::Begin(unsigned w, unsigned h) {
    if (!initialized) {
        ov::OverlayInitInfo init;
#ifndef __ANDROID__
        metrics.simple_height_dp = 32.f;
        metrics.minimum_touch_target_dp = 32.f;
#endif
        metrics.show_detail_button = false;
        metrics.detail_below_status = true;
        // Outlined text without a background: sit in the screen corner, only clear of its edge,
        // with a frame that hugs the readings instead of a 48dp touch-target row.
        metrics.status_margin_dp = 2.f;
        metrics.summary_touch_target_dp = 20.f;
        metrics.summary_padding_x_dp = 4.f;
        metrics.summary_padding_y_dp = 2.f;
        init.metrics = metrics;
        init.typography = ov::makeOverlayTypography(text_size);
        init.requested_font_mode = si::FontRenderMode::Sdf;
        if (auto* renderer = Core::Renderer()) {
            init.renderer_capabilities = renderer->Capabilities();
        }
        shell.initialize(init);
        theme.good = {.6f, .9f, .7f, 1.f};
        theme.warning = {1.f, .65f, .25f, 1.f};
        theme.section_label = {.4f, .85f, 1.f, 1.f};
        theme.simple_background.a = 0.f;
        settings_path =
            Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "status-overlay.json";
        bool restored = false;
        try {
            std::ifstream input(settings_path);
            if (input) {
                const auto json = nlohmann::json::parse(input);
                ov::OverlayPersistentState state;
                if (ov::deserializeOverlayState(json.value("shell", ""), state)) {
                    for (auto& mode : state.status_modes)
                        if (mode.mode == ov::StatusMode::Simple) mode.mode = ov::StatusMode::Summary;
                    shell.restorePersistentState(state);
                    restored = true;
                }
                text_size = ov::OverlayTextSize(std::clamp(json.value("text_size", 1), 0, 2));
                if (json.contains("status_anchor"))
                    status_anchor = ov::StatusAnchor(std::clamp(json.value("status_anchor", 0), 0, 3));
                theme.background.a = std::clamp(json.value("opacity", .94f), .25f, 1.f);
                if (json.contains("perf_hud") && !spatial::perf::deserializePerfHudSettings(
                                                     json.value("perf_hud", ""), perf_hud))
                    LOG_WARNING(ImGui, "Ignoring invalid performance HUD preferences");
            }
        } catch (const std::exception& e) {
            LOG_WARNING(ImGui, "Ignoring invalid overlay preferences: {}", e.what());
        }
#ifndef __ANDROID__
        if (!restored && !diag::status_overlay_enabled.load())
            Request("hide");
#endif
        shell.setTypography(ov::makeOverlayTypography(text_size));
        ov::OverlayIntent horizontal;
        horizontal.kind = ov::OverlayIntentKind::SetStatusOrientation;
        horizontal.status_orientation = ov::StatusOrientation::Horizontal;
        shell.controller().submitIntent(horizontal);
        shell.controller().processIntents();
        if (shell.controller().statusMode() == ov::StatusMode::Simple) Request("summary");
        metrics.status_anchor = status_anchor;
        initialized = true;
    }
    const float density = diag::StatusOverlayMailbox().PixelDensity();
#ifndef __ANDROID__
    const float top = WindowChrome::TopInset(), bottom = WindowChrome::BottomInset();
#else
    const float top = 0.f, bottom = 0.f;
#endif
    if (w != width || h != height || density != pixel_density || top != inset_top ||
        bottom != inset_bottom) {
        shell.notifyPresentationInvalidated(si::PointerCancelReason::PresentationChanged);
        ReleasePointer();
        width = w;
        height = h;
        pixel_density = density;
        inset_top = top;
        inset_bottom = bottom;
        environment = {};
        environment.output.extent = {float(w), float(h)};
        environment.views.push_back(environment.output);
        environment.safe_insets.top = top;
        environment.safe_insets.bottom = bottom;
        // This overlay is composited once onto the physical host output, including SBS games.
        environment.scale.pixels_per_dp = pixel_density;
        shell.setPresentation(environment);
    }
    const bool ime_active = Core::IsImeInputCaptured();
    if (ime_active != ime_captured) {
        shell.notifyPresentationInvalidated(si::PointerCancelReason::HostRequested);
        ReleasePointer();
        ime_captured = ime_active;
    }
    std::vector<diag::OverlayTouch> touches;
    std::vector<std::string> requests;
    diag::StatusOverlayMailbox().Drain(touches, requests);
    for (const auto& request : requests)
        Request(request);
    shell.controller().processIntents();
    ApplyCommands();
    for (const auto& touch : touches) {
        if (touch.id == -1) {
            shell.notifyPresentationInvalidated(si::PointerCancelReason::HostRequested);
            ReleasePointer();
            continue;
        }
        if (ime_captured) {
            if (!pointer && touch.phase == 0)
                pointer = touch.id;
            if (pointer && *pointer == touch.id) {
                GetIO().AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
                GetIO().AddMousePosEvent(touch.x * width, touch.y * height);
                if (touch.phase == 0)
                    GetIO().AddMouseButtonEvent(0, true);
                if (touch.phase >= 2)
                    ReleasePointer();
            }
            continue;
        }
        si::PointerEvent event;
        event.sequence = ++sequence;
        event.pointer_id = touch.id;
        event.source = si::InputSource::Touch;
        event.phase = touch.phase == 0   ? si::PointerPhase::Down
                      : touch.phase == 1 ? si::PointerPhase::Move
                      : touch.phase == 2 ? si::PointerPhase::Up
                                         : si::PointerPhase::Cancel;
        event.position = {touch.x * width, touch.y * height};
        event.buttons = touch.phase < 2 ? 1 : 0;
        shell.submitPointerEvent(event);
    }
}
void StatusOverlay::ReleasePointer() {
    GetIO().AddMouseButtonEvent(0, false);
    // A touch screen has no hover between touches. Left where the finger lifted, ImGui keeps the item
    // under it hovered until the next touch, and the Summary drew its hover fill as a background.
    // Queued after the release, so the tap still lands on that item.
    if (pointer)
        GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    pointer.reset();
}
void StatusOverlay::Controls() {
    ov::ControlPage page;
    page.id = ov::StableId("overlay");
    page.title = ov::LocalizedText("Overlay");
    auto choice = [&](const char* id, const char* label, int selected,
                      std::initializer_list<const char*> names) {
        ov::ControlDescriptor c;
        c.id = ov::StableId(id);
        c.label = ov::LocalizedText(label);
        c.kind = ov::ControlKind::Choice;
        c.selected_option = ov::StableId(std::to_string(selected));
        int i{};
        for (auto name : names)
            c.options.push_back({ov::StableId(std::to_string(i++)), ov::LocalizedText(name)});
        page.controls.push_back(std::move(c));
    };
    choice("mode", "Status", shell.controller().statusMode() == ov::StatusMode::None ? 0 : 1,
           {"Off", "On"});
    choice("text_size", "Text size", int(text_size), {"Small", "Medium", "Large"});
    choice("status_anchor", "Status position", int(status_anchor),
           {"Top left", "Top right", "Bottom left", "Bottom right"});
    // This opacity controls the settings panel; Status and Detail stay transparent.
    ov::ControlDescriptor opacity;
    opacity.id = ov::StableId("opacity");
    opacity.label = ov::LocalizedText("Panel opacity");
    opacity.kind = ov::ControlKind::FloatSlider;
    opacity.float_minimum = .25;
    opacity.float_maximum = 1.;
    opacity.float_value = theme.background.a;
    page.controls.push_back(opacity);
    auto hud = ov::makePerfHudControlPage(perf_hud, shell.controller().statusMode(),
                                          shell.controller().statusOrientation());
    std::erase_if(hud.controls, [](const ov::ControlDescriptor& control) {
        return control.id == ov::StableId(ov::perf_hud::kOpacityControl) ||
               control.id == ov::StableId(ov::perf_hud::kLayoutControl) ||
               control.id == ov::StableId(std::string(ov::perf_hud::kGraphPrefix) + "cpu") ||
               control.id == ov::StableId(std::string(ov::perf_hud::kGraphPrefix) + "gpu");
    });
    std::erase_if(hud.controls, [](const ov::ControlDescriptor& control) {
        return control.id == ov::StableId(ov::perf_hud::kPresetControl);
    });
    const auto toggle = [](std::string id, const char* label, bool value) {
        ov::ControlDescriptor control;
        control.id = ov::StableId(id);
        control.action = ov::StableId(id);
        control.kind = ov::ControlKind::Toggle;
        control.label = ov::LocalizedText(label);
        control.bool_value = value;
        return control;
    };
    auto rows = std::find_if(hud.controls.begin(), hud.controls.end(), [](const auto& control) {
        return control.id == ov::StableId("perf-hud.rows");
    });
    if (rows != hud.controls.end())
        hud.controls.insert(std::next(rows), toggle(std::string(ov::perf_hud::kShowPrefix) + "fps",
                                                  "FPS", perf_hud.shows(spatial::perf::PerfMetric::Fps)));
    snapshot.controls.pages = {GameControls(), std::move(page), std::move(hud)};
    snapshot.controls.initial_page = ov::StableId("game");
}
ov::ControlPage StatusOverlay::GameControls() {
    ov::ControlPage page;
    page.id = ov::StableId("game");
    page.title = ov::LocalizedText("Game");
    // Choice rows whose option ids are the values the per-game profile stores.
    auto choice = [&](const char* id, const char* label, const char* description,
                      const std::string& selected,
                      const std::vector<std::pair<std::string, std::string>>& options) {
        ov::ControlDescriptor c;
        c.id = ov::StableId(id);
        c.label = ov::LocalizedText(label);
        c.description = ov::LocalizedText(description);
        c.kind = ov::ControlKind::Choice;
        c.selected_option = ov::StableId(selected);
        for (const auto& [value, name] : options)
            c.options.push_back({ov::StableId(value), ov::LocalizedText(name)});
        page.controls.push_back(std::move(c));
    };
    std::vector<std::pair<std::string, std::string>> limits;
    for (const u32 fps : game_options::FrameLimits)
        limits.emplace_back(game_options::FrameLimitName(fps),
                            fps ? std::to_string(fps) + " FPS" : std::string("Off"));
    choice("game.frame_limit", "Frame limit",
           "Presents at most this many frames a second; the game slows to match. Saved for "
           "this game.",
           game_options::FrameLimitName(game_options::FrameLimit()), limits);
    std::vector<std::pair<std::string, std::string>> modes;
    for (const auto& [value, name] : kCompileModes)
        modes.emplace_back(value, name);
    choice("game.compile_mode", "Shader compilation",
           "For pipelines built from now on. Skip until ready can leave objects missing for a "
           "few frames. Saved for this game.",
           Vulkan::PipelineStats::CompileModeName(Vulkan::PipelineStats::ConfiguredCompileMode()),
           modes);
#ifdef __ANDROID__
    const auto spatial = Vulkan::HostPasses::GetSpatialOptions();
    std::vector<std::pair<std::string, std::string>> upscalers;
    for (const auto& [value, name] : kUpscalers)
        upscalers.emplace_back(value, name);
    choice("game.screen_upscaler", "Screen upscaler",
           "Reconstructs the game image at the screen size when Render Scale is below it. "
           "Saved for this game.",
           kUpscalers[std::min<std::size_t>(std::size_t(spatial.filter), kUpscalers.size() - 1)]
               .first,
           upscalers);
    std::vector<std::pair<std::string, std::string>> sharpness;
    u32 nearest = kSharpness[0];
    for (const u32 value : kSharpness) {
        sharpness.emplace_back(std::to_string(value), std::to_string(value));
        if (std::abs(int(value) - int(spatial.sharpness)) <
            std::abs(int(nearest) - int(spatial.sharpness)))
            nearest = value;
    }
    choice("game.sharpness", "Upscaler sharpness", "FSR 1 / SGSR 1 only. Saved for this game.",
           std::to_string(nearest), sharpness);
#else
    if (presenter) {
        const auto& fsr = presenter->GetFsrSettingsRef();
        auto toggle = [&](const char* id, const char* label, const char* description, bool value) {
            ov::ControlDescriptor c;
            c.id = ov::StableId(id);
            c.action = ov::StableId(id);
            c.kind = ov::ControlKind::Toggle;
            c.label = ov::LocalizedText(label);
            c.description = ov::LocalizedText(description);
            c.bool_value = value;
            page.controls.push_back(std::move(c));
        };
        toggle("game.fsr", "FSR", "AMD FSR 1 to the window size. Saved for this game.",
               fsr.enable);
        toggle("game.rcas", "RCAS sharpening", "Sharpens after FSR. Saved for this game.",
               fsr.use_rcas);
        std::vector<std::pair<std::string, std::string>> attenuation;
        int nearest = kRcasAttenuation[0].first;
        const int current = int(std::lround(fsr.rcas_attenuation * 1000.f));
        for (const auto& [value, name] : kRcasAttenuation) {
            attenuation.emplace_back(std::to_string(value), name);
            if (std::abs(value - current) < std::abs(nearest - current))
                nearest = value;
        }
        choice("game.rcas_attenuation", "Sharpening strength", "Saved for this game.",
               std::to_string(nearest), attenuation);
    }
#endif
    return page;
}
bool StatusOverlay::ApplyGameControl(const ov::OverlayCommand& command) {
    const std::string id = command.control.value();
    if (!id.starts_with("game."))
        return false;
    const auto* choice = std::get_if<ov::StableId>(&command.value);
    if (id == "game.frame_limit" && choice) {
        const u32 fps = game_options::ParseFrameLimit(choice->value());
        if (game_options::FrameLimitName(fps) != choice->value())
            return true;
        game_options::SetFrameLimit(fps);
        EmulatorSettings.SetFrameLimit(fps);
        game_options::Save("gpu.frame_limit", choice->value());
    } else if (id == "game.compile_mode" && choice) {
        for (const auto& [value, name] : kCompileModes) {
            if (choice->value() != value)
                continue;
            EmulatorSettings.SetPipelineCompileMode(value);
            Vulkan::PipelineStats::LatchConfiguredCompileMode();
            game_options::Save("vulkan.pipeline_compile_mode", value);
        }
    }
#ifdef __ANDROID__
    else if (id == "game.screen_upscaler" && choice) {
        for (std::size_t i = 0; i < kUpscalers.size(); ++i) {
            if (choice->value() != kUpscalers[i].first)
                continue;
            auto options = Vulkan::HostPasses::GetSpatialOptions();
            options.filter = Vulkan::HostPasses::SpatialFilter(i);
            Vulkan::HostPasses::SetSpatialOptions(options);
            game_options::Save("gpu.screen_upscaler", kUpscalers[i].first);
        }
    } else if (id == "game.sharpness" && choice) {
        for (const u32 value : kSharpness) {
            if (choice->value() != std::to_string(value))
                continue;
            auto options = Vulkan::HostPasses::GetSpatialOptions();
            options.sharpness = value;
            Vulkan::HostPasses::SetSpatialOptions(options);
            game_options::Save("gpu.xr_upscale_sharpness", choice->value());
        }
    }
#else
    else if (presenter) {
        auto& fsr = presenter->GetFsrSettingsRef();
        const auto* toggled = std::get_if<bool>(&command.value);
        if (id == "game.fsr" && toggled) {
            fsr.enable = *toggled;
            game_options::Save("gpu.fsr_enabled", *toggled ? "true" : "false");
        } else if (id == "game.rcas" && toggled) {
            fsr.use_rcas = *toggled;
            game_options::Save("gpu.rcas_enabled", *toggled ? "true" : "false");
        } else if (id == "game.rcas_attenuation" && choice) {
            for (const auto& [value, name] : kRcasAttenuation) {
                if (choice->value() != std::to_string(value))
                    continue;
                fsr.rcas_attenuation = float(value) / 1000.f;
                game_options::Save("gpu.rcas_attenuation", choice->value());
            }
        }
    }
#endif
    return true;
}
void StatusOverlay::Prepare(ov::StatusSnapshot status) {
    snapshot.status = std::move(status);
    if (WantsDetail()) {
        ov::DetailSection section;
        section.id = ov::StableId("Overlay renderer");
        section.title = ov::LocalizedText("Overlay renderer");
        section.initially_open = false;
        auto* renderer = Core::Renderer();
        section.properties.push_back({ov::LocalizedText("Font mode"),
                                      renderer ? renderer->FontDiagnostics() : std::string{}});
        ov::DetailSection diagnostics;
        diagnostics.id = ov::StableId("detail.diagnostics");
        diagnostics.title = ov::LocalizedText("Diagnostics");
        diagnostics.initially_open = false;
        diagnostics.groups.push_back(std::move(section));
        snapshot.status.detail_sections.push_back(std::move(diagnostics));
    }
    if (shell.controller().controlPanelOpen())
        Controls();
    auto resolved = shell.fonts();
    for (auto& font : resolved.roles) {
        font.handle = GetIO().FontDefault;
        font.resolved_weight = ov::TextWeight::Regular;
    }
    if (auto* renderer = Core::Renderer()) {
        shell.setSdfPipelineEvidence(renderer->Evidence());
    }
    shell.setResolvedFonts(resolved);
    ov::measureOverlayStatus(snapshot, shell.fonts(), theme, metrics, environment);
    // One visual block: Detail shares the horizontal HUD's width and starts at its bottom edge.
    if (metrics.summary_horizontal_width_dp)
        metrics.detail_width_dp = *metrics.summary_horizontal_width_dp;
    shell.setMetrics(metrics);
    if (!width || !height)
        return;
    frame = shell.render(std::max(GetIO().DeltaTime, 1e-6f));
    const bool capture = shell.controller().controlPanelOpen();
    if (capture != pad_captured) {
        if (capture)
            Core::AcquireGamepadInputCapture();
        else
            Core::ReleaseGamepadInputCapture();
        pad_captured = capture;
    }
    std::vector<diag::OverlayRect> regions;
    if (ime_captured)
        regions.push_back({0, 0, 1, 1});
    else
        for (const auto& layer : frame.layers)
            for (const auto& rect : layer.submissions)
                regions.push_back({rect.origin.x / width, rect.origin.y / height,
                                   rect.extent.x / width, rect.extent.y / height});
    diag::StatusOverlayMailbox().Publish(owner, std::move(regions));
    for (const auto& routed : frame.input.routed) {
        if (!pointer && routed.pressed)
            pointer = routed.pointer_id;
        if (!pointer || *pointer != routed.pointer_id)
            continue;
        // The router subtracts the view origin; this host has one output-sized canvas.
        const float x = routed.position.x, y = routed.position.y;
        GetIO().AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        GetIO().AddMousePosEvent(x, y);
        if (routed.pressed)
            GetIO().AddMouseButtonEvent(0, true);
        if (routed.released || routed.cancelled)
            ReleasePointer();
    }
    diag::status_overlay_enabled.store(frame.overlay_visible, std::memory_order_relaxed);
    ApplyCommands();
}
void StatusOverlay::ApplyCommands() {
    ov::OverlayCommand command;
    while (shell.pollCommand(command)) {
        if (command.kind == ov::OverlayCommandKind::PersistentStateChanged)
            Save();
        else if (command.kind == ov::OverlayCommandKind::CopyToClipboard)
            SetClipboardText(command.text.c_str());
        else if (command.kind == ov::OverlayCommandKind::ControlChanged) {
            if (ApplyGameControl(command))
                continue;
            // The HUD page's layout choice is controller state; its other rows are settings.
            const auto hud = ov::applyPerfHudControl(command, perf_hud);
            if (hud.handled) {
                for (const auto& intent : hud.intents)
                    submit(intent);
                if (hud.settings_changed)
                    Save();
                continue;
            }
            const auto* value = std::get_if<ov::StableId>(&command.value);
            if (command.control == ov::StableId("mode") && value) {
                if (value->value() == "0" || value->value() == "1" || value->value() == "2") {
                    ov::OverlayIntent intent;
                    intent.kind = ov::OverlayIntentKind::SetStatusMode;
                    intent.status_mode = value->value() == "0" ? ov::StatusMode::None
                                                              : ov::StatusMode::Summary;
                    submit(intent);
                }
            } else if (command.control == ov::StableId("text_size") && value) {
                if (value->value() == "0" || value->value() == "1" || value->value() == "2") {
                    text_size = ov::OverlayTextSize(value->value()[0] - '0');
                    shell.setTypography(ov::makeOverlayTypography(text_size));
                    Save();
                }
            } else if (command.control == ov::StableId("status_anchor") && value) {
                if (value->value().size() == 1 && value->value()[0] >= '0' && value->value()[0] <= '3') {
                    status_anchor = ov::StatusAnchor(value->value()[0] - '0');
                    metrics.status_anchor = status_anchor;
                    Save();
                }
            } else if (command.control == ov::StableId("opacity")) {
                if (const auto* alpha = std::get_if<double>(&command.value);
                    alpha && std::isfinite(*alpha)) {
                    theme.background.a = float(std::clamp(*alpha, .25, 1.));
                    Save();
                }
            }
        }
    }
}
void StatusOverlay::Draw() {
    if (!frame.overlay_visible || ime_captured)
        return;
    // Other host dialogs share this context; restore their style and font scaling.
    const auto saved_style = GetStyle();
    const auto saved_scale = GetIO().FontGlobalScale;
    GetIO().FontGlobalScale = 1.f;
    ov::applyOverlayTheme(theme, shell.fonts().scale.geometry());
    ov::OverlayShellDrawInfo draw;
    draw.frame = &frame;
    draw.snapshot = &snapshot;
    draw.theme = &theme;
    draw.intents = this;
    // Detail shares Summary's transparent, outlined text appearance.
    // Controls remains an icon without a permanent or touch-hover background.
    draw.status_appearance = ov::perfHudAppearance(perf_hud);
    draw.status_appearance.background_opacity = 0.f;
    draw.status_appearance.transparent_frame = true;
    draw.status_appearance.transparent_frame_width_dp = 2.f;
    draw.status_appearance.button_opacity = 0.f;
    draw.status_appearance.hover_feedback = false;
    draw.sectionOpen = [&](const ov::StableId& id, bool fallback) {
        return shell.controller().detailSectionOpen(id, fallback);
    };
    draw.setSectionOpen = [&](const ov::StableId& id, bool open) {
        shell.controller().setDetailSectionOpen(id, open);
    };
    ov::drawOverlayShellFrame(draw, shell.controller().imeSession(), ime);
    GetIO().FontGlobalScale = saved_scale;
    GetStyle() = saved_style;
    // Restore the current font's cached size as well as the global scale.
    PushFont(GetFont());
    PopFont();
}
} // namespace ImGui
