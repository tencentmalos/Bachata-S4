// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/mman.h>
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/specialization.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/tile.h"
using namespace Shader;
using namespace Core;
static unsigned checks{}, failures{};
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__)) {                                                                      \
            ++failures;                                                                            \
            std::printf("FAIL %d: %s\n", __LINE__, #__VA_ARGS__);                                  \
        }                                                                                          \
    } while (0)
struct TestMemory : GuestMemoryBackend {
    u8* allocation =
        static_cast<u8*>(mmap(nullptr, 0x24000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    u8* base = reinterpret_cast<u8*>((u64(allocation) + 0x3fff) & ~0x3fffull);
    std::vector<u8> backing = std::vector<u8>(0x20000);
    ~TestMemory() {
        munmap(allocation, 0x24000);
    }
    u8* BackingBase() const override {
        return const_cast<u8*>(backing.data());
    }
    boost::icl::interval_set<VAddr> UsableRegions() const override {
        boost::icl::interval_set<VAddr> result;
        result.add(boost::icl::interval<VAddr>::right_open(u64(base), u64(base) + 0x20000));
        return result;
    }
    bool OwnsRange(VAddr a, u64 n) const override {
        return a >= u64(base) && a - u64(base) < 0x20000 && n <= 0x20000 - (a - u64(base));
    }
    void* Map(VAddr a, u64 n, PAddr, bool) override {
        if (!OwnsRange(a, n) || mprotect(reinterpret_cast<void*>(a), n, PROT_READ | PROT_WRITE))
            throw std::runtime_error("map");
        return reinterpret_cast<void*>(a);
    }
    void* MapFile(VAddr, u64, u64, u32, uintptr_t, bool) override {
        throw std::runtime_error("unused");
    }
    void Unmap(VAddr a, u64 n) override {
        mprotect(reinterpret_cast<void*>(a), n, PROT_NONE);
    }
    void Protect(VAddr a, u64 n, MemoryPermission p) override {
        mprotect(reinterpret_cast<void*>(a), n, int(p));
    }
};
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestMemory backend;
    if (backend.allocation == MAP_FAILED)
        return 2;
    MemoryManager memory(&backend);
    Memory::Binding binding(memory);
    const u64 base = u64(backend.base);
    void* mapped{};
    CHECK(memory.MapMemory(&mapped, base, 0x8000, MemoryProt::CpuReadWrite, MemoryMapFlags::NoFlags,
                           VMAType::File, "srt-test") == 0);
    auto* table = reinterpret_cast<u32*>(base);
    auto* child = reinterpret_cast<u32*>(base + 0x100);
    const u64 child_pointer = u64(child) | 0x1234000000000000ULL;
    std::memcpy(table, &child_pointer, 8);
    table[2] = 6;
    child[0] = 55;
    child[1] = 66;
    child[2] = 33;
    child[6] = 77;
    u64 checked{};
    CHECK(memory.TryReadSrtMemory(base, &checked, 8) && checked == child_pointer);
    CHECK(!memory.TryReadSrtMemory(base + 0x8000, &checked, 8));
    CHECK(!memory.TryReadSrtMemory(UINT64_MAX - 3, &checked, 8));
    CHECK(memory.Protect(base + 0x4000, 0x4000, MemoryProt::NoAccess) == 0);
    CHECK(!memory.TryReadSrtMemory(base + 0x4000, &checked, 4));
    // A guest SRT cannot escape into a valid host allocation outside the guest VM.
    CHECK(!memory.TryReadSrtMemory(u64(&checks), &checked, 4));
    // Exact Android fallback rejects inaccessible native pages without delivering SIGSEGV.
    mprotect(reinterpret_cast<void*>(base), 0x4000, PROT_NONE);
    CHECK(!memory.TryReadSrtMemory(base, &checked, 8));
    mprotect(reinterpret_cast<void*>(base), 0x4000, PROT_READ | PROT_WRITE);
    // Physical SRT reads use the owned backing alias despite CPU watch protection.
    CHECK(memory.Allocate(0, 0x20000, 0x4000, 0x4000, 0) == 0);
    CHECK(memory.MapMemory(&mapped, base + 0x10000, 0x4000, MemoryProt::CpuReadWrite,
                           MemoryMapFlags::NoFlags, VMAType::Direct, "srt-direct", false, 0) == 0);
    const u32 sentinel = 0xfeedbeef;
    std::memcpy(backend.backing.data(), &sentinel, 4);
    mprotect(reinterpret_cast<void*>(base + 0x10000), 0x4000, PROT_NONE);
    CHECK(memory.TryReadSrtMemory(base + 0x10000, &checked, 4) && u32(checked) == sentinel);

    std::array<u32, 16> ud{};
    const u64 tagged = base | 0xffff000000000000ULL;
    std::memcpy(ud.data(), &tagged, 8);
    ud[3] = 1;
    Info info;
    info.user_data = ud;
    Common::ObjectPool<IR::Inst> pool(64);
    IR::Block block(pool);
    IR::IREmitter ir(block);
    IR::Program program(info);
    program.post_order_blocks.push_back(&block);
    const auto root =
        ir.CompositeConstruct(ir.GetUserData(IR::ScalarReg(0)), ir.GetUserData(IR::ScalarReg(1)));
    const auto lo = ir.ReadConst(root, ir.Imm32(0u));
    const auto hi = ir.ReadConst(root, ir.Imm32(1u));
    const auto index = ir.ReadConst(root, ir.Imm32(2u));
    const auto nested = ir.CompositeConstruct(lo, hi);
    const auto c0 = ir.ReadConst(nested, ir.Imm32(0u));
    const auto c1 = ir.ReadConst(nested, ir.Imm32(1u));
    const auto dynamic = ir.ReadConst(nested, IR::U32{ir.IAdd(index, ir.Imm32(0u))});
    const auto ud_dynamic = ir.ReadConst(nested, ir.GetUserData(IR::ScalarReg(3)));
    const auto duplicate = ir.ReadConst(nested, ir.Imm32(0u));
    IR::BufferInstInfo flags{};
    flags.sharp_source.Assign(1);
    const auto sharp = ir.ReadConstBuffer(nested, ir.Imm32(1u), flags);
    const auto index_alias = ir.ReadConstBuffer(root, ir.Imm32(2u), flags);
    const auto alias_dynamic = ir.ReadConst(nested, index_alias);
    const auto lo_alias = ir.ReadConstBuffer(root, ir.Imm32(0u), flags);
    const auto hi_alias = ir.ReadConstBuffer(root, ir.Imm32(1u), flags);
    const auto nested_alias = ir.CompositeConstruct(lo_alias, hi_alias);
    const auto alias_child = ir.ReadConst(nested_alias, ir.Imm32(2u));
    // Shader loop indices cannot be evaluated by the CPU SRT walker. Keep the
    // read (and reads through that pointer) on the GPU, while flattening the
    // independent constant/user-data loads from the same table.
    auto* phi = &*block.PrependNewInst(block.begin(), IR::Opcode::Phi);
    phi->SetFlags(IR::Type::U32);
    phi->AddPhiOperand(&block, ir.Imm32(0u));
    const auto next = IR::U32{ir.IAdd(IR::U32{IR::Value{phi}}, ir.Imm32(1u))};
    phi->AddPhiOperand(&block, next);
    const auto loop_offset = IR::U32{ir.IAdd(ir.Imm32(2u), IR::U32{IR::Value{phi}})};
    const auto gpu_lo = ir.ReadConst(root, loop_offset);
    const auto gpu_hi = ir.ReadConst(root, next);
    const auto gpu_pointer = ir.CompositeConstruct(gpu_lo, gpu_hi);
    const auto gpu_child = ir.ReadConst(gpu_pointer, ir.Imm32(0u));
    const auto dependent = ir.ReadConst(root, gpu_lo);
    EmulatorSettings.SetDirectMemoryAccessEnabled(true);
    Optimization::FlattenExtendedUserdataPass(program);
    CHECK(gpu_lo.Inst()->Flags<u16>() == 0 && gpu_hi.Inst()->Flags<u16>() == 0);
    CHECK(gpu_child.Inst()->Flags<u16>() == 0 && dependent.Inst()->Flags<u16>() == 0);
    CHECK(info.srt_info.portable.Validate(info.srt_info.flattened_bufsize_dw));
    auto value = [&](auto v) { return info.flattened_ud_buf[v.Inst()->template Flags<u16>()]; };
    CHECK(value(c0) == 55 && value(c1) == 66 && value(dynamic) == 77);
    CHECK(duplicate.Inst()->Flags<u16>() == c0.Inst()->Flags<u16>());
    CHECK(c1.Inst()->Flags<u16>() == c0.Inst()->Flags<u16>() + 1);
    CHECK(info.flattened_ud_buf[sharp.Inst()->Flags<IR::BufferInstInfo>().flatbuf_off_dw] == 66);
    CHECK(value(ud_dynamic) == 66);
    CHECK(value(alias_dynamic) == 77 && value(alias_child) == 33);
    child[6] = 88;
    child[2] = 99;
    ud[3] = 2;
    info.RefreshFlatBuf();
    CHECK(value(dynamic) == 88 && value(ud_dynamic) == 99);
    CHECK(value(alias_dynamic) == 88 && value(alias_child) == 99);
    // Cache roundtrip contains no executable pointers and re-evaluates current guest tables.
    Serialization::Archive ar;
    info.srt_info.Serialize(ar);
    auto bytes = ar.TakeOff();
    for (size_t n = 0; n < bytes.size(); ++n) {
        Serialization::Archive truncated(std::vector<u8>(bytes.begin(), bytes.begin() + n));
        PersistentSrtInfo rejected;
        CHECK(!rejected.Deserialize(truncated));
    }
    Serialization::Archive input(std::move(bytes));
    PersistentSrtInfo restored;
    CHECK(restored.Deserialize(input));
    CHECK(restored.portable.Validate(restored.flattened_bufsize_dw) && !restored.walker_func);
    std::vector<u32> flat(restored.flattened_bufsize_dw);
    std::copy(ud.begin(), ud.end(), flat.begin());
    restored.portable.Run(ud, flat, ReadSrtGuestMemory);
    CHECK(flat == info.flattened_ud_buf);
    // Failed reads zero only the unavailable table values, as desktop walker faults do.
    const u64 invalid = 0x1234;
    std::memcpy(table, &invalid, 8);
    info.RefreshFlatBuf();
    CHECK(value(c0) == 0 && value(dynamic) == 0);
    auto malformed = restored.portable;
    malformed.expressions[0].op = PortableSrt::Op::Add;
    malformed.expressions[0].a = 0;
    CHECK(!malformed.Validate(restored.flattened_bufsize_dw));
    malformed = restored.portable;
    malformed.commands.back().kind = PortableSrt::Kind::Copy;
    CHECK(!malformed.Validate(restored.flattened_bufsize_dw));
    CHECK(RegisterWalkerCode(reinterpret_cast<const u8*>("x86"), 3) == nullptr);

    using Op = PortableSrt::Op;
    const auto expr = [&](Op op, u32 a, u32 b, u32 c = 0) {
        PortableSrt p;
        p.expressions = {{Op::Constant, a}, {Op::Constant, b}, {Op::Constant, c}, {op, 0, 1, 2}};
        return p.Evaluate(3, {});
    };
    CHECK(expr(Op::Add, UINT32_MAX, 1) == 0);
    CHECK(expr(Op::Sub, 0, 1) == UINT32_MAX);
    CHECK(expr(Op::Mul, 0x80000000, 2) == 0);
    CHECK(expr(Op::Shl, 3, 33) == 6);
    CHECK(expr(Op::Shr, 0x80000000, 31) == 1);
    CHECK(expr(Op::And, 7, 6) == 6);
    CHECK(expr(Op::Or, 1, 4) == 5);
    CHECK(expr(Op::Xor, 7, 6) == 1);
    CHECK(expr(Op::Not, 0, 0) == UINT32_MAX);
    CHECK(expr(Op::Min, 5, 6) == 5);
    CHECK(expr(Op::Max, 5, 6) == 6);
    CHECK(expr(Op::Extract, 0xabcdef, 4, 8) == 0xde);
    CHECK(expr(Op::Extract, 0xabcdef, 0, 32) == 0xabcdef);
    CHECK(expr(Op::Extract, 7, 0, 0) == 0);

    // Consecutive constant-offset copies (a sharp) are read with one call; a failed batch reads
    // each dword on its own, so the flat buffer matches per-dword copies exactly.
    {
        constexpr u64 table_address = 0x1000;
        std::array<u32, 16> table{};
        for (u32 i = 0; i < table.size(); ++i)
            table[i] = 0xA0000000u + i;
        PortableSrt p;
        // e0: offset 0 (user data pointer), e1..e8: table dwords 0..7, e9: dword 12, e10: flat[16]
        p.expressions.push_back({Op::Constant, 0});
        for (u32 i = 0; i < 8; ++i)
            p.expressions.push_back({Op::Constant, i});
        p.expressions.push_back({Op::Constant, 12});
        p.expressions.push_back({Op::Flat, 16});
        p.commands.push_back({PortableSrt::Kind::Push, 0, 0});
        for (u32 i = 0; i < 8; ++i) // one run of 8 dwords into flat[16..23]
            p.commands.push_back({PortableSrt::Kind::Copy, 1 + i, 16 + i});
        p.commands.push_back({PortableSrt::Kind::Copy, 9, 30}); // breaks the run
        p.commands.push_back({PortableSrt::Kind::Copy, 10, 31}); // reads flat[16]: needs the flush
        p.commands.push_back({PortableSrt::Kind::Pop, 0, 0});
        CHECK(p.Validate(32));
        const std::array<u32, 2> user = {u32(table_address), u32(table_address >> 32)};
        u32 calls{};
        u32 fail_bytes_over{UINT32_MAX};
        u64 fail_address{};
        const auto reader = [&](u64 address, void* data, size_t size) {
            ++calls;
            if (size > fail_bytes_over || (fail_address && address <= fail_address &&
                                           fail_address < address + size))
                return false;
            if (address < table_address || address + size > table_address + sizeof(table))
                return false;
            std::memcpy(data, reinterpret_cast<const u8*>(table.data()) + (address - table_address),
                        size);
            return true;
        };
        std::vector<u32> expected(32);
        for (u32 i = 0; i < 8; ++i)
            expected[16 + i] = table[i];
        expected[30] = table[12];
        // Offset flat[16] << 2 (32-bit) points past the table: unreadable, so 0. Without the
        // flush before this command flat[16] would still be 0 and the copy would read table[0].
        expected[31] = 0;
        std::vector<u32> flat(32);
        p.Run(user, flat, reader);
        CHECK(std::equal(flat.begin() + 16, flat.end(), expected.begin() + 16));
        CHECK(calls == 3); // one batch, one single dword, one out-of-table dword
        // Batch refused: the same values through per-dword reads.
        std::vector<u32> fallback(32);
        calls = 0;
        fail_bytes_over = 4;
        p.Run(user, fallback, reader);
        CHECK(fallback == flat);
        CHECK(calls == 1 + 8 + 2);
        // One unreadable dword inside the run: only that dword is 0.
        std::vector<u32> hole(32);
        fail_bytes_over = UINT32_MAX;
        fail_address = table_address + 3 * 4;
        p.Run(user, hole, reader);
        CHECK(hole[16 + 3] == 0 && hole[16 + 2] == table[2] && hole[16 + 4] == table[4]);
        // No table pointer: every copied dword is 0 without reading.
        const std::array<u32, 2> null_user{};
        std::vector<u32> none(32, 0xFFFFFFFFu);
        calls = 0;
        fail_address = 0;
        p.Run(null_user, none, reader);
        CHECK(none[16] == 0 && none[23] == 0 && none[30] == 0 && calls == 0);
    }
    // A 1024x512 BC texture uses macro blocks at levels 0/1 and micro
    // blocks from level 2 onward. Literal byte footprints include padded tails.
    for (u32 bits : {64u, 128u}) {
        VideoCore::ImageInfo texture;
        texture.props.is_tiled = true;
        texture.props.is_block = true;
        texture.tile_mode = AmdGpu::TileMode::Thin2DThin;
        texture.array_mode = AmdGpu::ArrayMode::Array2DTiledThin1;
        texture.pitch = texture.size.width = 1024;
        texture.size.height = 512;
        texture.resources.levels = 11;
        texture.resources.layers = 1;
        texture.num_bits = bits;
        texture.UpdateSize();
        CHECK(texture.micro_mip_mask == 0x7fc);
        const std::array<u32, 11> bytes128{524288, 131072, 32768, 8192, 2048,
                                           1024, 1024, 1024, 1024, 1024, 1024};
        u32 offset{};
        for (u32 mip = 0; mip < 11; ++mip) {
            CHECK(texture.mips_layout[mip].size == bytes128[mip] * bits / 128);
            CHECK(texture.mips_layout[mip].offset == offset);
            offset += bytes128[mip] * bits / 128;
        }
        CHECK(texture.guest_size == offset);
        // Unpadded width 63 rounds to macro width 64 but still needs micro
        // addressing: this is why the decision is sent explicitly to the shader.
        const u32 macro_width = bits == 64 ? 128 : 64;
        CHECK(VideoCore::MipUsesMicroTiling(macro_width - 1, 64, bits, 1, texture.tile_mode, 1, false));
        CHECK(!VideoCore::MipUsesMicroTiling(macro_width, 64, bits, 1, texture.tile_mode, 1, false));
        texture.props.is_block = false;
        texture.UpdateSize();
        CHECK(texture.micro_mip_mask == 0x7f0); // pixel units: transition at mip 4
    }
    // Without GPU direct reads, report the required capability instead of
    // returning flat[0] for an expression that changes during shader execution.
    {
        Info no_dma_info;
        no_dma_info.user_data = ud;
        IR::Block dynamic_block(pool);
        IR::IREmitter dynamic_ir(dynamic_block);
        IR::Program dynamic_program(no_dma_info);
        dynamic_program.post_order_blocks.push_back(&dynamic_block);
        const auto root = dynamic_ir.CompositeConstruct(dynamic_ir.GetUserData(IR::ScalarReg(0)),
                                                        dynamic_ir.GetUserData(IR::ScalarReg(1)));
        auto* dynamic_phi = &*dynamic_block.PrependNewInst(dynamic_block.begin(), IR::Opcode::Phi);
        dynamic_phi->SetFlags(IR::Type::U32);
        dynamic_phi->AddPhiOperand(&dynamic_block, dynamic_ir.Imm32(1u));
        [[maybe_unused]] const auto read =
            dynamic_ir.ReadConst(root, IR::U32{IR::Value{dynamic_phi}});
        EmulatorSettings.SetDirectMemoryAccessEnabled(false);
        bool rejected{};
        try {
            Optimization::FlattenExtendedUserdataPass(dynamic_program);
        } catch (const std::runtime_error& e) {
            rejected = std::string_view(e.what()).find("requires direct memory access") !=
                       std::string_view::npos;
        }
        CHECK(rejected);
    }

    // Dynamic constant reads: retain the GPU offset while proving only its base
    // from user/flat data. A GPU-generated pointer must keep full synchronization.
    EmulatorSettings.SetDirectMemoryAccessEnabled(true);
    for (u32 kind = 0; kind < 5; ++kind) {
        Info dma_info{};
        dma_info.flattened_ud_buf.resize(32);
        dma_info.flattened_ud_buf[0] = 0x12345678;
        dma_info.flattened_ud_buf[1] = 0x20;
        dma_info.flattened_ud_buf[16] = 0x87654321;
        dma_info.flattened_ud_buf[17] = 0x21;
        IR::Block dma_block(pool);
        IR::IREmitter emit(dma_block);
        IR::Program dma_program(dma_info);
        dma_program.post_order_blocks.push_back(&dma_block);
        const auto root = emit.CompositeConstruct(emit.GetUserData(IR::ScalarReg(0)),
                                                  emit.GetUserData(IR::ScalarReg(1)));
        IR::Value pointer = root;
        if (kind == 1 || kind == 4) {
            auto lo = emit.ReadConst(root, emit.Imm32(0u));
            auto hi = emit.ReadConst(root, emit.Imm32(1u));
            if (kind == 1) {
                lo.Inst()->SetFlags(16u);
                hi.Inst()->SetFlags(17u);
            }
            pointer = emit.CompositeConstruct(lo, hi);
        } else if (kind == 2) {
            pointer = emit.CompositeConstruct(emit.Imm32(0x44440000u), emit.Imm32(0x22u));
        } else if (kind == 3) {
            auto* phi = &*dma_block.PrependNewInst(dma_block.begin(), IR::Opcode::Phi);
            phi->SetFlags(IR::Type::U32);
            phi->AddPhiOperand(&dma_block, emit.Imm32(0u));
            pointer = emit.CompositeConstruct(IR::U32{IR::Value{phi}}, emit.Imm32(0x23u));
        }
        (void)emit.ReadConst(pointer, emit.Imm32(3u));
        (void)emit.ReadConst(pointer, emit.Imm32(7u));
        Optimization::CollectShaderInfoPass(dma_program, Profile{});
        CHECK(dma_info.uses_dma);
        CHECK(dma_info.dma_unbounded == (kind >= 3));
        if (kind < 3) {
            CHECK(dma_info.dma_read_bases.size() == 1);
            u64 address{};
            CHECK(
                dma_info.dma_read_bases.front().Fetch(dma_info.flattened_ud_buf.data(), &address));
            CHECK(address == (kind == 0   ? 0x2012345678ull
                              : kind == 1 ? 0x2187654321ull
                                          : 0x2244440000ull));
        }
    }

    // Sparse image heaps: retain live T# values, deduplicate aliases, preserve changing
    // format/address words and reject layouts or populations we cannot bind faithfully.
    {
        Info heap_info{};
        const u64 heap = base + 0x1000;
        AmdGpu::Buffer heap_buffer{};
        heap_buffer.base_address = heap;
        heap_buffer.stride = 48;
        heap_buffer.num_records = 40;
        DynamicImageTable layout{};
        std::memcpy(layout.buffer_fetch.immediates.data(), &heap_buffer, sizeof(heap_buffer));
        layout.stride = 48;
        layout.image_offset = 16;
        layout.flat_base = 16;
        heap_info.dynamic_image_tables.push_back(layout);
        heap_info.flattened_ud_buf.resize(16 + DynamicImageTable::Capacity * 8);
        auto write = [&](u32 slot, const AmdGpu::Image& image) {
            std::memcpy(reinterpret_cast<void*>(heap + slot * 48 + 16), &image, sizeof(image));
        };
        auto make_image = [](u32 id) {
            auto image = AmdGpu::Image::Null(false);
            image.base_address = 0x1000 + id * 256;
            image.width = 31;
            image.height = 31;
            image.pitch = 31;
            return image;
        };
        std::memset(reinterpret_cast<void*>(heap), 0, 40 * 48);
        const auto a = make_image(1), b = make_image(2);
        write(1, a);
        write(5, b);
        write(17, a);
        auto buffer_record = b;
        buffer_record.type = 2; // occupied but not a T#
        write(6, buffer_record);
        heap_info.RefreshDynamicImageTables();
        CHECK(heap_info.dynamic_image_snapshots[0].images[0].Address() == a.Address());
        CHECK(heap_info.dynamic_image_snapshots[0].images[1].Address() == b.Address());
        CHECK(!heap_info.dynamic_image_snapshots[0].images[2].Valid());
        auto moved = b;
        moved.base_address += 1024;
        write(5, moved);
        heap_info.RefreshDynamicImageTables();
        CHECK(heap_info.dynamic_image_snapshots[0].images[1].Address() == moved.Address());
        write(1, {});
        write(17, {});
        heap_info.RefreshDynamicImageTables();
        CHECK(heap_info.dynamic_image_snapshots[0].images[0].Address() == moved.Address());
        CHECK(!heap_info.dynamic_image_snapshots[0].images[1].Valid());
        for (u32 n = 0; n < 32; ++n)
            write(n, make_image(n));
        heap_info.RefreshDynamicImageTables();
        CHECK(heap_info.dynamic_image_snapshots[0].images[31].Address() ==
              make_image(31).Address());
        auto rejects = [&] {
            try {
                heap_info.RefreshDynamicImageTables();
                return false;
            } catch (const std::runtime_error&) {
                return true;
            }
        };
        write(32, make_image(32));
        CHECK(rejects());
        write(32, {});
        heap_info.dynamic_image_tables[0].image_offset = 17;
        CHECK(rejects());
        heap_info.dynamic_image_tables[0].image_offset = 16;
        heap_buffer.base_address = base + 0x4000;
        std::memcpy(heap_info.dynamic_image_tables[0].buffer_fetch.immediates.data(), &heap_buffer,
                    sizeof(heap_buffer));
        CHECK(rejects());
        // Actual IR patterns: scalar constant-buffer loads and vector-load/ReadLane
        // waterfall both refer to the same 48-byte records at +16, never a fixed entry.
        for (bool lane_load : {false, true}) {
            std::memset(reinterpret_cast<void*>(heap), 0, 40 * 48);
            write(2, a);
            write(8, b);
            heap_buffer.base_address = heap;
            std::array<u32, 16> regs{};
            std::memcpy(regs.data(), &heap_buffer, sizeof(heap_buffer));
            Info images_info{};
            images_info.user_data = regs;
            images_info.hw_stage = HwStage::Compute;
            images_info.sw_stage = SwStage::Compute;
            IR::Block images_block(pool);
            IR::IREmitter emit(images_block);
            IR::Program images_program(images_info);
            images_program.blocks.push_back(&images_block);
            images_program.post_order_blocks.push_back(&images_block);
            images_program.syntax_list.push_back(
                {.data = {.block = &images_block}, .type = IR::AbstractSyntaxNode::Type::Block});
            images_program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
            const auto vsharp = emit.CompositeConstruct(
                emit.GetUserData(IR::ScalarReg(0)), emit.GetUserData(IR::ScalarReg(1)),
                emit.GetUserData(IR::ScalarReg(2)), emit.GetUserData(IR::ScalarReg(3)));
            IR::BufferInstInfo load_flags{};
            load_flags.index_enable.Assign(1);
            const auto index = IR::U32{emit.LoadBufferU32(
                1, vsharp, emit.CompositeConstruct(emit.Imm32(0U), emit.Imm32(0U), emit.Imm32(0U)),
                load_flags)};
            std::array<IR::Value, 8> words;
            if (lane_load) {
                for (u32 half = 0; half < 2; ++half) {
                    load_flags.inst_offset.Assign(16 + half * 16);
                    const auto loaded = emit.LoadBufferU32(
                        4, vsharp, emit.CompositeConstruct(index, emit.Imm32(0U), emit.Imm32(0U)),
                        load_flags);
                    for (u32 n = 0; n < 4; ++n)
                        words[half * 4 + n] = emit.ReadLane(
                            IR::U32{emit.CompositeExtract(loaded, n)}, emit.Imm32(0U));
                }
            } else {
                const auto bytes =
                    IR::U32{emit.IAdd(emit.IMul(index, emit.Imm32(48U)), emit.Imm32(16U))};
                const auto dwords = emit.ShiftRightLogical(bytes, emit.Imm32(2U));
                for (u32 n = 0; n < 8; ++n)
                    words[n] =
                        emit.ReadConstBuffer(vsharp, IR::U32{emit.IAdd(dwords, emit.Imm32(n))}, {});
            }
            const auto handle =
                emit.ImageHandle(emit.CompositeConstruct(words[0], words[1], words[2], words[3]),
                                 emit.CompositeConstruct(words[4], words[5], words[6], words[7]));
            const auto sampler = emit.CompositeConstruct(emit.Imm32(73U), emit.Imm32(16773120U),
                                                         emit.Imm32(105906176U), emit.Imm32(0U));
            const auto coords = emit.CompositeConstruct(emit.Imm32(.5f), emit.Imm32(.5f),
                                                        emit.Imm32(0.f), emit.Imm32(0.f));
            IR::TextureInstInfo texture_flags{};
            texture_flags.has_lod.Assign(1);
            const auto color = emit.ImageSampleRaw(handle, sampler, coords, coords, coords,
                                                   emit.Imm32(0.f), texture_flags);
            load_flags.inst_offset.Assign(0);
            emit.StoreBufferU32(
                4, vsharp, emit.CompositeConstruct(emit.Imm32(39U), emit.Imm32(0U), emit.Imm32(0U)),
                emit.CompositeConstruct(
                    emit.BitCast<IR::U32>(IR::F32{emit.CompositeExtract(color, 0)}),
                    emit.BitCast<IR::U32>(IR::F32{emit.CompositeExtract(color, 1)}),
                    emit.BitCast<IR::U32>(IR::F32{emit.CompositeExtract(color, 2)}),
                    emit.BitCast<IR::U32>(IR::F32{emit.CompositeExtract(color, 3)})),
                load_flags);
            Profile profile{};
            profile.supported_spirv = 0x10600;
            profile.subgroup_size = 64;
            const auto resources = Optimization::ResourceDiscoverPass(images_program, profile);
            const auto found = std::ranges::find_if(
                resources, [](const auto& r) { return r.image_table_buffer.num_dwords != 0; });
            CHECK(found != resources.end());
            if (found == resources.end())
                continue;
            CHECK(found->image_table_offset == 16);
            CHECK(found->image_table_stride == (lane_load ? 0 : 48));
            EmulatorSettings.SetDirectMemoryAccessEnabled(true);
            Optimization::FlattenExtendedUserdataPass(images_program);
            images_info.RefreshFlatBuf();
            Optimization::ResourcePatchingPass(images_info, resources, profile);
            CHECK(images_info.dynamic_image_tables.size() == 1);
            CHECK(images_info.images.size() == 2);
            Optimization::DeadCodeEliminationPass(images_program);
            Optimization::CollectShaderInfoPass(images_program, profile);
            RuntimeInfo runtime{};
            runtime.Initialize(HwStage::Compute, SwStage::Compute);
            runtime.hw.cs.workgroup_size = {1, 1, 1};
            const StageSpecialization initial_spec(images_info, runtime, profile, {});
            CHECK(initial_spec.dynamic_image_masks[0] == 3);
            // Inserting into an omitted slot must invalidate the previous binary,
            // even though its resource list has no descriptor for the new image.
            auto extra = images_info.dynamic_image_snapshots[0].images[0];
            extra.base_address += 0x1000;
            const u32 table_base = images_info.dynamic_image_tables[0].flat_base;
            std::memcpy(images_info.flattened_ud_buf.data() + table_base + 16, &extra,
                        sizeof(extra));
            const StageSpecialization inserted_spec(images_info, runtime, profile, {});
            CHECK(inserted_spec.dynamic_image_masks[0] == 7);
            CHECK(!(initial_spec == inserted_spec));
            CHECK(!(inserted_spec == initial_spec));
            std::fill_n(images_info.flattened_ud_buf.data() + table_base + 8, 16, 0U);
            const StageSpecialization removed_spec(images_info, runtime, profile, {});
            CHECK(removed_spec.dynamic_image_masks[0] == 1);
            CHECK(!(initial_spec == removed_spec));
            CHECK(!(removed_spec == initial_spec));
            images_info.RefreshFlatBuf();
            CHECK(initial_spec == StageSpecialization(images_info, runtime, profile, {}));
            Backend::Bindings bindings{};
            const auto spirv =
                Backend::SPIRV::EmitSPIRV(profile, runtime, images_program, bindings);
            CHECK(spirv.size() > 5 && spirv[0] == 0x07230203);
            std::ofstream output(lane_load ? "dynamic-image-lane.spv" : "dynamic-image-scalar.spv",
                                 std::ios::binary);
            output.write(reinterpret_cast<const char*>(spirv.data()), spirv.size() * sizeof(u32));
            std::ofstream flat("dynamic-image-flat.bin", std::ios::binary);
            flat.write(reinterpret_cast<const char*>(images_info.flattened_ud_buf.data()),
                       images_info.flattened_ud_buf.size() * 4);
            std::ofstream heap_file("dynamic-image-heap.bin", std::ios::binary);
            heap_file.write(reinterpret_cast<const char*>(heap), 40 * 48);
            std::ofstream user_file("dynamic-image-user.bin", std::ios::binary);
            user_file.write(reinterpret_cast<const char*>(regs.data()), 16 * 4);
            const std::array<u32, 3> layout{u32(images_info.buffers.size()),
                                            u32(images_info.images.size()),
                                            u32(images_info.samplers.size())};
            std::ofstream layout_file("dynamic-image-layout.bin", std::ios::binary);
            layout_file.write(reinterpret_cast<const char*>(layout.data()), sizeof(layout));
        }
    }
    std::printf("srt checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
