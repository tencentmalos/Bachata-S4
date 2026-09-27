#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Look at PS4 guest code offline: pull a module out of a game, make it disassemblable, and
find the function around an address -- typically the address of a guest crash in a shadPS4 log.

Commands:
  ls       list a directory inside a .zar (or a plain game directory)
  extract  copy one file out of a .zar
  elf      SELF/ELF -> analysis ELF (section table, one symbol per EH-described function,
           imports named from their NIDs, relocations applied) -- the input for IDA
  imports  imported functions/objects: NID, name, GOT slot, PLT stub, call sites
  xrefs    direct calls/jumps and RIP-relative references to module offsets
  func     function containing a module offset, from the module's .eh_frame_hdr
  disasm   disassemble the function (or a window) around a module offset, marking it
  str      C string at a module offset (assert messages, file names), or --hex bytes
  crash    log -> faulting module + offset -> extract -> disassemble, in one step
  selftest build synthetic ZAR/SELF/EH inputs in memory and check every parser

Addresses: a *module offset* is the ELF virtual address inside the module (shadPS4 loads a module
at `base + p_vaddr`, and PS4 modules start at vaddr 0), so `offset = runtime address - base`. The
analysis ELF keeps those virtual addresses, which makes llvm-objdump print module offsets.

The analysis ELF is for reading only: e_type is rewritten to ET_EXEC and a section table is
synthesised so tools accept it. The module's own relocations (DT_SCE_RELA: R_X86_64_RELATIVE and
self-defined symbols) are applied as if the module were loaded at 0, so vtables and pointer tables
read as the module offsets they hold at run time; imports stay unresolved but get named symbols
(`__imp_<name>` on the GOT slot, `<name>` on the PLT stub) from the NID table. Use --raw for the
untouched loadable bytes. Only fake-signed/decrypted SELFs are supported -- encrypted or
compressed segments are refused, not guessed.

Needs Python 3.10+. .zar reading needs zstd: the standard `compression.zstd` (3.14+) or
`pip install zstandard`. Disassembly needs llvm-objdump (PATH, --objdump or $LLVM_OBJDUMP).
"""

import argparse
import bisect
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# --------------------------------------------------------------------------------------------
# ZArchive (.zar) reader. Format from externals/zarchive: big-endian footer at the end of the
# file, zstd-compressed 64 KiB blocks addressed through offset records, a flat file tree.


def _zstd_decompress(data, size):
    try:
        from compression import zstd  # Python 3.14+
        return zstd.decompress(data)
    except ImportError:
        pass
    try:
        import zstandard
    except ImportError:
        raise SystemExit("reading .zar needs zstd: use Python 3.14+ or `pip install zstandard`")
    return zstandard.ZstdDecompressor().decompress(data, max_output_size=size)


class ZarError(RuntimeError):
    pass


class ZarReader:
    BLOCK = 64 * 1024
    PER_RECORD = 16
    MAGIC = 0x169F52D6
    VERSION1 = 0x61BF3A01
    FOOTER = struct.Struct(">12Q32sQII")  # six (offset, size) sections, hash, total, version, magic

    def __init__(self, path):
        self.path = Path(path)
        self.f = open(self.path, "rb")
        self.f.seek(0, os.SEEK_END)
        file_size = self.f.tell()
        if file_size <= self.FOOTER.size:
            raise ZarError(f"{path}: too small for a ZArchive")
        self.f.seek(file_size - self.FOOTER.size)
        fields = self.FOOTER.unpack(self.f.read(self.FOOTER.size))
        sections = [(fields[i], fields[i + 1]) for i in range(0, 12, 2)]
        total, version, magic = fields[13], fields[14], fields[15]
        if magic != self.MAGIC or version != self.VERSION1 or total != file_size:
            raise ZarError(f"{path}: not a ZArchive v1 (magic {magic:#x}, version {version:#x})")
        for offset, size in sections:
            if offset + size > file_size:
                raise ZarError(f"{path}: section out of range")
        (self.data_offset, self.data_size), records, names, tree = sections[:4]
        raw = self._read(*records)
        if len(raw) % 40:
            raise ZarError(f"{path}: bad offset record section")
        self.records = [struct.unpack_from(">Q16H", raw, i) for i in range(0, len(raw), 40)]
        self.names = self._read(*names)
        raw = self._read(*tree)
        if not raw or len(raw) % 16:
            raise ZarError(f"{path}: bad file tree")
        self.tree = [struct.unpack_from(">4I", raw, i) for i in range(0, len(raw), 16)]
        if self._is_file(0) or self._name(0):
            raise ZarError(f"{path}: first tree entry is not the root directory")
        self._cache = {}

    def _read(self, offset, size):
        self.f.seek(offset)
        data = self.f.read(size)
        if len(data) != size:
            raise ZarError(f"{self.path}: short read")
        return data

    def _is_file(self, node):
        return bool(self.tree[node][0] & 0x80000000)

    def _name(self, node):
        offset = self.tree[node][0] & 0x7FFFFFFF
        if offset == 0x7FFFFFFF or offset >= len(self.names):
            return ""
        length = self.names[offset] & 0x7F
        if self.names[offset] & 0x80:  # two-byte length, as the writer emits it
            length |= self.names[offset + 1] << 7
            offset += 2
        else:
            offset += 1
        return self.names[offset:offset + length].decode("utf-8", "replace")

    def _children(self, node):
        _, start, count, _ = self.tree[node]
        return range(start, start + count)

    def lookup(self, path):
        node = 0
        for part in re.split(r"[/\\]+", path.strip("/\\")):
            if not part:
                continue
            if self._is_file(node):
                return None
            for child in self._children(node):
                if self._name(child).lower() == part.lower():
                    node = child
                    break
            else:
                return None
        return node

    def listdir(self, path=""):
        node = self.lookup(path)
        if node is None or self._is_file(node):
            raise ZarError(f"{path or '/'}: no such directory in {self.path.name}")
        out = []
        for child in self._children(node):
            out.append((self._name(child), self._is_file(child),
                        self._file_size(child) if self._is_file(child) else 0))
        return out

    def _file_size(self, node):
        _, low, size_low, high = self.tree[node]
        return size_low | ((high & 0xFFFF0000) << 16)

    def _file_offset(self, node):
        _, offset_low, _, high = self.tree[node]
        return offset_low | ((high & 0xFFFF) << 32)

    def _block(self, index):
        block = self._cache.get(index)
        if block is not None:
            return block
        record = self.records[index // self.PER_RECORD]
        sub = index % self.PER_RECORD
        offset = record[0] + sum(record[1 + i] + 1 for i in range(sub))
        size = record[1 + sub] + 1
        if offset + size > self.data_size:
            raise ZarError(f"{self.path}: block {index} out of range")
        raw = self._read(self.data_offset + offset, size)
        block = raw if size == self.BLOCK else _zstd_decompress(raw, self.BLOCK)
        if len(block) != self.BLOCK:
            raise ZarError(f"{self.path}: block {index} decompressed to {len(block)} bytes")
        if len(self._cache) > 64:
            self._cache.clear()
        self._cache[index] = block
        return block

    def read(self, path):
        node = self.lookup(path)
        if node is None or not self._is_file(node):
            raise ZarError(f"{path}: no such file in {self.path.name}")
        position, remaining = self._file_offset(node), self._file_size(node)
        out = bytearray()
        while remaining:
            block = self._block(position // self.BLOCK)
            start = position % self.BLOCK
            step = min(remaining, self.BLOCK - start)
            out += block[start:start + step]
            position += step
            remaining -= step
        return bytes(out)


class GameSource:
    """A game root: a .zar or a directory. Lookups are case-insensitive, like the emulator's."""

    def __init__(self, path):
        self.path = Path(path)
        self.zar = ZarReader(self.path) if self.path.is_file() else None
        if self.zar is None and not self.path.is_dir():
            raise SystemExit(f"{path}: neither a .zar nor a directory")

    def exists(self, name):
        if self.zar:
            node = self.zar.lookup(name)
            return node is not None and self.zar._is_file(node)
        return self._dir_lookup(name) is not None

    def read(self, name):
        if self.zar:
            return self.zar.read(name)
        found = self._dir_lookup(name)
        if found is None:
            raise SystemExit(f"{name}: not found under {self.path}")
        return found.read_bytes()

    def listdir(self, sub=""):
        if self.zar:
            return self.zar.listdir(sub)
        base = self._dir_lookup(sub) if sub else self.path
        if base is None or not base.is_dir():
            raise SystemExit(f"{sub}: no such directory under {self.path}")
        return [(p.name, p.is_file(), p.stat().st_size if p.is_file() else 0)
                for p in sorted(base.iterdir())]

    def _dir_lookup(self, name):
        current = self.path
        for part in re.split(r"[/\\]+", name.strip("/\\")):
            if not part:
                continue
            if not current.is_dir():
                return None
            match = [p for p in current.iterdir() if p.name.lower() == part.lower()]
            if not match:
                return None
            current = match[0]
        return current if current.is_file() or current.is_dir() else None

    def __str__(self):
        return str(self.path)


