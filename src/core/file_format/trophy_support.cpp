// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/file_format/trophy_support.h"
#include "core/file_format/npbind.h"
#include "core/file_format/trp.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/string_util.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/emulator_settings.h"
#include "core/user_settings.h"
namespace Core {
std::map<s32, std::string> ExtractTrophies(std::string_view npbind_guest,
                                           std::string_view trophy_dir_guest) {
    std::map<s32, std::string> trophy_index_map{};

    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();

    NPBindFile npbind;
    const auto npbind_bytes = mnt->ReadFile(npbind_guest);
    if (!npbind_bytes || !npbind.Load(std::span<const u8>{*npbind_bytes})) {
        LOG_WARNING(Common_Filesystem, "Failed to load npbind.dat file");
        return trophy_index_map;
    }

    auto np_comm_ids = npbind.GetNpCommIds();
    if (np_comm_ids.empty()) {
        LOG_WARNING(Common_Filesystem, "No NPCommIDs in npbind.dat");
        return trophy_index_map;
    }
    auto& game_info = Common::ElfInfo::Instance();
    game_info.SetNpCommIds(np_comm_ids);

    if (!mnt->IsDirectory(trophy_dir_guest)) {
        LOG_WARNING(Common_Filesystem, "Game does not contain a trophy directory");
        return trophy_index_map;
    }

    auto dir = mnt->OpenDir(trophy_dir_guest);
    if (!dir) {
        return trophy_index_map;
    }

    const std::string pattern = "trophy";
    Core::FileSys::DirEntry entry;
    while (dir->Next(entry)) {
        if (entry.is_directory) {
            continue;
        }
        // Extension check: match TROPHY00.TRP as well as trophy00.trp.
        const std::string name_lower = Common::ToLower(entry.name);
        if (!name_lower.ends_with(".trp")) {
            continue;
        }
        const std::string stem = name_lower.substr(0, name_lower.size() - 4);
        if (!stem.starts_with(pattern)) {
            continue;
        }

        // Extract the number part
        const std::string num_str = stem.substr(pattern.length());
        s32 trophy_index;
        try {
            trophy_index = std::stoi(num_str);
        } catch (...) {
            continue;
        }

        if (trophy_index < 0 || static_cast<s32>(np_comm_ids.size()) <= trophy_index) {
            LOG_WARNING(Common_Filesystem, "Trophy index {} does not have a corresponding NPCommId",
                        trophy_index);
            continue;
        }

        const std::string np_comm_id = np_comm_ids[trophy_index];
        if (np_comm_id.empty() || np_comm_id == "." || np_comm_id == ".." ||
            np_comm_id.find_first_of("/\\") != std::string::npos) continue;
        trophy_index_map[trophy_index] = np_comm_id;
        LOG_DEBUG(Loader, "Mapped trophy index {} to NPCommID: {}", trophy_index, np_comm_id);

        // Extract the actual trophies if they're not extracted yet.
        const auto& trophy_output_dir =
            Common::FS::GetUserPath(Common::FS::PathType::TrophyDir) / np_comm_id;
        if (!std::filesystem::exists(trophy_output_dir)) {
            const std::string entry_guest = std::string(trophy_dir_guest) + "/" + entry.name;
            std::filesystem::path trp_source;
            std::filesystem::path temp_extract;
            if (auto handle = mnt->Open(entry_guest, /*writable=*/false)) {
                if (auto host = handle->GetHostPath(); host.has_value()) {
                    trp_source = *host;
                } else {
                    // Archive-backed: dump bytes to a temp file for TRP.
                    if (auto bytes = mnt->ReadFile(entry_guest)) {
                        temp_extract = std::filesystem::temp_directory_path() /
                                       (np_comm_id + "_" + entry.name);
                        Common::FS::IOFile out(temp_extract, Common::FS::FileAccessMode::Create);
                        out.WriteRaw<u8>(bytes->data(), bytes->size());
                        out.Close();
                        trp_source = temp_extract;
                    }
                }
            }
            if (trp_source.empty()) {
                LOG_ERROR(Loader, "Couldn't read trophy file {}", entry.name);
                continue;
            }
            TRP trp;
            bool ok = trp.Extract(trp_source, np_comm_id, trophy_output_dir);
            if (!ok) {
                // if it's an update and doesn't contain trophies fallback to base
                const auto base_source = mnt->GetHostPath(
                    entry_guest, nullptr, Core::FileSys::MntPoints::HostPathType::Base);
                if (!base_source.empty() && base_source != trp_source &&
                    std::filesystem::is_regular_file(base_source)) {
                    LOG_WARNING(Loader, "Retrying trophy extraction with base game file {}",
                                base_source.string());
                    ok = trp.Extract(base_source, np_comm_id, trophy_output_dir);
                }
            }
            if (!temp_extract.empty()) {
                std::error_code ec;
                std::filesystem::remove(temp_extract, ec);
            }
            if (!ok) {
                LOG_ERROR(Loader, "Couldn't extract trophy file {}", entry.name);
                continue;
            }
        }

        // Move extracted trophy contents into each user's folder
        for (User user : UserSettings.GetUserManager().GetValidUsers()) {
            auto const user_trophy_file = EmulatorSettings.GetHomeDir() /
                                          std::to_string(user.user_id) / "trophy" /
                                          (np_comm_id + ".xml");
            if (!std::filesystem::exists(user_trophy_file)) {
                auto temp = user_trophy_file.parent_path();
                std::filesystem::create_directories(temp);
                std::error_code ec;
                const auto tropconf = trophy_output_dir / "Xml" / "TROPCONF.XML";
                std::filesystem::copy_file(tropconf, user_trophy_file, ec);
                if (ec) {
                    LOG_ERROR(Loader, "Failed to copy {} to {}: {}", tropconf.string(),
                              user_trophy_file.string(), ec.message());
                }
            }
        }
    }

    if (trophy_index_map.empty()) {
        LOG_WARNING(Common_Filesystem, "No usable trophy files found in {}", trophy_dir_guest);
    }

    return trophy_index_map;
}

}
