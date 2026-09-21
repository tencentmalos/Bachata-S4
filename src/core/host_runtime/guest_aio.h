// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <condition_variable>
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <stop_token>
#include <thread>
#include "core/guest_cpu/api/address_space.h"
#include "core/host_runtime/guest_storage.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
namespace Core::HostRuntime {
inline constexpr std::string_view AioNids[]{
    "fR521KIGgb8", "3Lca1XBrQdY", "5TgME6AYty4", "Ft3EtsZzAoY", "vYU8P9Td2Zo",
    "nu4a0-arQis", "2pOuoWoCxdk", "o7O4z3jwKzo", "9WK-vhNXimw", "HgX7+AORI58",
    "lXT0m3P-vs4", "XQ8C8y+de+E", "xT3Cpz0yh6Y", "KOF-oJbQVvc", "lgK+oIWkJyA"};
// Owns real storage requests, never desktop host descriptors. Workers touch
// only pre-admitted data spans; their lifetime ends before storage/VM teardown.
class GuestAio final {
    using Space = GuestCpu::GuestAddressSpace;
    using Pin = GuestCpu::PinnedSpan;
    enum : s32 { Submitted = 1, Processing = 2, Completed = 3, Aborted = 4 };
public:
    struct Request { s64 offset, bytes; u64 buffer, result; s32 fd; u32 reserved; };
    struct Result { s64 value; s32 state; u32 reserved{}; };
    static_assert(sizeof(Request) == 40 && sizeof(Result) == 16);
    inline static constexpr std::array<u32, 15> DefaultParam{
        32,32,1,0x100000,0x100000, 32,32,1,0x100000,0x100000, 32,32,0,0,0};
private:
    struct Command {
        Request request{};
        GuestStorage::PositionedLease file;
        Pin data, result;
    };
    struct Job { std::vector<Command> commands; s32 state{Submitted}; bool cancel{}, write{}; u64 bytes{}; };
    Space& space;
    GuestStorage& storage;
    std::mutex mutex;
    std::condition_variable changed;
    bool initialized{}, stopping{};
    s32 next_id{1};
    u64 pending_bytes{};
    std::map<s32, std::shared_ptr<Job>> jobs;
    std::deque<std::shared_ptr<Job>> queue;
    std::array<std::thread, 2> workers;
    template<class T> bool Read(u64 address, T& value) {
        return address && bool(space.ReadData({address}, std::as_writable_bytes(std::span{&value, 1})));
    }
    static void Publish(Command& c, s64 value, s32 state) {
        const Result result{value, state};
        std::memcpy(c.result.WritableBytes().data(), &result, sizeof(result));
    }
    void CancelQueued(Job& job) { // mutex held; no active I/O may own these spans
        job.cancel = true;
        if (job.state != Submitted) return;
        for (auto& command : job.commands) Publish(command, ORBIS_KERNEL_ERROR_ECANCELED, Aborted);
        pending_bytes -= job.bytes; job.bytes = 0;
        job.commands.clear();
        job.state = Aborted;
    }
    void Work() {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stopping || !queue.empty(); });
                if (queue.empty()) return;
                job = std::move(queue.front()); queue.pop_front();
                if (job->state == Aborted) continue;
                job->state = Processing;
            }
            bool failed{};
            for (auto& c : job->commands) {
                Publish(c, 0, Processing);
                s64 total{}; s64 error{};
                do {
                    { std::lock_guard lock(mutex); if (job->cancel || stopping) error = ORBIS_KERNEL_ERROR_ECANCELED; }
                    if (error) break;
                    const auto count = std::min<s64>(c.request.bytes - total, 16 * 1024 * 1024);
                    auto* pointer = job->write ? const_cast<std::byte*>(c.data.Bytes().data())
                                               : c.data.WritableBytes().data();
                    GuestStorage::Buffer buffer{pointer ? pointer + total : nullptr, size_t(count)};
                    GuestStorage::IoResult io;
                    try { io = c.file(std::span{&buffer, 1}, c.request.offset + total, job->write); }
                    catch (...) { io = {-1, EIO}; }
                    if (io.error) {
                        error = Libraries::Kernel::ErrnoToSceKernelError(Libraries::Kernel::NativeToPosixErrno(io.error));
                        break;
                    }
                    total += io.value;
                    if (io.value < count) break;
                } while (total < c.request.bytes);
                // A completed syscall may race cancellation. Preserve bytes it
                // already transferred; never report cancellation as no mutation.
                Publish(c, total ? total : error, error && !total ? Aborted : Completed);
                failed |= error && !total;
                c.data = {}; c.result = {}; c.file = {};
            }
            {
                std::lock_guard lock(mutex);
                pending_bytes -= job->bytes; job->bytes = 0;
                job->commands.clear();
                job->state = failed ? Aborted : Completed;
            }
            changed.notify_all();
        }
    }