# --------------------------------------------------------------------------------------------
# SELF / ELF

SELF_MAGIC = 0x1D3D154F
ELF_MAGIC = b"\x7fELF"
PT_LOAD, PT_DYNAMIC, PT_TLS = 1, 2, 7
PT_GNU_EH_FRAME = 0x6474E550
PT_SCE_RELRO = 0x61000010
LOADABLE = (PT_LOAD, PT_SCE_RELRO)
PF_X, PF_W = 1, 2
EHDR = struct.Struct("<16sHHIQQQIHHHHHH")
PHDR = struct.Struct("<IIQQQQQQ")
SHDR = struct.Struct("<IIQQQQIIQQ")
SYM = struct.Struct("<IBBHQQ")
PT_SCE_DYNLIBDATA = 0x61000000
RELA = struct.Struct("<QQq")
R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT, R_X86_64_RELATIVE = 1, 6, 7, 8
DT_SCE_JMPREL, DT_SCE_PLTRELSZ = 0x61000029, 0x6100002D
DT_SCE_RELA, DT_SCE_RELASZ = 0x6100002F, 0x61000031
DT_SCE_STRTAB, DT_SCE_STRSZ = 0x61000035, 0x61000037
DT_SCE_SYMTAB, DT_SCE_SYMTABSZ = 0x61000039, 0x6100003F
PHDR_TYPE_NAMES = {
    1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE", 7: "TLS", PT_GNU_EH_FRAME: "GNU_EH_FRAME",
    0x61000000: "SCE_DYNLIBDATA", 0x61000001: "SCE_PROCPARAM", 0x61000002: "SCE_MODULE_PARAM",
    PT_SCE_RELRO: "SCE_RELRO", 0x6FFFFF00: "SCE_COMMENT", 0x6FFFFF01: "SCE_VERSION",
}


class Module:
    """A PS4 module image: program headers plus the loadable bytes, addressable by vaddr."""

    def __init__(self, data, name="module"):
        self.name = name
        self.raw = data
        magic = struct.unpack_from("<I", data, 0)[0] if len(data) >= 4 else 0
        if magic == SELF_MAGIC:
            self.kind = "SELF"
            segment_count = struct.unpack_from("<H", data, 24)[0]
            self.elf_offset = 32 + 32 * segment_count
            self.self_segments = [struct.unpack_from("<4Q", data, 32 + 32 * i)
                                  for i in range(segment_count)]
        elif data[:4] == ELF_MAGIC:
            self.kind = "ELF"
            self.elf_offset = 0
            self.self_segments = None
        else:
            raise SystemExit(f"{name}: neither SELF nor ELF (magic {magic:#x})")
        if data[self.elf_offset:self.elf_offset + 4] != ELF_MAGIC:
            raise SystemExit(f"{name}: no ELF header at {self.elf_offset:#x}")
        self.ehdr = list(EHDR.unpack_from(data, self.elf_offset))
        if self.ehdr[0][4] != 2 or self.ehdr[0][5] != 1:
            raise SystemExit(f"{name}: only ELF64 little-endian is supported")
        phoff, phnum = self.ehdr[5], self.ehdr[10]
        self.phdrs = [list(PHDR.unpack_from(data, self.elf_offset + phoff + i * PHDR.size))
                      for i in range(phnum)]
        self.segments = []  # (vaddr, bytes, memsz, flags) for loadable phdrs
        for index, ph in enumerate(self.phdrs):
            if ph[0] in LOADABLE:
                self.segments.append((ph[3], self.phdr_bytes(index), ph[6], ph[1]))
        self.segments.sort()
        self.extent = max((v + m for v, _, m, _ in self.segments), default=0)

    def phdr_bytes(self, index):
        p_type, _, p_offset, _, _, p_filesz, _, _ = self.phdrs[index]
        if p_filesz == 0:
            return b""
        if self.kind == "ELF":
            return self.raw[p_offset:p_offset + p_filesz]
        for flags, file_offset, file_size, _ in self.self_segments:
            if not flags & 0x800 or (flags >> 20) & 0xFFF != index:
                continue
            if flags & 0x2 or flags & 0x8:
                raise SystemExit(f"{self.name}: segment {index} is encrypted or compressed; "
                                 "decrypt the module first")
            return self.raw[file_offset:file_offset + min(file_size, p_filesz)]
        raise SystemExit(f"{self.name}: no SELF data for phdr {index}")

    def read(self, vaddr, size):
        for base, body, memsz, _ in self.segments:
            if base <= vaddr and vaddr + size <= base + memsz:
                start = vaddr - base
                chunk = body[start:start + size]
                return chunk + b"\0" * (size - len(chunk))  # .bss reads as zero
        raise ValueError(f"{vaddr:#x}+{size:#x} is outside the loadable segments")

    def executable(self, vaddr):
        return any(base <= vaddr < base + memsz and flags & PF_X
                   for base, _, memsz, flags in self.segments)

    def eh_frame_hdr(self):
        for ph in self.phdrs:
            if ph[0] == PT_GNU_EH_FRAME:
                return ph[3], ph[5]
        return None


_NID_NAMES = None
NID_TABLE = Path(__file__).resolve().parents[2] / "src" / "core" / "aerolib" / "aerolib.inl"


def nid_names(path=None):
    """NID -> function name, from the emulator's aerolib table (STUB("nid", name) lines)."""
    global _NID_NAMES
    if path is None and _NID_NAMES is not None:
        return _NID_NAMES
    table = {}
    source = Path(path) if path else NID_TABLE
    if source.is_file():
        for match in re.finditer(r'STUB\("([^"]+)",\s*([^)\s]+)\)', source.read_text(errors="replace")):
            table.setdefault(match.group(1), match.group(2))
    if path is None:
        _NID_NAMES = table
    return table


def symbol_name(text):
    """Printable identifier for a symbol table (IDA/objdump accept [A-Za-z0-9_.$@?])."""
    return re.sub(r"[^A-Za-z0-9_.$@?]", "_", text)


