// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <filesystem>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include "core/guest_cpu/api/address_space.h"
#include "core/libraries/save_data/savedata.h"
#include "core/host_runtime/guest_common_dialog.h"
namespace Core::HostRuntime {
// Platform-neutral, copied dialog model, shown by the ImGui save-data panel (2D and the
// OpenXR layer panel; gamepad and touch). The panel reads a copy and submits an explicit
// request-id-tagged action; no guest pointers survive Open. The host retains userData only
// as an opaque guest integer. Saved data is listed newest first.
class GuestSaveDialog final : public std::enable_shared_from_this<GuestSaveDialog> {
public:
    ~GuestSaveDialog() { Cancel(); }
    std::shared_ptr<GuestCommonDialog> CommonDomain() const { return common; }
    void Configure(std::filesystem::path home, std::string title, int user);
    u64 Invoke(std::string_view nid, GuestCpu::GuestAddressSpace& space,
               const std::array<u64, 6>& args);
    std::string SnapshotJson() const;
    bool Respond(u64 request, int action, int selection); // 0 cancel, 1 OK/yes, 2 no
    void Cancel();
    static bool IsSaveNid(std::string_view nid);
    static bool IsCommonNid(std::string_view nid);

    struct Row {
        std::string directory, title, subtitle, detail;
        s64 mtime{};    // Last write of the save's param.sfo, seconds since the epoch.
        bool is_new{};  // The "new saved data" entry of a save dialog.
    };
    struct View {
        u64 request{};
        u32 mode{}, disp_type{}, buttons{}, progress{};
        bool can_cancel{};
        int user{}, focus{};
        std::string text;
        std::vector<Row> rows;
    };
    // False unless a dialog is open. Rows are copied only when `with_rows` is set.
    bool Snapshot(View& out, bool with_rows) const;

private:
    void ReleaseCapture(); // mutex held
    mutable std::mutex mutex;
    std::shared_ptr<GuestCommonDialog> common = std::make_shared<GuestCommonDialog>();
    bool initialized{}, stopped{}, capture{};
    u32 status{}, mode{}, disp_type{}, buttons{}, progress{};
    bool can_cancel{}, ok_is_cancel{};
    u64 request{}, user_data{};
    int user{}, focus{};
    std::filesystem::path home;
    std::string title, text;
    struct Item {
        std::string directory, label;
        Libraries::SaveData::OrbisSaveDataParam param{};
    };
    std::vector<Item> items;
    u32 result{}, button{};
    int selected{-1};
};
void ShowGuestSaveDialog(const std::shared_ptr<GuestSaveDialog>& dialog);
inline constexpr std::string_view SaveDialogNids[]{
    "fH46Lag88XY", "yEiJ-qqr6Cg", "ERKzksauAJA", "s9e3+YpRnzw", "en7gNVnh878",
    "4tPhsP6FpDI", "V-uEeFKARJU", "hay1CfTmLyA", "YuH2FA7azqQ", "KK3Bdg1RWK0"};
} // namespace Core::HostRuntime