public:
    GuestAio(Space& space, GuestStorage& storage) : space(space), storage(storage) {
        // Construct transactionally: failed creation joins the first worker.
        try { for (auto& worker : workers) worker = std::thread([this] { Work(); }); }
        catch (...) { RequestStop(); for (auto& worker : workers) if (worker.joinable()) worker.join(); throw; }
    }
    ~GuestAio() { RequestStop(); for (auto& worker : workers) if (worker.joinable()) worker.join(); }
    void RequestStop() {
        { std::lock_guard lock(mutex); stopping = true; for (auto& [id, job] : jobs) CancelQueued(*job); }
        changed.notify_all();
    }
    u64 Dispatch(std::string_view nid, const std::array<u64, 6>& a, std::stop_token stop = {}) {
        using namespace GuestCpu;
        if (nid == "nu4a0-arQis") {
            return space.WriteData({a[0]}, std::as_bytes(std::span{DefaultParam})) ? 0 : u32(ORBIS_KERNEL_ERROR_EFAULT);
        }
        if (nid == "9WK-vhNXimw") {
            if (u32(a[1]) > 128 || u32(a[2]) > 128 || u32(a[3]) > 1 ||
                (u32(a[3]) && (u32(a[4]) - 1 > 0xffffff || u32(a[5]) - 1 > 0xffffff)))
                return u32(ORBIS_KERNEL_ERROR_EINVAL);
            const std::array<u32, 5> params{u32(a[1]),u32(a[2]),u32(a[3]),a[3] ? u32(a[4]) : 0,a[3] ? u32(a[5]) : 0};
            return space.WriteData({a[0]}, std::as_bytes(std::span{params})) ? 0 : u32(ORBIS_KERNEL_ERROR_EFAULT);
        }
        if (nid == "vYU8P9Td2Zo") {
            if (a[1] != sizeof(DefaultParam)) return u32(ORBIS_KERNEL_ERROR_EINVAL);
            std::array<u32, 15> param{};
            if (!Read(a[0], param)) return u32(ORBIS_KERNEL_ERROR_EFAULT);
            // Nondefault resource scheduling contracts are not implemented.
            if (param != DefaultParam) return u32(ORBIS_KERNEL_ERROR_ENOTSUP);
            std::lock_guard lock(mutex);
            if (stopping) return u32(ORBIS_KERNEL_ERROR_EINTR);
            initialized = true;
            return 0;
        }
        { std::lock_guard lock(mutex);
          if (stopping || stop.stop_requested()) return u32(ORBIS_KERNEL_ERROR_EINTR);
          if (!initialized) return u32(ORBIS_KERNEL_ERROR_ENXIO); }
        const bool read = nid == "HgX7+AORI58" || nid == "lXT0m3P-vs4";
        const bool write = nid == "XQ8C8y+de+E" || nid == "xT3Cpz0yh6Y";
        if (read || write) {
            const s32 count = s32(a[1]);
            if (count < 1 || count > 128) return u32(ORBIS_KERNEL_ERROR_EINVAL);
            const bool multiple = nid == "lXT0m3P-vs4" || nid == "xT3Cpz0yh6Y";
            std::vector<Request> input(count);
            if (!space.ReadData({a[0]}, std::as_writable_bytes(std::span{input}), stop)) return u32(ORBIS_KERNEL_ERROR_EFAULT);
            // Acquire all buffers together: a partially acquired batch may
            // otherwise deadlock a concurrent mapping retirement on later spans.
            std::vector<Space::DataRequest> ranges;
            u64 total{};
            for (const auto& r : input) {
                if (r.offset < 0 || r.bytes < 0 || r.bytes > INT64_MAX - r.offset) return u32(ORBIS_KERNEL_ERROR_EINVAL);
                if (u64(r.bytes) > 256 * 1024 * 1024 - total) return u32(ORBIS_KERNEL_ERROR_EAGAIN);
                total += r.bytes;
                ranges.push_back({{{r.result}, sizeof(Result)}, GuestPermission::Write});
                if (r.bytes) ranges.push_back({{{r.buffer}, u64(r.bytes)}, write ? GuestPermission::Read : GuestPermission::Write});
            }
            ranges.push_back({{{a[3]}, u64(multiple ? count : 1) * 4}, GuestPermission::Write});
            for (size_t i = 0; i < ranges.size(); ++i) {
                const auto& x = ranges[i];
                if (!GuestRange::Checked(x.range.base, x.range.size)) return u32(ORBIS_KERNEL_ERROR_EFAULT);
                for (size_t j = 0; j < i; ++j) {
                    const auto& y = ranges[j];
                    if (x.range.base.value < y.range.End() && y.range.base.value < x.range.End() &&
                        (x.permission == GuestPermission::Write || y.permission == GuestPermission::Write))
                        return u32(ORBIS_KERNEL_ERROR_EINVAL);
                }
            }
            auto pins = space.AcquireDataBatch(ranges, stop);
            if (!pins) return u32(ORBIS_KERNEL_ERROR_EFAULT);
            std::vector<std::shared_ptr<Job>> submitted(multiple ? count : 1);
            for (auto& job : submitted) { job = std::make_shared<Job>(); job->write = write; }
            size_t index{};
            for (s32 i = 0; i < count; ++i) {
                Command c; c.request = input[i]; c.file = storage.AcquirePositioned(c.request.fd);
                c.result = std::move(pins.Value()[index++]);
                if (c.request.bytes) c.data = std::move(pins.Value()[index++]);
                submitted[multiple ? i : 0]->bytes += c.request.bytes;
                submitted[multiple ? i : 0]->commands.push_back(std::move(c));
            }
            std::vector<s32> ids;
            ids.reserve(submitted.size());
            {
                std::lock_guard lock(mutex);
                if (stopping || stop.stop_requested()) return u32(ORBIS_KERNEL_ERROR_EINTR);
                if (total > 512 * 1024 * 1024 - pending_bytes || jobs.size() + submitted.size() > 512 || next_id > INT32_MAX - s32(submitted.size())) return u32(ORBIS_KERNEL_ERROR_EAGAIN);
                // Allocate queue/map metadata before publishing any guest
                // result or ID. On OOM no worker can observe a partial batch.
                const auto previous_queue_size = queue.size();
                try {
                    for (auto& job : submitted) {
                        const s32 id = next_id++; ids.push_back(id);
                        jobs.emplace(id, job); queue.push_back(job);
                    }
                } catch (const std::bad_alloc&) {
                    while (queue.size() > previous_queue_size) queue.pop_back();
                    for (s32 id : ids) jobs.erase(id);
                    return u32(ORBIS_KERNEL_ERROR_ENOMEM);
                }
                pending_bytes += total;
                for (auto& job : submitted)
                    for (auto& c : job->commands) Publish(c, 0, Submitted);
                std::memcpy(pins.Value()[index].WritableBytes().data(), ids.data(), ids.size() * 4);
            }
            changed.notify_all(); return 0;
        }
        const bool multiple = nid == "3Lca1XBrQdY" || nid == "Ft3EtsZzAoY" || nid == "o7O4z3jwKzo" || nid == "lgK+oIWkJyA";
        const bool wait = nid == "KOF-oJbQVvc" || nid == "lgK+oIWkJyA";
        const bool cancel = nid == "fR521KIGgb8" || nid == "3Lca1XBrQdY";
        const bool erase = nid == "5TgME6AYty4" || nid == "Ft3EtsZzAoY";
        const bool poll = nid == "2pOuoWoCxdk" || nid == "o7O4z3jwKzo";
        if (!wait && !cancel && !erase && !poll) return u32(ORBIS_KERNEL_ERROR_ENOSYS);
        const s32 count = multiple ? s32(a[1]) : 1;
        if (count < 1 || count > 128) return u32(ORBIS_KERNEL_ERROR_EINVAL);
        const u32 mode = multiple && wait ? u32(a[3]) : 1;
        if (wait && mode != 1 && mode != 2) return u32(ORBIS_KERNEL_ERROR_EINVAL);
        std::vector<s32> ids(count);
        if (multiple) { if (!space.ReadData({a[0]}, std::as_writable_bytes(std::span{ids}), stop)) return u32(ORBIS_KERNEL_ERROR_EFAULT); }
        else ids[0] = s32(a[0]);
        const u64 timeout_address = wait ? a[multiple ? 4 : 2] : 0;
        u32 usec{};
        if (timeout_address && !Read(timeout_address, usec)) return u32(ORBIS_KERNEL_ERROR_EFAULT);
        const auto begin = std::chrono::steady_clock::now();
        const auto deadline = begin + std::chrono::microseconds(usec);
        std::vector<Space::DataRequest> ranges{{{{a[multiple ? 2 : 1]}, u64(count) * 4}, GuestPermission::Write}};
        if (timeout_address) ranges.push_back({{{timeout_address}, 4}, GuestPermission::Write});
        auto pins = space.AcquireDataBatch(ranges, stop);
        if (!pins) return u32(ORBIS_KERNEL_ERROR_EFAULT);
        std::stop_callback callback(stop, [&] { std::lock_guard lock(mutex); changed.notify_all(); });
        std::unique_lock lock(mutex);
        std::vector<std::shared_ptr<Job>> selected;
        for (s32 id : ids) {
            auto it = jobs.find(id); if (it == jobs.end()) return u32(ORBIS_KERNEL_ERROR_ESRCH);
            selected.push_back(it->second);
        }
        if (cancel) { for (auto& job : selected) CancelQueued(*job); changed.notify_all(); }
        const auto ready = [&] {
            if (stopping || stop.stop_requested()) return true;
            const auto terminal = [](const auto& job) { return job->state >= Completed; };
            return mode == 2 ? std::ranges::any_of(selected, terminal) : std::ranges::all_of(selected, terminal);
        };
        bool timeout{};
        if (wait) {
            if (timeout_address) timeout = !changed.wait_until(lock, deadline, ready);
            else changed.wait(lock, ready);
        }
        std::vector<s32> states;
        for (size_t i = 0; i < selected.size(); ++i) {
            if (erase) {
                const bool done = selected[i]->state >= Completed;
                states.push_back(done ? 0 : ORBIS_KERNEL_ERROR_EBUSY);
                if (done) jobs.erase(ids[i]);
            } else states.push_back(selected[i]->state);
        }
        std::memcpy(pins.Value()[0].WritableBytes().data(), states.data(), states.size() * 4);
        if (timeout_address) {
            const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
            const u32 left = spent >= usec ? 0 : usec - spent;
            std::memcpy(pins.Value()[1].WritableBytes().data(), &left, 4);
        }
        if (stopping || stop.stop_requested()) return u32(ORBIS_KERNEL_ERROR_EINTR);
        return timeout ? u32(ORBIS_KERNEL_ERROR_ETIMEDOUT) : 0;
    }
};
} // namespace Core::HostRuntime