class DynamicInfo:
    """Relocations and symbols from a PS4 module's DT_SCE_* dynamic table.

    Symbol names are `NID#library#module`; `name` resolves the NID through the aerolib table.
    """

    def __init__(self, module, names=None):
        self.relocs = []   # (r_offset, type, symbol index, addend), DT_SCE_RELA then JMPREL
        self.symbols = []  # (raw name, st_info, st_shndx, st_value)
        self.names = names if names is not None else nid_names()
        dyn_index = next((i for i, ph in enumerate(module.phdrs) if ph[0] == PT_DYNAMIC), None)
        lib_index = next((i for i, ph in enumerate(module.phdrs) if ph[0] == PT_SCE_DYNLIBDATA), None)
        if dyn_index is None or lib_index is None:
            return
        try:
            dyn = module.phdr_bytes(dyn_index)
            lib = module.phdr_bytes(lib_index)
        except SystemExit:
            return
        tags = {}
        for pos in range(0, len(dyn) - 15, 16):
            tag, value = struct.unpack_from("<qQ", dyn, pos)
            if tag == 0:
                break
            tags.setdefault(tag, value)

        def blob(offset_tag, size_tag):
            if offset_tag not in tags or size_tag not in tags:
                return b""
            start = tags[offset_tag]
            return lib[start:start + tags[size_tag]]

        strtab = blob(DT_SCE_STRTAB, DT_SCE_STRSZ)
        symtab = blob(DT_SCE_SYMTAB, DT_SCE_SYMTABSZ)
        for pos in range(0, len(symtab) - SYM.size + 1, SYM.size):
            st_name, st_info, _, st_shndx, st_value, _ = SYM.unpack_from(symtab, pos)
            end = strtab.find(b"\0", st_name)
            raw = strtab[st_name:end if end >= 0 else len(strtab)].decode(errors="replace")
            self.symbols.append((raw, st_info, st_shndx, st_value))
        for data in (blob(DT_SCE_RELA, DT_SCE_RELASZ), blob(DT_SCE_JMPREL, DT_SCE_PLTRELSZ)):
            for pos in range(0, len(data) - RELA.size + 1, RELA.size):
                r_offset, r_info, r_addend = RELA.unpack_from(data, pos)
                self.relocs.append((r_offset, r_info & 0xFFFFFFFF, r_info >> 32, r_addend))

    def name(self, index):
        raw = self.symbols[index][0] if index < len(self.symbols) else ""
        nid = raw.split("#")[0]
        return self.names.get(nid) or (f"nid_{symbol_name(nid)}" if nid else f"sym_{index}")

    def defined(self, index):
        return index < len(self.symbols) and self.symbols[index][2] != 0

    def resolved(self):
        """(r_offset, value) for relocations that do not depend on another module."""
        for offset, kind, index, addend in self.relocs:
            if kind == R_X86_64_RELATIVE:
                yield offset, addend & 0xFFFFFFFFFFFFFFFF
            elif kind in (R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT) and self.defined(index):
                yield offset, (self.symbols[index][3] + addend) & 0xFFFFFFFFFFFFFFFF

    def imports(self):
        """(GOT slot, name, raw symbol, is_function) for relocations against undefined symbols."""
        for offset, kind, index, _ in self.relocs:
            if kind in (R_X86_64_JUMP_SLOT, R_X86_64_GLOB_DAT, R_X86_64_64) and not self.defined(index):
                raw = self.symbols[index][0] if index < len(self.symbols) else ""
                yield offset, self.name(index), raw, kind == R_X86_64_JUMP_SLOT


def scan_rel32(module, targets, opcodes=(b"\xe8", b"\xe9")):
    """(site, target, opcode) for every `opcode rel32` in executable segments that lands in
    `targets`. Byte scanning, not disassembly: a site can sit inside another instruction, so
    confirm interesting ones with disasm."""
    hits = []
    for base, body, _, flags in module.segments:
        if not flags & PF_X:
            continue
        for opcode in opcodes:
            pos = body.find(opcode)
            while pos != -1 and pos + len(opcode) + 4 <= len(body):
                rel = struct.unpack_from("<i", body, pos + len(opcode))[0]
                target = base + pos + len(opcode) + 4 + rel
                if target in targets:
                    hits.append((base + pos, target, opcode))
                pos = body.find(opcode, pos + 1)
    return sorted(hits)


RIP_REL = re.compile(rb"(?:[\x40-\x4f])?(?:\x8d|\x8b|\x89|\x3b|\x39|\x83|\x80|\xc7|\xc6|\xff)"
                     rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]")


def scan_rip_relative(module, targets):
    """(site, target) for REX? op modrm(rip) disp32 forms (lea/mov/cmp/call/jmp [rip+d]) whose
    effective address is in `targets`; the immediate forms (C7/C6/80/83) are sized accordingly."""
    immediate = {0xC7: 4, 0xC6: 1, 0x80: 1, 0x83: 1}
    hits = []
    for base, body, _, flags in module.segments:
        if not flags & PF_X:
            continue
        for match in RIP_REL.finditer(body):
            pos = match.start()
            op_at = pos + (1 if 0x40 <= body[pos] <= 0x4F else 0)
            disp_at = op_at + 2
            if disp_at + 4 > len(body):
                continue
            disp = struct.unpack_from("<i", body, disp_at)[0]
            end = disp_at + 4 + immediate.get(body[op_at], 0)
            target = base + end + disp
            if target in targets:
                hits.append((base + pos, target))
    return sorted(hits)


def plt_stubs(module, slots):
    """PLT stub address -> GOT slot, from `jmp qword ptr [rip+d]` (FF 25) aimed at a slot."""
    stubs = {}
    for base, body, _, flags in module.segments:
        if not flags & PF_X:
            continue
        pos = body.find(b"\xff\x25")
        while pos != -1 and pos + 6 <= len(body):
            target = base + pos + 6 + struct.unpack_from("<i", body, pos + 2)[0]
            if target in slots:
                stubs[base + pos] = target
            pos = body.find(b"\xff\x25", pos + 1)
    return stubs


# --------------------------------------------------------------------------------------------
# .eh_frame_hdr / .eh_frame: function ranges without symbols.


def _uleb(buf, pos):
    result = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, pos


def _sleb(buf, pos):
    result = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            if byte & 0x40:
                result -= 1 << shift
            return result, pos


def _encoded(module, vaddr, encoding, datarel=0):
    """Decodes one DW_EH_PE value stored at vaddr. Returns (value, size)."""
    fmt = encoding & 0x0F
    if fmt == 0x01 or fmt == 0x09:
        raw = module.read(vaddr, 16)
        value, used = (_uleb if fmt == 0x01 else _sleb)(raw, 0)
    else:
        width, code = {0x00: (8, "<Q"), 0x02: (2, "<H"), 0x03: (4, "<I"), 0x04: (8, "<Q"),
                       0x0A: (2, "<h"), 0x0B: (4, "<i"), 0x0C: (8, "<q")}[fmt]
        value, used = struct.unpack(code, module.read(vaddr, width))[0], width
    application = encoding & 0x70
    if application == 0x10:
        value += vaddr
    elif application == 0x30:
        value += datarel
    elif application not in (0x00,):
        raise ValueError(f"unsupported pointer application {application:#x}")
    return value & 0xFFFFFFFFFFFFFFFF, used


