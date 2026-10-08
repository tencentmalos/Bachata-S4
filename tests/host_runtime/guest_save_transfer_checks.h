// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Exercise the guest ABI, real savedata metadata and descriptor permissions.
static void CheckSaveTransfer(const std::filesystem::path& root) {
    using E = GuestStorage::Error;
    const auto home = root / "transfer-users";
    Core::FileSys::MntPoints mounts;
    GuestStorage storage(mounts, home, "CUSA99991", 1000);
    AddressSpaceConfig cfg{};
    cfg.reservation_size = 16 << 20;
    auto made = GuestAddressSpace::Create(cfg);
    CHECK(made);
    if (!made) return;
    auto space = std::move(made).Value();
    const auto base = space->ReservationBase().value;
    CHECK(space->Map({{base}, 0x4000}, GuestPermission::Read | GuestPermission::Write));
    struct Request { s32 user; u32 pad; u64 title, directory, fingerprint; u8 reserved[32]; };
    static_assert(sizeof(Request) == 64);
    const auto entry = std::find_if(std::begin(StorageEntries), std::end(StorageEntries),
        [](const auto& e) { return e.op == StorageOp::TransferringMount; });
    CHECK(entry != std::end(StorageEntries));
    if (entry == std::end(StorageEntries)) return;
    auto transfer = [&](std::string title = "CUSA99992", std::string dir = "slot", s32 user = 1000) {
        Request request{user, 0, base + 256, base + 512, 0, {}};
        CHECK(space->WriteData({base}, std::as_bytes(std::span{&request, 1})));
        CHECK(space->WriteData({base + 256}, std::as_bytes(std::span{title.c_str(), title.size() + 1})));
        CHECK(space->WriteData({base + 512}, std::as_bytes(std::span{dir.c_str(), dir.size() + 1})));
        auto result = E(DispatchStorage(storage, *space, *entry, {base, base + 1024}, [](int) {
            CHECK(false); return UINT64_MAX;
        }));
        CHECK(space->Counts().live_pins == 0);
        return result;
    };
    CHECK(transfer() == E::NOT_INITIALIZED);
    CHECK(storage.Initialize() == E::OK);
    CHECK(transfer() == E::NOT_FOUND); // fresh install: missing demo is not a bad parameter
    CHECK(!std::filesystem::exists(home / "1000/savedata/CUSA99992"));
    CHECK(transfer("CUSA99992", "slot", 1001) == E::INVALID_LOGIN_USER);
    CHECK(transfer("../escape") == E::PARAMETER);
    CHECK(transfer("") == E::PARAMETER);
    CHECK(transfer("CUSA99992", "../escape") == E::PARAMETER);
    GuestStorage::MountResult mount{};
    CHECK(storage.Mount(1000, "CUSA99992", "slot", 96, 1, mount) == E::PARAMETER);
    CHECK(storage.Mount(1000, "CUSA99992", "slot", 96, 34, mount) == E::PARAMETER);
    // Seed a real foreign save through its owning session, not fabricated SFO bytes.
    const std::array<u8, 4> value{1, 3, 5, 7};
    {
        GuestStorage source(mounts, home, "CUSA99992", 1000);
        CHECK(source.Initialize() == E::OK);
        CHECK(source.Mount(1000, "", "slot", 96, 34, mount) == E::OK);
        auto fd = source.Open("/savedata0/value", 0x602, 0600);
        CHECK(!fd.error);
        CHECK(source.Write(fd.value, value).value == 4);
        CHECK(source.Close(fd.value).value == 0);
        CHECK(source.Unmount("/savedata0") == E::OK);
    }
    const auto source_path = home / "1000/savedata/CUSA99992/slot";
    auto read_file = [](const auto& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>{in}, {});
    };
    const auto sfo_before = read_file(source_path / "sce_sys/param.sfo");
    CHECK(!sfo_before.empty());
    // Same directory name in different titles is not the same mounted save.
    CHECK(storage.Mount(1000, "", "slot", 96, 34, mount) == E::OK);
    const auto result = transfer();
    CHECK(result == E::OK);
    if (result == E::OK) {
        GuestStorage::MountResult transferred{};
        CHECK(space->ReadData({base + 1024}, std::as_writable_bytes(std::span{&transferred, 1})));
        const std::string point{transferred.point.data()};
        CHECK(point == "/savedata1" && transferred.status == 0);
        auto fd = storage.Open(point + "/value", 0, 0);
        CHECK(!fd.error);
        std::array<u8, 4> out{};
        CHECK(storage.Read(fd.value, out).value == 4 && out == value);
        CHECK(storage.Write(fd.value, value).error == EBADF);
        CHECK(storage.Close(fd.value).value == 0);
        CHECK(storage.Open(point + "/value", 2, 0).error == EROFS);
        CHECK(storage.Open(point + "/new", 0x602, 0600).error == EROFS);
        CHECK(storage.Unlink(point + "/value").error == EROFS);
        CHECK(storage.SetParam(point, 4, value) == E::BAD_MOUNTED);
        CHECK(storage.UnmountBackup(point) == E::BAD_MOUNTED);
        CHECK(transfer() == E::BUSY);
        CHECK(storage.Unmount(point) == E::OK);
        CHECK(read_file(source_path / "sce_sys/param.sfo") == sfo_before);
        CHECK(!std::filesystem::exists(source_path / "sce_backup"));
        CHECK(!std::filesystem::exists(source_path / "sce_sys/corrupted"));
    }
    CHECK(storage.Unmount("/savedata0") == E::OK);
    std::filesystem::create_directory_symlink(home / "1000/savedata/CUSA99992",
                                             home / "1000/savedata/CUSA99993");
    CHECK(transfer("CUSA99993") == E::INTERNAL);
    CHECK(!mounts.GetMountSnapshot("/savedata0"));
    CHECK(read_file(source_path / "value") == std::string("\1\3\5\7", 4));
}
