"""Summarise the bounded readback / texel-buffer-sync / hle-copy log lines of a run.
usage: readback_analyze.py <scale-log.txt> [more logs...]"""
import re, sys
from collections import Counter, defaultdict

rb, tb, hle, resume = Counter(), Counter(), Counter(), Counter()
rb_bytes = defaultdict(int); tb_bytes = defaultdict(int); hle_bytes = defaultdict(int)
for path in sys.argv[1:]:
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"Internal scale: readback (\S+) (\S+) L:(\d+) M:(\d+) (0x[0-9a-f]+) bytes=(\d+) scaled=(\w+) tiled=(\w+)", line)
        if m:
            key = f"{m.group(1)} {m.group(2)} L{m.group(3)} M{m.group(4)} {m.group(5)} scaled={m.group(7)} tiled={m.group(8)}"
            rb[key] += 1; rb_bytes[key] += int(m.group(6)); continue
        m = re.search(r"texel buffer sync from image (\S+) (\S+) L:(\d+) M:(\d+) (0x[0-9a-f]+) request=(\d+) guest_size=(\d+) scaled=(\w+) tiled=(\w+)", line)
        if m:
            key = f"{m.group(1)} {m.group(2)} L{m.group(3)} M{m.group(4)} {m.group(5)} req={m.group(6)} scaled={m.group(8)} tiled={m.group(9)}"
            tb[key] += 1; tb_bytes[key] += int(m.group(7)); continue
        m = re.search(r"hle copy src=(0x[0-9a-f]+) dst=(0x[0-9a-f]+) stride=(\d+) copies=(\d+) bytes=(\d+)", line)
        if m:
            key = f"src={m.group(1)} dst={m.group(2)} stride={m.group(3)}"
            hle[key] += 1; hle_bytes[key] += int(m.group(5)); continue
        m = re.search(r"pass resume after (\S+) (\S+) colors=(\d+) depth=(\w+)", line)
        if m:
            resume[f"{m.group(1)} {m.group(2)} colors={m.group(3)} depth={m.group(4)}"] += 1

def dump(title, c, b):
    print(f"\n== {title}: {sum(c.values())} lines, {sum(b.values())/1e6:.1f} MB")
    for k, n in c.most_common(15):
        print(f"  {n:5d}  {b[k]/1e6:8.1f} MB  {k}")

dump("readback (Image::Download)", rb, rb_bytes)
dump("texel buffer sync from image", tb, tb_bytes)
dump("hle copy", hle, hle_bytes)
print(f"\n== pass resume: {sum(resume.values())}")
for k, n in resume.most_common(10):
    print(f"  {n:5d}  {k}")
