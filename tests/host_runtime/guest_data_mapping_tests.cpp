// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <cstdio>
#include <future>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include "core/guest_cpu/api/address_space.h"
using namespace Core::GuestCpu;
namespace Core::GuestCpu::Test {
void SetMmapFailureForTest(bool);
void SetMprotectFailureForTest(bool);
}
static int checks{}, failures{};
static void Check(bool ok, const char* name) {
    ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); }
}
struct Sink final : CodeInvalidationSink {
    int calls{};
    std::string_view Name() const override { return "data-test"; }
    Status DiscardTranslations(GuestRange, InvalidationReason) override { ++calls; return Ok(); }
};
int main() {
    const auto p = HostPageSize();
    auto made = GuestAddressSpace::Create({.reservation_size = 512*p});
    if (!made) return 2;
    auto space = std::move(made).Value(); const auto base = space->ReservationBase().value;
    auto range = [&](unsigned i, unsigned n=1) { return GuestRange{{base+i*p}, n*p}; };
    constexpr auto rw = GuestPermission::Read | GuestPermission::Write;
    constexpr auto rx = GuestPermission::Read | GuestPermission::Execute;
    Check(bool(space->Map(range(0), rx)), "initial code");
    Check(bool(space->Map(range(1), rw)), "initial data");
    Sink sink; Check(bool(space->SetCodeInvalidationSink(&sink)), "sink");
    const auto code_generation = space->CodeGeneration();
    {
        auto running = space->AcquireExecutionLease();
        auto io = space->AcquirePinnedSpan(range(1), true);
        Check(bool(running) && bool(io), "live guest and unrelated I/O");
        Check(bool(space->MapFreshData(range(2,3), rw)), "data allocation while guest/I/O live");
        Check(bool(space->ProtectData(range(3), GuestPermission::None)), "new stack guard");
        Check(bool(space->ValidateRange(range(2), rw)), "left split");
        Check(!space->ValidateRange(range(3), rw), "guard protected");
        Check(bool(space->ValidateRange(range(4), rw)), "right split");
        Check(!space->ProtectData(range(1), GuestPermission::Read), "same buffer pin excluded");
        Check(!space->MapFreshData(range(0), rw), "code replacement excluded");
        Check(!space->MapFreshData(range(2), rw), "data replacement excluded");
        Check(!space->MapFreshData(range(5), rx), "executable publication excluded");
        Check(!space->ProtectData(range(0), rw), "cannot remove execute without retirement");
        Check(!space->ProtectData(range(2), rx), "cannot add execute without publication");
        Check(!space->ProtectData(range(4,2), GuestPermission::Read), "hole rejected");
        Check(space->CodeGeneration()==code_generation && sink.calls==0, "no cache flush");
        std::atomic<unsigned> writes{};
        std::jthread worker([&](std::stop_token stop) {
            std::array<std::byte,8> value{};
            while (!stop.stop_requested()) if (space->Write({base+p},value)) ++writes;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!writes.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        for (unsigned i=8;i<108;++i)
            Check(bool(space->MapFreshData(range(i),rw)), "concurrent data allocation");
        worker.request_stop(); worker.join();
        Check(writes>0, "independent writes progress");
    }
    // Shared backing mapping itself never writes the alias or retires code.
    char path[]="shad-data-map-XXXXXX";
    int fd=mkstemp(path); if (fd<0) return 2; unlink(path); ftruncate(fd,2*p);
    const std::array<std::byte,4> marker{std::byte{1},std::byte{2},std::byte{3},std::byte{4}};
    pwrite(fd,marker.data(),marker.size(),p);
    Check(bool(space->MapFreshData(range(120),rw,fd,p)), "shared backing offset");
    std::array<std::byte,4> read{};
    Check(bool(space->Read({base+120*p},read)) && read==marker, "exact shared bytes");
    close(fd);
    Check(!space->MapFreshData({{base+1},p},rw), "alignment");
    Check(!space->MapFreshData({{UINT64_MAX-p+1},p},rw), "overflow");
    Check(!space->MapFreshData(range(512),rw), "outside reservation");
    Check(!space->MapFreshData(range(121,0),rw), "zero size");
    Check(!space->MapFreshData(range(121),rw,-1,p), "anonymous offset invalid");
#if defined(GUEST_CPU_TEST_HOOKS)
    const auto before = space->MappingGeneration();
    Test::SetMmapFailureForTest(true);
    auto failed_map = space->MapFreshData(range(121),rw);
    Test::SetMmapFailureForTest(false);
    Check(!failed_map && failed_map.Category()==ErrorCategory::OutOfMemory,
          "additive mmap ENOMEM remains recoverable");
    Check(space->MappingGeneration()==before && !space->Query(range(121).base) &&
          !space->HasPoisonedCode(), "failed map leaves ledger and code untouched");
    Test::SetMprotectFailureForTest(true);
    auto failed_protect = space->ProtectData(range(2),GuestPermission::None);
    Test::SetMprotectFailureForTest(false);
    Check(!failed_protect && failed_protect.Category()==ErrorCategory::PermissionDenied,
          "data protection EACCES remains recoverable");
    Check(space->MappingGeneration()==before && bool(space->ValidateRange(range(2),rw)),
          "failed protect leaves ledger unchanged");
#endif
    auto token=space->Quiesce(1'000'000);
    Check(bool(token), "token admission");
    Check(!space->MapFreshData(range(121),rw), "token excludes additive mutation");
    Check(!space->ProtectData(range(2),rw), "token excludes data protect");
    token = MakeError(ErrorCategory::InvalidArgument,"test","release");
    Check(space->CodeGeneration()==code_generation && sink.calls==0, "all data changes preserve code generation");

    // One held I/O buffer blocks ONLY its own retirement. CPU execution, unrelated
    // writes and three other mapping edits continue; multi-buffer admission has
    // no partial pins while waiting, and cancellation leaves the old map intact.
    {
        auto running = space->AcquireExecutionLease();
        auto io = space->AcquireDataSpan(range(1), true);
        Check(bool(running) && bool(io), "data references during execution");
        const auto old = space->Query(range(1).base).Value().mapping_generation;
        auto edit = std::async(std::launch::async, [&] {
            return space->UpdateDataMapping(GuestAddressSpace::VmOperation::Map, range(1), rw);
        });
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!space->Counts().retiring_ranges && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        Check(space->Counts().retiring_ranges == 1, "overlap retiring handshake");
        Check(edit.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready,
              "old buffer retained");
        Check(bool(space->WriteData(range(2).base, marker)),
              "unrelated audio/mutex write progresses");
        Check(bool(space->UpdateDataMapping(GuestAddressSpace::VmOperation::Map, range(121), rw)),
              "unrelated map progresses");
        Check(bool(space->UpdateDataMapping(GuestAddressSpace::VmOperation::Protect, range(121),
                                            GuestPermission::Read)),
              "unrelated protect progresses");
        Check(bool(space->UpdateDataMapping(GuestAddressSpace::VmOperation::Unmap, range(121),
                                            GuestPermission::None)),
              "unrelated unmap progresses");
        const std::array<GuestAddressSpace::DataRequest, 2> requests{
            {{range(2), GuestPermission::Write}, {range(1), GuestPermission::Write}}};
        std::stop_source stop;
        std::promise<void> entered;
        auto batch = std::async(std::launch::async, [&] {
            entered.set_value();
            return space->AcquireDataBatch(requests, stop.get_token());
        });
        entered.get_future().wait();
        Check(batch.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready,
              "batch waits for last range");
        Check(space->Counts().live_pins == 1, "no partial batch pins");
        stop.request_stop();
        Check(!batch.get(), "batch wait cancellation");
        // Completion reentry may read other inputs while retaining the old buffer.
        Check(bool(space->ReadData(range(1).base, read)),
              "retiring range existing owner completes");
        io = MakeError(ErrorCategory::InvalidArgument, "test", "release");
        Check(bool(edit.get()), "overlap retires after reference release");
        const GuestAddressSpace::MappingIdentity identity{range(1).base.value, range(1).End(), old};
        const GuestAddressSpace::DataRequest stale{
            range(1), GuestPermission::Write, {&identity, 1}};
        Check(!space->AcquireDataBatch({&stale, 1}), "same VA stale async write refused");
        const std::array<GuestAddressSpace::DataRequest, 2> bad{
            {{range(2), GuestPermission::Write}, {range(511), GuestPermission::Write}}};
        Check(!space->AcquireDataBatch(bad) && space->Counts().live_pins == 0,
              "invalid final parameter atomic rejection");
        Check(space->CodeGeneration() == code_generation && sink.calls == 0,
              "range retirement never flushes code");
        Check(!space->UpdateDataMapping(GuestAddressSpace::VmOperation::Map, range(0), rw),
              "code replacement needs explicit publication");
    }
    {
        const auto before_cancel = space->Query(range(2).base).Value().mapping_generation;
        auto held = space->AcquireDataSpan(range(2), true);
        std::stop_source stop;
        auto retiring = std::async(std::launch::async, [&] {
            return space->UpdateDataMapping(GuestAddressSpace::VmOperation::Map, range(2), rw, -1,
                                            0, stop.get_token());
        });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!space->Counts().retiring_ranges && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        Check(space->Counts().retiring_ranges == 1, "cancel edit entered");
        stop.request_stop();
        held = MakeError(ErrorCategory::InvalidArgument, "test", "release alongside cancellation");
        Check(!retiring.get(), "cancellation wins before mapping commit even when pins drained");
        Check(space->Query(range(2).base).Value().mapping_generation == before_cancel &&
                  !space->Counts().retiring_ranges,
              "cancelled edit retains identity and reopens admission");
    }
    // Copy/observer work is outside the metadata lock, but still retains backing.
    struct Observer final : MemoryObserver {
        GuestAddressSpace* space{};
        bool observed{};
        std::string_view Name() const override {
            return "reentrant-copy";
        }
        void OnGuestWrite(GuestRange r) override {
            observed = space->Counts().live_pins > 0 && bool(space->Query(r.base));
        }
    } observer;
    observer.space = space.get();
    space->AddObserver(&observer);
    Check(bool(space->WriteData(range(2).base, marker)) && observer.observed,
          "observer unlocked with live mapping");
    space->RemoveObserver(&observer);
#if defined(GUEST_CPU_TEST_HOOKS)
    const auto old_data = space->Query(range(2).base).Value().mapping_generation;
    Test::SetMmapFailureForTest(true);
    auto replacement = space->UpdateDataMapping(GuestAddressSpace::VmOperation::Map, range(2), rw);
    Test::SetMmapFailureForTest(false);
    Check(!replacement && bool(space->ReadData(range(2).base, read)) && read == marker &&
              space->Query(range(2).base).Value().mapping_generation == old_data &&
              space->Counts().retiring_ranges == 0 && !space->HasPoisonedCode(),
          "failed replacement preserves bytes/identity/admission");
#endif
    {
        const std::array<GuestRange, 2> mixed{range(2), range(0)};
        auto denied = space->PrepareDataMapping(mixed);
        Check(!denied && denied.Category() == ErrorCategory::Unsupported &&
                  space->Counts().retiring_ranges == 0,
              "mixed code/data preparation closes no partial ranges");
        const std::array<GuestRange, 2> regions{range(2, 3), range(8)};
        auto prepared = space->PrepareDataMapping(regions);
        Check(bool(prepared) && space->Counts().retiring_ranges == 2, "prepare all data regions");
        auto running = space->AcquireExecutionLease();
        Check(bool(running), "prepared data does not park unrelated guest execution");
        Check(bool(space->UpdateDataMapping(GuestAddressSpace::VmOperation::Map, range(3), rw)),
              "prepared owner commits middle split");
        Check(bool(space->UpdateDataMapping(GuestAddressSpace::VmOperation::Protect, range(4),
                                            GuestPermission::Read)),
              "prepared owner commits next segment without reopening admission");
        Check(space->Counts().retiring_ranges == 2,
              "outer ticket retains all regions across commits");
        Check(bool(space->WriteData(range(9).base, marker)),
              "unrelated write during prepared transaction");
        std::stop_source cancel;
        auto foreign = std::async(std::launch::async, [&] {
            return space->UpdateDataMapping(GuestAddressSpace::VmOperation::Map, range(3), rw, -1,
                                            0, cancel.get_token());
        });
        Check(foreign.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
              "another physical owner cannot borrow preparation");
        cancel.request_stop();
        Check(!foreign.get(), "foreign mutation cancellation");
        std::stop_source access_cancel;
        auto reader = std::async(std::launch::async, [&] {
            return space->AcquireDataSpan(range(3), false, access_cancel.get_token());
        });
        Check(reader.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
              "new reader excluded until entire ledger commit");
        access_cancel.request_stop();
        Check(!reader.get(), "prepared range read cancellation");
        auto moved = std::move(prepared).Value();
        Check(space->Counts().retiring_ranges == 2, "moving handle retains closure");
        moved.reset();
        Check(space->Counts().retiring_ranges == 0 && bool(space->ReadData(range(3).base, read)),
              "ticket release reopens all regions");
    }
    {
        const auto single = range(2);
        auto pin = space->AcquireDataSpan(single, true);
        std::stop_source cancel;
        auto preparing = std::async(std::launch::async, [&] {
            return space->PrepareDataMapping({&single, 1}, false, cancel.get_token());
        });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!space->Counts().retiring_ranges && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        Check(space->Counts().retiring_ranges == 1, "preparation drain entered");
        Check(bool(space->AcquireExecutionLease()), "new Run admission while data pin drains");
        cancel.request_stop();
        Check(!preparing.get() && !space->Counts().retiring_ranges,
              "cancelled preparation reopens range with old pin retained");
    }
    {
        auto other = GuestAddressSpace::Create({.reservation_size = p});
        auto owned = std::move(other).Value();
        const GuestRange region{owned->ReservationBase(), p};
        auto late = owned->PrepareDataMapping({&region, 1});
        Check(bool(late), "unmapped data retirement");
        owned.reset();
        late.Value().reset();
        Check(true, "retirement outliving its address space releases safely");
    }
    space->ClearCodeInvalidationSink(&sink);
    std::printf("GUEST_DATA_MAPPING checks=%d failures=%d\n",checks,failures);
    return failures ? 1 : 0;
}
