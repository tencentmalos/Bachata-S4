// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Auxiliary LOAD AUDIT. The historical name is retained for build
// compatibility. This is not a production Module/Linker and never executes an
// unresolved image.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "core/guest_cpu/api/address_space.h"
#include "core/guest_cpu/api/memory.h"
#include "core/loader/elf.h"

using namespace Core::GuestCpu;
using Core::Loader::Elf;
namespace {
constexpr u64 Page = 4096;
constexpr u64 MaxImage = 512 * 1024 * 1024; // Bounded auxiliary probe, not PS4 VM geometry.
constexpr u32 R_X86_64_NONE = 0;
constexpr s64 DT_RELASZ = 8, DT_RELAENT = 9, DT_JMPREL = 23;
void Require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
u64 Add(u64 a, u64 b) {
    Require(b <= UINT64_MAX - a, "address overflow");
    return a + b;
}
u64 RoundUp(u64 n) {
    return Add(n, Page - 1) & ~(Page - 1);
}
struct Segment {
    elf_program_header ph;
    u64 low, high;
};
std::vector<std::byte> Read(Elf &elf, const elf_program_header &ph) {
    Require(ph.p_filesz <= MaxImage, "metadata/segment exceeds probe size limit");
    std::vector<std::byte> bytes(ph.p_filesz);
    if (!bytes.empty())
        Require(elf.TryLoadSegment(reinterpret_cast<u64>(bytes.data()), ph.p_offset, ph.p_filesz),
                "invalid/truncated/encoded segment");
    return bytes;
}

int Audit(const char *path, bool require_execution) {
    Elf elf;
    elf.Open(path);
    Require(elf.IsElfFile(), "invalid ELF/SELF ELF header");
    const auto eh = elf.GetElfHeader();
    const auto phdrs = elf.GetProgramHeader();
    // This probe implements a PIE load bias only. Fixed-address executables need
    // the production loader and must not silently be rebased here.
    Require(eh.e_type == ET_SCE_DYNEXEC, "probe requires ET_SCE_DYNEXEC");
    std::vector<Segment> segments;
    const elf_program_header *dynamic = nullptr, *dynlib = nullptr;
    u64 tls_bytes = 0;
    for (const auto &ph : phdrs) {
        if (ph.p_type == PT_DYNAMIC) {
            Require(!dynamic, "duplicate PT_DYNAMIC");
            dynamic = &ph;
        } else if (ph.p_type == PT_SCE_DYNLIBDATA) {
            Require(!dynlib, "duplicate PT_SCE_DYNLIBDATA");
            dynlib = &ph;
        } else if (ph.p_type == PT_TLS) {
            tls_bytes = Add(tls_bytes, ph.p_memsz);
        }
        if (ph.p_type != PT_LOAD && ph.p_type != PT_SCE_RELRO)
            continue;
        Require(ph.p_filesz <= ph.p_memsz, "filesz exceeds memsz");
        if (!ph.p_memsz)
            continue;
        Require(!((ph.p_flags & PF_EXEC) && (ph.p_flags & PF_WRITE)), "RWX image unsupported");
        segments.push_back({ph, ph.p_vaddr & ~(Page - 1), RoundUp(Add(ph.p_vaddr, ph.p_memsz))});
    }
    Require(!segments.empty(), "no loadable segments");
    std::sort(segments.begin(), segments.end(), [](auto &a, auto &b) { return a.low < b.low; });
    const auto lo = segments.front().low, hi = segments.back().high;
    Require(hi > lo && hi - lo <= MaxImage, "module span exceeds probe size limit");
    bool entry_executable = false;
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto &s = segments[i];
        Require(!i || segments[i - 1].high <= s.low, "overlapping mapped pages unsupported");
        if ((s.ph.p_flags & PF_EXEC) && eh.e_entry >= s.ph.p_vaddr &&
            eh.e_entry - s.ph.p_vaddr < s.ph.p_memsz)
            entry_executable = true;
    }
    Require(entry_executable, "entry outside executable segment");
    AddressSpaceConfig cfg{};
    cfg.reservation_size = hi - lo;
    auto created = GuestAddressSpace::Create(cfg);
    Require(bool(created), "GuestAddressSpace::Create failed");
    auto space = std::move(created.Value());
    const u64 base = space->ReservationBase().value;
    Require(base >= lo, "load bias underflow");
    const u64 bias = base - lo;
    auto contains = [&](u64 va, u64 size) {
        return std::any_of(segments.begin(), segments.end(), [&](const auto &s) {
            return va >= s.ph.p_vaddr && va - s.ph.p_vaddr <= s.ph.p_memsz &&
                   size <= s.ph.p_memsz - (va - s.ph.p_vaddr);
        });
    };
    size_t relro = 0;
    for (const auto &s : segments) {
        const auto &ph = s.ph;
        Require(bool(space->Map({GuestAddress{Add(bias, s.low)}, s.high - s.low},
                                GuestPermission::Read | GuestPermission::Write)),
                "Map failed");
        const auto bytes = Read(elf, ph);
        if (!bytes.empty())
            Require(bool(space->Write(GuestAddress{Add(bias, ph.p_vaddr)}, bytes)),
                    "segment Write failed");
        // Anonymous mapping zeroes only the segment's BSS/page padding. Gaps stay
        // inaccessible.
        if (ph.p_type == PT_SCE_RELRO)
            ++relro;
        std::printf("segment type=%#x va=%#llx filesz=%#llx memsz=%#llx flags=%u\n", ph.p_type,
                    (unsigned long long)ph.p_vaddr, (unsigned long long)ph.p_filesz,
                    (unsigned long long)ph.p_memsz, ph.p_flags);
    }
    std::map<u32, size_t> unresolved;
    size_t applied = 0, plt_count = 0;
    if (dynamic || dynlib) {
        Require(dynamic && dynlib, "incomplete dynamic metadata");
        auto entries = Read(elf, *dynamic), data = Read(elf, *dynlib);
        Require(!entries.empty() && entries.size() % sizeof(elf_dynamic) == 0,
                "invalid dynamic size");
        std::map<s64, u64> tags;
        bool terminated = false;
        for (size_t off = 0; off < entries.size(); off += sizeof(elf_dynamic)) {
            elf_dynamic d{};
            std::memcpy(&d, entries.data() + off, sizeof(d));
            if (d.d_tag == DT_NULL) {
                terminated = true;
                break;
            }
            switch (d.d_tag) {
            case DT_RELA:
            case DT_RELASZ:
            case DT_RELAENT:
            case DT_JMPREL:
                throw std::runtime_error("non-SCE relocation table unsupported by probe");
            case DT_SCE_RELA:
            case DT_SCE_RELASZ:
            case DT_SCE_RELAENT:
            case DT_SCE_JMPREL:
            case DT_SCE_PLTRELSZ:
            case DT_SCE_PLTREL:
                Require(tags.emplace(d.d_tag, d.d_un.d_val).second, "duplicate relocation tag");
                break;
            default:
                break;
            }
        }
        Require(terminated, "unterminated dynamic table");
        if (tags.contains(DT_SCE_RELAENT))
            Require(tags.at(DT_SCE_RELAENT) == sizeof(elf_relocation), "invalid RELA entry size");
        auto table = [&](s64 offset_tag, s64 size_tag, bool plt) {
            if (!tags.contains(offset_tag) && !tags.contains(size_tag))
                return;
            Require(tags.contains(offset_tag) && tags.contains(size_tag),
                    "incomplete relocation table");
            const u64 off = tags.at(offset_tag), size = tags.at(size_tag);
            Require(off <= data.size() && size <= data.size() - off &&
                        size % sizeof(elf_relocation) == 0,
                    "relocation table out of range");
            if (plt && size)
                Require(tags.contains(DT_SCE_PLTREL) && tags.at(DT_SCE_PLTREL) == DT_RELA,
                        "PLT table is not RELA");
            for (u64 at = 0; at < size; at += sizeof(elf_relocation)) {
                elf_relocation r{};
                std::memcpy(&r, data.data() + off + at, sizeof(r));
                if (plt)
                    ++plt_count;
                if (r.GetType() == R_X86_64_NONE)
                    continue;
                Require(contains(r.rel_offset, sizeof(u64)), "relocation target outside segment");
                if (r.GetType() != R_X86_64_RELATIVE) {
                    ++unresolved[r.GetType()];
                    continue;
                }
                Require(r.GetSymbol() == 0, "RELATIVE has a symbol");
                // Signed addend without unsigned wrap or INT64_MIN negation.
                const u64 magnitude =
                    r.rel_addend < 0 ? u64(-(r.rel_addend + 1)) + 1 : u64(r.rel_addend);
                Require(r.rel_addend >= 0 || bias >= magnitude, "relative addend underflow");
                const u64 value = r.rel_addend < 0 ? bias - magnitude : Add(bias, magnitude);
                Require(bool(space->Write(
                            GuestAddress{Add(bias, r.rel_offset)},
                            {reinterpret_cast<const std::byte *>(&value), sizeof(value)})),
                        "relocation Write failed");
                ++applied;
            }
        };
        table(DT_SCE_RELA, DT_SCE_RELASZ, false);
        table(DT_SCE_JMPREL, DT_SCE_PLTRELSZ, true);
    }
    // Final protections only AFTER all writes; no execute/write overlap.
    for (const auto &s : segments) {
        GuestPermission perm = GuestPermission::None;
        if (s.ph.p_flags & PF_READ)
            perm = perm | GuestPermission::Read;
        if (s.ph.p_flags & PF_WRITE)
            perm = perm | GuestPermission::Write;
        if (s.ph.p_flags & PF_EXEC)
            perm = perm | GuestPermission::Execute;
        Require(bool(space->Protect({GuestAddress{Add(bias, s.low)}, s.high - s.low}, perm)),
                "Protect failed");
    }
    for (auto [type, count] : unresolved)
        std::printf("unresolved relocation type=%u count=%zu\n", type, count);
    std::printf("LOAD_AUDIT_PASS segments=%zu relro=%zu relative=%zu "
                "plt_entries=%zu tls_bytes=%llu\n",
                segments.size(), relro, applied, plt_count, (unsigned long long)tls_bytes);
    std::printf("EXECUTION_NOT_RUN: production import/TLS/entry/session "
                "integration required; "
                "load audit is not execution evidence\n");
    return require_execution ? 3 : 0;
}
} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2 || argc > 3 || (argc == 3 && std::string_view(argv[2]) != "--require-execution")) {
        std::fprintf(stderr, "usage: %s <eboot.bin> [--require-execution]\n", argv[0]);
        return 2;
    }
    try {
        return Audit(argv[1], argc == 3);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "LOAD_AUDIT_FAIL: %s\nEXECUTION_NOT_RUN\n", e.what());
        return 1;
    }
}
