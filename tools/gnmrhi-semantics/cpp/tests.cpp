#include "recording_backend.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace kh3::gnm;
using namespace kh3::gnm::example;

namespace {
void Check(bool passed, const char* expression) {
    if (!passed) throw std::runtime_error(expression);
}
#define CHECK(...) Check((__VA_ARGS__), #__VA_ARGS__)

template <typename F> void Rejects(F&& action) {
    try { action(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("expected input-domain rejection");
}

std::vector<Event> Packets(const RecordingBackend& backend) {
    std::vector<Event> result;
    for (const auto& event : backend.events) if (event.name == "packet") result.push_back(event);
    return result;
}

void ConstantVersionsAndExtent() {
    RecordingBackend backend;
    LooseConstants constants(std::vector<std::byte>(64, std::byte{0x7b}));
    const std::array<std::byte, 1> x{std::byte{0x58}};
    constants.Write(33, x);
    auto first = constants.Commit(backend, true).value();
    CHECK(first.extent == 48 && first.copied_bytes == 34);
    CHECK(first.descriptor == BufferDescriptor{0, 0x100001, 3, 0x20077fac});
    CHECK(constants.Dirty());
    const auto old_bytes = backend.allocations[0].bytes;
    CHECK(old_bytes[33] == std::byte{0x58} && old_bytes[34] == std::byte{0xcd});
    constants.AcknowledgeBinding();
    constants.Write(0, x);
    auto retained = constants.Commit(backend, false).value();
    CHECK(retained.extent == 48 && retained.copied_bytes == 48);
    CHECK(first.address != retained.address && backend.allocations[0].bytes == old_bytes);
    constants.AcknowledgeBinding();
    constants.Write(0, x);
    auto shrunk = constants.Commit(backend, true).value();
    CHECK(shrunk.extent == 16 && shrunk.copied_bytes == 1);
    constants.AcknowledgeBinding();
    CHECK(!constants.Commit(backend, true));
}

void LargeConstantAllocator() {
    RecordingBackend backend;
    LooseConstants constants(std::vector<std::byte>(0x10020));
    constants.Write(0x10000, std::array<std::byte, 1>{std::byte{1}});
    CHECK(constants.Commit(backend, true)->extent == 0x10010);
    CHECK(backend.allocations[0].kind == "transient-constants");
    Rejects([&] { constants.Write(0, std::vector<std::byte>(0x10000)); });
}

void DescriptorRoutesAndPackets() {
    RecordingBackend backend;
    StageBindings cs(HardwareStage::CS);
    const auto desc = Descriptor::Buffer({0x12345678, 0x100001, 4, 0x20077fac});
    cs.Bind(backend, 0x8004, desc);
    CHECK(Packets(backend)[0].words == std::vector<std::uint32_t>{
        0xc0001000, 0x6875000d, 0xc0047602, 0x244, 0x12345678, 0x100001, 4, 0x20077fac});
    cs.Bind(backend, 0xffff, desc);
    CHECK(cs.Dirty() && Packets(backend).size() == 1);
    ComputeReflection reflection;
    CHECK(cs.CommitCompute(backend, reflection).has_value()); // Empty prefix still clears dirty.
    CHECK(!cs.CommitCompute(backend, reflection));
    StageBindings ps(HardwareStage::PS);
    ps.Bind(backend, 0x8004, desc);
    CHECK(Packets(backend).back().words[2] == 0xc0047600);
    CHECK(Packets(backend).back().words[3] == 0x10);
    Rejects([&] { ps.CommitCompute(backend, reflection); });
}

void TableVersionsAndSubpointers() {
    RecordingBackend backend;
    StageBindings cs(HardwareStage::CS);
    cs.SeedStaging(std::array<std::uint32_t, 16>{});
    cs.Bind(backend, 0x4004, Descriptor::Buffer({10, 11, 12, 13}));
    ComputeReflection reflection;
    reflection.table_dwords = 16;
    reflection.pointer_sgprs = {2, 4, 255, 255, 255};
    reflection.subtable_dwords = {4, 8, 12, 14};
    auto first = cs.CommitCompute(backend, reflection).value();
    CHECK(Packets(backend)[0].words == std::vector<std::uint32_t>{0xc0027602, 0x242, 0, 1});
    CHECK(Packets(backend)[1].words == std::vector<std::uint32_t>{0xc0027602, 0x244, 16, 1});
    cs.Bind(backend, 4, Descriptor::Buffer({20, 21, 22, 23}));
    auto second = cs.CommitCompute(backend, reflection).value();
    CHECK(first.address != second.address && first.words[4] == 10 && second.words[4] == 20);
    CHECK(backend.ReadWords(first.address, 16)[4] == 10);
}

void PackedUniformResources() {
    RecordingBackend backend;
    CapturedResources resources;
    resources.descriptors.emplace(100, Descriptor::Texture({1, 2, 3, 4, 5, 6, 7, 8}));
    resources.descriptors.emplace(101, Descriptor::Buffer({10, 11, 12, 13}));
    resources.descriptors.emplace(102, Descriptor::Buffer({20, 21, 22, 23}));
    UniformBuffer uniform{{}, {100, 101, 102}};
    UniformBindings bindings;
    bindings.buffers[1] = &uniform;
    bindings.dirty_mask = 0b1010;
    ShaderResourceTable tables;
    tables.uniform_mask = 0b0010;
    tables.maps = {{{0, 2, 0x01000003, 0xffffffff},
                    {0, 2, 0x01000104, 0xffffffff},
                    {0, 2, 0x01000202, 0xffffffff}}};
    ComputeReflection reflection;
    reflection.resource_slots = {0xffff, 0xffff, 0xffff, 0x8003, 4};
    reflection.sampler_slots = {0xffff, 0xffff, 12};
    reflection.table_dwords = 16;
    StageBindings stage(HardwareStage::CS);
    stage.SeedStaging(std::array<std::uint32_t, 16>{});
    CommitResourceTables(backend, resources, tables, reflection, bindings, stage);
    CHECK(bindings.dirty_mask == 0);
    CHECK(resources.accesses == std::vector<std::pair<ResourceKind, ResourceHandle>>{
        {ResourceKind::Texture, 100}, {ResourceKind::Srv, 101}, {ResourceKind::Sampler, 102}});
    CHECK(Packets(backend)[0].words[2] == 0xc0087602);
    auto upload = stage.CommitCompute(backend, reflection).value();
    CHECK(upload.words[4] == 10 && upload.words[12] == 20);
    auto entry = ResourceEntry::Decode(0x03012a07);
    CHECK(entry.uniform_slot == 3 && entry.resource_index == 298 && entry.bind_index == 7);
    bindings.dirty_mask = 2;
    tables.maps[0].pop_back();
    Rejects([&] { CommitResourceTables(backend, resources, tables, reflection, bindings, stage); });
}

void MissingStateIsNotInvented() {
    RecordingBackend backend;
    StageBindings cs(HardwareStage::CS);
    cs.Bind(backend, 4, Descriptor::Buffer({1, 2, 3, 4}));
    ComputeReflection reflection;
    reflection.table_dwords = 8;
    Rejects([&] { cs.CommitCompute(backend, reflection); });
    cs.SeedStaging(std::array<std::uint32_t, 4>{});
    reflection.has_unmodeled_extra_bindings = true;
    Rejects([&] { cs.CommitCompute(backend, reflection); });
    Rejects([&] { cs.Bind(backend, 1535, Descriptor::Buffer({1, 2, 3, 4})); });
}

void DynamicLockAndSrvFollowCurrentBacking() {
    MultiBuffer dynamic(Dynamic, 1024, {0x100000, 0x200000, 0x300000}, 0);
    auto never_allocate = [](std::uint32_t) -> GpuAddress { throw std::runtime_error("unexpected allocation"); };
    CHECK(dynamic.Lock(0, 16, never_allocate) == 0x100010);
    CHECK(dynamic.Lock(1, 16, never_allocate) == 0x200010);
    auto srv = RefreshBufferSrv({0x100000, 0xab123000, 64, 0x20077fac}, dynamic.CurrentData());
    CHECK(srv == BufferDescriptor{0x200000, 0xab123000, 64, 0x20077fac});
    CHECK(dynamic.Lock(1, 0, never_allocate) == 0x300000);
    CHECK(dynamic.Lock(1, 0, never_allocate) == 0x100000); // No invented frame wait.
    CHECK(BufferSlotCount(Dynamic | Uav) == 1);
    CHECK(BufferSlotCount(Static | Dynamic) == 1);
    MultiBuffer single(Dynamic | Uav, 1024, {0x400000}, 0);
    CHECK(single.Lock(1, 0, never_allocate) == 0x400000);
}

void VolatileAllocatesOnEachWrite() {
    MultiBuffer buffer(Volatile, 1024, {0}, 0);
    GpuAddress next = 0x100000;
    unsigned allocations = 0;
    auto allocate = [&](std::uint32_t size) { CHECK(size == 1024); ++allocations; return next += 1024; };
    const auto first = buffer.Lock(1, 0, allocate);
    CHECK(buffer.Lock(0, 0, allocate) == first);
    CHECK(buffer.Lock(1, 0, allocate) != first && allocations == 2);
    CHECK(RefreshBufferSrv({0, 0xab123005, 64, 0x20077fac}, 0x123456789a) ==
          BufferDescriptor{0x3456789a, 0xab123012, 64, 0x20077fac});
}

void FlushPacketSequence() {
    RecordingBackend backend;
    std::vector<FlushRange> ranges{{0x123400, 0x2000}};
    FlushPriorGraphics(backend, ranges);
    auto packets = Packets(backend);
    CHECK(ranges.empty() && packets.size() == 4);
    CHECK(packets[0].words == std::vector<std::uint32_t>{0xc0044700, 0x38504, 0, 0x40000001, 1, 0});
    CHECK(packets[1].words == std::vector<std::uint32_t>{0xc0053c00, 0x13, 0, 1, 1, 0xffffffff, 10});
    CHECK(packets[2].words == std::vector<std::uint32_t>{0xc0055800, 0x02003fc0, 0x20, 0, 0x1234, 0, 10});
    CHECK(packets[3].words == std::vector<std::uint32_t>{0xc0055800, 0xc40000, 1, 0, 0, 0, 10});
    FlushAfterCompute(backend);
    CHECK(Packets(backend)[4].words[0] == 0xc0034802);
    CHECK(Packets(backend)[4].words[1] == 0x62f);
    CHECK(Packets(backend).size() == 7);
}

void DispatchEndToEnd() {
    RecordingBackend backend;
    CapturedResources resources;
    GnmRhi rhi(backend, resources);
    LooseConstants constants(std::vector<std::byte>(64));
    ComputeShader shader;
    shader.uses_loose_constants = true;
    shader.backend_token = 7;
    shader.reflection.constant_slot0 = 4;
    shader.reflection.table_dwords = 8;
    shader.reflection.pointer_sgprs[0] = 2;
    ComputeContext context;
    context.shader = &shader;
    context.loose = &constants;
    context.shader_dirty = true;
    context.post_compute_flush = true; // AutoFlush off: no post-EOS.
    context.descriptors.SeedStaging(std::array<std::uint32_t, 8>{});
    constants.Write(0, std::array<std::byte, 1>{std::byte{1}});
    rhi.DispatchCompute(context, 8, 4, 1);
    CHECK(!constants.Dirty() && !context.shader_dirty && !context.descriptors.Dirty());
    CHECK(backend.events.back().name == "sdk-dispatch-direct");
    const auto first_table = backend.Tables()[0];
    constants.Write(0, std::array<std::byte, 1>{std::byte{2}});
    rhi.DispatchCompute(context, 8, 4, 1);
    CHECK(backend.Tables().size() == 2 && backend.Tables()[0].bytes == first_table.bytes);
    CHECK(backend.ReadWords(backend.Tables()[0].address, 8)[4] !=
          backend.ReadWords(backend.Tables()[1].address, 8)[4]);
    CHECK(std::count_if(backend.events.begin(), backend.events.end(),
                       [](const Event& e) { return e.name == "sdk-bind-cs"; }) == 1);
    context.auto_flush = true;
    rhi.DispatchCompute(context, 1, 1, 1);
    CHECK(Packets(backend).back().words[1] == 0xc40000);
}

void IndirectBasePrecedesLcueCommit() {
    RecordingBackend backend;
    CapturedResources resources;
    GnmRhi rhi(backend, resources);
    ComputeShader shader;
    shader.backend_token = 9;
    ComputeContext context;
    context.shader = &shader;
    context.shader_dirty = true;
    rhi.DispatchComputeIndirect(context, 0x1234567800, 12);
    CHECK(backend.events[0].words == std::vector<std::uint32_t>{0xc0004200, 0});
    CHECK(backend.events[1].words == std::vector<std::uint32_t>{0xc0021102, 1, 0x34567800, 0x12});
    CHECK(backend.events[2].name == "sdk-bind-cs");
    CHECK(backend.events[3].name == "sdk-dispatch-indirect");
    CHECK(backend.events[3].words[0] == 12);
}

void RetirementUsesCompletedTicketNotFrameCount() {
    DeferredReleaseQueue queue(5);
    std::vector<ResourceHandle> freed;
    auto release = [&](ResourceHandle handle) { freed.push_back(handle); };
    queue.Enqueue(100);
    CHECK(queue.RetireAndAdvance(4, release) == 5 && freed.empty());
    queue.Enqueue(200);
    CHECK(queue.RetireAndAdvance(5, release) == 6);
    CHECK(freed == std::vector<ResourceHandle>{100} && queue.Pending() == 1);
    CHECK(queue.RetireAndAdvance(6, release) == 7);
    CHECK(freed == std::vector<ResourceHandle>{100, 200} && queue.Pending() == 0);
}

void QueryReadyBitsAndBackendCount() {
    constexpr auto Ready = 1ull << 63;
    std::array<OcclusionPair, 16> pairs;
    pairs.fill({Ready | 10, Ready | 13});
    pairs[8].end = 13;
    CHECK(ReadOcclusion(pairs, false).value() == 24);
    CHECK(!ReadOcclusion(pairs, true));
    pairs[8].end |= Ready;
    CHECK(ReadOcclusion(pairs, true).value() == 48);
    pairs[0].begin = 10;
    CHECK(!ReadOcclusion(pairs, false));
    CHECK(!ReadTimestamp(0, false));
    CHECK(ReadTimestamp(1, false).has_value() && ReadTimestamp(1, false).value() == 0);
    CHECK(ReadTimestamp(8000, false).value() == 10 && ReadTimestamp(9110, true).value() == 10);
}
} // namespace

int main() {
    const std::pair<const char*, void(*)()> cases[]{
        {"constant versions and extent", ConstantVersionsAndExtent},
        {"large constant allocator", LargeConstantAllocator},
        {"descriptor routes and packets", DescriptorRoutesAndPackets},
        {"table versions and subpointers", TableVersionsAndSubpointers},
        {"packed uniform resources", PackedUniformResources},
        {"missing state rejected", MissingStateIsNotInvented},
        {"dynamic Lock and SRV", DynamicLockAndSrvFollowCurrentBacking},
        {"volatile Lock", VolatileAllocatesOnEachWrite},
        {"flush packet sequence", FlushPacketSequence},
        {"two-dispatch integration", DispatchEndToEnd},
        {"indirect ordering", IndirectBasePrecedesLcueCommit},
        {"retirement ticket", RetirementUsesCompletedTicketNotFrameCount},
        {"query readiness", QueryReadyBitsAndBackendCount}};
    unsigned failures = 0;
    for (const auto& [name, test] : cases) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    std::cout << std::size(cases) - failures << '/' << std::size(cases) << " scenarios passed\n";
    return failures == 0 ? 0 : 1;
}
