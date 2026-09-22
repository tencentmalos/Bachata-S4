"""Summarise 'Internal scale' log lines of one run: promotions by reason, native
allocations, native-pass seeds. Usage: python promo_analyze.py <scale-log.txt>"""
import re, sys, collections

path = sys.argv[1]
promote = collections.Counter()
promote_lines = []
alloc_native = []
passes = []
for raw in open(path, encoding="utf-8", errors="replace"):
    line = raw.rstrip("\n")
    m = re.search(r"Internal scale: promote (.*?) \((\S+)\) (\d+)x(\d+)x(\d+) (\S+) L:(\d+) M:(\d+) S:(\d+) (0x[0-9a-f]+):(0x[0-9a-f]+) scaled=(\w+) gpu_modified=(\w+) history=(0x[0-9a-f]+) origin=(\d+) mask=(0x[0-9a-f]+)", line)
    if m:
        why, code, w, h, d, fmt, L, M, S, addr, size, scaled, gpumod, hist, origin, mask = m.groups()
        promote[(why, code)] += 1
        promote_lines.append((why, code, f"{w}x{h}", fmt, f"L{L} M{M} S{S}", addr, size, scaled, gpumod, hist, origin, mask))
        continue
    m = re.search(r"Internal scale: native allocation (.*)", line)
    if m:
        alloc_native.append(m.group(1)); continue
    m = re.search(r"Internal scale: native pass (.*)", line)
    if m:
        passes.append(m.group(1)); continue

print("== promotions by (reason text, code):")
for (why, code), n in promote.most_common():
    print(f"  {n:4d}  {why!r:40s} {code}")
print("\n== promotion lines (order of occurrence):")
for p in promote_lines:
    print("  " + " ".join(p))
print("\n== native allocations (probable render targets):")
for a in alloc_native:
    print("  " + a)
print("\n== native passes:")
for p in passes:
    print("  " + p)
