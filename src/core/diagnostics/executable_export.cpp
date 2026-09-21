// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/diagnostics/executable_export.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>
#include <nlohmann/json.hpp>

#include "common/path_util.h"
#include "core/diagnostics/trace_identity.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/host_runtime/guest_patch.h"
#include "core/loader/elf.h"

namespace Core::Diagnostics::ExecutableExport {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr u64 MaxFile = 1ULL << 30, MaxOutput = 8ULL << 30;
constexpr u64 MaxEntries = 200000, MaxExecutables = 4096;
constexpr size_t Chunk = 1 << 20;
struct Cancelled {};
void Require(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
void CheckStop(std::stop_token stop) {
    if (stop.stop_requested())
        throw Cancelled{};
}

struct Service {
    std::mutex mutex;
    std::weak_ptr<const Source> current;
    Json receipt{{"schema", 1}, {"state", "idle"}};
    bool running{};
    std::jthread worker;
    ~Service() {
        worker.request_stop();
        if (worker.joinable())
            worker.join();
    }
};
Service& GetService() {
    static Service value;
    return value;
}

void ReadAt(FileSys::IFile& file, u64 off, void* data, u64 size) {
    Require(off <= file.Size() && size <= file.Size() - off, "truncated file");
    Require(file.Seek(static_cast<s64>(off), Common::FS::SeekOrigin::SetOrigin), "seek failed");
    Require(file.Read(data, size) == static_cast<s64>(size), "short file read");
}
std::string ShaFile(const fs::path& path, std::stop_token stop) {
    std::ifstream in(path, std::ios::binary);
    Require(bool(in), "hash open failed");
    return GuestPatch::StreamSha256([&](void* out, u64 size) -> s64 {
        CheckStop(stop);
        in.read(static_cast<char*>(out), size);
        if (in.bad())
            throw std::runtime_error("hash read failed");
        return in.gcount();
    });
}
void WriteJson(const fs::path& path, const Json& data) {
    std::ofstream out(path, std::ios::binary);
    out << data.dump(2) << '\n';
    out.close();
    Require(bool(out), "manifest write failed");
}
std::string Lower(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : char(c);
    });
    return name;
}
bool CandidateName(const fs::path& path) {
    const auto ext = Lower(path.extension().string());
    return Lower(path.filename().string()) == "eboot.bin" || ext == ".prx" || ext == ".sprx" ||
           ext == ".debug_prx" || ext == ".self" || ext == ".elf" || ext == ".dll" ||
           ext == ".exe" || ext == ".so";
}
bool SafeLeaf(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\\r\n\t") == std::string::npos &&
           name.find('\0') == std::string::npos;
}
// Resolve the same case fallback as HostFsBackend, but reject ambiguous names,
// symlinks and special files before a potentially blocking read or recursion.
void CheckHostPath(const FileSys::IBackend& backend, const std::string& relative, bool directory) {
    const auto root = backend.RootHostPath();
    if (!root)
        return;
    auto path = fs::canonical(*root);
    for (const auto& part : fs::path(relative)) {
        auto candidate = path / part;
        std::error_code ec;
        auto status = fs::symlink_status(candidate, ec);
        if (!fs::exists(status) && FileSys::NeedsCaseInsensitiveSearch) {
            std::optional<fs::path> found;
            for (const auto& entry : fs::directory_iterator(path)) {
                if (Lower(entry.path().filename().string()) != Lower(part.string()))
                    continue;
                Require(!found.has_value(), "ambiguous host filename");
                found = entry.path();
            }
            Require(found.has_value(), "host source disappeared");
            candidate = *found;
            status = fs::symlink_status(candidate);
        }
        Require(!fs::is_symlink(status), "source symlink is not supported");
        path = std::move(candidate);
    }
    Require(directory ? fs::is_directory(path) : fs::is_regular_file(path),
            "source is not a regular file/directory");
}
std::string Format(const std::array<u8, 64>& b, u64 size) {
    if (size >= 4 && std::memcmp(b.data(),
                                 "\x7f"
                                 "ELF",
                                 4) == 0)
        return "ELF";
    if (size >= 4 && std::memcmp(b.data(), "\x4f\x15\x3d\x1d", 4) == 0)
        return "SELF";
    if (size >= 2 && b[0] == 'M' && b[1] == 'Z')
        return "MZ";
    return size ? "other" : "empty";
}

