#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Portable packaging gate; no NDK/readelf required on the consuming host."""
import importlib.machinery
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader("turnip_build", str(ROOT / "scripts/android/build-turnip-source"))
spec = importlib.util.spec_from_loader(loader.name, loader)
build = importlib.util.module_from_spec(spec)
loader.exec_module(build)


def elf(needed=("libc.so", "libdl.so")):
    image = bytearray(1024)
    image[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHIQQQIHHHHHH", image, 16, 3, 183, 1, 0, 64, 0, 0,
                     64, 56, 2, 0, 0, 0)
    strings = b"\0"
    entries = []
    for name in needed:
        entries.append((1, len(strings)))
        strings += name.encode() + b"\0"
    entries += [(5, 0x1200), (10, len(strings)), (0, 0)]
    struct.pack_into("<IIQQQQQQ", image, 64, 1, 4, 0, 0x1000, 0, len(image), len(image), 4096)
    struct.pack_into("<IIQQQQQQ", image, 120, 2, 4, 0x100, 0x1100, 0,
                     len(entries) * 16, len(entries) * 16, 8)
    for i, entry in enumerate(entries):
        struct.pack_into("<qQ", image, 0x100 + 16 * i, *entry)
    image[0x200:0x200 + len(strings)] = strings
    return image


class TurnipRuntimeDependencies(unittest.TestCase):
    def test_stripped_elf_and_unrelated_runtime_string(self):
        image = elf()
        image[900:920] = b"libc++_shared.so\0xxxx"
        self.assertEqual(build.driver_dependencies(image), ["libc.so", "libdl.so"])

    def test_dynamic_runtime_dependencies_rejected(self):
        for runtime in ["libc++_shared.so", "libc++abi.so", "libunwind.so", "libstdc++.so.6"]:
            with self.subTest(runtime=runtime), self.assertRaisesRegex(ValueError, "dynamic C\\+\\+ runtime"):
                build.driver_dependencies(elf((runtime,)))

    def test_truncated_header(self):
        with self.assertRaisesRegex(ValueError, "out-of-file"):
            build.driver_dependencies(elf()[:32])

    def test_wrong_abi(self):
        for offset, value in [(4, 1), (5, 2), (18, 62)]:
            image = elf()
            image[offset] = value
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, "AArch64"):
                build.driver_dependencies(image)

    def test_dynamic_table_outside_file(self):
        image = elf()
        struct.pack_into("<Q", image, 120 + 8, 4096)
        with self.assertRaisesRegex(ValueError, "out-of-file"):
            build.driver_dependencies(image)

    def test_dynamic_table_without_terminator(self):
        image = elf()
        struct.pack_into("<qQ", image, 0x140, 1, 1)
        with self.assertRaisesRegex(ValueError, "unterminated"):
            build.driver_dependencies(image)

    def test_strings_in_zero_fill_are_not_file_backed(self):
        image = elf()
        struct.pack_into("<Q", image, 64 + 32, 0x200)
        with self.assertRaisesRegex(ValueError, "file-backed"):
            build.driver_dependencies(image)

    def test_needed_string_offset_out_of_bounds(self):
        image = elf()
        struct.pack_into("<Q", image, 0x108, 1000)
        with self.assertRaisesRegex(ValueError, "DT_NEEDED"):
            build.driver_dependencies(image)

    def test_needed_string_without_terminator(self):
        image = elf()
        image[0x200:0x220] = b"x" * 32
        with self.assertRaisesRegex(ValueError, "DT_NEEDED"):
            build.driver_dependencies(image)


if __name__ == "__main__":
    unittest.main()
