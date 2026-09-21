// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <thread>
#include "core/host_runtime/guest_signin_dialog.h"
#include "imgui/renderer/imgui_core.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using namespace Libraries::SigninDialog;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL line %d: %s\n", __LINE__, #__VA_ARGS__);                             \
        }                                                                                          \
    } while (0)
int main() {
    AddressSpaceConfig config{};
    config.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(config);
    if (!made)
        return 2;
    auto space = std::move(made).Value();
    const u64 base = space->ReservationBase().value;
    CHECK(space->Map({GuestAddress{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    CHECK(space->Map({GuestAddress{base + 0x4000}, 0x4000}, GuestPermission::Read));
    GuestSigninDialog guest({1000});
    auto call = [&](std::string_view nid, u64 address = 0) {
        return guest.Invoke(*space, nid, {address});
    };
    const auto invalid = u32(Libraries::SigninDialog::Error::InvalidArgument),
               uninit = u32(Libraries::SigninDialog::Error::NotInitialized);
    Param param{16, 1000, {0, 0}};
    auto open = [&] {
        CHECK(space->WriteData(GuestAddress{base}, std::as_bytes(std::span{&param, 1})));
        return call("JlpJVoRWv7U", base);
    };
    CHECK(call("2m077aeC+PA") == 0);
    CHECK(call("LXlmS6PvJdU") == uninit);
    CHECK(call("M3OkENHcyiU") == uninit);
    CHECK(open() == uninit);
    CHECK(call("mlYGfmqE3fQ") == 0);
    CHECK(call("mlYGfmqE3fQ") == u32(Libraries::SigninDialog::Error::AlreadyInitialized));
    CHECK(call("Bw31liTFT3A") == 1);
    for (u64 ptr : {u64{0}, u64{1}, base + 0x8000 - 8, UINT64_MAX})
        CHECK(call("JlpJVoRWv7U", ptr) == invalid);
    for (u64 ptr : {u64{0}, u64{1}, base + 0x4000 - 2, base + 0x4000, UINT64_MAX})
        CHECK(call("nqG7rqnYw1U", ptr) == invalid);
    param.size = 8;
    CHECK(open() == invalid);
    param.size = 16;
    param.reserved[0] = 1;
    CHECK(open() == invalid);
    param.reserved[0] = 0;
    param.reserved[1] = 1;
    CHECK(open() == invalid);
    param.reserved[1] = 0;
    param.user_id = -1;
    CHECK(open() == u32(Libraries::SigninDialog::Error::InvalidUser));
    param.user_id = 1000;
    CHECK(open() == 0);
    CHECK(ImGui::Core::IsGamepadInputCaptured());
    CHECK(open() == u32(Libraries::SigninDialog::Error::InvalidState));
    CHECK(call("Bw31liTFT3A") == 2);
    std::array<u32, 4> out{0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5};
    CHECK(space->WriteData(GuestAddress{base + 256}, std::as_bytes(std::span{out})));
    CHECK(call("nqG7rqnYw1U", base + 260) == u32(Libraries::SigninDialog::Error::InvalidState));
    CHECK(space->ReadData(GuestAddress{base + 256}, std::as_writable_bytes(std::span{out})));
    CHECK(out[0] == 0xa5a5a5a5 && out[1] == 1 && out[2] == 0xa5a5a5a5);
    CHECK(call("M3OkENHcyiU") == 0);
    CHECK(!ImGui::Core::IsGamepadInputCaptured());
    CHECK(call("Bw31liTFT3A") == 3);
    CHECK(call("nqG7rqnYw1U", base + 260) == 0);
    CHECK(call("M3OkENHcyiU") == 0);
    CHECK(open() == 0);              // finished may reopen
    CHECK(call("LXlmS6PvJdU") == 0); // running termination releases capture
    CHECK(!ImGui::Core::IsGamepadInputCaptured());
    CHECK(call("mlYGfmqE3fQ") == 0);
    CHECK(open() == 0);
    guest.Cancel();
    CHECK(call("2m077aeC+PA") == 0);
    CHECK(!ImGui::Core::IsGamepadInputCaptured());
    CHECK(call("mlYGfmqE3fQ") == u32(Libraries::SigninDialog::Error::InvalidState));
    CHECK(open() == u32(Libraries::SigninDialog::Error::InvalidState));
    CHECK(call("unknown") == invalid);
    for (auto nid : GuestSigninDialog::Nids)
        CHECK(GuestSigninDialog::IsNid(nid));
    CHECK(!GuestSigninDialog::IsNid("not-a-nid"));

    auto dialog = std::make_shared<Dialog>(std::vector<s32>{1000});
    CHECK(dialog->Initialize() == 0);
    CHECK(dialog->Open(&param) == 0);
    auto first = dialog->Read();
    CHECK(!dialog->CompleteCancelled(first.request + 1));
    CHECK(dialog->CompleteCancelled(first.request));
    CHECK(!dialog->CompleteCancelled(first.request));
    Libraries::SigninDialog::Result result{-99};
    CHECK(dialog->GetResult(&result) == 0 && result.result == 1);
    CHECK(dialog->Open(&param) == 0);
    CHECK(!dialog->CompleteCancelled(first.request));
    const auto second = dialog->Read();
    std::thread close([&] { dialog->CompleteCancelled(second.request); });
    dialog->Cancel();
    close.join();
    CHECK(dialog->GetStatus() == Libraries::SigninDialog::Status::NONE);
    CHECK(!ImGui::Core::IsGamepadInputCaptured());
    result.result = -99;
    CHECK(dialog->GetResult(&result) == uninit && result.result == -99);
    dialog.reset(); // no dangling renderer references
    auto next = std::make_shared<Dialog>();
    CHECK(next->Initialize() == 0);
    for (s32 id : {0x10000000, 0x3fffffff}) {
        param.user_id = id;
        CHECK(next->Open(&param) == 0);
        CHECK(next->Close() == 0);
    }
    for (s32 id : {0, 1000, 0x0fffffff, 0x40000000}) {
        param.user_id = id;
        CHECK(next->Open(&param) == u32(Libraries::SigninDialog::Error::InvalidUser));
    }
    CHECK(next->Terminate() == 0);
    // Exercise the production registered layer with real ImGui frames, without
    // a Vulkan backend. This verifies popup/input lifecycle, not GPU presentation.
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1920, 1080};
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    unsigned char* pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto frame = [&] {
        ImGui::Core::NewFrame();
        ImGui::Render();
    };
    auto visible = std::make_shared<Dialog>(std::vector<s32>{1000});
    CHECK(visible->Initialize() == 0);
    param.user_id = 1000;
    io.AddKeyEvent(ImGuiKey_Enter, true);
    CHECK(visible->Open(&param) == 0);
    frame();
    frame();
    CHECK(visible->GetStatus() == Libraries::SigninDialog::Status::RUNNING); // opening press cannot dismiss
    CHECK(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame();
    frame();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame();
    CHECK(visible->GetStatus() == Libraries::SigninDialog::Status::FINISHED);
    CHECK(!ImGui::Core::IsGamepadInputCaptured());
    CHECK(visible->GetResult(&result) == 0 && result.result == 1);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    frame();
    CHECK(visible->Open(&param) == 0);
    frame();
    frame();
    CHECK(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    visible->Cancel();
    frame();
    CHECK(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    CHECK(!ImGui::Core::IsGamepadInputCaptured());
    visible.reset();
    frame();
    ImGui::DestroyContext();
    std::printf("signin-dialog: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
