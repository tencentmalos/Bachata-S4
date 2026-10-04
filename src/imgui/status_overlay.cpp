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
#include "imgui/renderer/imgui_core.h"
#include "spatial/imgui/VulkanRenderer.hpp"
#include "imgui/status_overlay.h"
#ifndef __ANDROID__
#include "imgui/window_chrome.h"
#endif

namespace ImGui {
namespace ov = spatial::imgui::overlay;
namespace si = spatial::imgui;
namespace diag = ::Core::Diagnostics;
StatusOverlay::StatusOverlay() : owner(diag::StatusOverlayMailbox().Begin()) {}
StatusOverlay::~StatusOverlay() {
    diag::StatusOverlayMailbox().End(owner);
    if (pad_captured)
        Core::ReleaseGamepadInputCapture();
}
void StatusOverlay::submit(const ov::OverlayIntent& intent) {
    shell.controller().submitIntent(intent);
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
        intent.status_mode = command == "summary" || command == "show" ? ov::StatusMode::Summary
                                                                       : ov::StatusMode::Simple;
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
    choice("mode", "Status", int(shell.controller().statusMode()), {"None", "Only FPS", "Summary"});
    choice("text_size", "Text size", int(text_size), {"Small", "Medium", "Large"});
    choice("status_anchor", "FPS position", int(status_anchor),
           {"Top left", "Top right", "Bottom left", "Bottom right"});
    // Only FPS and Summary are drawn without a background; this is the Detail and Controls panels'.
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
        return control.id == ov::StableId(ov::perf_hud::kOpacityControl);
    });
    snapshot.controls.pages = {std::move(page), std::move(hud)};
    snapshot.controls.initial_page = ov::StableId("overlay");
}
void StatusOverlay::Prepare(ov::StatusSnapshot status) {
    snapshot.status = std::move(status);
    if (WantsDetail()) {
        ov::DetailSection section;
        section.id = ov::StableId("Overlay renderer");
        section.title = ov::LocalizedText("Overlay renderer");
        auto* renderer = Core::Renderer();
        section.properties.push_back({ov::LocalizedText("Font mode"),
                                      renderer ? renderer->FontDiagnostics() : std::string{}});
        snapshot.status.detail_sections.push_back(std::move(section));
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
                    intent.status_mode = ov::StatusMode(value->value()[0] - '0');
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
    // Only FPS and Summary are text over the game, without a panel; their two buttons keep a
    // translucent fill so they still read as buttons.
    draw.status_appearance = ov::perfHudAppearance(perf_hud);
    draw.status_appearance.background_opacity = 0.f;
    draw.status_appearance.button_opacity = .4f;
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