// Only the already copied original is consumed here. The ELF conversion cannot
// race a game file replacement between the original copy and program segments.
Json ConvertSelf(const fs::path& original, const fs::path& output, u64 remaining,
                 std::stop_token stop) {
    std::ifstream in(original, std::ios::binary);
    const auto file_size = fs::file_size(original);
    const auto read = [&](u64 off, void* dst, u64 size) {
        Require(off <= file_size && size <= file_size - off, "SELF range outside original");
        in.seekg(off);
        in.read(static_cast<char*>(dst), size);
        Require(bool(in), "SELF short read");
    };
    self_header header{};
    read(0, &header, sizeof(header));
    Require(header.magic == self_header::signature && header.segment_count <= 4096,
            "invalid SELF header");
    std::vector<self_segment_header> segments(header.segment_count);
    read(sizeof(header), segments.data(), segments.size() * sizeof(self_segment_header));
    const u64 elf_offset = sizeof(header) + segments.size() * sizeof(self_segment_header);
    // Explicit little-endian fields: supported hosts and PS4 are little-endian.
    struct Header {
        std::array<u8, 16> ident;
        u16 type, machine;
        u32 version;
        u64 entry, phoff, shoff;
        u32 flags;
        u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
    } elf{};
    static_assert(sizeof(Header) == 64);
    struct Ph {
        u32 type, flags;
        u64 offset, va, pa, filesz, memsz, align;
    };
    static_assert(sizeof(Ph) == 56);
    read(elf_offset, &elf, sizeof(elf));
    Require(std::memcmp(elf.ident.data(),
                        "\x7f"
                        "ELF\x02\x01",
                        6) == 0 &&
                elf.machine == 62 && elf.ehsize == 64 && elf.phentsize == 56 && elf.phnum > 0 &&
                elf.phnum <= 4096,
            "invalid PS4 ELF header");
    Require(elf.phoff >= sizeof(elf) && elf.phoff <= MaxFile &&
                u64(elf.phnum) * sizeof(Ph) <= MaxFile - elf.phoff,
            "ELF program-header bounds");
    std::vector<Ph> ph(elf.phnum);
    Require(elf.phoff <= file_size - std::min(file_size, elf_offset), "SELF ELF offset overflow");
    read(elf_offset + elf.phoff, ph.data(), ph.size() * sizeof(Ph));
    u64 output_size = std::max<u64>(sizeof(elf), elf.phoff + ph.size() * sizeof(Ph));
    for (const auto& p : ph) {
        Require(p.offset <= MaxFile && p.filesz <= MaxFile - p.offset, "ELF output limit exceeded");
        output_size = std::max(output_size, p.offset + p.filesz);
    }
    bool encrypted{}, compressed{};
    std::set<u32> mapped;
    std::vector<std::pair<u64, u64>> intervals;
    for (const auto& s : segments) {
        if (!s.IsBlocked())
            continue;
        Require(s.GetId() < ph.size() && mapped.insert(s.GetId()).second,
                "invalid SELF segment id");
        encrypted |= s.IsEncrypted();
        compressed |= s.IsCompressed();
        const auto& p = ph[s.GetId()];
        Require(s.file_offset <= file_size && s.file_size <= file_size - s.file_offset,
                "truncated SELF segment");
        if (!s.IsEncrypted() && !s.IsCompressed())
            Require(p.filesz <= s.file_size, "short SELF segment");
        if (p.filesz)
            intervals.emplace_back(p.offset, p.offset + p.filesz);
    }
    if (encrypted)
        return {{"status", "encrypted_SELF_original_only"}};
    if (compressed)
        return {{"status", "compressed_SELF_original_only"}};
    for (u32 i = 0; i < ph.size(); ++i)
        if ((ph[i].type == 1 || ph[i].type == 0x61000000) && ph[i].filesz)
            Require(mapped.contains(i), "SELF is missing a load/dynamic-data segment");
    std::sort(intervals.begin(), intervals.end());
    u64 end{};
    for (const auto& [begin, next] : intervals) {
        Require(begin >= end, "overlapping SELF segments");
        end = next;
        Require(begin >= sizeof(elf) &&
                    (next <= elf.phoff || begin >= elf.phoff + ph.size() * sizeof(Ph)),
                "SELF program data overlaps ELF header");
    }
    Require(output_size <= remaining, "export total byte limit exceeded");
    CheckStop(stop);
    fs::create_directories(output.parent_path());
    const auto temporary = fs::path(output.string() + ".partial");
    std::ofstream out(temporary, std::ios::binary);
    Require(bool(out), "ELF output open failed");
    out.seekp(output_size - 1);
    out.put(0);
    Json result{{"status", "ELF_reconstructed"},
                {"bytes", output_size},
                {"entry", elf.entry},
                {"elf_type", elf.type},
                {"machine", "x86_64"},
                {"original_section_table",
                 {{"offset", elf.shoff},
                  {"count", elf.shnum},
                  {"entry_size", elf.shentsize},
                  {"string_index", elf.shstrndx}}},
                {"section_table_removed", true},
                {"segments", Json::array()}};
    elf.shoff = 0;
    elf.shentsize = 0;
    elf.shnum = 0;
    elf.shstrndx = 0;
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(&elf), sizeof(elf));
    out.seekp(elf.phoff);
    out.write(reinterpret_cast<const char*>(ph.data()), ph.size() * sizeof(Ph));
    std::vector<char> bytes(Chunk);
    for (const auto& s : segments) {
        if (!s.IsBlocked())
            continue;
        const auto& p = ph[s.GetId()];
        out.seekp(p.offset);
        for (u64 off = 0; off < p.filesz;) {
            CheckStop(stop);
            const auto n = std::min<u64>(bytes.size(), p.filesz - off);
            read(s.file_offset + off, bytes.data(), n);
            out.write(bytes.data(), n);
            Require(bool(out), "ELF write failed");
            off += n;
        }
        result["segments"].push_back({{"index", s.GetId()},
                                      {"type", p.type},
                                      {"offset", p.offset},
                                      {"va", p.va},
                                      {"bytes", p.filesz}});
    }
    out.close();
    Require(bool(out), "ELF close failed");
    // Verify every reconstructed program segment before publishing this ELF.
    std::ifstream verify(temporary, std::ios::binary);
    std::vector<char> copied(Chunk);
    for (const auto& s : segments) {
        if (!s.IsBlocked())
            continue;
        const auto& p = ph[s.GetId()];
        verify.seekg(p.offset);
        for (u64 off = 0; off < p.filesz;) {
            CheckStop(stop);
            const auto n = std::min<u64>(bytes.size(), p.filesz - off);
            read(s.file_offset + off, bytes.data(), n);
            verify.read(copied.data(), n);
            Require(bool(verify) && std::equal(bytes.begin(), bytes.begin() + n, copied.begin()),
                    "ELF segment verification failed");
            off += n;
        }
    }
    result["sha256"] = ShaFile(temporary, stop);
    fs::rename(temporary, output);
    return result;
}

