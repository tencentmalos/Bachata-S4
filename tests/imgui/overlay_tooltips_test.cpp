// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "imgui/runtime_tooltips.h"
static uint64_t now = 1000000000;
static std::vector<std::string> records;
void assert_fail_debug_msg(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}
namespace Core::Diagnostics {
uint64_t DiagnosticNowNs() {
    return now;
}
} // namespace Core::Diagnostics
namespace Common::Log {
void WriteOverlayEvent(std::string_view line) noexcept {
    records.emplace_back(line);
}
} // namespace Common::Log
namespace ImGui {
void Layer::AddLayer(Layer*) {}
void Layer::RemoveLayer(Layer*) {}
} // namespace ImGui
int main() {
    ImGui::RuntimeTooltips layer;
    Core::Diagnostics::DiagnosticsSnapshot state;
    state.pid = 42;
    state.generation = 7;
    state.run_uuid = "test-session";
    state.stage = "Running";
    state.snapshot_ns = now;
    layer.Update(state, false, false);
    assert(records.size() == 1 && layer.ShouldKeepDrawing());
    for (int i = 0; i < 100; ++i)
        layer.Update(state, false, false);
    assert(records.size() == 1);
    layer.Update(state, true, true);
    assert(records.size() == 3 &&
           records[1].find("tag=present.stall pid=42 generation=7 run_uuid=test-session") !=
               std::string::npos);
    layer.Update(state, true, true);
    assert(records.size() == 3);
    now += 8000000000ULL;
    assert(!layer.ShouldKeepDrawing());
    state.snapshot_ns = now;
    layer.Update(state, false, false);
    assert(records.size() == 5 && records[3].find("resumed") != std::string::npos);
    state.terminal_detail = "GPU timeout\nold line";
    layer.Update(state, false, false);
    assert(records.size() == 6 && records.back().find('\n') == std::string::npos);
    state.generation = 8;
    state.terminal_detail.clear();
    layer.Update(state, false, false);
    assert(records.size() == 7 && records.back().find("generation=8") != std::string::npos);
    std::puts("Runtime tooltips: transition deduplication, expiry, recovery, identity and log "
              "sanitization passed");
}
