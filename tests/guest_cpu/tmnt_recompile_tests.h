// SPDX-License-Identifier: GPL-2.0-or-later
// Differential replay against ORIGINAL game machine code, in isolated FEX VM.
// Input image is extracted locally by make_recompile_fixture.py; never bundled.
#include "core/guest_cpu/hle/veneer_allocator.h"

int RunRecompileTests(const std::filesystem::path& root) {
    using Json = nlohmann::json;
    Harness h(root);
    const auto metadata = Json::parse(std::ifstream(root / "recompile.json"));
    Check("differential input image SHA", FileSha256(root / "target.bin") == metadata.at("image_sha256").get<std::string>());
    const auto scratch = h.base + 0x7000000;
    Must(h.space->Map({GuestAddress{scratch}, 0x10000}, GuestPermission::Read | GuestPermission::Write));
    struct Mock final : Hle::HleCallAdapter {
        unsigned op{};
        uint64_t scratch{}, grow_result{0x1234567801ULL}, draw_words{3};
        std::vector<uint64_t>* events{};
        bool SignatureSupported() const noexcept override { return true; }
        std::string SignatureDescription() const override { return "isolated TMNT replay effect recorder"; }
        Status Invoke(Hle::HleCallFrame& f) const override {
            Hle::CallCursor cursor(f);
            const unsigned argc = op == 0 ? 3 : op == 1 ? 2 : op == 2 ? 9 : 10;
            std::vector<uint64_t> a;
            for (unsigned i = 0; i < argc; ++i) a.push_back(Must(cursor.NextInteger()));
            events->push_back(op);
            auto read = [&](uint64_t address, size_t bytes) {
                Bytes result(bytes); Must(f.space->Read(GuestAddress{address}, result)); return result;
            };
            auto write64 = [&](uint64_t address, uint64_t value) {
                Must(f.space->Write(GuestAddress{address}, std::as_bytes(std::span{&value, 1})));
            };
            uint64_t result;
            if (op == 0) {
                events->insert(events->end(), a.begin(), a.end());
                if (grow_result & 255) { write64(a[0] + 16, scratch + 0x3000); write64(a[0] + 8, scratch + 0x5000); }
                result = grow_result;
            } else if (op == 1) {
                events->push_back(a[0]); events->push_back(uint32_t(a[1]));
                const uint32_t words[] = {0xabcdef01, 0x98765432, 0x13579bdf};
                Must(f.space->Write(GuestAddress{a[0]}, std::as_bytes(std::span{words})));
                result = draw_words;
            } else {
                const unsigned shift = op == 3 ? 1 : 0;
                if (shift) events->push_back(a[0]);
                const auto n = uint32_t(a[shift]);
                if (n > 16) throw std::runtime_error("invalid replay array count");
                events->push_back(n);
                for (unsigned j = 1; j <= 4; ++j) {
                    const auto values = read(a[j + shift], n * (j % 2 ? 8 : 4));
                    for (auto b : values) events->push_back(std::to_integer<unsigned char>(b));
                }
                for (unsigned j = 5 + shift; j < argc; ++j) events->push_back(a[j]);
                result = 0x1234567881234567ULL;
            }
            f.registers.Set(Gpr::Rax, result);
            return Ok();
        }
    };
    auto* registry = static_cast<Hle::HleCallRegistry*>(Fex::FexHleRegistryPointer(*h.cpu));
    std::vector<uint64_t> events;
    std::array<std::shared_ptr<Mock>, 4> mocks;
    const auto veneers = h.base + 0x6000000;
    Must(h.space->Map({GuestAddress{veneers}, 4096}, GuestPermission::Read | GuestPermission::Write));
    for (unsigned i = 0; i < 4; ++i) {
        auto m = mocks[i] = std::make_shared<Mock>(); m->op = i; m->scratch = scratch; m->events = &events;
        const auto id = Must(registry->Adopt(m, "ReplayMock" + std::to_string(i)));
        const auto code = Hle::HleVeneerAllocator::Encode(id);
        Must(h.space->Write(GuestAddress{veneers + i * 16}, code));
    }
    Must(h.space->Protect({GuestAddress{veneers}, 4096}, GuestPermission::Read | GuestPermission::Execute));
    {
        auto token = Must(h.cpu->QuiesceContext(0));
        // Patch only the fixture's loader relocations. Original function code,
        // instructions, branches and stack layout remain byte-for-byte intact.
        auto pointer = [&](uint64_t offset, uint64_t value) {
            const auto address = h.target + offset;
            const auto page = address & ~4095ULL;
            Must(h.space->UpdateVmUnderToken(token, GuestAddressSpace::VmOperation::Protect,
                {GuestAddress{page}, 4096}, GuestPermission::Read | GuestPermission::Write));
            Must(h.space->PublishCode(token, {GuestAddress{address}, 8}, std::as_bytes(std::span{&value, 1})));
            Must(h.space->UpdateVmUnderToken(token, GuestAddressSpace::VmOperation::Protect,
                {GuestAddress{page}, 4096}, GuestPermission::Read));
        };
        pointer(metadata.at("canary_got"), scratch + 0x50);
        pointer(metadata.at("draw_got"), veneers + 16);
        pointer(metadata.at("submit_got"), veneers + 32);
        pointer(metadata.at("device_got"), veneers + 48);
        h.Install(token);
    }
    auto put = [](Bytes& bytes, size_t offset, auto value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
    unsigned cases = 0;
    for (unsigned mode = 0; mode < 5; ++mode) {
        // Boundary geometry, callback success/failure with nonzero high RAX,
        // output-word overflow, 0/15 prior buffers and both submit ABIs.
        for (unsigned scenario = 0; scenario < 8; ++scenario) {
            Bytes initial(0x10000);
            for (size_t i = 0; i < initial.size(); ++i) initial[i] = std::byte((i * 13 + 37) & 255);
            const uint64_t c = scratch + 0x100, data = scratch + 0x1000;
            const auto words = scenario % 4 == 0 ? 63 : scenario % 4 == 1 ? 64 : scenario % 4 == 2 ? 255 : 256;
            put(initial, 0x100, data - 0x80);
            put(initial, 0x108, data + uint64_t(words) * 4);
            put(initial, 0x110, data);
            put(initial, 0x118, veneers);
            put(initial, 0x120, uint64_t(0x9876543212345678));
            put(initial, 0x140, data + 0x200);
            put(initial, 0x150, data + 0x208);
            put(initial, 0x190, data - 0x10);
            put(initial, 0x198, data + 0x204);
            const uint32_t count = scenario & 1 ? 15 : 0;
            put(initial, 0x2c4, count);
            for (uint32_t i = 0; i < count; ++i) {
                put(initial, 0x1c4 + i * 16, uint32_t(i * 3));
                put(initial, 0x1c8 + i * 16, uint32_t(i & 1 ? 0xffffffff : i));
                put(initial, 0x1cc + i * 16, uint32_t(i * 5));
                put(initial, 0x1d0 + i * 16, uint32_t(i & 1 ? 0 : 3));
            }
            mocks[0]->grow_result = scenario & 4 ? 0xaabbccdd00ULL : 0xaabbccdd01ULL;
            mocks[1]->draw_words = scenario == 7 ? 0xffffffff : 3;
            Bytes before, after;
            std::vector<uint64_t> reference_events;
            uint64_t reference_result{};
            for (bool enabled : {false, true}) {
                { auto token = Must(h.cpu->QuiesceContext(0)); h.manager->SetEnabled(enabled, token); }
                Must(h.space->Write(GuestAddress{scratch}, initial)); events.clear();
                uint64_t result;
                if (mode == 0) result = h.Call(h.Address("write_label"), {c, 0x80112233, 0x1122334455667788, 0xf1234567, 0xa1234567});
                else if (mode == 1) result = h.Call(h.Address("default_state"), {c});
                else if (mode <= 3) result = h.Call(h.Address("submit_packets"), {mode == 3 ? 0xabcd : 0ULL, 0xfedcba98, 0x81234567, 0x12345678, 0x8877665544332211, c});
                else result = h.Call(h.manager->Export("replay_submit_label"), {h.Address("submit_label"), c, data + 0x7000});
                Bytes memory(initial.size()); Must(h.space->Read(GuestAddress{scratch}, memory));
                if (!enabled) { reference_result = result; reference_events = events; before = std::move(memory); }
                else {
                    Check("original machine code vs recompiled: return/memory/ordered effects", result == reference_result && memory == before && events == reference_events);
                    if (result != reference_result || memory != before || events != reference_events)
                        std::printf("mode=%u scenario=%u return=%llx/%llx memory=%d effects=%d\n", mode, scenario,
                            (unsigned long long)reference_result, (unsigned long long)result, memory == before, events == reference_events);
                }
            }
            ++cases;
        }
    }
    std::printf("RECOMPILE_DIFFERENTIAL cases=%u checks=%u failures=%u\n", cases, checks, failures);
    return failures ? 1 : 0;
}
