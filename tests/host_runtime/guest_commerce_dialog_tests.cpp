// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include "core/host_runtime/guest_commerce_dialog.h"
#include "core/host_runtime/guest_save_dialog.h"
#include "core/libraries/np/np_commerce/np_commerce.h"
#include "core/libraries/system/userservice.h"
#include "imgui/renderer/imgui_core.h"

using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
namespace NC = Libraries::Np::NpCommerce;
namespace CD = Libraries::CommonDialog;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const auto base = space->ReservationBase().value;
    CHECK(space->Map({{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    CHECK(space->Map({{base + 0x4000}, 0x4000}, GuestPermission::Read));
    auto write = [&](u64 address, const auto& value) {
        CHECK(space->WriteData({address}, std::as_bytes(std::span{&value, 1})));
    };
    auto code = [](CD::Error e) { return u32(e); };
    GuestSaveDialog save;
    GuestCommerceDialog commerce(save.CommonDomain(), {1000}, 0x09000000);
    auto call = [&](std::string_view nid, std::array<u64, 6> args = {}) {
        return commerce.Invoke(*space, nid, args);
    };
    for (auto nid : GuestCommerceDialog::Nids)
        CHECK(GuestCommerceDialog::IsNid(nid));
    CHECK(!GuestCommerceDialog::IsNid("unknown"));
    CHECK(call("CCbC+lqqvF0") == 0);
    CHECK(call("0aR2aWmQal4") == code(CD::Error::NOT_SYSTEM_INITIALIZED));
    CHECK(save.Invoke("uoUpLGNkygk", *space, {}) == 0);
    CHECK(call("0aR2aWmQal4") == 0);
    CHECK(call("9ZiLXAGG5rg") == code(CD::Error::ALREADY_INITIALIZED));
    CHECK(save.Invoke("s9e3+YpRnzw", *space, {}) == code(CD::Error::BUSY));
    CHECK(call("NU3ckGHMFXo") == code(CD::Error::NOT_RUNNING));
    CHECK(call("DfSCDRA3EjY") == code(CD::Error::ARG_NULL));
    NC::OrbisNpCommerceDialogParam p{};
    p.baseParam.size = sizeof(p.baseParam);
    p.baseParam.magic = u32(base + 0xc0d1a109);
    p.size = sizeof(p);
    p.userId = 1000;
    p.userData = reinterpret_cast<void*>(0xdead12345678ULL);
    auto open = [&] {
        write(base, p);
        return call("DfSCDRA3EjY", {base});
    };
    const char target[] = "test-product-sku";
    write(base + 1024, target);
    std::array<u64, 10> pointers;
    pointers.fill(base + 1024);
    write(base + 512, pointers);
    for (s32 mode = 0; mode <= 5; ++mode) {
        p.mode = mode;
        p.numTargets = (mode == 1 || mode == 3) ? 1 : 0;
        p.targets = p.numTargets ? reinterpret_cast<const char* const*>(base + 512) : nullptr;
        p.features = mode == 5 ? 1 : 0;
        CHECK(open() == 0);
        auto view = commerce.Read();
        CHECK(view.dialog.status == CD::Status::RUNNING && ImGui::Core::IsGamepadInputCaptured());
        CHECK(view.dialog.text.find("offline") != std::string::npos);
        CHECK(call("r42bWcQbtZY", {base + 2048}) == code(CD::Error::NOT_FINISHED));
        CHECK(!commerce.Respond(view.dialog.request + 1));
        CHECK(commerce.Respond(view.dialog.request));
        CHECK(!commerce.Respond(view.dialog.request));
        CHECK(!ImGui::Core::IsGamepadInputCaptured());
        std::array<u8, 52> sentinel;
        sentinel.fill(0xa5);
        write(base + 2048, sentinel);
        CHECK(call("r42bWcQbtZY", {base + 2049}) == 1);
        CHECK(space->ReadData({base + 2048}, std::as_writable_bytes(std::span{sentinel})));
        NC::OrbisNpCommerceDialogResult result{};
        std::memcpy(&result, sentinel.data() + 1, sizeof(result));
        CHECK(result.result == 1 && !result.authorized && result.userData == p.userData);
        CHECK(sentinel[0] == 0xa5 && sentinel[49] == 0xa5 &&
              std::ranges::all_of(result.reserved, [](auto c) { return c == 0; }));
        CHECK(call("NU3ckGHMFXo") == code(CD::Error::ALREADY_CLOSE));
    }
    p.mode = 0;
    p.features = 0;
    for (auto address : {u64{0}, UINT64_MAX, base + 0x4000 - 1, base + 0x4000})
        CHECK(call("r42bWcQbtZY", {address}) == code(CD::Error::ARG_NULL));
    p.size = 0;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.size = sizeof(p);
    ++p.baseParam.magic;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    --p.baseParam.magic;
    p.reserved[31] = 1;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.reserved[31] = 0;
    p.userId = 1001;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.userId = 1000;
    p.serviceLabel = 8;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.serviceLabel = 0;
    p.mode = 1000;
    CHECK(open() == code(CD::Error::NOT_SUPPORTED));
    p.mode = -1;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.mode = 1;
    p.numTargets = 0;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.numTargets = 1;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.targets = reinterpret_cast<const char* const*>(UINT64_MAX - 4);
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.targets = reinterpret_cast<const char* const*>(base + 512);
    p.numTargets = 11;
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    p.numTargets = 1;
    pointers[0] = UINT64_MAX;
    write(base + 512, pointers);
    CHECK(open() == code(CD::Error::PARAM_INVALID));
    pointers[0] = base + 1024;
    write(base + 512, pointers);
    CHECK(open() == 0);
    CHECK(commerce.Read().dialog.text.find(target) != std::string::npos);
    const char changed[] = "changed";
    write(base + 1024, changed);
    CHECK(commerce.Read().dialog.text.find(target) != std::string::npos);
    CHECK(call("NU3ckGHMFXo") == 0);
    CHECK(call("r42bWcQbtZY", {base + 2048}) == 0);
    CHECK(call("m-I92Ab50W8") == 0);
    CHECK(!save.CommonDomain()->IsUsed());
    CHECK(call("m-I92Ab50W8") == code(CD::Error::NOT_INITIALIZED));
    CHECK(save.Invoke("s9e3+YpRnzw", *space, {}) == 0);
    CHECK(call("0aR2aWmQal4") == code(CD::Error::BUSY));
    CHECK(save.Invoke("YuH2FA7azqQ", *space, {}) == 0);
    CHECK(call("9ZiLXAGG5rg") == 0);

    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1920, 1080};
    io.DeltaTime = 1.0f / 60;
    io.Fonts->AddFontDefault();
    unsigned char* pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&] {
        ImGui::Core::NewFrame();
        ImGui::Render();
    };
    CHECK(call("uKTDW8hk-ts", {2}) == 0);
    CHECK(call("DHmwsa6S8Tc", {1}) == 0);
    frame();
    frame();
    CHECK(commerce.Read().icon_visible && commerce.Read().icon_position == 1 &&
          commerce.Read().icon_layout == 2);
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    CHECK(call("uKTDW8hk-ts", {99}) == 0 && commerce.Read().icon_layout == 0);
    CHECK(call("DHmwsa6S8Tc", {99}) == 0 && commerce.Read().icon_position == 0);
    CHECK(call("dsqCVsNM0Zg") == 0);
    frame();
    frame();
    CHECK(!commerce.Read().icon_visible);
    CHECK(open() == 0);
    frame();
    frame();
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame();
    CHECK(commerce.Read().dialog.status == CD::Status::FINISHED);
    CHECK(call("r42bWcQbtZY", {base + 2048}) == 1);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame();
    CHECK(open() == 0);
    frame();
    auto pending = commerce.Read().dialog.request;
    std::thread responder([&] { commerce.Respond(pending); });
    commerce.Cancel();
    responder.join();
    frame();
    frame();
    CHECK(!ImGui::Core::IsGamepadInputCaptured() && !save.CommonDomain()->IsUsed());
    CHECK(call("0aR2aWmQal4") == code(CD::Error::INVALID_STATE));
    CHECK(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    ImGui::DestroyContext();
    GuestCommerceDialog legacy(save.CommonDomain(), {1000}, 0x01500000);
    CHECK(legacy.Invoke(*space, "0aR2aWmQal4", {}) == 0);
    CHECK(legacy.Invoke(*space, "DfSCDRA3EjY", {base}) == code(CD::Error::NOT_SUPPORTED));
    legacy.Cancel();
    std::printf("COMMERCE_DIALOG checks=%u failures=%u\n", checks, failures);
    return failures != 0;
}