std::vector<fs::path> DiscoverDlc(const fs::path& install, const std::string& title) {
    std::vector<fs::path> roots;
    if (title.size() == 9 && title.starts_with("CUSA") &&
        std::all_of(title.begin() + 4, title.end(), [](char c) { return c >= '0' && c <= '9'; }))
        roots = FileSys::ListContentRoots(EmulatorSettings.GetAddonInstallDir() / title);
    if (auto sibling =
            FileSys::ResolveGameRoot(FileSys::OverlayPath(install, FileSys::DlcSuffix))) {
        auto list = FileSys::IsZArchiveFile(*sibling) ? FileSys::ExpandBundleRoots(*sibling)
                                                      : FileSys::ListContentRoots(*sibling);
        roots.insert(roots.end(), list.begin(), list.end());
    }
    if (FileSys::IsAllInOneArchive(install)) {
        auto list = FileSys::ListContentRoots(install / FileSys::AllInOneDlc);
        roots.insert(roots.end(), list.begin(), list.end());
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}
std::shared_ptr<Source> OpenSource(fs::path path) {
    Require(path.is_absolute(), "source path must be absolute");
    path = fs::canonical(path);
    if (fs::is_regular_file(path) && !FileSys::IsZArchiveFile(path)) {
        Require(Lower(path.filename().string()) == "eboot.bin",
                "expected game root, ZAR or eboot.bin");
        path = path.parent_path();
    }
    Require(fs::is_directory(path) || FileSys::IsZArchiveFile(path), "invalid game root");
    // Probe without Mount's assertion before admitting externally supplied roots.
    auto probe = FileSys::OpenGameBackend(path);
    Require(bool(probe), "source backend could not be opened");
    FileSys::MntPoints mounts;
    mounts.Mount(path, "/app0", true);
    Require(mounts.Exists("/app0/eboot.bin") && !mounts.IsDirectory("/app0/eboot.bin"),
            "source is not a game root (eboot.bin missing)");
    auto source = std::make_shared<Source>();
    source->app = *mounts.GetMountSnapshot("/app0");
    if (auto file = mounts.Open("/app0/sce_sys/param.sfo")) {
        Require(file->Size() <= 1 << 20, "SFO too large");
        std::vector<u8> bytes(file->Size());
        if (!bytes.empty())
            ReadAt(*file, 0, bytes.data(), bytes.size());
        PSF psf;
        Require(psf.Open(bytes), "invalid SFO");
        source->title_id = psf.GetString("TITLE_ID").value_or("");
        source->app_version = psf.GetString("APP_VER").value_or("");
    }
    source->additional_content = DiscoverDlc(path, source->title_id);
    return source;
}

struct Exporter {
    fs::path destination;
    std::stop_token stop;
    Json manifest;
    u64 scanned{}, entries_seen{}, candidates{}, output_bytes{};
    unsigned warnings{};
    std::vector<char> buffer = std::vector<char>(Chunk);
    std::ofstream inventory;
    Exporter(fs::path path, std::stop_token token)
        : destination(std::move(path)), stop(token),
          manifest{{"schema", 1},
                   {"kind", "shadps4.executable_export"},
                   {"files", Json::array()},
                   {"content_roots", Json::array()},
                   {"scope", "disk files, not relocated guest memory"},
                   {"limits",
                    {{"max_entries", MaxEntries},
                     {"max_executables", MaxExecutables},
                     {"max_file_bytes", MaxFile},
                     {"max_output_bytes", MaxOutput},
                     {"max_depth", 64}}}} {}
    void Progress() {
        std::lock_guard lock(GetService().mutex);
        GetService().receipt["scanned_files"] = scanned;
        GetService().receipt["exported_files"] = candidates;
        GetService().receipt["output_bytes"] = output_bytes;
    }
    void Copy(FileSys::IFile& file, const fs::path& out) {
        const auto size = file.Size();
        Require(size <= MaxFile && size <= MaxOutput - output_bytes, "export byte limit exceeded");
        FileSys::FileStat before{}, after{};
        file.Stat(before);
        fs::create_directories(out.parent_path());
        std::ofstream dest(out, std::ios::binary);
        Require(bool(dest), "output open failed");
        for (u64 off = 0; off < size;) {
            CheckStop(stop);
            const auto n = std::min<u64>(buffer.size(), size - off);
            ReadAt(file, off, buffer.data(), n);
            dest.write(buffer.data(), n);
            Require(bool(dest), "copy write failed");
            off += n;
        }
        dest.close();
        Require(bool(dest), "copy close failed");
        file.Stat(after);
        Require(before.size == after.size && before.mtime_sec == after.mtime_sec &&
                    before.mtime_nsec == after.mtime_nsec && before.ctime_sec == after.ctime_sec &&
                    before.ctime_nsec == after.ctime_nsec,
                "source file changed during export");
        output_bytes += size;
    }
    void File(FileSys::MntPoints& view, const FileSys::MntPoints::MntPair& mount,
              const std::string& relative, const std::string& bucket) {
        CheckStop(stop);
        Require(++scanned <= MaxEntries, "scan entry limit exceeded");
        const auto guest = mount.mount + "/" + relative;
        auto file = view.Open(guest);
        Require(bool(file), "scan file could not be opened");
        std::array<u8, 64> header{};
        const auto size = file->Size();
        if (size)
            ReadAt(*file, 0, header.data(), std::min<u64>(size, header.size()));
        const auto format = Format(header, size);
        const bool selected =
            format == "SELF" || format == "ELF" || format == "MZ" || CandidateName(relative);
        inventory << Json{{"content", bucket},
                          {"path", relative},
                          {"bytes", size},
                          {"format", format},
                          {"selected", selected}}
                         .dump()
                  << '\n';
        Require(bool(inventory), "inventory write failed");
        if (!selected) {
            if ((scanned & 127) == 0)
                Progress();
            return;
        }
        Require(++candidates <= MaxExecutables, "executable count limit exceeded");
        const auto original_rel = fs::path(bucket) / "original" / relative;
        const auto original = destination / original_rel;
        Copy(*file, original);
        Json entry{{"path", relative},
                   {"content", bucket},
                   {"bytes", size},
                   {"format", format},
                   {"original", original_rel.generic_string()},
                   {"sha256", ShaFile(original, stop)}};
        for (size_t i = 0; i < mount.backends.size(); ++i)
            if (mount.backends[i]->Exists(relative) && !mount.backends[i]->IsDirectory(relative)) {
                entry["backend_index"] = i;
                entry["backend_root"] = mount.backends[i]->RootPath().string();
                break;
            }
        if (auto host = file->GetHostPath())
            entry["host_path"] = host->string();
        if (format == "SELF") {
            const auto elf_rel = fs::path(bucket) / "elf" / (relative + ".elf");
            auto result =
                ConvertSelf(original, destination / elf_rel, MaxOutput - output_bytes, stop);
            entry["status"] = result["status"];
            if (result["status"] == "ELF_reconstructed") {
                result["path"] = elf_rel.generic_string();
                output_bytes += result["bytes"].get<u64>();
                entry["elf"] = std::move(result);
            } else
                ++warnings;
        } else {
            entry["status"] = size == 0         ? "empty_placeholder"
                              : format == "ELF" ? "original_ELF"
                              : format == "MZ"  ? "original_MZ"
                                                : "unrecognized_original_only";
            if (!size || format == "other")
                ++warnings;
        }
        manifest["files"].push_back(std::move(entry));
        Progress();
    }
    void Walk(FileSys::MntPoints& view, const FileSys::MntPoints::MntPair& mount,
              const std::string& relative, const std::string& bucket, unsigned depth = 0) {
        CheckStop(stop);
        Require(depth <= 64, "directory depth limit exceeded");
        auto dir = view.OpenDir(mount.mount + (relative.empty() ? "" : "/" + relative));
        Require(bool(dir), "directory could not be enumerated");
        std::vector<FileSys::DirEntry> entries;
        FileSys::DirEntry e;
        while (dir->Next(e)) {
            CheckStop(stop);
            Require(SafeLeaf(e.name), "unsafe source filename");
            if (e.name.starts_with("._"))
                continue;
            Require(entries.size() < MaxEntries, "directory entry limit exceeded");
            entries.push_back(e);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.name < b.name; });
        for (const auto& item : entries) {
            const auto rel = relative.empty() ? item.name : relative + "/" + item.name;
            Require(++entries_seen <= MaxEntries, "scan entry limit exceeded");
            for (const auto& backend : mount.backends) {
                if (!backend->Exists(rel))
                    continue;
                CheckHostPath(*backend, rel, item.is_directory);
                break;
            }
            if (item.is_directory)
                Walk(view, mount, rel, bucket, depth + 1);
            else
                File(view, mount, rel, bucket);
        }
    }
    void Content(const FileSys::MntPoints::MntPair& mount, const std::string& bucket) {
        FileSys::MntPoints view(mount);
        Json roots = Json::array();
        for (const auto& b : mount.backends)
            roots.push_back(b->RootPath().string());
        manifest["content_roots"].push_back({{"namespace", bucket},
                                             {"install_root", mount.host_path.string()},
                                             {"backend_roots_priority_order", roots}});
        Walk(view, mount, "", bucket);
        if (auto sfo = view.Open(mount.mount + "/sce_sys/param.sfo"))
            Copy(*sfo, destination / bucket / "metadata/param.sfo");
    }
    void Run(const Source& source) {
        inventory.open(destination / "inventory.jsonl", std::ios::binary);
        Require(bool(inventory), "inventory open failed");
        manifest["title_id"] = source.title_id;
        manifest["app_version"] = source.app_version;
        manifest["context_id"] = source.context_id;
        Content(source.app, "app0");
        std::set<fs::path> seen;
        size_t index{};
        for (const auto& root : source.additional_content) {
            CheckStop(stop);
            if (!seen.insert(root).second)
                continue;
            Require(bool(FileSys::OpenGameBackend(root)), "DLC backend open failed");
            FileSys::MntPoints dlc;
            dlc.Mount(root, "/content", true);
            Content(*dlc.GetMountSnapshot("/content"), "dlc/" + std::to_string(index++));
        }
        index = 0;
        for (const auto& module : source.external_modules) {
            CheckStop(stop);
            Require(fs::is_regular_file(module) && !fs::is_symlink(module),
                    "external module unavailable");
            FileSys::MntPoints view;
            view.Mount(module.parent_path(), "/external", true);
            auto mount = *view.GetMountSnapshot("/external");
            File(view, mount, module.filename().string(), "external/" + std::to_string(index++));
        }
        inventory.close();
        Require(bool(inventory), "inventory close failed");
        manifest["scanned_files"] = scanned;
        manifest["exported_files"] = candidates;
        manifest["output_bytes"] = output_bytes;
        manifest["warnings"] = warnings;
        manifest["state"] = "ready";
        manifest["inventory_sha256"] = ShaFile(destination / "inventory.jsonl", stop);
        CheckStop(stop);
        WriteJson(destination / "manifest.json", manifest);
    }
};