class EhIndex:
    """Sorted (start, end) function ranges read from the module's .eh_frame_hdr table."""

    def __init__(self, module):
        self.starts, self.ends = [], []
        header = module.eh_frame_hdr()
        if not header:
            return
        hdr, _ = header
        version, ptr_enc, count_enc, table_enc = module.read(hdr, 4)
        if version != 1 or table_enc == 0xFF:
            return
        pos = hdr + 4
        _, used = _encoded(module, pos, ptr_enc, hdr)
        pos += used
        count, used = _encoded(module, pos, count_enc, hdr)
        pos += used
        entry = {0x03: 4, 0x0B: 4, 0x04: 8, 0x0C: 8}[table_enc & 0x0F]
        cie_cache = {}
        ranges = []
        for i in range(count):
            at = pos + i * 2 * entry
            start, _ = _encoded(module, at, table_enc, hdr)
            fde, _ = _encoded(module, at + entry, table_enc, hdr)
            length = self._fde_range(module, fde, cie_cache)
            ranges.append((start, start + length))
        ranges.sort()
        self.starts = [s for s, _ in ranges]
        self.ends = [e for _, e in ranges]

    @staticmethod
    def _fde_range(module, fde, cie_cache):
        length = struct.unpack("<I", module.read(fde, 4))[0]
        pos = fde + 4
        if length == 0xFFFFFFFF:
            pos += 8
        cie_pointer_at = pos
        cie = cie_pointer_at - struct.unpack("<I", module.read(pos, 4))[0]
        pos += 4
        encoding = cie_cache.get(cie)
        if encoding is None:
            encoding = EhIndex._cie_fde_encoding(module, cie)
            cie_cache[cie] = encoding
        _, used = _encoded(module, pos, encoding)
        pos += used
        pc_range, _ = _encoded(module, pos, encoding & 0x0F)
        return pc_range

    @staticmethod
    def _cie_fde_encoding(module, cie):
        length = struct.unpack("<I", module.read(cie, 4))[0]
        body = module.read(cie + 4, min(length, 64) if length != 0xFFFFFFFF else 64)
        pos = 4  # CIE id
        version = body[pos]
        pos += 1
        end = body.index(0, pos)
        augmentation = body[pos:end].decode("ascii", "replace")
        pos = end + 1
        _, pos = _uleb(body, pos)  # code alignment
        _, pos = _sleb(body, pos)  # data alignment
        if version == 1:
            pos += 1
        else:
            _, pos = _uleb(body, pos)
        if not augmentation.startswith("z"):
            return 0x00
        _, pos = _uleb(body, pos)
        for char in augmentation[1:]:
            if char == "R":
                return body[pos]
            if char == "P":
                personality_enc = body[pos]
                pos += 1
                pos += {0x00: 8, 0x03: 4, 0x0B: 4, 0x04: 8, 0x0C: 8, 0x02: 2, 0x0A: 2}[
                    personality_enc & 0x0F]
            elif char == "L":
                pos += 1
        return 0x00

    def containing(self, offset):
        i = bisect.bisect_right(self.starts, offset) - 1
        if i >= 0 and offset < self.ends[i]:
            return self.starts[i], self.ends[i]
        return None

    def __len__(self):
        return len(self.starts)


# --------------------------------------------------------------------------------------------
# Analysis ELF


