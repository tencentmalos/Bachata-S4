// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
#include "imgui/layer_registry.h"

using namespace std::chrono_literals;

static void Check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::abort();
    }
}

struct Probe final : ImGui::Layer {
    explicit Probe(ImGui::LayerRegistry& registry) : registry(registry) {
        registry.Add(this);
    }
    ~Probe() override {
        registry.Remove(this);
    }
    void Draw() override {
        ++draws;
        // The callback may destroy this object. Do not read its members after the call.
        auto callback = draw;
        if (callback) callback();
    }
    bool ShouldKeepDrawing() override {
        ++queries;
        auto callback = query;
        return callback ? callback() : false;
    }
    ImGui::LayerRegistry& registry;
    unsigned draws{}, queries{};
    std::function<void()> draw;
    std::function<bool()> query;
};

static void SessionRelaunch() {
    ImGui::LayerRegistry registry;
    Probe persistent(registry); // e.g. a guest dialog registered only once per process
    for (unsigned session = 0; session < 100; ++session) {
        auto temporary = std::make_unique<Probe>(registry);
        registry.Draw();
        Check(!registry.MustKeepDrawing(), "idle layers must allow direct presentation");
        temporary.reset(); // no NewFrame between destruction and the next query
        Check(registry.MustKeepDrawing(), "removal must request an overlay-clearing frame");
        registry.Draw();
        Check(!registry.MustKeepDrawing(), "destroyed layer must never be queried again");
    }
    Check(persistent.draws == 200, "session teardown must preserve once-registered dialogs");
}

static void RegistrationChanges() {
    ImGui::LayerRegistry registry;
    {
        Probe transient(registry); // add/remove before the first frame
    }
    registry.Draw();
    Check(!registry.MustKeepDrawing(), "pending add must not resurrect a destroyed layer");
    Probe one(registry);
    registry.Add(&one);
    registry.Add(&one);
    registry.Draw();
    Check(one.draws == 1, "duplicate registration must not duplicate callbacks");
    auto self = std::make_unique<Probe>(registry);
    self->draw = [&] { self.reset(); };
    registry.Draw();
    registry.Draw();
    Check(!registry.MustKeepDrawing(), "callback may destroy itself");
}

static void AddressReuse() {
    ImGui::LayerRegistry registry;
    Probe first(registry);
    alignas(Probe) std::byte storage[sizeof(Probe)];
    auto* later = new (storage) Probe(registry);
    first.draw = [&] {
        later->~Probe();
        later = new (storage) Probe(registry);
        first.draw = {};
    };
    registry.Draw();
    Check(later->draws == 0, "old traversal must skip a replacement at the same address");
    registry.Draw();
    Check(later->draws == 1, "replacement must be drawn on the next traversal");
    first.query = [&] {
        later->~Probe();
        later = nullptr;
        return false;
    };
    Check(registry.MustKeepDrawing(), "query callback removal must request a redraw");
    first.query = {};
    registry.Draw();
    Check(!registry.MustKeepDrawing(), "query must skip a removed later entry");
}

static void ConcurrentRemoval(bool query) {
    ImGui::LayerRegistry registry;
    Probe layer(registry);
    registry.Draw();
    std::promise<void> entered, release, removing;
    auto released = release.get_future().share();
    auto callback = [&] {
        entered.set_value();
        released.wait();
    };
    if (query) layer.query = [&] { callback(); return false; };
    else layer.draw = callback;
    auto reader = std::async(std::launch::async, [&] {
        if (query) registry.MustKeepDrawing();
        else registry.Draw();
    });
    Check(entered.get_future().wait_for(2s) == std::future_status::ready,
          "callback must start");
    auto remover = std::async(std::launch::async, [&] {
        removing.set_value();
        registry.Remove(&layer);
    });
    removing.get_future().wait();
    Check(remover.wait_for(50ms) == std::future_status::timeout,
          "removal must wait for an in-flight callback");
    release.set_value();
    Check(reader.wait_for(2s) == std::future_status::ready, "callback must finish");
    Check(remover.wait_for(2s) == std::future_status::ready, "removal must finish after callback");
    reader.get();
    remover.get();
    registry.Draw();
    Check(!registry.MustKeepDrawing(), "removed callback must not be invoked again");
}

int main() {
    SessionRelaunch();
    RegistrationChanges();
    AddressReuse();
    ConcurrentRemoval(false);
    ConcurrentRemoval(true);
    std::cout << "Layer registry: session relaunch, transient/duplicate registration, self-removal, "
                 "address reuse and concurrent draw/query removal passed\n";
}