void RunJob(std::stop_token stop, std::shared_ptr<const Source> source, fs::path input,
            fs::path parent, const std::string& id) {
    const auto partial = parent / (id + ".partial"), ready = parent / id;
    try {
        CheckStop(stop);
        if (!source)
            source = OpenSource(std::move(input));
        Require(parent.is_absolute(), "export root not initialized");
        fs::create_directories(parent);
        Require(fs::create_directory(partial), "export directory collision");
        Exporter exporter(partial, stop);
        exporter.manifest["request_id"] = id;
        exporter.Run(*source);
        const auto manifest_sha = ShaFile(partial / "manifest.json", stop);
        // Cancel and publish are serialized, so cancellation cannot return
        // accepted while the same request is being published as Ready.
        std::lock_guard lock(GetService().mutex);
        CheckStop(stop);
        fs::rename(partial, ready);
        auto& receipt = GetService().receipt;
        receipt["state"] = "ready";
        receipt["directory"] = ready.string();
        receipt["manifest"] = (ready / "manifest.json").string();
        receipt["manifest_sha256"] = manifest_sha;
        receipt["warnings"] = exporter.warnings;
        GetService().running = false;
    } catch (const Cancelled&) {
        std::error_code ec;
        fs::remove_all(partial, ec);
        std::lock_guard lock(GetService().mutex);
        GetService().receipt["state"] = "cancelled";
        GetService().running = false;
        if (ec)
            GetService().receipt["cleanup_error"] = ec.message();
    } catch (const std::exception& e) {
        // Failed partial artifacts remain explicitly incomplete for inspection.
        std::lock_guard lock(GetService().mutex);
        auto& receipt = GetService().receipt;
        receipt["state"] = "failed";
        receipt["error"] = e.what();
        receipt["partial_directory"] = partial.string();
        GetService().running = false;
    }
}
Json Error(const char* status) {
    return {{"schema", 1}, {"state", status}};
}
} // namespace

