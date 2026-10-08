#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build independent PKG read expectations using the existing Python extractor.

Requires cryptography. Outputs metadata and selected fully extracted files to a
user-chosen evidence directory; never writes into the source package directory.
Run shadps4_pkg_read verify PACKAGE manifest.json against the generated manifest.
"""
import argparse
import hashlib
import json
import random
from pathlib import Path
from zar_packer.pkg import PkgFile, _decrypt_pfs, _decompress_pfsc
from zar_packer.sfo import parse_sfo


def digest_file(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for data in iter(lambda: f.read(1024 * 1024), b""):
            h.update(data)
    return h.hexdigest()


def reference_read(pkg, source, node, offset, length):
    out = bytearray()
    while length:
        block, inside = divmod(offset, 65536)
        a, b = pkg.sector_map[node.loc + block:node.loc + block + 2]
        start = pkg.pfsc_offset + a
        aligned = start & ~4095
        count = (start - aligned + b - a + 4095) & ~4095
        source.seek(pkg.header["pfs_image_offset"] + aligned)
        encrypted = source.read(count)
        if len(encrypted) != count:
            raise ValueError("short reference read")
        plain = _decrypt_pfs(pkg._data_key, pkg._tweak_key, encrypted, aligned // 4096)
        packed = plain[start - aligned:start - aligned + b - a]
        data = packed if b - a == 65536 else _decompress_pfsc(packed)
        take = min(length, 65536 - inside)
        out += data[inside:inside + take]
        offset += take
        length -= take
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--extract-largest", type=int, default=1)
    args = parser.parse_args()
    pkg = PkgFile(args.package)
    pkg.open()
    pkg.prepare()
    args.output.mkdir(parents=True, exist_ok=False)
    extracted = args.output / "extracted"
    pkg.extract_sce_sys(extracted)
    metadata = parse_sfo(pkg.sfo)
    entries = {}
    reads = []
    for f in sorted((extracted / "sce_sys").iterdir()):
        if f.is_file():
            name = f.relative_to(extracted).as_posix()
            entries[name] = dict(path=name, size=f.stat().st_size, directory=False)
            reads.append(dict(path=name, offset=0, size=f.stat().st_size, sha256=digest_file(f)))
    files = list(pkg.iter_files())
    largest = sorted(files, key=lambda e: pkg.inodes[e.inode].size, reverse=True)[:args.extract_largest]
    random_source = random.Random(20261007)
    beyond_4g = 0
    with args.package.open("rb") as source:
        for entry in files:
            node = pkg.inodes[entry.inode]
            entries[entry.path] = dict(path=entry.path, size=node.size, directory=False)
            positions = {0, max(0, node.size - 8192), min(65520, node.size)}
            for _ in range(4):
                positions.add(random_source.randrange(max(1, node.size)))
            for offset in sorted(positions):
                length = min(8192, node.size - offset)
                data = reference_read(pkg, source, node, offset, length)
                reads.append(dict(path=entry.path, offset=offset, size=length,
                                  sha256=hashlib.sha256(data).hexdigest()))
                if length and pkg.header["pfs_image_offset"] + pkg.pfsc_offset + pkg.sector_map[node.loc + offset // 65536] >= 2**32:
                    beyond_4g += 1
            if entry in largest or entry.path.endswith(("eboot.bin", ".prx", ".sprx")) or node.size <= 1024 * 1024:
                path = extracted / entry.path
                path.parent.mkdir(parents=True, exist_ok=True)
                pkg._extract_one(source, node, path, pkg.header["pfs_image_offset"])
                if path.stat().st_size != node.size:
                    raise ValueError(f"short extraction: {entry.path}")
                reads.append(dict(path=entry.path, offset=0, size=node.size, sha256=digest_file(path)))
                print(f"extracted {entry.path}: {node.size} bytes", flush=True)
    manifest = dict(package=args.package.name, metadata=metadata, entries=list(entries.values()), reads=reads,
                    reads_beyond_4g=beyond_4g,
                    full_extracted_bytes=sum(p.stat().st_size for p in extracted.rglob("*") if p.is_file()))
    (args.output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2))
    print(json.dumps({k:manifest[k] for k in ("package", "reads_beyond_4g", "full_extracted_bytes")}), flush=True)


if __name__ == "__main__":
    main()
