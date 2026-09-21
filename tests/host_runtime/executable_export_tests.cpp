// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>
#include <zarchive/zarchivewriter.h>
#include "common/path_util.h"
#include "core/diagnostics/diagnostics_service.h"
#include "core/diagnostics/executable_export.h"
#include "core/file_format/psf.h"
#include "core/file_sys/backends/host_fs.h"
#include "core/host_runtime/guest_patch.h"

namespace Export = Core::Diagnostics::ExecutableExport;
namespace FS = Core::FileSys;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace std::chrono_literals;
static unsigned checks{}, failures{};
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)
static Json Cmd(std::vector<std::string> args) {
    return Json::parse(Export::Command(args));
}
static Json Wait(const Json& started) {
    const auto id = started.at("request_id").get<std::string>();
    for (unsigned i = 0; i < 2000; ++i) {
        auto r = Cmd({"status", id});
        const auto s = r.value("state", "");
        if (s != "running" && s != "cancelling")
            return r;
        std::this_thread::sleep_for(5ms);
    }
    Cmd({"cancel", id});
    throw std::runtime_error("export test timeout");
}
static std::vector<u8> Read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
static void Write(const fs::path& p, const std::vector<u8>& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
template <class T>
static void Put(std::vector<u8>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
static std::vector<u8> Self(u64 flags = 0x800, bool bad_range = false) {
    std::vector<u8> b(0x210);
    Put<u32>(b, 0, 0x1d3d154f);
    Put<u16>(b, 24, 1);
    Put<u64>(b, 32, flags);
    Put<u64>(b, 40, 0x200);
    Put<u64>(b, 48, 16);
    const size_t e = 64;
    std::memcpy(b.data() + e,
                "\x7f"
                "ELF\x02\x01\x01",
                7);
    Put<u16>(b, e + 16, 0xfe10);
    Put<u16>(b, e + 18, 62);
    Put<u32>(b, e + 20, 1);
    Put<u64>(b, e + 24, 0x400000);
    Put<u64>(b, e + 32, 64);
    Put<u64>(b, e + 40, 0xdeadbeef);
    Put<u16>(b, e + 52, 64);
    Put<u16>(b, e + 54, 56);
    Put<u16>(b, e + 56, 1);
    Put<u16>(b, e + 58, 64);
    Put<u16>(b, e + 60, 20);
    Put<u16>(b, e + 62, 19);
    const size_t p = e + 64;
    Put<u32>(b, p, 1);
    Put<u32>(b, p + 4, 5);
    Put<u64>(b, p + 8, bad_range ? (2ULL << 30) : 0x1000);
    Put<u64>(b, p + 16, 0x400000);
    Put<u64>(b, p + 32, 16);
    Put<u64>(b, p + 40, 16);
    Put<u64>(b, p + 48, 0x1000);
    for (size_t i = 0; i < 16; ++i)
        b[0x200 + i] = static_cast<u8>(0xa0 + i);
    return b;
}
static std::vector<u8> Sfo(const char* version) {
    PSF sfo;
    sfo.AddString("TITLE_ID", "CUSA99999");
    sfo.AddString("APP_VER", version);
    return sfo.Encode();
}
static void Pack(const fs::path& path, const std::map<std::string, std::vector<u8>>& files) {
    std::ofstream out(path, std::ios::binary);
    ZArchiveWriter writer([](int32_t, void*) {},
                          [](const void* b, size_t n, void* p) {
                              static_cast<std::ofstream*>(p)->write(static_cast<const char*>(b), n);
                          },
                          &out);
    for (const auto& [name, bytes] : files) {
        auto parent = fs::path(name).parent_path().generic_string();
        if (!parent.empty())
            writer.MakeDir(parent.c_str(), true);
        if (!writer.StartNewFile(name.c_str()))
            throw std::runtime_error("ZAR fixture");
        writer.AppendData(bytes.data(), bytes.size());
    }
    writer.Finalize();
}
static Json Manifest(const Json& receipt) {
    std::ifstream in(receipt.at("manifest").get<std::string>());
    Json m;
    in >> m;
    return m;
}
static const Json& Find(const Json& m, const std::string& path) {
    for (const auto& entry : m["files"])
        if (entry["path"] == path)
            return entry;
    throw std::runtime_error("missing exported path: " + path);
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{}, release{};
};
class SlowFile final : public FS::IFile {
    std::unique_ptr<FS::IFile> file;
    std::shared_ptr<Gate> gate;

public:
    SlowFile(std::unique_ptr<FS::IFile> f, std::shared_ptr<Gate> g)
        : file(std::move(f)), gate(std::move(g)) {}
    s64 Read(void* p, u64 n) override {
        std::unique_lock lock(gate->mutex);
        gate->entered = true;
        gate->cv.notify_all();
        if (!gate->cv.wait_for(lock, 3s, [&] { return gate->release; }))
            return -1;
        lock.unlock();
        return file->Read(p, n);
    }
    s64 Write(const void*, u64) override {
        return -1;
    }
    bool Seek(s64 o, Common::FS::SeekOrigin w) override {
        return file->Seek(o, w);
    }
    u64 Tell() const override {
        return file->Tell();
    }
    u64 Size() const override {
        return file->Size();
    }
    bool Flush() override {
        return true;
    }
    bool IsOpen() const override {
        return file->IsOpen();
    }
};
class SlowBackend final : public FS::IBackend {
    FS::HostFsBackend backend;
    std::shared_ptr<Gate> gate;

public:
    SlowBackend(const fs::path& p, std::shared_ptr<Gate> g)
        : backend(p, true), gate(std::move(g)) {}
    bool Exists(std::string_view p) override {
        return backend.Exists(p);
    }
    bool IsDirectory(std::string_view p) override {
        return backend.IsDirectory(p);
    }
    std::unique_ptr<FS::IFile> Open(std::string_view p, Common::FS::FileAccessMode m) override {
        auto f = backend.Open(p, m);
        return f ? std::make_unique<SlowFile>(std::move(f), gate) : nullptr;
    }
    std::unique_ptr<FS::IDirectory> OpenDir(std::string_view p) override {
        return backend.OpenDir(p);
    }
    bool IsReadOnly() const override {
        return true;
    }
    fs::path RootPath() const override {
        return backend.RootPath();
    }
    std::optional<std::vector<u8>> ReadFile(std::string_view p) const override {
        return backend.ReadFile(p);
    }
};
int main(int argc, char** argv) {
    try {
        const bool export_only = argc == 4 && std::string(argv[1]) == "--export";
        const auto root =
            export_only
                ? fs::path(argv[3])
                : fs::temp_directory_path() /
                      ("shad-export-tests-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "log");
#ifdef __ANDROID__
        Common::FS::InitializeAndroidUserPaths(root);
#else
        Common::FS::SetUserPath(Common::FS::PathType::LogDir, root / "log");
#endif
        const auto log_root = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
        if (export_only) {
            auto start = Json::parse(Core::Diagnostics::HandleDebugCommand(
                std::string("guest_executable_export start ") + argv[2]));
            std::printf("%s\n", start.dump().c_str());
            auto result = Wait(start);
            std::printf("%s\n", result.dump().c_str());
            return result["state"] == "ready" ? 0 : 1;
        }
        CHECK(Cmd({})["state"] == "idle");
        CHECK(Cmd({"start"})["state"] == "no_current_session");
        CHECK(Cmd({"cancel", "unknown"})["state"] == "not_found");
        for (const auto& a : std::vector<std::vector<std::string>>{{"bogus"},
                                                                   {"cancel"},
                                                                   {"status", "a", "b"},
                                                                   {"start", "relative"},
                                                                   {"start", "path_hex", "0"},
                                                                   {"start", "path_hex", "gg"},
                                                                   {"start", "path_hex", "00"}})
            CHECK(Cmd(a)["state"] == "invalid_arguments");
        CHECK(Core::Diagnostics::HandleDebugCommand("help").find("guest_executable_export") !=
              std::string::npos);
        const auto game = root / "game";
        Write(game / "eboot.bin", Self());
        Write(game / "sce_sys/param.sfo", Sfo("01.00"));
        Write(game / "Media/Plugins/odd_extension.dat", Self());
        Write(game / "sce_module/encrypted.sprx", Self(0x802));
        Write(game / "sce_module/compressed.prx", Self(0x808));
        Write(game / "sce_module/empty.prx", {});
        Write(game / "._eboot.bin", Self());
        Write(game / "assets.data", {1, 2, 3, 4});
        Write(game / "sce_module/unrecognized.prx", {1, 2, 3, 4});
        auto r = Wait(Cmd({"start", game.string()}));
        CHECK(r["state"] == "ready");
        if (r["state"] != "ready")
            throw std::runtime_error(r.dump());
        auto m = Manifest(r);
        CHECK(m["app_version"] == "01.00");
        CHECK(m["exported_files"] == 6);
        CHECK(m["warnings"] == 4);
        const auto dir = fs::path(r["directory"].get<std::string>());
        const auto& main = Find(m, "eboot.bin");
        CHECK(main["status"] == "ELF_reconstructed");
        CHECK(main["sha256"] == Core::GuestPatch::FileSha256(game / "eboot.bin"));
        auto elf = Read(dir / main["elf"]["path"].get<std::string>());
        CHECK(elf.size() == 0x1010);
        CHECK(std::equal(elf.begin() + 0x1000, elf.end(), Self().begin() + 0x200));
        u64 shoff{};
        std::memcpy(&shoff, elf.data() + 40, 8);
        CHECK(shoff == 0);
        CHECK(Find(m, "sce_module/encrypted.sprx")["status"] == "encrypted_SELF_original_only");
        CHECK(Find(m, "sce_module/compressed.prx")["status"] == "compressed_SELF_original_only");
        CHECK(Find(m, "sce_module/empty.prx")["status"] == "empty_placeholder");
        CHECK(Find(m, "Media/Plugins/odd_extension.dat")["status"] == "ELF_reconstructed");
        CHECK(!fs::exists(dir / "app0/original/._eboot.bin"));
        CHECK(!fs::exists(dir / "app0/elf/sce_module/encrypted.sprx.elf"));
        CHECK(Cmd({"cancel", r["request_id"]})["state"] == "ready");
        CHECK(r["manifest_sha256"] == Core::GuestPatch::FileSha256(dir / "manifest.json"));
        // A native ELF remains byte-identical; it is not rebuilt from SELF metadata.
        const auto raw_game = root / "raw-elf";
        Write(raw_game / "eboot.bin", elf);
        r = Wait(Cmd({"start", raw_game.string()}));
        CHECK(r["state"] == "ready");
        auto raw_manifest = Manifest(r);
        CHECK(Find(raw_manifest, "eboot.bin")["status"] == "original_ELF");
        CHECK(Find(raw_manifest, "eboot.bin")["sha256"] ==
              Core::GuestPatch::FileSha256(raw_game / "eboot.bin"));
        CHECK(!Find(raw_manifest, "eboot.bin").contains("elf"));
        CHECK(Cmd({"start", "path_hex", "2fff"})["state"] == "invalid_arguments");
        const auto not_game = root / "not-game";
        Write(not_game / "only.prx", Self());
        CHECK(Wait(Cmd({"start", not_game.string()}))["state"] == "failed");
        // Exact snapshots survive retirement; no new current export can use them.
        FS::MntPoints mounts;
        mounts.Mount(game, "/app0", true);
        auto source = std::make_shared<Export::Source>();
        source->app = *mounts.GetMountSnapshot("/app0");
        source->context_id = 789;
        Export::Publish(source);
        mounts.UnmountAll();
        r = Wait(Cmd({"start"}));
        CHECK(r["state"] == "ready");
        CHECK(Manifest(r)["context_id"] == 789);
        Export::Retire(source);
        CHECK(Cmd({"start"})["state"] == "no_current_session");
        auto replacement = std::make_shared<Export::Source>(*source);
        Export::Publish(replacement);
        Export::Retire(source);
        r = Wait(Cmd({"start"}));
        CHECK(r["state"] == "ready");
        Export::Retire(replacement);
        // Overlay priority + casing + empty files must use the mounted namespace.
        const auto zar = root / "packed.zar";
        Pack(zar, {{"eboot.bin", Self()},
                   {"sce_sys/param.sfo", Sfo("01.00")},
                   {"Lib.PRX", Self()},
                   {"dead.prx", Self()}});
        auto changed = Self();
        changed.back() = 0x77;
        Pack(root / "packed-UPD.zar",
             {{"sce_sys/param.sfo", Sfo("02.04")}, {"lib.prx", changed}, {"dead.prx", {}}});
        r = Wait(Cmd({"start", zar.string()}));
        CHECK(r["state"] == "ready");
        m = Manifest(r);
        CHECK(m["app_version"] == "02.04");
        CHECK(m["exported_files"] == 3);
        CHECK(Find(m, "lib.prx")["sha256"] ==
              Core::GuestPatch::Sha256(std::as_bytes(std::span(changed))));
        CHECK(Find(m, "dead.prx")["status"] == "empty_placeholder");
        // All-in-one app/update plus bundled DLC discovery.
        const auto aio = root / "all.zar";
        Pack(aio, {{"app/eboot.bin", Self()},
                   {"app/sce_sys/param.sfo", Sfo("01.00")},
                   {"update/eboot.bin", changed},
                   {"update/sce_sys/param.sfo", Sfo("02.04")},
                   {"dlc/A/sce_sys/param.sfo", Sfo("01.00")},
                   {"dlc/A/plugin.prx", Self()}});
        r = Wait(Cmd({"start", aio.string()}));
        CHECK(r["state"] == "ready");
        m = Manifest(r);
        CHECK(m["exported_files"] == 2);
        CHECK(m["content_roots"].size() == 2);
        CHECK(Find(m, "eboot.bin")["sha256"] ==
              Core::GuestPatch::Sha256(std::as_bytes(std::span(changed))));
        // An absolute UTF-8/spaced source is sent as hex because DebugBus tokenizes whitespace.
        const auto spaced = root / "space dir";
        Write(spaced / "eboot.bin", Self());
        std::string hex;
        for (unsigned char c : spaced.string()) {
            hex += "0123456789abcdef"[c >> 4];
            hex += "0123456789abcdef"[c & 15];
        }
        r = Wait(Cmd({"start", "path_hex", hex}));
        CHECK(r["state"] == "ready");
        // Malformed inputs and unavailable sources never publish a Ready artifact.
        const auto broken = root / "broken";
        Write(broken / "eboot.bin", Self(0x800, true));
        r = Wait(Cmd({"start", broken.string()}));
        CHECK(r["state"] == "failed");
        CHECK(!r.contains("directory"));
        Write(broken / "eboot.bin", {0x4f, 0x15, 0x3d, 0x1d});
        r = Wait(Cmd({"start", broken.string()}));
        CHECK(r["state"] == "failed");
        r = Wait(Cmd({"start", (root / "missing").string()}));
        CHECK(r["state"] == "failed");
        fs::remove(broken / "eboot.bin");
        fs::create_symlink(game / "eboot.bin", broken / "eboot.bin");
        r = Wait(Cmd({"start", broken.string()}));
        CHECK(r["state"] == "failed");
        // Deterministically hold one read: status/busy/cancel must remain non-blocking.
        auto gate = std::make_shared<Gate>();
        auto slow = std::make_shared<Export::Source>();
        slow->app = {game, "/app0", true, {std::make_shared<SlowBackend>(game, gate)}};
        Export::Publish(slow);
        auto start = Cmd({"start"});
        {
            std::unique_lock lock(gate->mutex);
            CHECK(gate->cv.wait_for(lock, 2s, [&] { return gate->entered; }));
        }
        CHECK(Cmd({"status", start["request_id"]})["state"] == "running");
        CHECK(Cmd({"start"})["state"] == "busy");
        CHECK(Cmd({"cancel", "wrong"})["state"] == "not_found");
        CHECK(Cmd({"cancel", start["request_id"]})["state"] == "cancelling");
        Export::Retire(slow);
        {
            std::lock_guard lock(gate->mutex);
            gate->release = true;
            gate->cv.notify_all();
        }
        r = Wait(start);
        CHECK(r["state"] == "cancelled");
        CHECK(!r.contains("directory"));
        CHECK(!fs::exists(log_root / "executable-exports" /
                          (start["request_id"].get<std::string>() + ".partial")));
        CHECK(Cmd({"start"})["state"] == "no_current_session");
        CHECK(Cmd({"status", "old"})["state"] == "not_found");
        fs::remove_all(root);
        std::printf("executable_export: %u checks / %u failures\n", checks, failures);
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL exception: %s\n", e.what());
        return 1;
    }
}
