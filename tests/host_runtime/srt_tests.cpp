// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include "common/serdes.h"
#include "core/memory.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
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
    void* MapFile(VAddr, u64, u64, u32, uintptr_t) override {
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
    Optimization::FlattenExtendedUserdataPass(program);
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
    std::printf("srt checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
