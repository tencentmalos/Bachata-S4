# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Minimal param.sfo reader.

Only what the packer needs to route content: TITLE_ID, CONTENT_ID, CATEGORY,
TITLE and APP_VER. A Python port of the read side of
``src/core/file_format/psf.cpp``.
"""

from __future__ import annotations

import struct
from pathlib import Path

PSF_MAGIC = 0x46535000  # "\0PSF" little-endian

FMT_UTF8_SPECIAL = 0x0004
FMT_UTF8 = 0x0204
FMT_INTEGER = 0x0404


class SfoError(RuntimeError):
    """Raised when a param.sfo cannot be parsed."""


def parse_sfo(data: bytes) -> dict[str, str | int]:
    """Parse a param.sfo blob into a plain dict."""
    if len(data) < 20:
        raise SfoError("param.sfo is too small")

    magic, version, key_table_off, data_table_off, num_entries = struct.unpack_from(
        "<IIIII", data, 0
    )
    if magic != PSF_MAGIC:
        raise SfoError(f"bad param.sfo magic {magic:#010x}")

    out: dict[str, str | int] = {}
    for i in range(num_entries):
        base = 20 + i * 16
        if base + 16 > len(data):
            break
        key_off, fmt, used_len, _max_len, data_off = struct.unpack_from(
            "<HHIII", data, base
        )

        key_start = key_table_off + key_off
        key_end = data.find(b"\x00", key_start)
        if key_end == -1:
            continue
        key = data[key_start:key_end].decode("utf-8", "replace")

        value_start = data_table_off + data_off
        raw = data[value_start : value_start + used_len]

        if fmt == FMT_INTEGER:
            if len(raw) >= 4:
                out[key] = struct.unpack_from("<I", raw, 0)[0]
        else:
            out[key] = raw.split(b"\x00", 1)[0].decode("utf-8", "replace")

    return out


def read_sfo(path: Path) -> dict[str, str | int]:
    return parse_sfo(Path(path).read_bytes())


def is_addon(sfo: dict[str, str | int]) -> bool:
    """True when this param.sfo describes additional content (DLC).

    Matches the emulator's own test in ``sceAppContentInitialize``, which
    accepts any CATEGORY starting with "ac".
    """
    category = sfo.get("CATEGORY")
    return isinstance(category, str) and category.startswith("ac")


def entitlement_label(sfo: dict[str, str | int]) -> str:
    """Extract the DLC entitlement label from CONTENT_ID.

    CONTENT_IDs look like ``UP4882-CUSA12878_00-P25S4XXXXXXXXXXX``; the
    emulator slices at a fixed offset of 20 characters
    (``ORBIS_APP_CONTENT_ENTITLEMENT_LABEL_OFFSET``).
    """
    content_id = sfo.get("CONTENT_ID")
    if not isinstance(content_id, str) or len(content_id) <= 20:
        return ""
    return content_id[20:]
