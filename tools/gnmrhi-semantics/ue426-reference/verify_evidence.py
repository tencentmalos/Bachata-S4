#!/usr/bin/env python3
"""Read-only identity/range verification; does not import names into a debugger."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("elf", type=Path)
args = parser.parse_args()
manifest = json.loads(Path(__file__).with_name("evidence.json").read_text())
data = args.elf.read_bytes()
if hashlib.sha256(data).hexdigest() != manifest["elf_sha256"]:
    raise SystemExit("ELF identity mismatch: refusing to apply Tetris 4.26 offsets")
for fn in manifest["functions"]:
    begin = int(fn["file_offset"], 16)
    blob = data[begin:begin + fn["range_bytes"]]
    if hashlib.sha256(blob).hexdigest() != fn["range_sha256"]:
        raise SystemExit("Function range mismatch: " + fn["candidate_name"])
print(f"ELF identity and {len(manifest['functions'])} function ranges verified")
