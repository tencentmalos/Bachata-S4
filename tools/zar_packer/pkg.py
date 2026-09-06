# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""PS4 PKG reader/extractor.

A Python port of the PKG/PFS code that lived in ``src/core/file_format/pkg.cpp``
and ``src/core/crypto/crypto.cpp`` before commit be22674f removed it from the
emulator. The emulator no longer parses PKG at runtime, so extraction has to
happen out-of-tree; this module keeps that logic close to the original so the
two can be diffed if PFS handling ever needs revisiting.

Only fake-signed (fpkg) packages decrypt here. Retail packages are signed with
keys that are not in this repository, and the RSA step will produce garbage
rather than a clear error -- ``PkgFile.open`` raises when the resulting PFS
header fails its sanity check.
"""

from __future__ import annotations

import dataclasses
import hashlib
import hmac
import struct
import zlib
from pathlib import Path
from typing import BinaryIO, Callable, Iterator

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

from ._pkg_keys import FakeKeyset, PkgDerivedKey3Keyset
from ._pkg_types import entry_name

PKG_MAGIC = 0x7F434E54
PFSC_MAGIC = 0x43534650

# PFS dirent types (pfs.h).
PFS_FILE = 2
PFS_DIR = 3
PFS_CURRENT_DIR = 4
PFS_PARENT_DIR = 5

PFS_BLOCK_SIZE = 0x10000
XTS_SECTOR_SIZE = 0x1000
INODE_SIZE = 0xA8

ProgressFn = Callable[[str, int, int], None]


class PkgError(RuntimeError):
    """Raised when a PKG cannot be parsed or decrypted."""


# ── crypto primitives ────────────────────────────────────────────────
# Ports of Crypto:: methods. The PS4 uses textbook RSA with PKCS#1 v1.5
# padding, AES-CBC with a key/IV pair derived by SHA-256, and AES-XTS for the
# PFS image. cryptography's high-level RSA API insists on well-formed keys and
# valid padding, so the modular exponentiation is done directly instead.


def _rsa2048_decrypt(ciphertext: bytes, keyset: dict[str, bytes]) -> bytes:
    """Raw RSA-2048 private-key operation, then strip PKCS#1 v1.5 padding.

    Mirrors ``Crypto::RSA2048Decrypt``. Returns the full padded block when no
    valid 0x00 0x02 .. 0x00 structure is found; callers only read the leading
    32 bytes, and the C++ original was equally forgiving.
    """
    n = int.from_bytes(keyset["Modulus"], "big")
    d = int.from_bytes(keyset["PrivateExponent"], "big")
    c = int.from_bytes(ciphertext, "big")
    m = pow(c, d, n)
    block = m.to_bytes(256, "big")

    if block[0] == 0x00 and block[1] == 0x02:
        sep = block.find(b"\x00", 2)
        if sep != -1:
            return block[sep + 1 :]
    return block


def _iv_key_hash256(cipher_input: bytes) -> bytes:
    """``Crypto::ivKeyHASH256`` -- SHA-256 over the 64-byte entry||dk3 blob."""
    return hashlib.sha256(cipher_input).digest()


def _aes_cbc_decrypt(ivkey: bytes, ciphertext: bytes) -> bytes:
    """``Crypto::aesCbcCfb128Decrypt``: key is ivkey[16:32], IV is ivkey[0:16].

    Whole blocks only; a trailing partial block is dropped. Used for the
    256-byte IMAGE_KEY blob, which is always block-aligned.
    """
    key, iv = ivkey[16:32], ivkey[0:16]
    usable = len(ciphertext) - (len(ciphertext) % 16)
    decryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    return decryptor.update(ciphertext[:usable]) + decryptor.finalize()


def _aes_cbc_decrypt_entry(ivkey: bytes, ciphertext: bytes, size: int) -> bytes:
    """``Crypto::aesCbcCfb128DecryptEntry`` -- for license/npbind entries.

    These entries are not block-aligned. The C++ reads ``size`` bytes into a
    zero-padded block-aligned buffer, decrypts the whole thing, then writes back
    only the first ``size`` bytes. The final partial block therefore decrypts
    against trailing zeros rather than being left as ciphertext.
    """
    key, iv = ivkey[16:32], ivkey[0:16]
    padded_size = (size + 15) // 16 * 16
    padded = ciphertext[:size].ljust(padded_size, b"\x00")
    decryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    return (decryptor.update(padded) + decryptor.finalize())[:size]


def _pfs_gen_crypto_key(ekpfs: bytes, seed: bytes) -> tuple[bytes, bytes]:
    """``Crypto::PfsGenCryptoKey`` -- returns ``(data_key, tweak_key)``.

    HMAC-SHA256(ekpfs) over ``u32 index=1 || seed``; the digest's first half is
    the tweak key and the second half the data key. Note the swap: the C++ code
    copies into ``tweakKey`` first.
    """
    d = struct.pack("<I", 1) + seed
    digest = hmac.new(ekpfs, d, hashlib.sha256).digest()
    tweak_key, data_key = digest[0:16], digest[16:32]
    return data_key, tweak_key


def _decrypt_pfs(data_key: bytes, tweak_key: bytes, src: bytes, sector: int) -> bytes:
    """``Crypto::decryptPFS`` -- AES-XTS over 0x1000-byte sectors.

    The tweak is the little-endian sector number. ``src`` must be a whole
    number of sectors; the caller pads reads to guarantee that.
    """
    out = bytearray(len(src))
    xts_key = data_key + tweak_key  # cryptography takes data||tweak concatenated
    for i in range(0, len(src), XTS_SECTOR_SIZE):
        chunk = src[i : i + XTS_SECTOR_SIZE]
        if len(chunk) < 16:
            out[i : i + len(chunk)] = chunk
            continue
        current_sector = sector + (i // XTS_SECTOR_SIZE)
        tweak = struct.pack("<Q", current_sector) + b"\x00" * 8
        decryptor = Cipher(algorithms.AES(xts_key), modes.XTS(tweak)).decryptor()
        out[i : i + len(chunk)] = decryptor.update(chunk) + decryptor.finalize()
    return bytes(out)


def _decompress_pfsc(compressed: bytes, out_size: int = PFS_BLOCK_SIZE) -> bytes:
    """zlib-inflate one PFSC block, tolerating the truncated streams PFS uses."""
    d = zlib.decompressobj()
    try:
        data = d.decompress(compressed, out_size)
    except zlib.error as exc:
        raise PkgError(f"PFSC block failed to inflate: {exc}") from exc
    if len(data) < out_size:
        data += b"\x00" * (out_size - len(data))
    return data[:out_size]


# ── header structures ────────────────────────────────────────────────

# Only the fields the extractor actually reads; offsets are from pkg.h.
_HDR_FIELDS = {
    "magic": (0x00, ">I"),
    "pkg_type": (0x04, ">I"),
    "pkg_file_count": (0x0C, ">I"),
    "pkg_table_entry_count": (0x10, ">I"),
    "pkg_table_entry_offset": (0x18, ">I"),
    "pkg_body_offset": (0x24, ">Q"),
    "pkg_body_size": (0x2C, ">Q"),
    "pkg_content_offset": (0x34, ">Q"),
    "pkg_content_size": (0x3C, ">Q"),
    "pkg_drm_type": (0x70, ">I"),
    "pkg_content_type": (0x74, ">I"),
    "pkg_content_flags": (0x78, ">I"),
    "pfs_image_offset": (0x410, ">Q"),
    "pfs_image_size": (0x418, ">Q"),
    "mount_image_offset": (0x420, ">Q"),
    "mount_image_size": (0x428, ">Q"),
    "pkg_size": (0x430, ">Q"),
    "pfs_signed_size": (0x438, ">I"),
    "pfs_cache_size": (0x43C, ">I"),
}

PKG_HEADER_SIZE = 0x1000

CONTENT_FLAGS = {
    0x100000: "FIRST_PATCH",
    0x200000: "PATCHGO",
    0x400000: "REMASTER",
    0x800000: "PS_CLOUD",
    0x2000000: "GD_AC",
    0x4000000: "NON_GAME",
    0x8000000: "UNKNOWN_0x8000000",
    0x40000000: "SUBSEQUENT_PATCH",
    0x41000000: "DELTA_PATCH",
    0x60000000: "CUMULATIVE_PATCH",
}


@dataclasses.dataclass
class PkgEntry:
    entry_id: int
    filename_offset: int
    flags1: int
    flags2: int
    offset: int
    size: int

    @property
    def name(self) -> str:
        return entry_name(self.entry_id)

    def to_bytes(self) -> bytes:
        """Re-serialise the 32-byte on-disk record (needed for the IV hash)."""
        return struct.pack(
            ">IIIIII",
            self.entry_id,
            self.filename_offset,
            self.flags1,
            self.flags2,
            self.offset,
            self.size,
        ) + b"\x00" * 8


@dataclasses.dataclass
class Inode:
    mode: int
    nlink: int
    flags: int
    size: int
    size_compressed: int
    blocks: int
    loc: int

    @property
    def is_dir(self) -> bool:
        return bool(self.mode & 0x4000)

    @property
    def is_file(self) -> bool:
        return bool(self.mode & 0x8000)


@dataclasses.dataclass
class FsEntry:
    """One name in the PFS flat path table."""

    name: str
    inode: int
    entry_type: int
    path: str  # archive-relative, '/'-separated, no leading slash


def _parse_inode(buf: bytes) -> Inode:
    # struct Inode in pfs.h is 0x68 bytes but the on-disk stride is INODE_SIZE
    # (0xA8), so Blocks/loc live at 0x60/0x64 -- not at the end of the record.
    mode, nlink, flags, size, size_compressed = struct.unpack_from("<HHIqq", buf, 0)
    blocks, loc = struct.unpack_from("<II", buf, 0x60)
    return Inode(mode, nlink, flags, size, size_compressed, blocks, loc)


class PkgFile:
    """Reads a PS4 PKG and extracts its sce_sys entries and PFS payload."""

    def __init__(self, path: Path):
        self.path = Path(path)
        self.header: dict[str, int] = {}
        self.entries: list[PkgEntry] = []
        self.sfo: bytes = b""
        self.fs_table: list[FsEntry] = []
        self.inodes: list[Inode] = []
        self.sector_map: list[int] = []
        self.pfsc_offset: int = 0
        self._data_key: bytes = b""
        self._tweak_key: bytes = b""
        self._ekpfs: bytes = b""

    # ── header / sce_sys ─────────────────────────────────────────────

    def open(self) -> None:
        """Parse the header and entry table. Cheap: no PFS work happens here."""
        size_on_disk = self.path.stat().st_size
        with self.path.open("rb") as f:
            raw = f.read(PKG_HEADER_SIZE)
            if len(raw) < PKG_HEADER_SIZE:
                raise PkgError(f"{self.path.name}: file is too small to be a PKG")

            self.header = {
                key: struct.unpack_from(fmt, raw, off)[0]
                for key, (off, fmt) in _HDR_FIELDS.items()
            }
            if self.header["magic"] != PKG_MAGIC:
                raise PkgError(
                    f"{self.path.name}: bad magic "
                    f"{self.header['magic']:#010x}, expected {PKG_MAGIC:#010x}"
                )

            self.content_id = raw[0x40:0x64].rstrip(b"\x00").decode("ascii", "replace")
            self.title_id = self.content_id[7:16] if len(self.content_id) >= 16 else ""

            if self.header["pkg_size"] > size_on_disk:
                raise PkgError(
                    f"{self.path.name}: truncated -- header declares "
                    f"{self.header['pkg_size']} bytes, file is {size_on_disk}. "
                    "A multi-part download is probably incomplete."
                )

            self._read_entry_table(f)

    def _read_entry_table(self, f: BinaryIO) -> None:
        offset = self.header["pkg_table_entry_offset"]
        count = self.header["pkg_table_entry_count"]
        f.seek(offset)
        table = f.read(count * 32)
        self.entries = []
        for i in range(count):
            vals = struct.unpack_from(">IIIIII", table, i * 32)
            entry = PkgEntry(*vals)
            self.entries.append(entry)
            if entry.name == "param.sfo":
                f.seek(entry.offset)
                self.sfo = f.read(entry.size)

    @property
    def content_flags(self) -> list[str]:
        flags = self.header.get("pkg_content_flags", 0)
        return [name for bit, name in CONTENT_FLAGS.items() if flags & bit]

    # ── key derivation ───────────────────────────────────────────────

    def _derive_keys(self, f: BinaryIO) -> None:
        """Walk ENTRY_KEYS/IMAGE_KEY to recover ekpfs, then the XTS key pair."""
        dk3 = b""
        for entry in self.entries:
            if entry.entry_id == 0x10:  # ENTRY_KEYS
                f.seek(entry.offset)
                blob = f.read(0x20 + 7 * 0x20 + 7 * 0x100)
                key1_3 = blob[0x20 + 7 * 0x20 + 3 * 0x100 :][:0x100]
                dk3 = _rsa2048_decrypt(key1_3, PkgDerivedKey3Keyset)[:32]
            elif entry.entry_id == 0x20:  # IMAGE_KEY
                if not dk3:
                    raise PkgError("IMAGE_KEY seen before ENTRY_KEYS; PKG entry table is odd")
                f.seek(entry.offset)
                imgkeydata = f.read(0x100)
                ivkey = _iv_key_hash256(entry.to_bytes() + dk3)
                imgkey = _aes_cbc_decrypt(ivkey, imgkeydata)
                self._ekpfs = _rsa2048_decrypt(imgkey, FakeKeyset)[:32]

        if not self._ekpfs:
            raise PkgError(
                "no IMAGE_KEY entry -- this PKG has no PFS payload, or is not fake-signed"
            )

        f.seek(self.header["pfs_image_offset"] + 0x370)
        seed = f.read(16)
        self._data_key, self._tweak_key = _pfs_gen_crypto_key(self._ekpfs, seed)

    # ── PFS walk ─────────────────────────────────────────────────────

    def _load_pfs(self, f: BinaryIO) -> None:
        """Decrypt the PFS header region and build the inode + name tables."""
        length = self.header["pfs_cache_size"] * 2
        if length == 0:
            raise PkgError("pfs_cache_size is 0 -- nothing to extract")

        f.seek(self.header["pfs_image_offset"])
        encrypted = f.read(length)
        if not encrypted:
            raise PkgError("PFS image offset is past the end of the file")
        # Small packages (single-track DLC in particular) have a PFS image
        # shorter than pfs_cache_size * 2. The C++ reads into a fixed-size
        # buffer and leaves the tail zeroed, so pad rather than fail.
        if len(encrypted) < length:
            encrypted += b"\x00" * (length - len(encrypted))
        decrypted = _decrypt_pfs(self._data_key, self._tweak_key, encrypted, 0)

        self.pfsc_offset = self._find_pfsc_offset(decrypted)
        if self.pfsc_offset < 0:
            raise PkgError(
                "PFSC magic not found after decryption. This usually means the PKG "
                "is retail-signed rather than fake-signed, so the keys in this "
                "repository cannot unwrap it."
            )

        pfsc = decrypted[self.pfsc_offset :]
        # The C++ original keeps a buffer of the full cache length and only
        # fills the part after pfsc_offset, so trailing blocks read as zeros
        # instead of running off the end. Reproduce that padding.
        if len(pfsc) < length:
            pfsc += b"\x00" * (length - len(pfsc))
        _, _, _, _, block_sz2, block_offsets, _, data_length = struct.unpack_from(
            "<iiiiqqQq", pfsc, 0
        )
        if block_sz2 <= 0:
            raise PkgError("PFSC header is malformed (block size <= 0)")

        num_blocks = data_length // block_sz2
        self.sector_map = list(
            struct.unpack_from(f"<{num_blocks + 1}Q", pfsc, block_offsets)
        )

        self._walk_pfs_blocks(pfsc, num_blocks)

    @staticmethod
    def _find_pfsc_offset(pfs_image: bytes) -> int:
        for i in range(0x20000, len(pfs_image), 0x10000):
            if struct.unpack_from("<I", pfs_image, i)[0] == PFSC_MAGIC:
                return i
        return -1

    def _walk_pfs_blocks(self, pfsc: bytes, num_blocks: int) -> None:
        """Reproduce the inode/dirent scan from ``PKG::Extract``.

        The layout is: block 0 holds the superblock (inode count at 0x30),
        the next N blocks hold the inode array, and the remaining blocks hold
        dirent lists. Paths are assembled by tracking the '.' entry of each
        directory block.
        """
        ndinode = 0
        occupied_blocks = 0
        ndinode_counter = 0
        uroot_reached = False
        dinode_reached = False
        current_dir = ""
        paths: dict[int, str] = {}

        for i in range(num_blocks):
            sector_offset = self.sector_map[i]
            sector_size = self.sector_map[i + 1] - sector_offset
            compressed = pfsc[sector_offset : sector_offset + sector_size]

            if sector_size == PFS_BLOCK_SIZE:
                block = compressed
            elif sector_size < PFS_BLOCK_SIZE:
                block = _decompress_pfsc(compressed)
            else:
                continue

            if i == 0:
                ndinode = struct.unpack_from("<I", block, 0x30)[0]
                occupied_blocks = -(-(ndinode * INODE_SIZE) // PFS_BLOCK_SIZE)

            if 1 <= i <= occupied_blocks:
                for p in range(0, PFS_BLOCK_SIZE, INODE_SIZE):
                    node = _parse_inode(block[p : p + INODE_SIZE])
                    if node.mode == 0:
                        break
                    self.inodes.append(node)

            if block[0x10:0x1F] == b"flat_path_table":
                uroot_reached = True

            if uroot_reached:
                pos = 0
                while pos < PFS_BLOCK_SIZE - 16:
                    ino, _, _, entsize = struct.unpack_from("<iiii", block, pos)
                    # Order matters: the terminating entry has ino == 0 but may
                    # carry a zero entsize, so it has to be tested first or the
                    # scan spills into the next block and miscounts inodes.
                    if ino == 0:
                        paths[ndinode_counter] = ""
                        uroot_reached = False
                        break
                    ndinode_counter += 1
                    if entsize <= 0:
                        break
                    pos += entsize

            if block[0x10] == ord(".") and block[0x28:0x2A] == b"..":
                dinode_reached = True

            if dinode_reached:
                pos = 0
                end_reached = False
                while pos < PFS_BLOCK_SIZE - 16:
                    ino, etype, namelen, entsize = struct.unpack_from("<iiii", block, pos)
                    if ino == 0:
                        break
                    name = block[pos + 16 : pos + 16 + namelen].decode("utf-8", "replace")

                    if etype == PFS_CURRENT_DIR:
                        current_dir = paths.get(ino, "")
                    child = f"{current_dir}/{name}" if current_dir else name
                    paths[ino] = child

                    if etype in (PFS_FILE, PFS_DIR):
                        self.fs_table.append(FsEntry(name, ino, etype, child))
                        ndinode_counter += 1
                        if ndinode_counter + 1 == ndinode:
                            end_reached = True
                    if entsize <= 0:
                        break
                    pos += entsize
                if end_reached:
                    break

    # ── extraction ───────────────────────────────────────────────────

    def extract_sce_sys(self, dest: Path) -> list[str]:
        """Write the sce_sys/ metadata entries. Returns the names written."""
        sce_sys = dest / "sce_sys"
        sce_sys.mkdir(parents=True, exist_ok=True)
        written: list[str] = []

        with self.path.open("rb") as f:
            dk3 = b""
            for entry in self.entries:
                if entry.entry_id == 0x10:
                    f.seek(entry.offset)
                    blob = f.read(0x20 + 7 * 0x20 + 7 * 0x100)
                    key1_3 = blob[0x20 + 7 * 0x20 + 3 * 0x100 :][:0x100]
                    dk3 = _rsa2048_decrypt(key1_3, PkgDerivedKey3Keyset)[:32]

                name = entry.name or str(entry.entry_id)
                out_path = sce_sys / name
                out_path.parent.mkdir(parents=True, exist_ok=True)

                f.seek(entry.offset)
                data = f.read(entry.size)

                # npbind.dat and friends are AES-CBC wrapped with a per-entry key.
                if entry.entry_id in (0x400, 0x401, 0x402, 0x403) and dk3:
                    ivkey = _iv_key_hash256(entry.to_bytes() + dk3)
                    data = _aes_cbc_decrypt_entry(ivkey, data, entry.size)

                out_path.write_bytes(data)
                written.append(name)

        return written

    def iter_files(self) -> Iterator[FsEntry]:
        """Yield PFS file entries (directories are created by the extractor)."""
        for entry in self.fs_table:
            if entry.entry_type == PFS_FILE:
                yield entry

    def prepare(self) -> None:
        """Derive keys and build the PFS tables. Call once before extraction."""
        with self.path.open("rb") as f:
            self._derive_keys(f)
            self._load_pfs(f)

    def total_pfs_bytes(self) -> int:
        return sum(
            self.inodes[e.inode].size
            for e in self.iter_files()
            if e.inode < len(self.inodes)
        )

    def extract_pfs(self, dest: Path, progress: ProgressFn | None = None) -> int:
        """Extract every PFS file into ``dest``. Returns the file count."""
        dest.mkdir(parents=True, exist_ok=True)

        for entry in self.fs_table:
            if entry.entry_type == PFS_DIR:
                (dest / entry.path).mkdir(parents=True, exist_ok=True)

        files = list(self.iter_files())
        total = len(files)
        pfs_image_offset = self.header["pfs_image_offset"]

        with self.path.open("rb") as f:
            for index, entry in enumerate(files, start=1):
                if entry.inode >= len(self.inodes):
                    raise PkgError(
                        f"{entry.path}: inode {entry.inode} is out of range "
                        f"({len(self.inodes)} inodes parsed)"
                    )
                node = self.inodes[entry.inode]
                out_path = dest / entry.path
                out_path.parent.mkdir(parents=True, exist_ok=True)

                if progress:
                    progress(entry.path, index, total)

                self._extract_one(f, node, out_path, pfs_image_offset)

        return total

    def _extract_one(
        self, f: BinaryIO, node: Inode, out_path: Path, pfs_image_offset: int
    ) -> None:
        """Stream one file's blocks out, decrypting and inflating as needed."""
        written = 0
        with out_path.open("wb") as out:
            for j in range(node.blocks):
                map_index = node.loc + j
                if map_index + 1 >= len(self.sector_map):
                    raise PkgError(f"{out_path.name}: sector map overrun")

                sector_offset = self.sector_map[map_index]
                sector_size = self.sector_map[map_index + 1] - sector_offset
                absolute = self.pfsc_offset + sector_offset

                # XTS works on aligned 0x1000 sectors, so read from the
                # containing sector boundary and skip the leading slack.
                aligned = absolute & ~0xFFF
                slack = absolute - aligned
                read_len = ((slack + sector_size + 0xFFF) // 0x1000) * 0x1000

                f.seek(pfs_image_offset + aligned)
                encrypted = f.read(read_len)
                if len(encrypted) < read_len:
                    encrypted += b"\x00" * (read_len - len(encrypted))

                decrypted = _decrypt_pfs(
                    self._data_key, self._tweak_key, encrypted, aligned // 0x1000
                )
                compressed = decrypted[slack : slack + sector_size]

                if sector_size == PFS_BLOCK_SIZE:
                    block = compressed
                elif sector_size < PFS_BLOCK_SIZE:
                    block = _decompress_pfsc(compressed)
                else:
                    raise PkgError(f"{out_path.name}: impossible sector size {sector_size}")

                remaining = node.size - written
                if remaining <= 0:
                    break
                take = min(len(block), remaining)
                out.write(block[:take])
                written += take
