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
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "imgui/status_overlay.h"

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
                            {"fps_opacity", theme.simple_background.a}};
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
        init.renderer_capabilities = Vulkan::FontCapabilities();
        shell.initialize(init);
        theme.good = {.6f, .9f, .7f, 1.f};
        theme.warning = {1.f, .65f, .25f, 1.f};
        theme.section_label = {.4f, .85f, 1.f, 1.f};
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
                theme.simple_background.a = std::clamp(json.value("fps_opacity", .55f), 0.f, 1.f);
                theme.background.a = std::clamp(json.value("opacity", .94f), .25f, 1.f);
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
    if (w != width || h != height || density != pixel_density) {
        shell.notifyPresentationInvalidated(si::PointerCancelReason::PresentationChanged);
        pointer.reset();
        GetIO().AddMouseButtonEvent(0, false);
        width = w;
        height = h;
        pixel_density = density;
        ov::PresentationEnvironment env;
        env.output.extent = {float(w), float(h)};
        env.views.push_back(env.output);
        // This overlay is composited once onto the physical host output, including SBS games.
        env.scale.pixels_per_dp = pixel_density;
        shell.setPresentation(env);
    }
    const bool ime_active = Core::IsImeInputCaptured();
    if (ime_active != ime_captured) {
        shell.notifyPresentationInvalidated(si::PointerCancelReason::HostRequested);
        pointer.reset();
        GetIO().AddMouseButtonEvent(0, false);
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
            pointer.reset();
            GetIO().AddMouseButtonEvent(0, false);
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
                if (touch.phase >= 2) {
                    GetIO().AddMouseButtonEvent(0, false);
                    pointer.reset();
                }
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
    ov::ControlDescriptor opacity;
    opacity.id = ov::StableId("opacity");
    opacity.label = ov::LocalizedText("Panel opacity");
    opacity.kind = ov::ControlKind::FloatSlider;
    opacity.float_minimum = .25;
    opacity.float_maximum = 1.;
    opacity.float_value = theme.background.a;
    page.controls.push_back(opacity);
    ov::ControlDescriptor fps_opacity = opacity;
    fps_opacity.id = ov::StableId("fps_opacity");
    fps_opacity.label = ov::LocalizedText("FPS opacity");
    fps_opacity.float_minimum = 0.;
    fps_opacity.float_value = theme.simple_background.a;
    page.controls.push_back(fps_opacity);
    snapshot.controls.pages = {std::move(page)};
    snapshot.controls.initial_page = ov::StableId("overlay");
}
void StatusOverlay::Prepare(ov::StatusSnapshot status) {
    snapshot.status = std::move(status);
    if (WantsDetail()) {
        ov::DetailSection section;
        section.id = ov::StableId("Overlay renderer");
        section.title = ov::LocalizedText("Overlay renderer");
        section.properties.push_back({ov::LocalizedText("Font mode"), Vulkan::FontDiagnostics()});
        snapshot.status.detail_sections.push_back(std::move(section));
    }
    if (shell.controller().controlPanelOpen())
        Controls();
    auto resolved = shell.fonts();
    for (auto& font : resolved.roles) {
        font.handle = GetIO().FontDefault;
        font.resolved_weight = ov::TextWeight::Regular;
    }
    shell.setSdfPipelineEvidence(Vulkan::FontEvidence());
    shell.setResolvedFonts(resolved);
    ov::measureOverlayStatus(snapshot, shell.fonts(), theme, metrics);
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
        if (routed.released || routed.cancelled) {
            GetIO().AddMouseButtonEvent(0, false);
            pointer.reset();
        }
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
            } else if (command.control == ov::StableId("fps_opacity")) {
                if (const auto* alpha = std::get_if<double>(&command.value);
                    alpha && std::isfinite(*alpha)) {
                    theme.simple_background.a = float(std::clamp(*alpha, 0., 1.));
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
