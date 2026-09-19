// SPDX-License-Identifier: GPL-2.0-or-later
// Replay measured read-only requests against the SAME files and buffer via
// native preadv, GuestStorage, and checked ABI dispatch. No FIOS/FEX execution.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include "common/path_util.h"
#include "core/file_sys/fs.h"
#include "core/host_runtime/guest_storage_hle.h"
using namespace Core::HostRuntime;
using namespace Core::GuestCpu;
using Clock = std::chrono::steady_clock;
struct Request { std::string path; u64 offset, bytes, hash{}; s64 result{}; };
static u64 Hash(const u8* bytes, size_t size) {
    u64 h = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) h = (h ^ bytes[i]) * 1099511628211ULL;
    return h;
}
int main(int argc, char** argv) {
    if (argc != 4) return 2; // game root, TSV(path offset size), fresh scratch root
    const std::filesystem::path root = argv[1], scratch = argv[3];
    if (!root.is_absolute() || std::filesystem::exists(scratch)) return 2;
    std::vector<Request> requests;
    std::ifstream input(argv[2]); std::string line;
    u64 total{};
    while (std::getline(input, line)) {
        Request r; std::istringstream row(line);
        if (!(row >> r.path >> r.offset >> r.bytes) || r.path.starts_with('/') ||
            r.path.find("..") != std::string::npos || !r.bytes || r.bytes > 16*1024*1024 ||
            r.offset > u64(INT64_MAX) - r.bytes || requests.size() >= 100000 ||
            total + r.bytes > (2ULL << 30)) return 2;
        total += r.bytes; requests.push_back(r);
    }
    if (requests.empty()) return 2;
    Common::FS::InitializeAndroidUserPaths(scratch);
    Core::FileSys::MntPoints mounts; mounts.Mount(root, "/app0");
    GuestStorage storage(mounts, scratch / "users", "CUSA99991", 1000);
    auto made = GuestAddressSpace::Create({.reservation_size = 64ULL << 20});
    if (!made) return 2;
    auto space = std::move(made).Value(); const u64 base = space->ReservationBase().value;
    if (!space->Map({GuestAddress{base}, 16ULL << 20}, GuestPermission::Read | GuestPermission::Write)) return 2;
    auto* buffer = reinterpret_cast<u8*>(base);
    std::memset(buffer, 0, 16ULL << 20); // prefault identical destination for every mode
    std::map<std::string, std::pair<int, int>> descriptors;
    for (const auto& r : requests) if (!descriptors.contains(r.path)) {
        const int native = ::open((root / r.path).c_str(), O_RDONLY | O_CLOEXEC);
        auto guest = storage.Open("/app0/" + r.path, 0, 0);
        if (native < 0 || guest.error) { std::perror(r.path.c_str()); return 3; }
        descriptors.emplace(r.path, std::pair{native, int(guest.value)});
    }
    unsigned errors{};
    // First native replay establishes output checksums and warms this exact
    // request set. Then rotate modes to avoid always giving one mode first use.
    for (int round = -1; round < 5; ++round) for (int index = 0; index < (round < 0 ? 1 : 3); ++index) {
        const int mode = round < 0 ? 0 : (index + round) % 3;
        std::vector<u64> times; u64 ns{}, bytes{};
        for (auto& r : requests) {
            const auto [native, guest] = descriptors.at(r.path);
            const GuestStorage::Buffer b{buffer, size_t(r.bytes)};
            iovec v{buffer, size_t(r.bytes)};
            const auto start = Clock::now(); s64 result{};
            if (mode == 0) result = ::preadv(native, &v, 1, r.offset);
            else if (mode == 1) result = storage.Positioned(guest, std::span{&b, 1}, r.offset, false).value;
            else
                result = static_cast<s64>(DispatchStorage(
                    storage, *space, {"replay", StorageOp::Pread, false},
                    {u64(guest), base, r.bytes, r.offset}, [](int) { return UINT64_MAX; }));
            const u64 elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
            if (result < 0 || u64(result) > r.bytes) { ++errors; continue; }
            const auto hash = Hash(buffer, result); // outside timed interval
            if (round < 0) { r.hash = hash; r.result = result; }
            else if (hash != r.hash || result != r.result) ++errors;
            times.push_back(elapsed); ns += elapsed; bytes += result;
        }
        if (times.empty()) return 3;
        std::sort(times.begin(), times.end());
        std::printf("REPLAY round=%d mode=%d requests=%zu bytes=%llu total_ns=%llu p50_ns=%llu p95_ns=%llu max_ns=%llu errors=%u\n",
            round, mode, requests.size(), (unsigned long long)bytes, (unsigned long long)ns,
            (unsigned long long)times[times.size()/2], (unsigned long long)times[(times.size()-1)*95/100],
            (unsigned long long)times.back(), errors);
        std::fflush(stdout);
    }
    for (auto& [path, fds] : descriptors) { ::close(fds.first); storage.Close(fds.second); }
    return errors ? 1 : 0;
}