def build_analysis_elf(module, eh=None, dyn=None, relocate=True, stats=None):
    """ELF64 ET_EXEC with the loadable bytes at their original offsets, one section per loadable
    segment, an fn_<vaddr> symbol per EH-described function and, with `dyn`, the module's own
    relocations applied (load base 0) plus named import symbols."""
    eh = eh if eh is not None else EhIndex(module)
    stats = stats if stats is not None else {}
    header_end = EHDR.size + len(module.phdrs) * PHDR.size
    # Keep the original file offsets when they leave room for the headers; otherwise relocate.
    cursor = max([header_end] + [ph[2] + ph[5] for ph in module.phdrs if ph[0] in LOADABLE])
    phdrs = [list(ph) for ph in module.phdrs]
    placed = []
    for ph in phdrs:
        if ph[0] in LOADABLE:
            if ph[2] < header_end:
                ph[2] = (cursor + 0xFFF) & ~0xFFF
                cursor = ph[2] + ph[5]
            placed.append(ph)
    # Headers that describe memory inside a load (EH frame, DYNAMIC, TLS image) point at that
    # load's bytes; the rest (SCE_DYNLIBDATA, comments) have no bytes in the analysis copy.
    for ph in phdrs:
        if ph[0] in LOADABLE:
            continue
        covering = [load for load in placed
                    if ph[5] and load[3] <= ph[3] and ph[3] + ph[5] <= load[3] + load[5]]
        if covering:
            ph[2] = covering[0][2] + (ph[3] - covering[0][3])
        else:
            ph[2], ph[5] = 0, 0
    size = max([header_end] + [ph[2] + ph[5] for ph in placed])
    out = bytearray(size)
    for original, ph in zip(module.phdrs, phdrs):
        if ph[0] in LOADABLE and ph[5]:
            body = module.read(original[3], original[5])
            out[ph[2]:ph[2] + len(body)] = body

    def file_offset(vaddr, size):
        for ph in placed:
            if ph[3] <= vaddr and vaddr + size <= ph[3] + ph[5]:
                return ph[2] + (vaddr - ph[3])
        return None

    applied = outside = 0
    if dyn is not None and relocate:
        for offset, value in dyn.resolved():
            at = file_offset(offset, 8)
            if at is None:
                outside += 1  # .bss or a non-loadable offset: nothing to write in the file
                continue
            struct.pack_into("<Q", out, at, value)
            applied += 1
    stats.update(relocations_applied=applied, relocations_outside=outside)

    # Section table: null, one section per load, .symtab, .strtab, .shstrtab.
    shstr = bytearray(b"\0")

    def shname(name):
        offset = len(shstr)
        shstr.extend(name.encode() + b"\0")
        return offset

    sections = [SHDR.pack(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    counts = {}
    for ph in placed:
        executable, writable = ph[1] & PF_X, ph[1] & PF_W
        base = ".text" if executable else (".data" if writable else ".rodata")
        if ph[0] == PT_SCE_RELRO:
            base = ".data.rel.ro"
        counts[base] = counts.get(base, 0) + 1
        name = base if counts[base] == 1 else f"{base}.{counts[base] - 1}"
        flags = 0x2 | (0x4 if executable else 0) | (0x1 if writable or ph[0] == PT_SCE_RELRO else 0)
        sections.append(SHDR.pack(shname(name), 1, flags, ph[3], ph[2], ph[5], 0, 0, 16, 0))
        if ph[6] > ph[5]:
            sections.append(SHDR.pack(shname(name.replace(base, ".bss", 1)), 8, flags,
                                      ph[3] + ph[5], ph[2] + ph[5], ph[6] - ph[5], 0, 0, 16, 0))

    def section_index(vaddr):
        for index, raw in enumerate(sections[1:], 1):
            _, sh_type, _, addr, _, sh_size, *_ = SHDR.unpack(raw)
            if sh_type == 1 and addr <= vaddr < addr + sh_size:
                return index
        return 0

    strtab = bytearray(b"\0")
    symtab = bytearray(SYM.pack(0, 0, 0, 0, 0, 0))

    def add_symbol(name, info, vaddr, size):
        offset = len(strtab)
        strtab.extend(name.encode() + b"\0")
        symtab.extend(SYM.pack(offset, info, 0, section_index(vaddr), vaddr, size))

    for start, end in zip(eh.starts, eh.ends):
        add_symbol(f"fn_{start:x}", 0x12, start, end - start)  # GLOBAL FUNC
    imports = stubs = 0
    if dyn is not None:
        slots, used = {}, {}
        for slot, name, _, is_function in dyn.imports():
            if slot in slots:
                continue
            name = symbol_name(name)
            count = used.get(name, 0)
            used[name] = count + 1
            unique = name if count == 0 else f"{name}_{count + 1}"
            slots[slot] = (unique, is_function)
            add_symbol(f"__imp_{unique}", 0x11, slot, 8)  # GLOBAL OBJECT
            imports += 1
        for stub, slot in sorted(plt_stubs(module, slots).items()):
            name, is_function = slots[slot]
            if is_function:
                add_symbol(name, 0x12, stub, 6)
                stubs += 1
    stats.update(imports=imports, plt_stubs=stubs)

    def append_blob(blob, align=8):
        nonlocal out
        out.extend(b"\0" * ((-len(out)) % align))
        at = len(out)
        out.extend(blob)
        return at

    symtab_at = append_blob(symtab)
    strtab_at = append_blob(strtab)
    symtab_index = len(sections)
    sections.append(SHDR.pack(shname(".symtab"), 2, 0, 0, symtab_at, len(symtab),
                              symtab_index + 1, 1, 8, SYM.size))
    sections.append(SHDR.pack(shname(".strtab"), 3, 0, 0, strtab_at, len(strtab), 0, 0, 1, 0))
    shstr_index = len(sections)
    name_offset = shname(".shstrtab")
    shstr_at = append_blob(shstr, 1)
    sections.append(SHDR.pack(name_offset, 3, 0, 0, shstr_at, len(shstr), 0, 0, 1, 0))
    shoff = append_blob(b"".join(sections))

    ehdr = list(module.ehdr)
    ehdr[1] = 2  # ET_EXEC: tools reject the SCE e_types
    ehdr[5] = EHDR.size
    ehdr[6] = shoff
    ehdr[9] = PHDR.size
    ehdr[11] = SHDR.size
    ehdr[12] = len(sections)
    ehdr[13] = shstr_index
    out[0:EHDR.size] = EHDR.pack(*ehdr)
    for i, ph in enumerate(phdrs):
        out[EHDR.size + i * PHDR.size:EHDR.size + (i + 1) * PHDR.size] = PHDR.pack(*ph)
    return bytes(out)


# --------------------------------------------------------------------------------------------
# Disassembly


def find_objdump(explicit=None):
    candidates = [explicit, os.environ.get("LLVM_OBJDUMP"), shutil.which("llvm-objdump")]
    for root in (r"C:\Program Files\Microsoft Visual Studio", r"D:\Program Files\Microsoft Visual Studio"):
        for path in sorted(Path(root).glob("*/*/VC/Tools/Llvm/x64/bin/llvm-objdump.exe")) if Path(root).is_dir() else []:
            candidates.append(str(path))
    candidates += ["/opt/homebrew/opt/llvm/bin/llvm-objdump", "/usr/local/opt/llvm/bin/llvm-objdump"]
    for sdk in (os.environ.get("ANDROID_NDK_HOME"), os.environ.get("ANDROID_NDK_ROOT")):
        if sdk:
            candidates += [str(p) for p in Path(sdk).glob("toolchains/llvm/prebuilt/*/bin/llvm-objdump*")]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    raise SystemExit("llvm-objdump not found: pass --objdump or set LLVM_OBJDUMP")


def disassemble(elf_path, start, end, objdump, mark=None, base=None, syntax="intel"):
    args = [objdump, "-d", f"--start-address={start:#x}", f"--stop-address={end:#x}", str(elf_path)]
    if syntax == "intel":
        args.insert(2, "-Mintel")
    text = subprocess.run(args, capture_output=True, text=True, check=True).stdout
    lines = []
    for line in text.splitlines():
        match = re.match(r"\s*([0-9a-f]+):\s", line)
        if not match:
            if "file format" in line or line.startswith("Disassembly") or not line.strip():
                continue
            lines.append(line)
            continue
        address = int(match.group(1), 16)
        prefix = "==>" if address == mark else "   "
        runtime = f" [{base + address:#x}]" if base is not None else ""
        lines.append(f"{prefix}{line.rstrip()}{runtime}")
    if mark is not None and not any(l.startswith("==>") for l in lines):
        lines.append(f"(note: {mark:#x} is not an instruction boundary in this decode)")
    return "\n".join(lines)


# --------------------------------------------------------------------------------------------
# Log parsing

LOAD_RE = re.compile(r"Loading module (\S+) to (0x[0-9a-fA-F]+)")
# Frame lines of the crash report written by src/core/signals.cpp (Windows).
FRAME_RE = re.compile(r"frame #\s*(\d+)\s+(?:(\S+)\+(0x[0-9a-fA-F]+) \()?(0x[0-9a-fA-F]+)")
CRASH_RES = [
    re.compile(r"Unhandled Exception code (0x[0-9a-fA-F]+) at (0x[0-9a-fA-F]+)"),
    re.compile(r"(SIG\w+|signal \d+)\S*.*?\b(?:at|rip|pc)[=: ]+(0x[0-9a-fA-F]+)", re.IGNORECASE),
]


def parse_log(path):
    """Loaded modules, crash lines and the frames reported after the last crash line."""
    modules, crashes, frames = [], [], []
    with open(path, "r", encoding="utf-8", errors="replace") as log:
        for number, line in enumerate(log, 1):
            match = LOAD_RE.search(line)
            if match:
                modules.append((match.group(1), int(match.group(2), 16)))
                continue
            match = FRAME_RE.search(line)
            if match and crashes:
                frames.append((int(match.group(1)), int(match.group(4), 16)))
                continue
            for regex in CRASH_RES:
                match = regex.search(line)
                if match:
                    crashes.append((number, match.group(1), int(match.group(2), 16), line.strip()))
                    frames = []
                    break
    return modules, crashes, frames


def candidate_modules(modules, address):
    """Loaded modules whose base is at or below the address, nearest first."""
    below = [(address - base, name, base) for name, base in modules if base <= address]
    return [(name, base) for _, name, base in sorted(below)]


def find_module_file(name, sources):
    for source in sources:
        for path in (name, f"sce_module/{name}", f"sce_sys/{name}"):
            if source.exists(path):
                return source, path
    return None, None


# --------------------------------------------------------------------------------------------
# Commands


def load_module(path, name=None):
    path = Path(path)
    return Module(path.read_bytes(), name or path.name)


def cmd_ls(args):
    source = GameSource(args.game)
    for name, is_file, size in source.listdir(args.path or ""):
        print(f"{size:>14,}  {name}" if is_file else f"{'<dir>':>14}  {name}/")
    return 0


def cmd_extract(args):
    data = GameSource(args.game).read(args.path)
    out = Path(args.output or Path(args.path).name)
    out.write_bytes(data)
    print(f"{out}: {len(data):,} bytes")
    return 0


def cmd_elf(args):
    module = load_module(args.module)
    eh = EhIndex(module)
    dyn = None if args.raw else DynamicInfo(module, nid_names(args.nid_table))
    stats = {}
    elf = build_analysis_elf(module, eh, dyn, relocate=not args.raw, stats=stats)
    out = Path(args.output or Path(args.module).with_suffix(".analysis.elf"))
    out.write_bytes(elf)
    print(f"{out}: {module.kind} -> analysis ELF, {len(module.segments)} loadable segments, "
          f"{len(eh)} EH functions, extent {module.extent:#x}")
    if dyn is not None:
        print(f"  relocations applied {stats.get('relocations_applied', 0)} "
              f"(+{stats.get('relocations_outside', 0)} in .bss, left to run time); "
              f"imports {stats.get('imports', 0)}, PLT stubs named {stats.get('plt_stubs', 0)}")
    for ph in module.phdrs:
        flags = "".join(c if ph[1] & bit else "-" for c, bit in (("R", 4), ("W", 2), ("X", 1)))
        print(f"  {PHDR_TYPE_NAMES.get(ph[0], hex(ph[0])):<16} vaddr {ph[3]:#010x} "
              f"filesz {ph[5]:#x} memsz {ph[6]:#x} {flags}")
    return 0


def cmd_imports(args):
    module = load_module(args.module)
    dyn = DynamicInfo(module, nid_names(args.nid_table))
    rows = list(dyn.imports())
    wanted = [w.lower() for w in args.name]
    if wanted:
        rows = [r for r in rows if any(w in r[1].lower() or w in r[2].lower() for w in wanted)]
    slots = {slot: name for slot, name, _, _ in rows}
    stubs = plt_stubs(module, slots)
    stub_of = {slot: stub for stub, slot in stubs.items()}
    calls = {}
    if args.calls:
        for site, target, _ in scan_rel32(module, set(stubs), (b"\xe8", b"\xe9")):
            calls.setdefault(stubs[target], []).append(site)
        for site, target in scan_rip_relative(module, set(slots)):
            if site not in stubs:  # the stub's own jmp [rip+d]
                calls.setdefault(target, []).append(site)
    for slot, name, raw, is_function in rows:
        stub = stub_of.get(slot)
        where = (f"plt {stub:#010x}" if stub is not None
                 else ("no plt" if is_function else "object"))
        print(f"{name:<48} {raw:<40} got {slot:#010x}  {where}")
        for site in sorted(calls.get(slot, [])):
            print(f"    called from {site:#x}")
    print(f"{len(rows)} imports")
    return 0


def cmd_xrefs(args):
    module = load_module(args.module)
    targets = {int(t, 0) for t in args.offset}
    eh = EhIndex(module)

    def where(site):
        fn = eh.containing(site)
        return f"fn_{fn[0]:x}+{site - fn[0]:#x}" if fn else "?"

    for site, target, opcode in scan_rel32(module, targets):
        kind = "call" if opcode == b"\xe8" else "jmp "
        print(f"{kind} {site:#x} ({where(site)}) -> {target:#x}")
    for site, target in scan_rip_relative(module, targets):
        print(f"rip  {site:#x} ({where(site)}) -> {target:#x}")
    if args.data:
        dyn = DynamicInfo(module)
        for offset, value in dyn.resolved():
            if value in targets:
                print(f"data {offset:#x} holds {value:#x} (relocated pointer, e.g. a vtable slot)")
    return 0


def cmd_str(args):
    module = load_module(args.module)
    for text in args.offset:
        offset = int(text, 0)
        data = module.read(offset, args.length)
        if args.hex:
            print(f"{offset:#x}: {data.hex(' ')}")
        else:
            print(f"{offset:#x}: {data.split(bytes(1))[0].decode('utf-8', 'replace')!r}")
    return 0


def describe_function(module, eh, offset):
    found = eh.containing(offset)
    if found:
        start, end = found
        return start, end, f"fn_{start:x} [{start:#x}, {end:#x}) size {end - start:#x}, +{offset - start:#x}"
    return None, None, "no EH-described function covers this offset"


def cmd_func(args):
    module = load_module(args.module)
    eh = EhIndex(module)
    offset = int(args.offset, 0)
    _, _, text = describe_function(module, eh, offset)
    print(f"{module.name} {offset:#x}: {text}"
          f"{'' if module.executable(offset) else '  (not in an executable segment)'}")
    return 0


def disasm_report(module, offset, objdump, window, base=None, max_function=0x2000, syntax="intel"):
    eh = EhIndex(module)
    start, end, text = describe_function(module, eh, offset)
    report = [f"{module.name} +{offset:#x}"
              f"{f' (runtime {base + offset:#x}, base {base:#x})' if base is not None else ''}",
              f"function: {text}"]
    if not module.executable(offset):
        report.append("warning: offset is not inside an executable segment")
    if start is None or end - start > max_function:
        start, end = max(0, offset - window), offset + window
        report.append(f"showing window [{start:#x}, {end:#x})")
    with tempfile.TemporaryDirectory() as tmp:
        elf = Path(tmp) / "analysis.elf"
        elf.write_bytes(build_analysis_elf(module, eh))
        report.append(disassemble(elf, start, end, objdump, mark=offset, base=base, syntax=syntax))
    return "\n".join(report)


def cmd_disasm(args):
    module = load_module(args.module)
    base = int(args.base, 0) if args.base else None
    print(disasm_report(module, int(args.offset, 0), find_objdump(args.objdump), args.window,
                        base, args.max_function, args.syntax))
    return 0


def locate(address, modules, sources, cache):
    """(module, base) holding a runtime address, from the loaded-module list and the files."""
    for name, base in candidate_modules(modules, address):
        if name not in cache:
            source, path = find_module_file(name, sources)
            cache[name] = Module(source.read(path), name) if source else None
        module = cache[name]
        if module is not None and address - base < module.extent:
            return module, base
    return None, None


def cmd_crash(args):
    modules, crashes, frames = parse_log(args.log)
    if args.address:
        crashes = [(0, "--address", int(args.address, 0), "")]
    if not crashes:
        raise SystemExit(f"{args.log}: no crash line found (pass --address)")
    if not modules:
        raise SystemExit(f"{args.log}: no 'Loading module ... to 0x...' lines")
    sources = [GameSource(p) for p in args.game] + [GameSource(p) for p in args.sys_modules]
    objdump = find_objdump(args.objdump)
    line, code, address, text = crashes[-1]
    print(f"crash: {text or hex(address)}" + (f"  (log line {line})" if line else ""))
    print("modules loaded below it (nearest first):")
    candidates = candidate_modules(modules, address)
    for name, base in candidates[:6]:
        print(f"  {name:<32} base {base:#x}  offset {address - base:#x}")
    for name, base in candidates:
        source, path = find_module_file(name, sources)
        if source is None:
            print(f"{name}: file not found in {', '.join(map(str, sources))}; skipping")
            continue
        module = Module(source.read(path), name)
        offset = address - base
        if offset >= module.extent:
            print(f"{name}: offset {offset:#x} is past its image ({module.extent:#x}); not this one")
            continue
        if args.save:
            out = Path(args.save)
            out.mkdir(parents=True, exist_ok=True)
            (out / name).write_bytes(module.raw)
            (out / f"{name}.analysis.elf").write_bytes(build_analysis_elf(module))
            print(f"saved {out / name} and {out / (name + '.analysis.elf')}")
        print()
        print(disasm_report(module, offset, objdump, args.window, base, args.max_function,
                            args.syntax))
        cache = {name: module}
        for depth, return_address in frames[:args.frames]:
            caller, caller_base = locate(return_address, modules, sources, cache)
            print()
            if caller is None:
                print(f"frame #{depth} {return_address:#x}: not in a loaded module")
                continue
            print(f"frame #{depth} returns to:")
            print(disasm_report(caller, return_address - caller_base, objdump, 0x30, caller_base,
                                args.max_function, args.syntax))
        return 0
    raise SystemExit("no loaded module covers the address (JIT/host code, or modules not found)")


# --------------------------------------------------------------------------------------------
# Self test: synthetic ZAR, SELF and EH data.


def _selftest_zar():
    payload = bytes(range(256)) * 700  # 179200 bytes: spans three 64 KiB blocks
    blocks = [payload[i:i + ZarReader.BLOCK] for i in range(0, len(payload), ZarReader.BLOCK)]
    blocks[-1] = blocks[-1] + b"\0" * (ZarReader.BLOCK - len(blocks[-1]))
    # Stored (uncompressed) blocks: size-1 == 0xFFFF marks a raw 64 KiB block.
    data = b"".join(blocks)
    record = struct.pack(">Q16H", 0, *([0xFFFF] * len(blocks) + [0] * (16 - len(blocks))))
    names = bytearray(b"\x7f\xff")  # unused slot so name offsets are not zero
    def add(name):
        at = len(names)
        names.extend(bytes([len(name)]) + name.encode())
        return at
    root = struct.pack(">4I", 0x7FFFFFFF, 1, 1, 0)
    sub = struct.pack(">4I", add("SCE_MODULE"), 2, 1, 0)
    file = struct.pack(">4I", 0x80000000 | add("Eboot.bin"), 0, len(payload), 0)
    tree = root + sub + file
    body = data + record + bytes(names) + tree
    sections = [(0, len(data)), (len(data), len(record)), (len(data) + len(record), len(names)),
                (len(data) + len(record) + len(names), len(tree)), (len(body), 0), (len(body), 0)]
    total = len(body) + ZarReader.FOOTER.size
    footer = ZarReader.FOOTER.pack(*[v for s in sections for v in s], b"\0" * 32, total,
                                   ZarReader.VERSION1, ZarReader.MAGIC)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "t.zar"
        path.write_bytes(body + footer)
        zar = ZarReader(path)
        assert zar.read("sce_module/eboot.BIN") == payload, "zar read mismatch"
        assert [n for n, *_ in zar.listdir("")] == ["SCE_MODULE"]
        assert zar.lookup("missing") is None
        zar.f.close()


def _selftest_module():
    # Code: two functions at 0x0 and 0x20, described by .eh_frame + .eh_frame_hdr in a RO load.
    code = bytes([0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x07, 0x5D, 0xC3]) + b"\x90" * 23
    code += bytes([0x31, 0xC0, 0xC3]) + b"\x90" * 29
    ro_base = 0x1000
    cie = struct.pack("<IIB", 0, 0, 1) + b"zR\0" + bytes([1, 0x78, 16, 1, 0x1B]) + b"\0" * 3
    cie = struct.pack("<I", len(cie) - 4) + cie[4:]
    eh_frame = bytearray(cie)
    fde_offsets = []
    for start, size in ((0x0, 9), (0x20, 3)):
        fde_at = ro_base + len(eh_frame)
        fde_offsets.append((start, fde_at))
        pc_field = fde_at + 8
        body = struct.pack("<Ii", fde_at + 4 - ro_base, start - pc_field) + struct.pack("<I", size) + b"\0"
        body += b"\0" * ((-len(body) - 4) % 4)
        eh_frame += struct.pack("<I", len(body)) + body
    eh_frame += b"\0" * 4
    hdr_at = ro_base + len(eh_frame)
    hdr = bytearray(bytes([1, 0x1B, 0x03, 0x3B]))
    hdr += struct.pack("<i", ro_base - (hdr_at + 4)) + struct.pack("<I", len(fde_offsets))
    for start, fde_at in fde_offsets:
        hdr += struct.pack("<ii", start - hdr_at, fde_at - hdr_at)
    rodata = bytes(eh_frame) + bytes(hdr)
    phdrs = [(PT_LOAD, PF_X | 4, 0x1000, 0, 0, len(code), len(code), 0x1000),
             (PT_LOAD, 4, 0x2000, ro_base, ro_base, len(rodata), len(rodata), 0x1000),
             (PT_GNU_EH_FRAME, 4, 0x2000 + len(eh_frame), hdr_at, hdr_at, len(hdr), len(hdr), 4)]
    ident = ELF_MAGIC + bytes([2, 1, 1, 9]) + b"\0" * 8
    ehdr = EHDR.pack(ident, 0xFE10, 0x3E, 1, 0, EHDR.size, 0, 0, EHDR.size, PHDR.size,
                     len(phdrs), 0, 0, 0)
    elf_headers = ehdr + b"".join(PHDR.pack(*p) for p in phdrs)
    # SELF: header, two blocked segments (ids 0 and 1), embedded ELF headers, then the data.
    data_at = 32 + 2 * 32 + len(elf_headers)
    data_at += (-data_at) % 16
    segs = [(0x800 | (0 << 20), data_at, len(code), len(code)),
            (0x800 | (1 << 20), data_at + len(code), len(rodata), len(rodata))]
    self_header = struct.pack("<IBBBBBBHHHIIHHI", SELF_MAGIC, 0, 1, 1, 0x12, 1, 1, 0, 0, 0,
                              0, 0, len(segs), 0x22, 0)
    blob = bytearray(self_header + b"".join(struct.pack("<4Q", *s) for s in segs) + elf_headers)
    blob += b"\0" * (data_at - len(blob))
    blob += code + rodata
    module = Module(bytes(blob), "synthetic")
    assert module.read(0x4, 3) == bytes([0x48, 0x8B, 0x07])
    eh = EhIndex(module)
    assert eh.starts == [0x0, 0x20] and eh.ends == [0x9, 0x23], (eh.starts, eh.ends)
    assert eh.containing(0x4) == (0x0, 0x9) and eh.containing(0x10) is None
    elf = build_analysis_elf(module, eh)
    again = Module(elf, "analysis")
    assert again.read(0x20, 3) == bytes([0x31, 0xC0, 0xC3])
    assert EhIndex(again).starts == [0x0, 0x20]
    encrypted = bytearray(blob)
    struct.pack_into("<Q", encrypted, 32, 0x800 | 0x2)
    try:
        Module(bytes(encrypted), "encrypted").phdr_bytes(0)
        raise AssertionError("encrypted segment accepted")
    except SystemExit:
        pass
    return elf


def _selftest_dynamic():
    # Module: code with a PLT stub and a call to it, data with a vtable-like pointer slot and a GOT
    # slot, and DYNAMIC/SCE_DYNLIBDATA describing one RELATIVE and one JUMP_SLOT relocation.
    code_base, data_base = 0x0, 0x1000
    code = bytearray(b"\x90" * 0x40)
    code[0x0:0x5] = b"\xe8" + struct.pack("<i", 0x20 - 0x5)            # call 0x20 (the stub)
    code[0x10:0x17] = b"\x48\x8d\x05" + struct.pack("<i", 0x1000 - 0x17)  # lea rax,[rip+..] -> 0x1000
    code[0x20:0x26] = b"\xff\x25" + struct.pack("<i", 0x1008 - 0x26)   # jmp [rip+..] -> GOT 0x1008
    data = bytearray(16)
    strtab = b"\0yFVnOdGxvZY#A#B\0"
    symtab = SYM.pack(0, 0, 0, 0, 0, 0) + SYM.pack(1, 0x12, 0, 0, 0, 0)
    rela = RELA.pack(0x1000, R_X86_64_RELATIVE, 0x10)
    jmprel = RELA.pack(0x1008, (1 << 32) | R_X86_64_JUMP_SLOT, 0)
    lib = strtab + b"\0" * ((-len(strtab)) % 8)
    offsets = {}
    for key, blob in (("sym", symtab), ("rela", rela), ("jmp", jmprel)):
        offsets[key] = len(lib)
        lib += blob
    dyn = b"".join(struct.pack("<qQ", t, v) for t, v in (
        (DT_SCE_STRTAB, 0), (DT_SCE_STRSZ, len(strtab)), (DT_SCE_SYMTAB, offsets["sym"]),
        (DT_SCE_SYMTABSZ, len(symtab)), (DT_SCE_RELA, offsets["rela"]), (DT_SCE_RELASZ, len(rela)),
        (DT_SCE_JMPREL, offsets["jmp"]), (DT_SCE_PLTRELSZ, len(jmprel)), (0, 0)))
    phdrs_at = EHDR.size
    phnum = 4
    code_at = 0x1000
    data_at = code_at + len(code)
    dyn_at = data_at + len(data)
    lib_at = dyn_at + len(dyn)
    phdrs = [(PT_LOAD, PF_X | 4, code_at, code_base, code_base, len(code), len(code), 0x1000),
             (PT_LOAD, PF_W | 4, data_at, data_base, data_base, len(data), len(data), 0x1000),
             (PT_DYNAMIC, 4, dyn_at, 0, 0, len(dyn), len(dyn), 8),
             (PT_SCE_DYNLIBDATA, 4, lib_at, 0, 0, len(lib), 0, 8)]
    ident = ELF_MAGIC + bytes([2, 1, 1, 9]) + b"\0" * 8
    blob = bytearray(EHDR.pack(ident, 0xFE10, 0x3E, 1, 0, phdrs_at, 0, 0, EHDR.size, PHDR.size,
                               phnum, 0, 0, 0))
    blob += b"".join(PHDR.pack(*p) for p in phdrs)
    blob += b"\0" * (code_at - len(blob))
    blob += code + data + dyn + lib
    module = Module(bytes(blob), "dynamic")
    info = DynamicInfo(module, {"yFVnOdGxvZY": "scePadSetVibration"})
    assert list(info.resolved()) == [(0x1000, 0x10)], list(info.resolved())
    assert [(s, n) for s, n, _, _ in info.imports()] == [(0x1008, "scePadSetVibration")]
    assert plt_stubs(module, {0x1008}) == {0x20: 0x1008}
    assert scan_rel32(module, {0x20}) == [(0x0, 0x20, b"\xe8")]
    assert scan_rip_relative(module, {0x1000}) == [(0x10, 0x1000)]
    stats = {}
    elf = build_analysis_elf(module, EhIndex(module), info, stats=stats)
    again = Module(elf, "analysis")
    assert struct.unpack("<Q", again.read(0x1000, 8))[0] == 0x10
    assert stats["relocations_applied"] == 1 and stats["imports"] == 1 and stats["plt_stubs"] == 1
    assert b"scePadSetVibration\0" in elf and b"__imp_scePadSetVibration\0" in elf


def cmd_selftest(args):
    _selftest_zar()
    elf = _selftest_module()
    _selftest_dynamic()
    log = ("[Core.Linker] <Info> module.cpp:148 LoadModuleToMemory: Loading module eboot.bin to 0x400000\n"
           "[Core.Linker] <Info> module.cpp:148 LoadModuleToMemory: Loading module libc.prx to 0x800000\n"
           "[Debug] <Critical> signals.cpp:193 SignalHandler: Unhandled Exception code 0xc0000005 at 0x400004\n"
           "[Debug] <Critical> signals.cpp:184 ReportUnhandledException:   frame #0  eboot.bin+0x20 (0x400020)\n"
           "[Debug] <Critical> signals.cpp:184 ReportUnhandledException:   frame #1  0x7\n")
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "log.txt"
        path.write_text(log)
        modules, crashes, frames = parse_log(path)
        assert modules == [("eboot.bin", 0x400000), ("libc.prx", 0x800000)]
        assert crashes and crashes[0][2] == 0x400004
        assert frames == [(0, 0x400020), (1, 0x7)], frames
        assert candidate_modules(modules, 0x400004) == [("eboot.bin", 0x400000)]
        objdump = None
        try:
            objdump = find_objdump(args.objdump)
        except SystemExit:
            print("selftest: llvm-objdump not found, disassembly check skipped")
        if objdump:
            elf_path = Path(tmp) / "t.elf"
            elf_path.write_bytes(elf)
            text = disassemble(elf_path, 0, 9, objdump, mark=4, syntax="intel")
            assert "==>" in text and "mov" in text, text
    print("selftest: ok")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("ls", help="list a directory inside a .zar or game directory")
    p.add_argument("game")
    p.add_argument("path", nargs="?")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("extract", help="copy one file out of a .zar or game directory")
    p.add_argument("game")
    p.add_argument("path")
    p.add_argument("-o", "--output")
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("elf", help="SELF/ELF -> analysis ELF (for IDA, objdump, gdb)")
    p.add_argument("module")
    p.add_argument("-o", "--output")
    p.add_argument("--raw", action="store_true",
                   help="original loadable bytes only: no relocations, no import symbols")
    p.add_argument("--nid-table", help="STUB(\"nid\", name) table (default: src/core/aerolib/aerolib.inl)")
    p.set_defaults(func=cmd_elf)

    p = sub.add_parser("imports", help="imports with NID, name, GOT slot and PLT stub")
    p.add_argument("module")
    p.add_argument("name", nargs="*", help="substring filter on the name or NID")
    p.add_argument("--calls", action="store_true", help="also list call sites")
    p.add_argument("--nid-table")
    p.set_defaults(func=cmd_imports)

    p = sub.add_parser("xrefs", help="calls/jumps and RIP-relative references to module offsets")
    p.add_argument("module")
    p.add_argument("offset", nargs="+")
    p.add_argument("--data", action="store_true",
                   help="also relocated pointers holding the offset (vtables, tables)")
    p.set_defaults(func=cmd_xrefs)

    p = sub.add_parser("str", help="C string (or --hex bytes) at module offsets")
    p.add_argument("module")
    p.add_argument("offset", nargs="+")
    p.add_argument("--length", type=lambda v: int(v, 0), default=256)
    p.add_argument("--hex", action="store_true")
    p.set_defaults(func=cmd_str)

    p = sub.add_parser("func", help="function containing a module offset")
    p.add_argument("module")
    p.add_argument("offset")
    p.set_defaults(func=cmd_func)

    def disasm_options(p):
        p.add_argument("--objdump")
        p.add_argument("--window", type=lambda v: int(v, 0), default=0x80,
                       help="bytes around the offset when no small function covers it")
        p.add_argument("--max-function", type=lambda v: int(v, 0), default=0x2000,
                       help="functions larger than this are shown as a window")
        p.add_argument("--syntax", choices=("intel", "att"), default="intel")

    p = sub.add_parser("disasm", help="disassemble around a module offset")
    p.add_argument("module")
    p.add_argument("offset")
    p.add_argument("--base", help="runtime load base, to also print runtime addresses")
    disasm_options(p)
    p.set_defaults(func=cmd_disasm)

    p = sub.add_parser("crash", help="log crash -> module offset -> disassembly")
    p.add_argument("log")
    p.add_argument("--game", action="append", required=True,
                   help=".zar or directory; repeat for an update, update first")
    p.add_argument("--sys-modules", action="append", default=[],
                   help="directory with system modules (user/sys_modules)")
    p.add_argument("--address", help="use this address instead of the last crash line")
    p.add_argument("--save", help="directory to keep the module and its analysis ELF")
    p.add_argument("--frames", type=int, default=0,
                   help="also disassemble the callers of the first N reported frames")
    disasm_options(p)
    p.set_defaults(func=cmd_crash)

    p = sub.add_parser("selftest", help="check the parsers on synthetic inputs")
    p.add_argument("--objdump")
    p.set_defaults(func=cmd_selftest)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
