#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Copy the XR cinema GLBs for the APK with ASTC 4x4 textures.

  python tools/xr/compress_cinema_textures.py [--out build/xr-cinema/apk-models]

The authored GLBs (assets/xr/cinema/models, including the git-ignored
local/ ones) keep PNG images: Lite Editor runs on desktop GPUs, which have
no ASTC. The headset gets the same GLBs with every embedded PNG replaced by
a KTX2 holding ASTC 4x4 blocks and its full mip chain (1 byte per texel
instead of 4, no GPU mip generation on device). Colour slots (base colour,
emissive) are encoded sRGB, everything else linear, the same split
Lite Engine applies to PNGs; normal-map mips are renormalised.

Needs astcenc (ARM's encoder): $ASTCENC, or astcenc-avx2/astcenc on PATH,
or C:/workspace/tools/astcenc/bin. Encoded blocks are cached by source
bytes under build/xr-cinema/astc-cache, so a rerun only encodes what
changed.
"""
import argparse
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'assets/xr/cinema/models'
CACHE = ROOT / 'build/xr-cinema/astc-cache'
RECIPE = 1  # bump when the encoding below changes
KTX2_ID = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
VK_ASTC_4x4_UNORM, VK_ASTC_4x4_SRGB = 157, 158


def astcenc():
    for c in (os.environ.get('ASTCENC'), shutil.which('astcenc-avx2'), shutil.which('astcenc'),
              'C:/workspace/tools/astcenc/bin/astcenc-avx2.exe'):
        if c and Path(c).is_file():
            return c
    sys.exit('astcenc not found: set ASTCENC to ARM astcenc (https://github.com/ARM-software/astc-encoder)')


def mip_chain(img, normal):
    """Box-filtered levels down to 1x1; normal maps renormalised."""
    levels = [img]
    while img.width > 1 or img.height > 1:
        img = img.resize((max(1, img.width // 2), max(1, img.height // 2)), Image.BOX)
        if normal:
            a = np.asarray(img).astype(np.float32)
            n = a[..., :3] / 127.5 - 1
            n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
            a[..., :3] = np.clip((n + 1) * 127.5 + .5, 0, 255)
            img = Image.fromarray(a.astype(np.uint8))
        levels.append(img)
    return levels


def ktx2(width, height, srgb, levels):
    """KTX2 in the layout Lite Engine's WriteKtx2 produces (basic DFD, one
    ASTC sample, levels stored smallest first, 16-byte aligned)."""
    n = len(levels)
    head = KTX2_ID + struct.pack('<9I', VK_ASTC_4x4_SRGB if srgb else VK_ASTC_4x4_UNORM, 1, width, height,
                                 0, 0, 1, n, 0)
    dfd_at = len(head) + 16 + 16 + n * 24
    block = 24 + 16
    dfd = struct.pack('<IIHH', 4 + block, 0, 2, block) + bytes([162, 1, 2 if srgb else 1, 0, 3, 3, 0, 0, 16]) \
        + bytes(7) + struct.pack('<HBB4xII', 0, 127, 0, 0, 0xFFFFFFFF)
    body = bytearray(head + struct.pack('<IIII', dfd_at, len(dfd), 0, 0) + bytes(16) + bytes(n * 24) + dfd)
    offsets = [0] * n
    for i in reversed(range(n)):
        body += bytes((-len(body)) % 16)
        offsets[i] = len(body)
        body += levels[i]
    for i in range(n):
        struct.pack_into('<QQQ', body, len(head) + 32 + i * 24, offsets[i], len(levels[i]), len(levels[i]))
    return bytes(body)


def encode(png, srgb, normal, tool):
    key = hashlib.sha256(png + bytes([srgb, normal, RECIPE])).hexdigest()
    cached = CACHE / (key + '.ktx2')
    if cached.is_file():
        return cached.read_bytes()
    img = Image.open(io.BytesIO(png))
    img = img.convert('RGBA' if 'A' in img.getbands() else 'RGB')
    blocks = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, level in enumerate(mip_chain(img, normal)):
            src, dst = Path(tmp) / f'{i}.png', Path(tmp) / f'{i}.astc'
            level.save(src)
            subprocess.run([tool, '-cs' if srgb else '-cl', str(src), str(dst), '4x4', '-medium', '-silent'],
                           check=True)
            data = dst.read_bytes()
            w, h = int.from_bytes(data[7:10], 'little'), int.from_bytes(data[10:13], 'little')
            assert data[:4] == b'\x13\xab\xa1\x5c' and (w, h) == level.size, dst
            assert len(data) - 16 == ((w + 3) // 4) * ((h + 3) // 4) * 16, dst
            blocks.append(data[16:])
    out = ktx2(img.width, img.height, srgb, blocks)
    CACHE.mkdir(parents=True, exist_ok=True)
    cached.write_bytes(out)
    return out


def convert(src, dst, tool):
    raw = src.read_bytes()
    magic, version, _ = struct.unpack_from('<III', raw)
    assert magic == 0x46546C67 and version == 2, src
    jlen = struct.unpack_from('<I', raw, 12)[0]
    doc = json.loads(raw[20:20 + jlen])
    pos = 20 + jlen
    blen = struct.unpack_from('<I', raw, pos)[0]
    bin_ = raw[pos + 8:pos + 8 + blen]
    images, textures = doc.get('images', []), doc.get('textures', [])
    colour, normal = set(), set()

    def mark(slot, into):
        if slot and 0 <= slot.get('index', -1) < len(textures):
            src_ = textures[slot['index']].get('source', -1)
            if src_ >= 0:
                into.add(src_)
    for m in doc.get('materials', []):
        mark(m.get('pbrMetallicRoughness', {}).get('baseColorTexture'), colour)
        mark(m.get('emissiveTexture'), colour)
        mark(m.get('normalTexture'), normal)
    views = doc.get('bufferViews', [])
    data = [bin_[v.get('byteOffset', 0):v.get('byteOffset', 0) + v['byteLength']] for v in views]
    before = after = 0
    for i, image in enumerate(images):
        v = image.get('bufferView')
        if v is None or image.get('mimeType') != 'image/png':
            continue
        before += len(data[v])
        data[v] = encode(data[v], i in colour, i in normal, tool)
        after += len(data[v])
        image['mimeType'] = 'image/ktx2'
    new_bin = bytearray()
    for v, chunk in zip(views, data):
        new_bin += bytes((-len(new_bin)) % 16)
        v['byteOffset'] = len(new_bin)
        v['byteLength'] = len(chunk)
        new_bin += chunk
    new_bin += bytes((-len(new_bin)) % 4)
    doc['buffers'][0]['byteLength'] = len(new_bin)
    j = json.dumps(doc, separators=(',', ':')).encode()
    j += b' ' * ((-len(j)) % 4)
    body = struct.pack('<I4s', len(j), b'JSON') + j + struct.pack('<I4s', len(new_bin), b'BIN\0') + new_bin
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(struct.pack('<III', 0x46546C67, 2, 12 + len(body)) + body)
    return before, after


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/xr-cinema/apk-models')
    args = ap.parse_args()
    tool = astcenc()
    seen = set()
    total_png = total_ktx = 0
    for src in sorted(SRC.rglob('*')):
        rel = src.relative_to(SRC)
        if not src.is_file() or src.name == '.gitignore':
            continue
        dst = args.out / rel
        seen.add(dst)
        if src.suffix == '.glb':
            png, ktx = convert(src, dst, tool)
            total_png += png
            total_ktx += ktx
        elif not dst.is_file() or dst.read_bytes() != src.read_bytes():
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
    for stale in args.out.rglob('*') if args.out.is_dir() else []:
        if stale.is_file() and stale not in seen:
            stale.unlink()
    # GPU memory: PNG decodes to 4 bytes/texel + mips; ASTC 4x4 is 1 byte/texel.
    print(f'XR_CINEMA_ASTC_PASS {len(seen)} files, embedded images {total_png / 1048576:.1f} MiB PNG -> '
          f'{total_ktx / 1048576:.1f} MiB KTX2 (ASTC 4x4 + mips) in {args.out}')


if __name__ == '__main__':
    main()