void Publish(const std::shared_ptr<const Source>& source) {
    std::lock_guard lock(GetService().mutex);
    GetService().current = source;
}
void Retire(const std::shared_ptr<const Source>& source) {
    std::lock_guard lock(GetService().mutex);
    if (GetService().current.lock() == source)
        GetService().current.reset();
}
std::string Command(const std::vector<std::string>& args) {
    auto& service = GetService();
    std::unique_lock lock(service.mutex);
    const auto reply = [](const Json& value) { return value.dump() + "\n"; };
    const auto action = args.empty() ? "status" : args[0];
    if (action == "status" && args.size() <= 2) {
        if (args.size() == 2 && service.receipt.value("request_id", "") != args[1])
            return reply(Error("not_found"));
        return reply(service.receipt);
    }
    if (action == "cancel" && args.size() == 2) {
        if (service.receipt.value("request_id", "") != args[1])
            return reply(Error("not_found"));
        if (service.running) {
            service.worker.request_stop();
            service.receipt["state"] = "cancelling";
        }
        return reply(service.receipt);
    }
    if (action != "start" || args.size() > 3)
        return reply(Error("invalid_arguments"));
    std::string path = args.size() >= 2 ? args[1] : "current";
    if (args.size() == 3) {
        if (path != "path_hex" || args[2].empty() || args[2].size() > 8192 || args[2].size() % 2)
            return reply(Error("invalid_arguments"));
        path.clear();
        for (size_t i = 0; i < args[2].size(); i += 2) {
            unsigned byte{};
            const auto* b = args[2].data() + i;
            const auto [end, ec] = std::from_chars(b, b + 2, byte, 16);
            if (ec != std::errc{} || end != b + 2 || byte == 0)
                return reply(Error("invalid_arguments"));
            path += static_cast<char>(byte);
        }
    }
    if (service.running)
        return reply(
            Json{{"schema", 1}, {"state", "busy"}, {"request_id", service.receipt["request_id"]}});
    auto source = path == "current" ? service.current.lock() : nullptr;
    if (path == "current" && !source)
        return reply(Error("no_current_session"));
    if (path != "current" && (path.size() > 4096 || !fs::path(path).is_absolute()))
        return reply(Error("invalid_arguments"));
    try {
        (void)Json(path).dump();
    } catch (const Json::exception&) {
        return reply(Error("invalid_arguments"));
    }
    // A completed worker has released this mutex; joining it cannot wait for
    // filesystem work. A running worker is rejected above, never joined here.
    if (service.worker.joinable())
        service.worker.join();
    const auto id = MakeRunUuid();
    const auto parent =
        Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "executable-exports";
    service.receipt = {{"schema", 1},
                       {"state", "running"},
                       {"request_id", id},
                       {"source", path},
                       {"context_id", source ? source->context_id : 0},
                       {"scanned_files", 0},
                       {"exported_files", 0},
                       {"output_bytes", 0}};
    service.running = true;
    try {
        service.worker = std::jthread(RunJob, std::move(source), path, parent, id);
    } catch (const std::exception& e) {
        service.running = false;
        service.receipt["state"] = "failed";
        service.receipt["error"] = e.what();
    }
    return reply(service.receipt);
}
} // namespace Core::Diagnostics::ExecutableExport
