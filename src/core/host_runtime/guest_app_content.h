// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_storage.h"
#include "core/libraries/app_content/app_content.h"
#include "core/libraries/app_content/app_content_error.h"

namespace Core::HostRuntime {
inline constexpr std::string_view AppContentNids[]{"R9lA82OraNs", "99b82IKXpH4", "xnd8BJzAxmk",
                                                   "m47juOmH0VE", "XTWR0UXvcgs", "VANhIWcqYak",
                                                   "buYbeLOGWmA", "SaKib2Ug0yI", "Gl6w5i0JokY"};
inline bool IsAppContentNid(std::string_view nid) {
    return std::find(std::begin(AppContentNids), std::end(AppContentNids), nid) !=
           std::end(AppContentNids);
}
// Immutable metadata and DLC inventory per Session. The caller supplies actual
// roots using the desktop folder-discovery rules. No desktop singletons, global
// entitlement count, download stubs or invented key are used.
class GuestAppContent {
public:
    GuestAppContent(Core::FileSys::MntPoints& mounts, u32 sdk, std::string title,
                    std::array<s32, 5> parameters, std::vector<std::filesystem::path> roots,
                    std::function<bool()> notify = {})
        : mounts(mounts), sdk(sdk), title(std::move(title)), parameters(parameters),
          roots(std::move(roots)), notify(std::move(notify)) {}
    ~GuestAppContent() {
        for (const auto& [point, root] : mounted)
            mounts.UnmountOwned(root, point);
    }
    u32 Dispatch(GuestCpu::GuestAddressSpace& space, std::string_view nid,
                 const std::array<u64, 6>& a, GuestStorage* storage = nullptr) {
        std::lock_guard state_lock(mutex);
        using namespace GuestCpu;
        using namespace Libraries::AppContent;
        auto read = [&](u64 addr, auto& value) {
            return bool(
                space.ReadData(GuestAddress{addr}, std::as_writable_bytes(std::span{&value, 1})));
        };
        auto output = [&](u64 addr, u64 size) {
            return space.AcquireDataSpan({GuestAddress{addr}, size}, true);
        };
        if (nid == "R9lA82OraNs") {
            if (initialized)
                return sdk >= 0x01500000 ? u32(ORBIS_APP_CONTENT_ERROR_BUSY) : 0;
            OrbisAppContentInitParam init{};
            if (a[0] && !read(a[0], init))
                return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
            // Desktop currently leaves bootParam unchanged. Validate its full
            // output extent without claiming semantics for undocumented attr bits.
            if (a[1] && !space.ValidateRange({GuestAddress{a[1]}, sizeof(OrbisAppContentBootParam)},
                                             GuestPermission::Write))
                return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
            std::vector<Entry> inventory;
            auto sorted = roots;
            std::sort(sorted.begin(), sorted.end());
            sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
            for (const auto& root : sorted) {
                auto bytes = Core::FileSys::ReadGameFile(root, "sce_sys/param.sfo");
                if (!bytes)
                    continue;
                PSF psf;
                if (!psf.Open(*bytes))
                    continue;
                const auto category = psf.GetString("CATEGORY").value_or("");
                const auto content = psf.GetString("CONTENT_ID").value_or("");
                if (category != "ac" || content.size() != 36 || content.substr(7, 9) != title)
                    continue;
                Entry entry{};
                entry.root = root;
                std::memcpy(entry.info.entitlement_label.data, content.data() + 20, 16);
                entry.info.status = OrbisAppContentAddcontDownloadStatus::Installed;
                if (std::any_of(inventory.begin(), inventory.end(), [&](const auto& previous) {
                        return std::memcmp(previous.info.entitlement_label.data,
                                           entry.info.entitlement_label.data, 16) == 0;
                    }))
                    continue;
                if (inventory.size() == ORBIS_APP_CONTENT_INFO_LIST_MAX_SIZE)
                    return u32(ORBIS_APP_CONTENT_ERROR_BUSY);
                inventory.push_back(std::move(entry));
            }
            if (!inventory.empty() && notify && !notify())
                return u32(ORBIS_APP_CONTENT_ERROR_BUSY);
            entries = std::move(inventory);
            initialized = true;
            return 0;
        }
        if (nid == "99b82IKXpH4") {
            if (u32(a[0]) >= parameters.size())
                return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
            auto pin = output(a[1], 4);
            if (!pin)
                return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
            const auto value = parameters[u32(a[0])];
            std::memcpy(pin.Value().WritableBytes().data(), &value, 4);
            return 0;
        }
        if (nid == "Gl6w5i0JokY") {
            // Download-data capacity is a local filesystem query. Keep the
            // mount-point and output pointer guest-safe, then report the
            // actual host filesystem capacity in KiB. An unresolved guest
            // mount remains NOT_FOUND; do not turn an unavailable download
            // provider into an invented online success.
            if (!a[1])
                return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
            OrbisAppContentMountPoint point{};
            if (a[0] && (!read(a[0], point) ||
                         !std::memchr(point.data, 0, sizeof(point.data))))
                return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
            const std::string_view guest_point = a[0] ? std::string_view(point.data) : "/app0";
            const auto host_path = mounts.GetHostPath(guest_point);
            if (host_path.empty())
                return u32(ORBIS_APP_CONTENT_ERROR_NOT_FOUND);
            std::error_code error;
            const auto capacity = std::filesystem::space(host_path, error);
            if (error)
                return u32(ORBIS_APP_CONTENT_ERROR_BUSY);
            auto pin = output(a[1], sizeof(u64));
            if (!pin)
                return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
            const u64 available_kib = capacity.available / 1024;
            std::memcpy(pin.Value().WritableBytes().data(), &available_kib, sizeof(available_kib));
            return 0;
        }
        if (!initialized)
            return u32(ORBIS_APP_CONTENT_ERROR_BUSY);
        if (nid == "buYbeLOGWmA" || nid == "SaKib2Ug0yI") {
            if (!storage)
                return u32(ORBIS_APP_CONTENT_ERROR_BUSY);
            auto failure = [](int error) -> u32 {
                if (error == EINVAL)
                    return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
                if (error == ENOENT)
                    return u32(ORBIS_APP_CONTENT_ERROR_NOT_FOUND);
                return u32(ORBIS_APP_CONTENT_ERROR_BUSY);
            };
            OrbisAppContentMountPoint point{};
            if (nid == "buYbeLOGWmA") {
                if (u32(a[0]) > 1)
                    return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
                auto pin = output(a[1], sizeof(point));
                if (!pin)
                    return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
                std::array<char, 16> mounted{};
                if (const int error = storage->MountTemporary(u32(a[0]), mounted))
                    return failure(error);
                std::memcpy(pin.Value().WritableBytes().data(), mounted.data(), mounted.size());
                return 0;
            }
            if (!read(a[0], point) || !std::memchr(point.data, 0, sizeof(point.data)))
                return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
            auto pin = output(a[1], sizeof(u64));
            if (!pin)
                return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
            u64 available{};
            if (const int error = storage->TemporarySpace(point.data, available))
                return failure(error);
            std::memcpy(pin.Value().WritableBytes().data(), &available, sizeof(available));
            return 0;
        }
        if (nid == "xnd8BJzAxmk") {
            const u32 count = u32(a[2]);
            if (!count || !a[1]) {
                auto hit = output(a[3], 4);
                if (!hit)
                    return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
                const u32 size = entries.size();
                std::memcpy(hit.Value().WritableBytes().data(), &size, 4);
                return 0;
            }
            const u32 size = std::min<size_t>(count, entries.size());
            std::vector<GuestAddressSpace::DataRequest> requests;
            if (size)
                requests.push_back({{GuestAddress{a[1]}, size * sizeof(OrbisAppContentAddcontInfo)},
                                    GuestPermission::Write});
            if (a[3])
                requests.push_back({{GuestAddress{a[3]}, 4}, GuestPermission::Write});
            auto pinned = space.AcquireDataBatch(requests);
            if (!pinned)
                return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
            auto* list = size ? &pinned.Value()[0] : nullptr;
            auto* hit = a[3] ? &pinned.Value()[size ? 1 : 0] : nullptr;
            for (u32 i = 0; i < size; ++i)
                std::memcpy(list->WritableBytes().data() + i * sizeof(OrbisAppContentAddcontInfo),
                            &entries[i].info, sizeof(OrbisAppContentAddcontInfo));
            if (hit)
                std::memcpy(hit->WritableBytes().data(), &size, 4);
            return 0;
        }
        OrbisNpUnifiedEntitlementLabel label{};
        if (!read(a[1], label))
            return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& e) {
            return std::memcmp(label.data, e.info.entitlement_label.data, 16) == 0;
        });
        const u64 size = nid == "m47juOmH0VE" ? sizeof(OrbisAppContentAddcontInfo) : 16;
        auto pin = output(a[2], size);
        if (!pin)
            return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
        if (entry == entries.end())
            return u32(nid == "VANhIWcqYak" ? ORBIS_APP_CONTENT_ERROR_NOT_FOUND
                                            : ORBIS_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT);
        if (nid == "m47juOmH0VE") {
            std::memcpy(pin.Value().WritableBytes().data(), &entry->info, sizeof(entry->info));
            return 0;
        }
        if (nid == "XTWR0UXvcgs")
            return u32(ORBIS_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT); // installed bytes are not an
                                                                    // entitlement key
        if (nid != "VANhIWcqYak")
            return u32(ORBIS_APP_CONTENT_ERROR_PARAMETER);
        if (!Core::FileSys::OpenGameBackend(entry->root))
            return u32(ORBIS_APP_CONTENT_ERROR_NOT_FOUND);
        const std::string point =
            "/addcont" + std::to_string(std::distance(entries.begin(), entry));
        if (!mounted.contains(point)) {
            mounts.Mount(entry->root, point, true);
            mounted.emplace(point, entry->root);
        }
        OrbisAppContentMountPoint value{};
        std::memcpy(value.data, point.data(), point.size());
        std::memcpy(pin.Value().WritableBytes().data(), &value, sizeof(value));
        return 0;
    }

private:
    struct Entry {
        std::filesystem::path root;
        Libraries::AppContent::OrbisAppContentAddcontInfo info{};
    };
    Core::FileSys::MntPoints& mounts;
    u32 sdk;
    std::string title;
    std::array<s32, 5> parameters;
    std::vector<std::filesystem::path> roots;
    std::vector<Entry> entries;
    std::map<std::string, std::filesystem::path> mounted;
    std::function<bool()> notify;
    bool initialized{};
    std::mutex mutex;
};
} // namespace Core::HostRuntime
